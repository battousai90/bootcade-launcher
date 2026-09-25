
// src/RomScanner.cpp
#include "RomScanner.h"
#include "RomResolve.h"
#include "DatSource.h"
#include <filesystem>
#include <zlib.h>
#include <zip.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <cstdint>
#include <map>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

static bool find_rom_by_crc_in_zip(const std::string& zip_path, uLong expected_crc);

// Normalize filename for comparison (handle : vs - differences)
static std::string normalize_filename(const std::string& filename) {
    std::string normalized = filename;
    // Replace - with : in title areas (before year parentheses)
    size_t first_paren = normalized.find('(');
    if (first_paren != std::string::npos) {
        for (size_t i = 0; i < first_paren; ++i) {
            if (normalized[i] == '-' && i > 0 && normalized[i-1] != ' ' && i < normalized.length()-1 && normalized[i+1] == ' ') {
                normalized[i] = ':';
            }
        }
    }
    return normalized;
}

std::string RomScanner::normalize_name(const std::string& filename) {
    return normalize_filename(filename);
}

static bool find_rom_by_crc_in_zip(const std::string& zip_path, uLong expected_crc) {
    int zip_error = 0;
    zip_t* zip = zip_open(zip_path.c_str(), ZIP_RDONLY, &zip_error);
    if (!zip) return false;

    zip_int64_t num_entries = zip_get_num_entries(zip, 0);
    for (zip_uint64_t i = 0; i < num_entries; ++i) {
        zip_file_t* zip_file = zip_fopen_index(zip, i, 0);
        if (!zip_file) continue;

        uLong crc = crc32(0L, Z_NULL, 0);
        char buffer[8192];
        int len;
        while ((len = zip_fread(zip_file, buffer, sizeof(buffer))) > 0) {
            crc = crc32(crc, (const Bytef*)buffer, len);
        }
        zip_fclose(zip_file);

        if (crc == expected_crc) {
            zip_close(zip);
            return true;
        }
    }
    zip_close(zip);
    return false;
}

static uLong compute_crc32(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) return 0;
    uLong crc = crc32(0L, Z_NULL, 0);
    char buffer[8192];
    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
        crc = crc32(crc, (const Bytef*)buffer, file.gcount());
    }
    return crc;
}

uLong compute_crc32_in_zip(const std::string& zip_path, const std::string& rom_name) {
    int zip_error = 0;
    zip_t* zip = zip_open(zip_path.c_str(), ZIP_RDONLY, &zip_error);
    if (!zip) return 0;
    
    // Try exact name first
    zip_file_t* zip_file = zip_fopen(zip, rom_name.c_str(), 0);
    
    // If not found, try searching with normalization
    if (!zip_file) {
        zip_int64_t num_entries = zip_get_num_entries(zip, 0);
        std::string normalized_target = normalize_filename(rom_name);
        
        for (zip_uint64_t i = 0; i < num_entries; ++i) {
            zip_stat_t sb;
            if (zip_stat_index(zip, i, 0, &sb) == 0) {
                std::string entry_name = sb.name;
                std::string normalized_entry = normalize_filename(entry_name);
                
                if (normalized_entry == normalized_target) {
                    zip_file = zip_fopen(zip, entry_name.c_str(), 0);
                    break;
                }
            }
        }
    }
    
    if (!zip_file) {
        zip_close(zip);
        return 0;
    }
    
    uLong crc = crc32(0L, Z_NULL, 0);
    char buffer[8192];
    int len;
    while ((len = zip_fread(zip_file, buffer, sizeof(buffer))) > 0) {
        crc = crc32(crc, (const Bytef*)buffer, len);
    }
    zip_fclose(zip_file);
    zip_close(zip);
    return crc;
}

bool RomScanner::read_zip_entries(const std::string& zip_path, std::vector<ZipEntry>& out) {
    int zip_error = 0;
    zip_t* zip = zip_open(zip_path.c_str(), ZIP_RDONLY, &zip_error);
    if (!zip) return false;

    constexpr size_t BUF = 65536;
    std::vector<char> buf(BUF);
    zip_int64_t num_entries = zip_get_num_entries(zip, 0);
    for (zip_uint64_t i = 0; i < (zip_uint64_t)num_entries; ++i) {
        zip_stat_t sb;
        if (zip_stat_index(zip, i, 0, &sb) != 0) continue;
        std::string entry_name = sb.name;
        if (entry_name.empty() || entry_name.back() == '/') continue;

        zip_file_t* zf = zip_fopen_index(zip, i, 0);
        if (!zf) continue;
        uLong crc = crc32(0L, Z_NULL, 0);
        uint64_t bytes = 0;
        zip_int64_t len;
        while ((len = zip_fread(zf, buf.data(), BUF)) > 0) {
            crc = crc32(crc, (const Bytef*)buf.data(), (uInt)len);
            bytes += (uint64_t)len;
        }
        zip_fclose(zf);

        out.push_back({entry_name, (unsigned long)crc, bytes});
    }
    zip_close(zip);
    return true;
}

uLong hex_to_crc(const std::string& hex) {
    uLong crc;
    std::stringstream ss;
    ss << std::hex << hex;
    ss >> crc;
    return crc;
}

// One zip, one game: the rule lives in RomResolve so the live scan, the cache
// re-match and the audit can never disagree. Evaluated as non-merged on
// purpose whatever the configured style : a single zip cannot tell whether an
// inherited ROM sits in the parent's; the split pass runs afterwards, from the
// cache, once every archive's contents are known (see RomResolve::
// resolve_inherited_from_cache).
static std::string check_game_maps(const Game& game, const RomResolve::Archive& archive) {
    return RomResolve::status_of(game, &archive, RomResolve::SetStyle::NonMerged, {}, {});
}

void RomScanner::check_availability(Game& game, const std::string& roms_path) {
    // If no ROMs defined for this game, consider it missing (can't verify)
    if (game.roms.empty()) {
        game.status = "missing";
        return;
    }

    bool all_present = true;
    bool all_correct = true;
    

    for (const auto& rom : game.roms) {
        std::string rom_path = roms_path + "/" + rom.name;
        std::string zip_path = roms_path + "/" + game.name + ".zip";
        bool rom_found = false;

        // Check for individual ROM file
        if (std::filesystem::exists(rom_path)) {
            rom_found = true;
            size_t file_size = std::filesystem::file_size(rom_path);
            if (file_size != rom.size) {
                all_correct = false;
                continue;
            }

            uLong actual_crc = compute_crc32(rom_path);
            uLong expected_crc = hex_to_crc(rom.crc);
            if (actual_crc != expected_crc) {
                all_correct = false;
            }
        }
        // Check for ROM in ZIP file - first try by filename, then by CRC
        else if (std::filesystem::exists(zip_path)) {
            uLong actual_crc = compute_crc32_in_zip(zip_path, rom.name);
            uLong expected_crc = hex_to_crc(rom.crc);
            
            if (actual_crc != 0 && actual_crc == expected_crc) {
                // Found by filename and CRC matches
                rom_found = true;
            } else if (actual_crc == 0) {
                // Not found by filename, try searching by CRC
                if (find_rom_by_crc_in_zip(zip_path, expected_crc)) {
                    rom_found = true;
                } else {
                    rom_found = false;
                }
            } else {
                // Found by filename but wrong CRC
                rom_found = true;
                all_correct = false;
            }
        }

        // If this ROM is not found anywhere, game is missing
        if (!rom_found) {
            all_present = false;
            break;
        }
    }

    if (!all_present) {
        game.status = "missing";
    } else if (!all_correct) {
        game.status = "incorrect";
    } else {
        game.status = "available";
    }
}

void RomScanner::check_availability(Game& game, const std::vector<std::string>& roms_paths) {
    // Try each ROMs directory until we find a match or exhaust all paths
    for (const auto& roms_path : roms_paths) {
        check_availability(game, roms_path);
        
        
        // If we found the game (available or incorrect), stop searching
        if (game.status == "available" || game.status == "incorrect") {
            return;
        }
    }
    
    // If we get here, the game wasn't found in any directory
    game.status = "missing";
}

// DELETED - USELESS METHOD

// DELETED - SECOND USELESS METHOD

void RomScanner::check_availability_db(const std::string& game_name, const std::string& system, std::shared_ptr<DatabaseManager> db, const std::string& roms_path) {
    // Get the specific game by name AND system
    Game game = db->getGame(game_name, system);
    
    if (game.name.empty()) {
        return; // Game not found in database
    }

    // If no ROMs defined for this game, consider it missing
    if (game.roms.empty()) {
        db->updateGameStatus(game_name, "missing", system);
        return;
    }

    bool all_present = true;
    bool all_correct = true;
    
    std::string zip_path = roms_path + "/" + game_name + ".zip";

    for (const auto& rom : game.roms) {
        std::string rom_path = roms_path + "/" + rom.name;
        bool rom_found = false;

        // Check for individual ROM file first
        if (std::filesystem::exists(rom_path)) {
            rom_found = true;
            size_t file_size = std::filesystem::file_size(rom_path);
            if (file_size != rom.size) {
                all_correct = false;
                continue;
            }

            uLong actual_crc = compute_crc32(rom_path);
            uLong expected_crc = hex_to_crc(rom.crc);
            if (actual_crc != expected_crc) {
                all_correct = false;
            }
        }
        // Check for ROM in ZIP file - EXACT FILENAME + CRC ONLY
        else if (std::filesystem::exists(zip_path)) {
            uLong actual_crc = compute_crc32_in_zip(zip_path, rom.name);
            uLong expected_crc = hex_to_crc(rom.crc);

            if (actual_crc != 0 && actual_crc == expected_crc) {
                // Found by EXACT filename AND CRC matches - PERFECT
                rom_found = true;
            } else if (actual_crc == 0) {
                // ROM file not found by exact name in ZIP
                rom_found = false;
            } else {
                // Found by filename but wrong CRC
                rom_found = true;
                all_correct = false;
            }
        } else {
            // ZIP not found
            rom_found = false;
        }

        // If this ROM is not found, game is missing
        if (!rom_found) {
            all_present = false;
            break;
        }
    }

    std::string status;
    if (!all_present) {
        status = "missing";
    } else if (!all_correct) {
        status = "incorrect";
    } else {
        status = "available";
    }

    // Update status with system-specific lookup
    db->updateGameStatus(game_name, status, system);
}

// FAST SCAN METHOD : single-pass ZIP read.
// The ZIP is opened ONCE. All entries are read and their CRCs computed into an
// in-memory map, so every subsequent candidate check is a cheap map lookup
// instead of a repeated open/read/close cycle.
void RomScanner::scan_zip_file(const std::string& zip_path, std::shared_ptr<DatabaseManager> db) {
    std::string game_name = std::filesystem::path(zip_path).stem().string();

    // ── Phase 1: open ZIP once, build {filename → crc} map ──────────────────
    int zip_error = 0;
    zip_t* zip = zip_open(zip_path.c_str(), ZIP_RDONLY, &zip_error);
    if (!zip) return;

    RomResolve::Archive archive;
    archive.path = zip_path;

    constexpr size_t BUF = 65536;
    std::vector<char> buf(BUF);

    zip_int64_t num_entries = zip_get_num_entries(zip, 0);
    for (zip_uint64_t i = 0; i < (zip_uint64_t)num_entries; ++i) {
        zip_stat_t sb;
        if (zip_stat_index(zip, i, 0, &sb) != 0) continue;
        std::string entry_name = sb.name;
        if (entry_name.empty() || entry_name.back() == '/') continue; // dir

        zip_file_t* zf = zip_fopen_index(zip, i, 0);
        if (!zf) continue;

        uLong crc = crc32(0L, Z_NULL, 0);
        zip_int64_t len;
        while ((len = zip_fread(zf, buf.data(), BUF)) > 0)
            crc = crc32(crc, (const Bytef*)buf.data(), (uInt)len);
        zip_fclose(zf);

        archive.add(entry_name, (unsigned long)crc);
    }
    zip_close(zip);

    auto check_game = [&](const Game& game) { return check_game_maps(game, archive); };

    // ── Phase 2: name-based candidates ──────────────────────────────────────
    std::vector<Game> candidates = db->getAllGamesWithName(game_name);
    for (const auto& candidate : candidates) {
        Game game = db->getGame(candidate.name, candidate.system);
        std::string status = check_game(game);
        if (status == "available" || status == "incorrect")
            db->updateGameStatus(game.name, status, game.system);
    }

    // ── Phase 3: CRC-only discovery for ZIPs with no name match ─────────────
    if (candidates.empty()) {
        for (const auto& [crc, _] : archive.name_by_crc) {
            std::vector<Game> crc_cands = db->getGamesByRomCrc(crc);
            for (const auto& cand : crc_cands) {
                Game game = db->getGame(cand.name, cand.system);
                std::string status = check_game(game);
                if (status == "available" || status == "incorrect")
                    db->updateGameStatus(game.name, status, game.system);
            }
        }
    }
}

// Thread-safe variant: same logic but returns results instead of writing to DB.
// Safe to call from multiple threads concurrently (only DB reads, no writes).
std::vector<RomScanner::ScanResult>
RomScanner::scan_zip_file_collect(const std::string& zip_path,
                                   std::shared_ptr<DatabaseManager> db,
                                   std::vector<std::pair<std::string, unsigned long>>* out_entries)
{
    std::vector<ScanResult> results;
    std::string game_name = std::filesystem::path(zip_path).stem().string();

    std::vector<ZipEntry> entries;
    if (!read_zip_entries(zip_path, entries)) return results;

    RomResolve::Archive archive;
    archive.path = zip_path;
    for (const auto& e : entries) {
        archive.add(e.name, e.crc);
        // Capture the real (un-normalized) contents for the content-addressed cache.
        if (out_entries)
            out_entries->emplace_back(e.name, e.crc);
    }

    // Parent directory of the ZIP : stored as source_directory for each found game
    std::string source_dir = std::filesystem::path(zip_path).parent_path().string();

    // Name-based candidates (read-only DB queries : thread-safe)
    std::vector<Game> candidates = db->getAllGamesWithName(game_name);
    for (const auto& candidate : candidates) {
        Game game = db->getGame(candidate.name, candidate.system);
        std::string status = check_game_maps(game, archive);
        if (status == "available" || status == "incorrect")
            results.push_back({game.name, game.system, status, source_dir});
    }

    // CRC-only discovery
    if (candidates.empty()) {
        for (const auto& [crc, _] : archive.name_by_crc) {
            std::vector<Game> crc_cands = db->getGamesByRomCrc(crc);
            for (const auto& cand : crc_cands) {
                Game game = db->getGame(cand.name, cand.system);
                std::string status = check_game_maps(game, archive);
                if (status == "available" || status == "incorrect")
                    results.push_back({game.name, game.system, status, source_dir});
            }
        }
    }

    return results;
}

// Re-derive availability from the content-addressed cache (zip_contents) : no disk I/O.
// Mirrors the live scan (name candidates + CRC-only discovery) but sources ZIP
// contents from the DB instead of reading files. Only upgrades statuses
// (missing → available/incorrect), never downgrades, exactly like a real scan.
// ── Keeping zip_contents true to the libraries ──────────────────────────────

std::vector<std::string> RomScanner::all_library_roots() {
    std::set<std::string> emulators = {"fbneo", "mame"};
    for (const auto& g : DatSource::load_groups()) emulators.insert(g.emulator);
    std::vector<std::string> roots;
    std::error_code ec;
    for (const auto& emu : emulators)
        for (const auto& r : DatSource::roms_paths_for(emu))
            if (!r.empty()) roots.push_back(std::filesystem::weakly_canonical(r, ec).string());
    return roots;
}

int RomScanner::prune_zip_cache(std::shared_ptr<DatabaseManager> db) {
    // Settings only, never the disk : a drive that is not plugged in keeps
    // its archives, a folder removed from every library loses them.
    const auto roots = all_library_roots();
    if (roots.empty()) return 0;   // no library configured at all : nothing is decided
    std::vector<std::string> out_of_libraries;
    for (const auto& p : db->getZipContentPaths())
        if (!RomResolve::under_any_root(p, roots)) out_of_libraries.push_back(p);
    return db->forgetZipContents(out_of_libraries);
}

// ── Scan of one emulator's library into the cache ───────────────────────────

RomScanner::CacheScanReport
RomScanner::scan_into_cache(std::shared_ptr<DatabaseManager> db,
                            const std::vector<std::string>& roots,
                            const std::string& emulator,
                            bool recursive,
                            const std::function<bool(double, const std::string&)>& progress,
                            const std::function<void(const std::string&, bool)>& log) {
    namespace fs = std::filesystem;
    CacheScanReport rep;
    auto say  = [&](const std::string& m) { if (log) log(m, false); };
    auto warn = [&](const std::string& m) { if (log) log(m, true); };
    auto step = [&](double pct, const std::string& m) {
        if (progress && !progress(pct, m)) rep.cancelled = true;
        return !rep.cancelled;
    };

    // ── 1. What the roots hold ──────────────────────────────────────────────
    struct File { std::string path; long long size = 0, mtime = 0; };
    std::vector<File> files;
    // The roots listed to the end and holding at least one archive : only
    // under those can an archive the cache knows be called gone. A root that
    // lists nothing may be the mount point of a drive that is not plugged in.
    std::vector<std::string> listed_roots;
    for (const auto& root : roots) {
        std::error_code ec;
        if (root.empty() || !fs::is_directory(root, ec)) {
            warn("Configured ROM path does not exist: " + root);
            ++rep.missing_roots;
            continue;
        }
        // Paths are built under the canonical root : the key zip_contents
        // stores, so the freshness lookup below needs no realpath per file.
        const fs::path base = fs::weakly_canonical(fs::path(root), ec);
        auto visit = [&](const fs::directory_entry& e) {
            std::error_code fec;
            if (!e.is_regular_file(fec)) return;
            std::string ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (ext != ".zip") return;
            File f;
            f.path  = e.path().string();
            f.size  = (long long)e.file_size(fec);
            auto ft = e.last_write_time(fec);
            f.mtime = (long long)std::chrono::system_clock::to_time_t(
                std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now()));
            files.push_back(std::move(f));
            if ((files.size() % 2048) == 0) step(2.0, "Listing archives… " + std::to_string(files.size()));
        };
        const size_t before = files.size();
        if (recursive) {
            for (auto it = fs::recursive_directory_iterator(base, fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator() && !rep.cancelled; it.increment(ec))
                visit(*it);
        } else {
            for (auto it = fs::directory_iterator(base, ec); it != fs::directory_iterator() && !rep.cancelled; it.increment(ec))
                visit(*it);
        }
        if (rep.cancelled) return rep;
        if (!ec && files.size() > before) listed_roots.push_back(base.string());
    }
    rep.archives = files.size();
    say("Found " + std::to_string(files.size()) + " archive(s) under " + std::to_string(roots.size()) + " ROM path(s)");

    // ── 2. Only new or changed files are read ───────────────────────────────
    const auto stamps = db->getZipContentStamps();
    std::vector<const File*> to_read;
    for (const auto& f : files) {
        auto it = stamps.find(f.path);
        // Same size, mtime within the tolerance rom_cache applies.
        if (it != stamps.end() && it->second.first == f.size && std::llabs(it->second.second - f.mtime) <= 2) continue;
        to_read.push_back(&f);
    }
    say(std::to_string(to_read.size()) + " archive(s) new or changed : reading them; "
        + std::to_string(files.size() - to_read.size()) + " unchanged since the cache read them");

    struct Read { const File* file = nullptr; bool ok = false; std::vector<ZipEntry> entries; };
    std::vector<Read> reads(to_read.size());
    {
        std::atomic<size_t> next{0}, done{0};
        std::atomic<bool> stop{false};
        const size_t n_threads = std::max<size_t>(1, std::min<size_t>(std::thread::hardware_concurrency(), to_read.size()));
        std::vector<std::thread> pool;
        for (size_t t = 0; t < n_threads; ++t) {
            pool.emplace_back([&] {
                for (size_t i = next++; i < to_read.size() && !stop; i = next++) {
                    reads[i].file = to_read[i];
                    reads[i].ok = read_zip_entries(to_read[i]->path, reads[i].entries);
                    ++done;
                }
            });
        }
        // Progress from this thread only : the callbacks belong to the caller.
        while (done < to_read.size()) {
            if (!step(5.0 + 75.0 * (double)done / (double)to_read.size(),
                      "Reading " + std::to_string(done.load()) + " / " + std::to_string(to_read.size())))
                stop = true;
            if (stop) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        for (auto& th : pool) th.join();
    }

    // What was read is kept even on cancel : it is true of the files.
    db->beginTransaction();
    for (const auto& r : reads) {
        if (!r.file) continue;   // never reached (cancelled)
        if (!r.ok) { ++rep.unreadable; warn("Cannot read " + r.file->path); continue; }
        std::vector<std::pair<std::string, unsigned long>> entries;
        entries.reserve(r.entries.size());
        for (const auto& e : r.entries) entries.emplace_back(e.name, e.crc);
        db->storeZipContents(r.file->path, entries);
        db->stampZipContents(r.file->path, r.file->size, r.file->mtime);
        ++rep.reread;
    }
    db->commitTransaction();
    if (rep.cancelled) return rep;

    // ── 3. What left the disk or the libraries leaves the cache ────────────
    // So that whoever reads the cache afterwards (a DAT update) can trust it
    // without checking every file on the disk.
    {
        std::unordered_set<std::string> listed;
        listed.reserve(files.size());
        for (const auto& f : files) listed.insert(f.path);
        std::vector<std::string> gone;
        for (const auto& p : db->getZipContentPaths())
            if (!listed.count(p) && RomResolve::under_any_root(p, listed_roots)) gone.push_back(p);
        rep.forgotten = db->forgetZipContents(gone) + prune_zip_cache(db);
        if (rep.forgotten) say(std::to_string(rep.forgotten) + " archive(s) no longer in the library : forgotten");
    }

    // ── 4. Every status of this emulator, from the cache ────────────────────
    step(82.0, "Resolving sets…");
    const RomResolve::SetStyle style = RomResolve::load_style(emulator);
    say("Resolving " + emulator + " sets from the cache (" + RomResolve::to_string(style) + " collection)...");
    rep.statuses = RomResolve::resolve_all_from_cache(db, roots, style, emulator,
        [&](size_t d, size_t total) {
            return step(82.0 + 17.0 * (double)d / (double)std::max<size_t>(1, total),
                        "Resolving sets… " + std::to_string(d) + " / " + std::to_string(total));
        });
    rep.cancelled = rep.cancelled || rep.statuses.cancelled;
    say(std::to_string(rep.statuses.available) + " available, " + std::to_string(rep.statuses.incorrect)
        + " incorrect, " + std::to_string(rep.statuses.missing) + " missing; "
        + std::to_string(rep.statuses.changed) + " status(es) changed");
    return rep;
}
