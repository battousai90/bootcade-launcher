// src/ROMScanDialog.cpp
#include "ROMScanDialog.h"
#include "i18n.h"
#include "RomScanner.h"
#include "RomResolve.h"
#include <iostream>
#include <filesystem>
#include <set>
#include <unordered_set>
#include <zlib.h>
#include <zip.h>
#include <fstream>
#include <sstream>
#include <chrono>
#include <future>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>
#include <vector>

// Throttle for the per-thread progress dispatcher: emit at most one signal
// per UI_DISPATCH_INTERVAL_MS across all worker threads. Without this, 8 async
// workers each emitting per file flooded the GTK main loop and triggered
// "Not Responding" mid-scan.
static constexpr int UI_DISPATCH_INTERVAL_MS = 100;

extern uLong hex_to_crc(const std::string& hex);
extern uLong compute_crc32_in_zip(const std::string& zip_path, const std::string& rom_name);

// filesystem clock -> time_t, matching what the ROM cache stores.
static time_t to_time_t(const std::filesystem::path& p) {
    auto ftime = std::filesystem::last_write_time(p);
    auto sctp  = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::system_clock::to_time_t(sctp);
}

// Snapshot one directory as {name, size, mtime} per file. Size and mtime are what
// let the pre-scan notice a ROM set that was rebuilt in place under the same names.
static std::vector<DatabaseManager::DirFileInfo>
snapshot_dir(const std::filesystem::path& dir, bool include_loose_files) {
    std::vector<DatabaseManager::DirFileInfo> out;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        if (!include_loose_files && entry.path().extension() != ".zip") continue;
        DatabaseManager::DirFileInfo f;
        f.filename = entry.path().filename().string();
        try {
            f.size  = (long long)std::filesystem::file_size(entry.path());
            f.mtime = (long long)to_time_t(entry.path());
        } catch (...) {} // unreadable file: keep 0/0 so it still shows up as a change
        out.push_back(std::move(f));
    }
    std::sort(out.begin(), out.end());
    return out;
}

ROMScanDialog::ROMScanDialog(Gtk::Window& parent, std::shared_ptr<DatabaseManager> db, const std::vector<std::string>& roms_paths, bool scan_recursive, bool include_loose_files,
                             const std::string& emulator)
    : Gtk::Dialog()
    , m_db(db)
    , m_roms_paths(roms_paths)
    , m_scan_recursive(scan_recursive)
    , m_include_loose_files(include_loose_files)
    , m_emulator(emulator.empty() ? std::string("fbneo") : emulator)
    , m_cancelled(false)
    , m_found_count(0)
    , m_scan_finished(false)
{
    namespace ui = SettingsUi;

    set_transient_for(parent);
    set_default_size(680, 580);
    set_position(Gtk::WIN_POS_CENTER_ON_PARENT);
    set_modal(false);  // non-modal: main window stays fully interactive

    // L'en-tete de ROM Management : tuile a pictogramme, titre, et sous-titre
    // qui porte ce que le scan est en train de lire.
    ui::Header head = ui::window_header(*this, "bc-search.svg", _("ROM Scanner"),
                                        _("Initialization..."),
                                        [this] { on_close_requested(); });
    m_step_label = head.subtitle;

    m_body.set_margin_start(20);
    m_body.set_margin_end(20);
    m_body.set_margin_top(18);
    m_body.set_margin_bottom(18);

    m_progress_bar.set_show_text(false);
    m_progress_bar.set_fraction(0.0);
    m_percentage_label.set_text("0%");
    m_percentage_label.set_halign(Gtk::ALIGN_END);
    m_percentage_label.get_style_context()->add_class("set-sub");
    m_progress_box.pack_start(m_progress_bar, Gtk::PACK_SHRINK);
    m_progress_box.pack_start(m_percentage_label, Gtk::PACK_SHRINK);
    m_body.pack_start(m_progress_box, Gtk::PACK_SHRINK);

    m_log = Gtk::make_managed<ui::LogPanel>(_("Logs"),
                                            _("What the scan is reading, step by step."),
                                            ui::LogPanel::None);
    m_body.pack_start(*m_log, Gtk::PACK_EXPAND_WIDGET);
    m_main_box.pack_start(m_body, Gtk::PACK_EXPAND_WIDGET);

    // Actions en bas a droite, la principale en dernier.
    auto* foot = ui::footer();
    m_close_button  = ui::button(_("Close"), "", ui::Tone::Accent);
    m_bg_button     = ui::button(_("Run in Background"), "bc-window.svg");
    m_cancel_button = ui::button(_("Cancel"));
    m_close_button->set_size_request(96, -1);
    m_cancel_button->set_size_request(96, -1);
    m_close_button->set_sensitive(false);
    foot->pack_end(*m_close_button, Gtk::PACK_SHRINK);
    foot->pack_end(*m_bg_button, Gtk::PACK_SHRINK);
    foot->pack_end(*m_cancel_button, Gtk::PACK_SHRINK);
    m_main_box.pack_start(*foot, Gtk::PACK_SHRINK);

    m_cancel_button->signal_clicked().connect(sigc::mem_fun(*this, &ROMScanDialog::on_cancel_clicked));

    // "Run in Background": hide the window, scan continues, progress shown in status bar
    m_bg_button->signal_clicked().connect([this]() {
        hide();
        m_signal_run_in_background.emit();
    });

    m_close_button->signal_clicked().connect([this]() { response(Gtk::RESPONSE_CLOSE); });

    get_content_area()->set_spacing(0);
    get_content_area()->pack_start(m_main_box, Gtk::PACK_EXPAND_WIDGET);

    // Connect dispatchers
    m_progress_dispatcher.connect(sigc::mem_fun(*this, &ROMScanDialog::on_progress_update));
    m_finished_dispatcher.connect(sigc::mem_fun(*this, &ROMScanDialog::on_scan_finished));

    show_all_children();
}

ROMScanDialog::~ROMScanDialog() {
    if (m_worker_thread.joinable()) {
        m_cancelled = true;
        m_worker_thread.join();
    }
}

void ROMScanDialog::start_scan() {
    m_cancelled = false;
    m_found_count = 0;
    m_scan_finished = false;
    
    add_log_message("Starting ROM scan...");
    
    // Start worker thread
    m_worker_thread = std::thread(m_emulator == "fbneo" ? &ROMScanDialog::worker_thread
                                                        : &ROMScanDialog::worker_cache_scan, this);
}

void ROMScanDialog::worker_thread() {
    try {
        // Get game count from database (fast COUNT query)
        size_t total_games = m_db->getGameCount();
        add_log_message("Database loaded: " + std::to_string(total_games) + " games");

        // Get current DAT timestamp
        time_t current_dat_timestamp = m_db->getLastDatTimestamp();

        // Cleanup cache: remove entries for files that no longer exist
        add_log_message("Cleaning up ROM cache...");
        // First, detect any saved roots that were removed from settings and
        // explicitly delete their cache/snapshots so removed folders don't linger.
        std::vector<std::string> saved_roots;
        if (m_db->getSavedRomRoots(saved_roots)) {
            // Build canonicalized current set for reliable comparison
            std::set<std::string> current_set;
            for (const auto& p : m_roms_paths) {
                std::string c = p;
                try { if (!p.empty() && std::filesystem::exists(p)) c = std::filesystem::canonical(p).string(); } catch (...) { c = p; }
                if (!c.empty() && c.back() == '/') c.pop_back();
                current_set.insert(c);
            }

            for (const auto& old_root : saved_roots) {
                if (current_set.find(old_root) == current_set.end()) {
                    add_log_message("Detected removed ROM root: " + old_root + " : purging cache and resetting affected games", Level::Muted);
                    // Reset only games that were found in this specific directory
                    m_db->resetGamesFromDirectory(old_root);
                    // Purge cache entries (rom_cache, snapshots, file lists) for
                    // this root without any safety threshold : the user explicitly
                    // removed this root, so the deletion is intentional.
                    m_db->purgeCacheForRoot(old_root);
                }
            }
        }

        // Helper: update the status message and pulse the progress bar.
        // Called from slow phases where total count is unknown.
        int prescan_dir_count = 0;
        auto update_prescan = [&](const std::string& msg) {
            ++prescan_dir_count;
            // Oscillate progress between 0 and 15% to show activity (indeterminate feel)
            double pulse = 5.0 + 10.0 * (0.5 + 0.5 * std::sin(prescan_dir_count * 0.15));
            {
                std::lock_guard<std::mutex> lk(m_shared_mutex);
                m_current_progress.store(pulse);
                m_current_message = msg;
            }
            // Don't flood the dispatcher : update every 10 directories
            if (prescan_dir_count % 10 == 0)
                m_progress_dispatcher();
        };

        // Ensure general cleanup for other reasons (deleted files or paths outside configured roots)
        {
            std::lock_guard<std::mutex> lk(m_shared_mutex);
            m_current_message = "Cleaning up ROM cache...";
        }
        m_progress_dispatcher();
        m_db->cleanupRomCache(m_roms_paths);
        // And the archives of a directory that no library holds any more.
        if (int n = RomScanner::prune_zip_cache(m_db))
            add_log_message(std::to_string(n) + " archive(s) no longer in any library : forgotten", Level::Muted);

        // Pre-scan: compute per-directory file counts and mtimes to avoid
        // deep scans for folders that haven't changed since last snapshot.
        add_log_message("Pre-scan directories to detect changed folders...");
        std::vector<std::string> paths_to_scan;
        int missing_roots = 0;
        for (const auto& root_path : m_roms_paths) {
            try {
                if (!std::filesystem::exists(root_path)) {
                    // Surface unmounted / typo'd paths instead of silently skipping // before this, a missing drive looked like "no changes detected".
                    add_log_message("Configured ROM path does not exist: " + root_path, Level::Warn);
                    ++missing_roots;
                    continue;
                }

                for (auto it = std::filesystem::recursive_directory_iterator(root_path); it != std::filesystem::recursive_directory_iterator(); ++it) {
                    try {
                        if (!it->is_directory()) continue;
                        std::string dirpath = it->path().string();

                        // Show current directory in the status bar
                        std::string short_dir = dirpath.size() > 50
                            ? "…" + dirpath.substr(dirpath.size() - 49) : dirpath;
                        update_prescan("Checking: " + short_dir);

                        // Build file list directly under this directory (non-recursive)
                        auto current_files = snapshot_dir(it->path(), m_include_loose_files);

                        // Compare with the stored list: a difference in names OR in
                        // any file's size/mtime schedules a deep scan.
                        std::vector<DatabaseManager::DirFileInfo> prev_files;
                        bool has_prev = m_db->getDirectoryFileList(dirpath, prev_files);
                        if (!has_prev) {
                            paths_to_scan.push_back(dirpath);
                        } else {
                            std::sort(prev_files.begin(), prev_files.end());
                            if (prev_files != current_files) paths_to_scan.push_back(dirpath);
                        }
                    } catch (...) {
                        continue;
                    }
                }
                // Also include the root_path itself (in case files sit at top level)
                try {
                    std::string dirpath = root_path;
                    update_prescan("Checking root: " + root_path);
                    auto current_files = snapshot_dir(root_path, m_include_loose_files);
                    std::vector<DatabaseManager::DirFileInfo> prev_files;
                    bool has_prev = m_db->getDirectoryFileList(dirpath, prev_files);
                    if (!has_prev) {
                        paths_to_scan.push_back(dirpath);
                    } else {
                        std::sort(prev_files.begin(), prev_files.end());
                        if (prev_files != current_files) paths_to_scan.push_back(dirpath);
                    }
                } catch (...) {}
            } catch (...) {
                continue;
            }
        }
        // Final dispatch to flush last directory name
        m_progress_dispatcher();

        // If nothing changed at folder level, skip deep scan entirely
        if (paths_to_scan.empty()) {
            if (missing_roots > 0 && missing_roots == (int)m_roms_paths.size()) {
                add_log_message("All " + std::to_string(missing_roots)
                                + " configured ROM paths are missing : nothing to scan.", Level::Error);
                add_log_message("Check that the drive is mounted or update the paths in Settings.", Level::Muted);
            } else if (missing_roots > 0) {
                add_log_message(std::to_string(missing_roots)
                                + " ROM path(s) missing; the rest had no folder-level changes.", Level::Warn);
            } else {
                add_log_message("No folder-level changes detected : skipping deep scan", Level::Ok);
            }
            // Also update saved roots to current configuration
            m_db->updateSavedRomRoots(m_roms_paths);
            // Update counts / found games and finish
            size_t available_count = m_db->getGameCountByStatus("available");
            size_t incorrect_count = m_db->getGameCountByStatus("incorrect");
            m_found_count = available_count + incorrect_count;
            add_log_message("Cache results: " + std::to_string(m_found_count) + " games found");
            m_scan_finished = true;
            m_finished_dispatcher();
            return;
        }

        // Replace roms_paths with the reduced set of directories that changed
        std::vector<std::string> effective_roots = paths_to_scan;

        add_log_message("Checking for outdated ROM files...");
        {
            std::lock_guard<std::mutex> lk(m_shared_mutex);
            m_current_progress.store(5.0);
            m_current_message = "Checking for outdated ROM files in " + std::to_string(effective_roots.size()) + " directories...";
        }
        m_progress_dispatcher();

        // Get only outdated/new ROM files that need scanning (use reduced roots).
        // This walks every file under effective_roots with no other feedback, so
        // on a large or slow (external/network) drive it can look hung for
        // minutes : report a live counter, throttled the same way the per-file
        // scan progress below is. There's no cheap way to know the true file
        // count up front without a separate full walk, so the already-cached
        // ROM count is used as a stand-in denominator: usually close (the
        // library doesn't change size much scan to scan), always cheap (one
        // indexed COUNT query), and it beats a bar frozen at 5% even when off.
        std::atomic<long long> last_outdated_dispatch_ms{0};
        int cache_estimate = std::max(1, m_db->getRomCacheCount());
        std::vector<std::string> files_to_scan = m_db->getOutdatedRomFiles(
            effective_roots, current_dat_timestamp, m_scan_recursive, m_include_loose_files,
            [this, &last_outdated_dispatch_ms, cache_estimate](size_t checked, const std::string& current_root) -> bool {
                {
                    std::lock_guard<std::mutex> lk(m_shared_mutex);
                    // Capped short of 100: the estimate can undershoot the real
                    // count, and this phase isn't "done" until the call returns.
                    double pct = std::min(95.0, 5.0 + (double)checked / cache_estimate * 90.0);
                    m_current_progress.store(pct);
                    m_current_message = "Checking for outdated ROM files… " + std::to_string(checked)
                        + " scanned (" + std::filesystem::path(current_root).filename().string() + ")";
                }
                auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                long long prev = last_outdated_dispatch_ms.load(std::memory_order_relaxed);
                if ((now_ms - prev) >= UI_DISPATCH_INTERVAL_MS) {
                    if (last_outdated_dispatch_ms.compare_exchange_strong(prev, now_ms, std::memory_order_acq_rel))
                        m_progress_dispatcher();
                }
                return !m_cancelled;
            });

        if (m_cancelled) {
            add_log_message("Scan cancelled", Level::Warn);
            m_scan_finished = true;
            m_finished_dispatcher();
            return;
        }

        if (files_to_scan.empty()) {
            add_log_message("All ROM files are up to date - no scanning needed!", Level::Ok);
            add_log_message("Tip: ROMs will be re-scanned if you update DAT files or add/modify ZIP files", Level::Muted);

            // Just count cached results
            size_t available_count = m_db->getGameCountByStatus("available");
            size_t incorrect_count = m_db->getGameCountByStatus("incorrect");
            m_found_count = available_count + incorrect_count;

            add_log_message("Cache results: " + std::to_string(m_found_count) + " games found");
            m_scan_finished = true;
            m_finished_dispatcher();
            return;
        }

        add_log_message("Found " + std::to_string(files_to_scan.size()) + " ROM files to scan (new or modified)");

        // Log the directories we will deep-scan with counts
        for (const auto& d : effective_roots) {
            int count = 0;
            try {
                for (const auto& e : std::filesystem::directory_iterator(d)) {
                    if (!e.is_regular_file()) continue;
                    if (!m_include_loose_files && e.path().extension() != ".zip") continue;
                    ++count;
                }
            } catch (...) {}
            add_log_message("Scanning directory: " + d + " (" + std::to_string(count) + " files)");
        }

        // Only reset games to missing if we're doing an incremental scan
        // For full scans, we'll keep existing statuses and only update what changes
        if (files_to_scan.size() > 0) {
            add_log_message("Preparing for incremental scan...");
        }
        
        // NOUVELLE LOGIQUE PROPRE : ZIP -> fichiers -> nom+CRC -> query DB
        add_log_message("Processing ROM files with clean logic...");

        // Collect file metadata for cache (BEFORE transaction)
        struct FileMetadata {
            std::string filename;
            std::string filepath;
            time_t last_modified;
            size_t file_size;
        };
        std::vector<FileMetadata> scanned_files;
        scanned_files.reserve(files_to_scan.size());

        for (const auto& rom_file : files_to_scan) {
            std::string filename = std::filesystem::path(rom_file).filename().string();
            auto ftime = std::filesystem::last_write_time(std::filesystem::path(rom_file));
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
            );
            time_t last_modified = std::chrono::system_clock::to_time_t(sctp);
            size_t file_size = std::filesystem::file_size(rom_file);

            scanned_files.push_back({filename, rom_file, last_modified, file_size});
        }

        // ── Parallel ZIP scan ────────────────────────────────────────────────
        // Worker threads scan ZIPs and collect (name, system, status) results.
        // Only the main scan thread writes to the DB (inside a transaction).
        struct GameResult { std::string name, system, status, source_directory; };

        const size_t hw = std::max(1u, std::thread::hardware_concurrency());
        const size_t num_threads = std::min(hw, files_to_scan.size());
        add_log_message("Using " + std::to_string(num_threads) + " parallel threads");

        // Shared counter for progress reporting
        std::atomic<int> processed_count{0};
        // Last UI dispatch timestamp (ms since epoch). Workers only fire the
        // dispatcher if at least UI_DISPATCH_INTERVAL_MS elapsed since the
        // previous emission : this is the dam against dispatcher flooding.
        std::atomic<long long> last_dispatch_ms{0};
        std::mutex results_mutex;
        std::vector<GameResult> all_results;
        all_results.reserve(files_to_scan.size() * 2); // avg ~2 candidates per ZIP
        // Content-addressed cache payload: {zip_path -> [{entry_name, crc}, ...]}.
        // Collected here, persisted to zip_contents inside the registration tx below.
        std::vector<std::pair<std::string, std::vector<std::pair<std::string, unsigned long>>>> all_contents;
        all_contents.reserve(files_to_scan.size());

        // Divide work into chunks, one per thread
        size_t chunk = (files_to_scan.size() + num_threads - 1) / num_threads;
        std::vector<std::future<void>> futures;
        futures.reserve(num_threads);

        for (size_t t = 0; t < num_threads; ++t) {
            size_t begin = t * chunk;
            size_t end   = std::min(begin + chunk, files_to_scan.size());
            if (begin >= end) break;

            futures.emplace_back(std::async(std::launch::async,
                [&, begin, end]() {
                    for (size_t i = begin; i < end; ++i) {
                        if (m_cancelled) return;
                        const auto& zip_path = files_to_scan[i];
                        std::string game_name = std::filesystem::path(zip_path).stem().string();

                        // scan_zip_file_collect: returns results without touching DB.
                        // out_entries captures the ZIP's {name, crc} contents for the cache.
                        std::vector<std::pair<std::string, unsigned long>> entries;
                        auto chunk_results = RomScanner::scan_zip_file_collect(zip_path, m_db, &entries);
                        {
                            std::lock_guard<std::mutex> lk(results_mutex);
                            for (auto& r : chunk_results)
                                all_results.push_back({r.name, r.system, r.status, r.source_directory});
                            all_contents.emplace_back(zip_path, std::move(entries));
                        }

                        int done = ++processed_count;
                        double pct = static_cast<double>(done) / files_to_scan.size() * 100.0;
                        {
                            std::lock_guard<std::mutex> lk(m_shared_mutex);
                            m_current_progress.store(pct);
                            m_current_message = "Processing: " + game_name
                                + " (" + std::to_string(done) + "/" + std::to_string(files_to_scan.size()) + ")";
                        }

                        // Throttle dispatcher: only fire if enough time has passed
                        // since the last emission. Always fire on the final file so
                        // the UI shows 100% at the end.
                        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                        long long prev = last_dispatch_ms.load(std::memory_order_relaxed);
                        bool is_last = (done == (int)files_to_scan.size());
                        if (is_last || (now_ms - prev) >= UI_DISPATCH_INTERVAL_MS) {
                            if (last_dispatch_ms.compare_exchange_strong(prev, now_ms,
                                    std::memory_order_acq_rel)) {
                                m_progress_dispatcher();
                            }
                        }

                        if (done % 50 == 0 || is_last)
                            add_log_message("Processed " + std::to_string(done)
                                + "/" + std::to_string(files_to_scan.size()) + " ZIP files");
                    }
                }
            ));
        }

        // Wait for all threads
        for (auto& f : futures) f.get();
        int games_processed = processed_count.load();

        // Collapse the results to the best status per (game, system).
        // A game name can exist in several systems, and a ZIP is offered to every
        // candidate sharing its name : so a foreign folder's ZIP gets to vote on a
        // game it does not own. Zemina's Korean titles ship the very same dump as
        // ".rom" on MSX and ".sms" on Master System: scanning the MSX ZIP finds the
        // Master System game's ROM by CRC under the "wrong" filename and votes
        // "incorrect", which could overwrite the "available" its own folder's ZIP
        // had just earned. Keeping the best verdict makes the outcome correct and
        // order-independent (workers finish in arbitrary order).
        auto rank = [](const std::string& s) {
            return s == "available" ? 2 : s == "incorrect" ? 1 : 0;
        };
        std::unordered_map<std::string, GameResult> best_by_game;
        best_by_game.reserve(all_results.size());
        for (const auto& r : all_results) {
            if (r.status.empty()) continue;
            std::string key = r.name + '\x1f' + r.system;
            auto it = best_by_game.find(key);
            if (it == best_by_game.end() || rank(r.status) > rank(it->second.status))
                best_by_game[key] = r;
        }

        if (m_cancelled) {
            // Commit whatever was found before the user cancelled : don't throw away partial results.
            if (!best_by_game.empty()) {
                add_log_message("Saving " + std::to_string(best_by_game.size()) + " partial results before cancel...");
                if (m_db->beginTransaction()) {
                    for (const auto& [key, r] : best_by_game)
                        m_db->updateGameStatusWithSource(r.name, r.status, r.system, r.source_directory);
                    m_db->commitTransaction();
                    m_found_count = (int)best_by_game.size();
                }
            }
            add_log_message("Scan cancelled : " + std::to_string(m_found_count) + " results saved", Level::Warn);
            m_scan_finished = true;
            m_finished_dispatcher();
            return;
        }

        add_log_message("ZIP scan complete : writing " + std::to_string(best_by_game.size()) + " status updates to DB", Level::Ok);

        // BEGIN TRANSACTION : write all collected results in one batch
        if (!m_db->beginTransaction()) {
            add_log_message("Failed to start transaction", Level::Error);
            m_scan_finished = true;
            m_finished_dispatcher();
            return;
        }

        for (const auto& [key, r] : best_by_game) {
            m_db->updateGameStatusWithSource(r.name, r.status, r.system, r.source_directory);
        }

        // Register scanned files in cache (BEFORE committing transaction).
        // We use the metadata already collected in scanned_files : no extra filesystem reads.
        add_log_message("Updating ROM cache (" + std::to_string(scanned_files.size()) + " files)...");

        int successful_registrations = 0;
        const size_t batch_size = 500;
        size_t batch_count = 0;

        // Index the collected ZIP contents by path for O(1) lookup during registration.
        std::unordered_map<std::string, std::vector<std::pair<std::string, unsigned long>>*> contents_by_path;
        contents_by_path.reserve(all_contents.size());
        for (auto& c : all_contents) contents_by_path[c.first] = &c.second;

        for (size_t i = 0; i < scanned_files.size(); ++i) {
            const auto& metadata = scanned_files[i];

            // Use the metadata collected during the pre-scan : no redundant filesystem reads.
            if (m_db->registerRomFile(metadata.filename, metadata.filepath,
                                          metadata.last_modified, metadata.file_size, current_dat_timestamp)) {
                successful_registrations++;
            }

            // Persist the ZIP's content fingerprint so game status can be re-derived
            // from cache after a DAT update (no disk I/O). Skip empty payloads
            // (loose/non-ZIP files produce none).
            auto cit = contents_by_path.find(metadata.filepath);
            if (cit != contents_by_path.end() && !cit->second->empty()) {
                m_db->storeZipContents(metadata.filepath, *cit->second);
            }

            // Update progress
            if ((i % 50) == 0 || i + 1 == scanned_files.size()) {
                double reg_progress = static_cast<double>(i + 1) / static_cast<double>(scanned_files.size()) * 100.0;
                {
                    std::lock_guard<std::mutex> lk(m_shared_mutex);
                    m_current_progress.store(reg_progress);
                    m_current_message = "Registering cache: " + std::to_string(i + 1) + "/" + std::to_string(scanned_files.size());
                }
                m_progress_dispatcher();
            }

            batch_count++;
            if (batch_count >= batch_size) {
                // Commit current transaction and start a new one to keep transaction sizes reasonable
                if (!m_db->commitTransaction()) {
                    add_log_message("Failed to commit batch updates", Level::Error);
                    m_db->rollbackTransaction();
                    break;
                }
                add_log_message("Committed batch of " + std::to_string(batch_count) + " cache registrations");
                if (!m_db->beginTransaction()) {
                    add_log_message("Failed to start transaction after batch commit", Level::Error);
                    break;
                }
                batch_count = 0;
            }
        }
        add_log_message("Registered " + std::to_string(successful_registrations) + "/" + std::to_string(scanned_files.size()) + " files in cache", Level::Ok);

        // COMMIT TRANSACTION - commits both game updates AND cache registrations together.
        // On cancel we still commit partial results rather than discarding them.
        if (!m_db->commitTransaction()) {
            add_log_message("Failed to commit updates", Level::Error);
            m_db->rollbackTransaction();
            m_scan_finished = true;
            m_finished_dispatcher();
            return;
        }
        if (m_cancelled) {
            add_log_message("Scan cancelled : partial results saved", Level::Warn);
            m_scan_finished = true;
            m_finished_dispatcher();
            return;
        }
        add_log_message("Game statuses and cache updated", Level::Ok);

        // Playable : a zip on its own cannot say whether FinalBurn Neo finds
        // the ROMs a set inherits in its parent's or its BIOS's archive, so
        // the per-zip votes above only looked at the set's own. Now that
        // every archive read this run is in the cache, re-derive the sets that
        // inherit : the ones just scanned, and the clones of any parent just
        // scanned. Whatever the merge mode of their DAT : that is the ROM
        // Manager's business, not the emulator's.
        {
            std::unordered_set<std::string> touched;
            for (const auto& f : scanned_files) {
                std::string stem = std::filesystem::path(f.filename).stem().string();
                std::transform(stem.begin(), stem.end(), stem.begin(),
                               [](unsigned char c) { return (char)std::tolower(c); });
                touched.insert(stem);
            }
            add_log_message("Resolving inherited ROMs through parent and BIOS sets...");
            int changed = RomResolve::resolve_inherited_from_cache(m_db, m_roms_paths, touched);
            add_log_message(std::to_string(changed) + " set status(es) changed by inheritance");
        }

        // Verify cache entries were actually inserted
            // Update directory snapshots for the directories we decided to scan
            try {
                for (const auto& dir : effective_roots) {
                    try {
                        if (!std::filesystem::exists(dir)) continue;
                        auto current_files = snapshot_dir(dir, m_include_loose_files);
                        time_t latest_mtime = 0;
                        for (const auto& f : current_files)
                            if ((time_t)f.mtime > latest_mtime) latest_mtime = (time_t)f.mtime;
                        m_db->updateDirectorySnapshot(dir, (int)current_files.size(), latest_mtime);
                        m_db->updateDirectoryFileList(dir, current_files);
                    } catch (...) { continue; }
                }
            } catch (...) {}

        int cache_count = m_db->getRomCacheCount();
        std::cerr << "[DEBUG] Cache now contains " << cache_count << " entries" << std::endl;
        add_log_message("Verified: ROM cache now contains " + std::to_string(cache_count) + " entries");

        // Persist the current configured ROM roots so future scans can detect removals
        m_db->updateSavedRomRoots(m_roms_paths);

        // Count found games using fast SQL COUNT queries
        size_t available_count = m_db->getGameCountByStatus("available");
        size_t incorrect_count = m_db->getGameCountByStatus("incorrect");
        m_found_count = available_count + incorrect_count;
        
        if (!m_cancelled) {
            add_log_message("Scan completed! " + std::to_string(m_found_count) + " games found from " + std::to_string(games_processed) + " files processed", Level::Ok);
        }
        
        m_scan_finished = true;
        m_finished_dispatcher();
        
    } catch (const std::exception& e) {
        add_log_message("Error: " + std::string(e.what()), Level::Error);
        m_scan_finished = true;
        m_finished_dispatcher();
    }
}

void ROMScanDialog::worker_cache_scan() {
    try {
        add_log_message("Scanning the " + m_emulator + " library: "
                        + std::to_string(m_roms_paths.size()) + " ROM path(s)");
        auto rep = RomScanner::scan_into_cache(m_db, m_roms_paths, m_emulator, m_scan_recursive,
            [this](double pct, const std::string& msg) {
                {
                    std::lock_guard<std::mutex> lk(m_shared_mutex);
                    m_current_progress.store(pct);
                    m_current_message = msg;
                }
                m_progress_dispatcher();
                return !m_cancelled;
            },
            [this](const std::string& line, bool warn) {
                add_log_message(line, warn ? Level::Warn : Level::Info);
            });
        if (rep.missing_roots > 0 && rep.missing_roots == (int)m_roms_paths.size())
            add_log_message("Check that the drive is mounted or update the paths in Settings.", Level::Muted);
        m_found_count = rep.statuses.available + rep.statuses.incorrect;
        if (rep.cancelled) add_log_message("Scan cancelled : what was read is kept", Level::Warn);
        else add_log_message("Scan completed! " + std::to_string(m_found_count) + " games found from "
                             + std::to_string(rep.archives) + " archives ("
                             + std::to_string(rep.reread) + " read)", Level::Ok);
    } catch (const std::exception& e) {
        add_log_message("Error: " + std::string(e.what()), Level::Error);
    }
    m_scan_finished = true;
    m_finished_dispatcher();
}

void ROMScanDialog::on_progress_update() {
    // Snapshot shared state under lock, then update UI without holding the lock
    std::string message;
    std::vector<std::pair<std::string, Level>> pending_logs;
    double progress;
    {
        std::lock_guard<std::mutex> lk(m_shared_mutex);
        message      = m_current_message;
        progress     = m_current_progress.load();
        pending_logs.swap(m_log_messages);
    }

    m_step_label->set_text(message);
    m_progress_bar.set_fraction(progress / 100.0);
    int pct = static_cast<int>(progress);
    m_percentage_label.set_text(std::to_string(pct) + "%");

    for (const auto& entry : pending_logs) m_log->append(entry.first, entry.second);
}

void ROMScanDialog::on_scan_finished() {
    if (m_cancelled) {
        m_step_label->set_text(_("Scan cancelled"));
    } else {
        m_step_label->set_text(_("Scan completed - ") + std::to_string(m_found_count) + " games found");
        m_progress_bar.set_fraction(1.0);
        m_percentage_label.set_text("100%");
    }

    m_cancel_button->set_sensitive(false);
    m_bg_button->set_sensitive(false);
    m_close_button->set_sensitive(true);

    // Process any remaining log messages
    on_progress_update();

    // Notify MainWindow (or any observer) that the scan is done
    m_signal_scan_complete.emit();
}

void ROMScanDialog::add_log_message(const std::string& message, Level level) {
    std::cout << "[ROM SCAN] " << message << std::endl;
    {
        std::lock_guard<std::mutex> lk(m_shared_mutex);
        m_log_messages.emplace_back(message, level);
    }
    m_progress_dispatcher(); // safe to call from any thread
}

void ROMScanDialog::on_cancel_clicked() {
    m_cancelled = true;
    m_cancel_button->set_sensitive(false);
    add_log_message("Cancelling...", Level::Warn);
}

// La croix de l'en-tete : pendant le scan elle range la fenetre sans rien
// interrompre (le scan continue, la barre d'etat le suit), comme « Run in
// Background » ; une fois fini, elle ferme pour de bon.
void ROMScanDialog::on_close_requested() {
    if (m_scan_finished) { response(Gtk::RESPONSE_CLOSE); return; }
    hide();
    m_signal_run_in_background.emit();
}