// src/DATUpdateDialog.cpp
#include "DATUpdateDialog.h"
#include "i18n.h"
#include "DatParser.h"
#include "RomScanner.h"
#include "DatSource.h"
#include "RomResolve.h"
#include <algorithm>
#include <set>
#include <thread>
#include <iostream>
#include <filesystem>
#include <ctime>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Le temps laisse pour lire le « Terminé ! » avant que la boite ne se ferme
// seule. Assez pour que l'oeil s'y pose, trop court pour qu'on attende.
static constexpr unsigned int kAutoCloseSeconds = 4;

DATUpdateDialog::DATUpdateDialog(Gtk::Window& parent, std::shared_ptr<DatabaseManager> db, const std::string& dat_path,
                                 std::vector<std::string> files, std::string emulator)
    : Gtk::Dialog()
    , m_db(db)
    , m_dat_path(dat_path), m_files(std::move(files)), m_emulator(std::move(emulator))
{
    namespace ui = SettingsUi;

    set_transient_for(parent);
    set_modal(true);
    set_default_size(640, 560);
    set_position(Gtk::WIN_POS_CENTER_ON_PARENT);

    // L'en-tete de la charte : tuile a pictogramme, titre, et sous-titre qui
    // porte l'etape en cours. La croix annule tant que le traitement tourne.
    ui::Header head = ui::window_header(*this, "database.svg", _("DAT Database Update"),
                                        _("Preparing..."),
                                        [this] { on_close_requested(); });
    m_step_label = head.subtitle;

    m_body.set_margin_start(20);
    m_body.set_margin_end(20);
    m_body.set_margin_top(18);
    m_body.set_margin_bottom(18);

    // Progression : la barre, et le compte a droite. L'etape est dans
    // l'en-tete, elle n'a pas a etre repetee ici.
    m_progress_bar.set_fraction(0.0);
    m_progress_bar.set_show_text(false);
    m_percentage_label.set_text("0%");
    m_percentage_label.set_halign(Gtk::ALIGN_END);
    m_percentage_label.get_style_context()->add_class("set-sub");
    m_progress_box.pack_start(m_progress_bar, Gtk::PACK_SHRINK);
    m_progress_box.pack_start(m_percentage_label, Gtk::PACK_SHRINK);
    m_body.pack_start(m_progress_box, Gtk::PACK_SHRINK);

    // Le journal de l'onglet Import, tel quel : monospace dans un cadre en
    // creux, les niveaux peints avec les couleurs d'etat de l'application.
    m_log = Gtk::make_managed<ui::LogPanel>(_("Details"),
                                            _("What the update is doing, step by step."),
                                            ui::LogPanel::None);
    m_body.pack_start(*m_log, Gtk::PACK_EXPAND_WIDGET);
    m_main_box.pack_start(m_body, Gtk::PACK_EXPAND_WIDGET);

    auto* foot = ui::footer();
    m_close_button  = ui::button(_("Close"), "", ui::Tone::Accent);
    m_cancel_button = ui::button(_("Cancel"));
    m_close_button->set_size_request(96, -1);
    m_cancel_button->set_size_request(96, -1);
    m_close_button->set_sensitive(false);
    foot->pack_end(*m_close_button, Gtk::PACK_SHRINK);
    foot->pack_end(*m_cancel_button, Gtk::PACK_SHRINK);
    m_main_box.pack_start(*foot, Gtk::PACK_SHRINK);

    m_cancel_button->signal_clicked().connect(sigc::mem_fun(*this, &DATUpdateDialog::on_cancel_clicked));
    m_close_button->signal_clicked().connect(sigc::mem_fun(*this, &DATUpdateDialog::hide));

    get_content_area()->set_spacing(0);
    get_content_area()->pack_start(m_main_box, Gtk::PACK_EXPAND_WIDGET);

    // Threading
    m_progress_dispatcher.connect(sigc::mem_fun(*this, &DATUpdateDialog::on_progress_update));
    m_finished_dispatcher.connect(sigc::mem_fun(*this, &DATUpdateDialog::on_update_finished));

    show_all_children();
}

std::vector<std::string> DATUpdateDialog::files_for_update(const std::string& emulator,
                                                          std::vector<std::string>* conflicts) {
    auto groups = DatSource::load_groups();
    if (!emulator.empty())
        groups.erase(std::remove_if(groups.begin(), groups.end(),
                                    [&](const DatSource::Group& g) { return g.emulator != emulator; }),
                     groups.end());
    return DatSource::files_to_load(groups, conflicts);
}

DATUpdateDialog::~DATUpdateDialog() {
    // Avant tout le reste : un minuteur encore arme se declencherait sur un
    // objet en cours de destruction.
    m_autoclose.disconnect();
    if (m_worker_thread.joinable()) {
        m_cancelled.store(true);
        m_worker_thread.join();
    }
}

void DATUpdateDialog::start_update() {
    m_cancelled.store(false);
    m_update_finished.store(false);

    add_log_message("Starting DAT update...");

    m_worker_thread = std::thread(&DATUpdateDialog::worker_thread, this);
}

void DATUpdateDialog::note_conflicts(const std::vector<std::string>& conflicts) {
    for (const auto& c : conflicts) add_log_message("Skipped: " + c, Level::Warn);
}

void DATUpdateDialog::on_cancel_clicked() {
    m_cancelled.store(true);
    m_cancel_button->set_sensitive(false);
    add_log_message("Cancellation requested...", Level::Warn);
}

// La croix de l'en-tete : tant que le traitement tourne, fermer c'est
// renoncer ; une fois fini, c'est simplement ranger la boite.
void DATUpdateDialog::on_close_requested() {
    if (m_update_finished.load()) hide();
    else if (!m_cancelled.load()) on_cancel_clicked();
}

void DATUpdateDialog::worker_thread() {
    // Helper to append a log message + notify UI, all under the shared mutex.
    auto log = [this](const std::string& msg, Level level = Level::Info) {
        {
            std::lock_guard<std::mutex> lk(m_shared_mutex);
            m_log_messages.emplace_back(msg, level);
        }
        m_progress_dispatcher();
    };

    try {
        // Phase 1: Reset database
        update_progress(0.1, "", "Clearing database...");
        log("Removing all existing data...");

        if (m_cancelled.load()) { m_update_finished.store(true); m_finished_dispatcher(); return; }

        // DIFF: capture current game statuses + ROM signatures BEFORE wiping the
        // games table, so unchanged games keep their availability status without a
        // full ROM re-scan (see Phase 4).
        log("Snapshotting current game statuses for diff...");
        if (!m_emulator.empty())
            log("Updating the " + m_emulator + " games only : the other emulators' games are left as they are");
        std::unordered_map<std::string, std::string> old_snapshot = m_db->snapshotStatusSignatures(m_emulator);
        log("Captured " + std::to_string(old_snapshot.size()) + " game statuses", Level::Muted);

        // Favourites and play history survive the wipe on their own: the games
        // table carries triggers that copy them out on delete and put them back
        // on insert (see DatabaseManager). Reported here so the operation is
        // visibly accounted for rather than silently trusted.
        log(std::to_string(m_db->protectedPlayerStats())
            + " game(s) with play history : carried across the rebuild");

        if (!m_db->clearGames(m_emulator)) {
            log("Error clearing database", Level::Error);
            m_failed.store(true);
            m_update_finished.store(true);
            m_finished_dispatcher();
            return;
        }
        m_database_changed.store(true);
        // NOTE: rom_cache and directory snapshots are intentionally PRESERVED.
        // The physical ROM files on disk are unaffected by a DAT refresh, so their
        // cached CRC/metadata stays valid. Only games whose ROM definition actually
        // changed have their cache entry invalidated afterwards (Phase 4), so the
        // next scan re-reads just the diff instead of the whole collection.

        log("Database cleared (ROM cache preserved)", Level::Ok);

        // Phase 2: Scan DAT files
        if (m_cancelled.load()) { m_update_finished.store(true); m_finished_dispatcher(); return; }

        update_progress(0.2, "", "Scanning DAT files...");
        log("Searching for DAT files in: " + m_dat_path);

        // What the DAT groups select, resolved by the caller : a file the
        // folder holds but no active group wants is simply not here.
        std::vector<std::string> dat_files;
        for (const auto& f : m_files)
            if (std::filesystem::is_regular_file(f)) dat_files.push_back(f);

        if (dat_files.empty()) {
            log("No DAT files found in: " + m_dat_path, Level::Error);
            m_failed.store(true);
            m_update_finished.store(true);
            m_finished_dispatcher();
            return;
        }

        log("Found " + std::to_string(dat_files.size()) + " DAT files");

        // Phase 3: Loading DAT files
        double progress_per_file = 0.7 / dat_files.size(); // 70% for loading
        double current_progress = 0.2;

        for (size_t i = 0; i < dat_files.size() && !m_cancelled.load(); ++i) {
            const auto& filepath = dat_files[i];
            std::string filename = std::filesystem::path(filepath).filename().string();

            current_progress += progress_per_file;
            update_progress(current_progress, filename, "Loading...");
            log("Loading: " + filename, Level::Muted);

            // parseToDatabase now returns the number of games loaded (or -1 on error)
            std::string note;
            int games_added = DatParser::parseToDatabase(filepath, m_db, &note);

            if (games_added >= 0) {
                if (!note.empty()) log(filename + ": " + note, Level::Muted);
                log(filename + " loaded (" + std::to_string(games_added) + " games)", Level::Ok);
            } else {
                log("Error loading: " + filename, Level::Error);
            }
        }

        if (m_cancelled.load()) { m_update_finished.store(true); m_finished_dispatcher(); return; }

        // Phase 4: Finalization + DIFF apply
        update_progress(0.95, "", "Finalizing...");
        size_t final_game_count = m_db->getGameCount(/*every emulator*/ "");
        log("Total: " + std::to_string(final_game_count) + " games loaded");

        // Restore statuses for games whose ROM definition is unchanged, and invalidate
        // the ROM cache only for games that are new or whose definition changed.
        log("Applying diff (restoring statuses for unchanged games)...");
        std::vector<std::string> changed_zip_names;
        std::vector<std::string> changed_keys;
        int restored = m_db->applyPreservedStatuses(old_snapshot, changed_zip_names, m_emulator, &changed_keys);
        log("Restored " + std::to_string(restored) + " game statuses : no re-scan needed", Level::Ok);
        log(std::to_string(changed_keys.size()) + " new/changed games to re-evaluate");

        // Judge the new/changed games from the ROM content cache, one emulator
        // at a time, over that emulator's own ROM directories. The cache is
        // trusted as it stands : the scans drop from it what left the disk,
        // so no file is checked here. Unchanged games keep the status
        // restored above.
        std::map<std::string, std::unordered_set<std::string>> changed_by_emu;
        for (const auto& key : changed_keys) {
            // "name\x1fsystem\x1femulator" (DatabaseManager::snapshotStatusSignatures)
            const size_t last = key.rfind('\x1f');
            if (last == std::string::npos) continue;
            changed_by_emu[key.substr(last + 1)].insert(key.substr(0, last));
        }
        size_t total = 0, done_before = 0;
        for (const auto& [emu, keys] : changed_by_emu) total += keys.size();
        for (const auto& [emu, keys] : changed_by_emu) {
            if (m_cancelled.load()) break;
            const auto roots = DatSource::roms_paths_for(emu);
            if (roots.empty()) {
                log("No ROM directory set for " + emu + " : its " + std::to_string(keys.size())
                    + " new/changed games stay missing until a scan", Level::Muted);
                continue;
            }
            update_progress(0.96, "", "Re-matching from cache...");
            log("Re-matching " + std::to_string(keys.size()) + " " + emu + " games from the ROM content cache...");
            auto r = RomResolve::resolve_changed_from_cache(
                m_db, roots, emu, keys,
                [&](size_t done, size_t of) {
                    // The share of this emulator in the last 4 % of the bar.
                    const double part = total ? (double)keys.size() / (double)total : 1.0;
                    const double base = total ? (double)done_before / (double)total : 0.0;
                    update_progress(0.96 + 0.04 * (base + part * (of ? (double)done / (double)of : 1.0)), "",
                                    "Re-matching from cache... " + std::to_string(done) + " / " + std::to_string(of));
                    return !m_cancelled.load();
                });
            done_before += keys.size();
            log(emu + " : " + std::to_string(r.evaluated) + " sets judged, " + std::to_string(r.available)
                + " available, " + std::to_string(r.incorrect) + " incorrect, " + std::to_string(r.missing)
                + " missing", Level::Ok);
            if (r.cancelled)
                log("Re-matching stopped : the games not judged yet stay missing until the next scan", Level::Warn);
        }

        // Fallback: for anything the cache could not resolve, invalidate its cache
        // entry so a subsequent ROM scan re-reads just those files.
        m_db->invalidateRomCacheForFiles(changed_zip_names);

        if (m_cancelled.load()) { m_update_finished.store(true); m_finished_dispatcher(); return; }

        // MAME's sets now carry their devices : the reload offered for that
        // at startup is no longer needed.
        if (m_emulator.empty() || m_emulator == "mame") m_db->clearMameDatResyncFlag();

        update_progress(1.0, "", "Complete!");
        log("Update completed. Statuses are up to date : a full re-scan is no longer required.", Level::Ok);

    } catch (const std::exception& e) {
        log(std::string("Error: ") + e.what(), Level::Error);
        m_failed.store(true);
    }

    m_update_finished.store(true);
    m_finished_dispatcher();
}

void DATUpdateDialog::update_progress(double percentage, const std::string& current_file, const std::string& message) {
    m_current_progress.store(percentage);
    {
        std::lock_guard<std::mutex> lk(m_shared_mutex);
        m_current_file = current_file;
        m_current_message = message;
    }
    m_progress_dispatcher();
}

void DATUpdateDialog::add_log_message(const std::string& message, Level level) {
    {
        std::lock_guard<std::mutex> lk(m_shared_mutex);
        m_log_messages.emplace_back(message, level);
    }
    m_progress_dispatcher();
}

void DATUpdateDialog::on_progress_update() {
    // Snapshot shared state under lock, then update widgets without holding it.
    std::string current_file;
    std::string current_message;
    std::vector<std::pair<std::string, Level>> pending_logs;
    double progress = m_current_progress.load();
    {
        std::lock_guard<std::mutex> lk(m_shared_mutex);
        current_file = m_current_file;
        current_message = m_current_message;
        pending_logs.swap(m_log_messages);
    }

    m_progress_bar.set_fraction(progress);
    m_percentage_label.set_text(std::to_string(static_cast<int>(progress * 100)) + "%");

    // L'etape en cours vit dans le sous-titre de l'en-tete, la ou ROM
    // Management met la sienne.
    if (!current_file.empty())        m_step_label->set_text(_("File: ") + current_file);
    else if (!current_message.empty()) m_step_label->set_text(current_message);

    for (const auto& entry : pending_logs) m_log->append(entry.first, entry.second);
}

void DATUpdateDialog::on_update_finished() {
    m_cancel_button->set_sensitive(false);
    m_close_button->set_sensitive(true);

    if (m_cancelled.load()) {
        m_step_label->set_text(_("Operation cancelled"));
        add_log_message("Operation cancelled by user", Level::Warn);
    } else {
        m_step_label->set_text(_("Update completed!"));
    }

    on_progress_update();

    // Une mise a jour qui s'est bien passee n'a rien a faire acquitter : la
    // boite se retire d'elle-meme et la bibliotheque revient sans un clic.
    // Une annulation ou une erreur, elle, reste affichee : son journal est le
    // seul compte rendu de ce qui s'est produit.
    if (!m_cancelled.load() && !m_failed.load()) {
        m_autoclose = Glib::signal_timeout().connect_seconds(
            [this]() { hide(); return false; }, kAutoCloseSeconds);
    }
}