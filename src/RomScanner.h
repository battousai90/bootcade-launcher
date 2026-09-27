// src/RomScanner.h
#pragma once
#include "Game.h"
#include "DatabaseManager.h"
#include "RomResolve.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <utility>
#include <cstdint>

class RomScanner {
public:
    // One file inside a ZIP, with its real (recomputed) CRC32 : never the value
    // stored in the ZIP's central directory, which a corrupt archive can lie about.
    struct ZipEntry {
        std::string   name;
        unsigned long crc  = 0;
        uint64_t      size = 0;
    };

    // Filesystem-safe rewrite of a DAT ROM name: ':' is illegal in many filenames,
    // so dumps store "Spider-Man- Return..." where the DAT says "Spider-Man: Return".
    // Exposed so every consumer compares names the same way the scanner does : the
    // audit and the scanner disagreeing on a set's status is worse than either
    // being wrong.
    static std::string normalize_name(const std::string& filename);

    // Read every entry of a ZIP, decompressing each one to recompute its CRC32.
    // Directory entries are skipped. Returns false if the archive cannot be opened
    // (which is also what happens for a .7z/.rar handed to libzip).
    static bool read_zip_entries(const std::string& zip_path, std::vector<ZipEntry>& out);

    struct ScanResult {
        std::string name;
        std::string system;
        std::string status;           // "available", "incorrect", "missing" : empty = no change
        std::string source_directory; // parent directory of the scanned ZIP
    };

    static void check_availability(Game& game, const std::string& roms_path);
    static void check_availability(Game& game, const std::vector<std::string>& roms_paths);

    // Scan a ZIP and write results directly to DB (single-threaded path)
    static void scan_zip_file(const std::string& zip_path, std::shared_ptr<DatabaseManager> db);

    // Scan a ZIP and return results without touching DB (thread-pool path).
    // If out_entries is non-null it is filled with the ZIP's {entry_name, crc}
    // contents so the caller can persist them to the content-addressed cache.
    static std::vector<ScanResult> scan_zip_file_collect(const std::string& zip_path,
                                                          std::shared_ptr<DatabaseManager> db,
                                                          std::vector<std::pair<std::string, unsigned long>>* out_entries = nullptr);

    // Every ROM directory of every emulator, canonical : the libraries the
    // zip_contents cache is allowed to describe.
    static std::vector<std::string> all_library_roots();
    // Forget the cached archives that lie under no library any more (a
    // directory removed in Settings). Decided from Settings alone, no disk
    // access ; nothing happens when no library is configured. Returns how many
    // archives were forgotten. Archives gone from a directory still in a
    // library are the business of that library's scan.
    static int prune_zip_cache(std::shared_ptr<DatabaseManager> db);

    // ── Scan of one emulator's library into the cache ──────────────────────
    //
    // The FinalBurn Neo scan (ROMScanDialog) votes a status per zip as it
    // reads it, and remembers what it read in rom_cache, directory_files and
    // saved_roots : bookkeeping that describes ITS roots and ITS statuses.
    // Another emulator cannot share it : a file the MAME scan registered in
    // rom_cache would look "already scanned" to the FinalBurn Neo one, and
    // the roots of one would read as "removed" to the other.
    //
    // So this scan does two separate things. It brings zip_contents up to date
    // for every archive under `roots`, re-reading only the files whose size or
    // mtime differs from what the cache was read from (zip_contents_stamp, or
    // the FinalBurn Neo scan's rom_cache row for a file it already read), and
    // forgets the archives that are gone from a root it listed. Then
    // it derives the status of every set of `emulator` from the cache alone
    // (RomResolve::resolve_all_from_cache). Only that emulator's rows of
    // `games` are written; rom_cache, directory snapshots, saved roots and
    // mame_catalog are never touched.
    struct CacheScanReport {
        size_t archives = 0;      // archives found under the roots
        size_t reread   = 0;      // of which new or changed, read again
        size_t unreadable = 0;    // could not be opened as a zip
        int    missing_roots = 0;
        int    forgotten = 0;     // archives gone from the disk or the libraries, dropped from the cache
        RomResolve::CacheResolveResult statuses;
        bool   cancelled = false;
    };
    // `progress(pct, message)` returning false cancels. `log(line, warn)`.
    static CacheScanReport scan_into_cache(std::shared_ptr<DatabaseManager> db,
                                           const std::vector<std::string>& roots,
                                           const std::string& emulator,
                                           bool recursive,
                                           const std::function<bool(double, const std::string&)>& progress,
                                           const std::function<void(const std::string&, bool)>& log);

    // Database-specific availability check (used by scan_zip_file)
    static void check_availability_db(const std::string& game_name, const std::string& system,
                                       std::shared_ptr<DatabaseManager> db, const std::string& roms_path);
};