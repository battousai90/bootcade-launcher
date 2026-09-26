// src/RomResolve.h
//
// The one place that decides whether a set is available, incorrect or missing.
//
// Three code paths used to carry their own copy of that rule : the live scan
// (one zip at a time, in parallel), the cache re-match after a DAT update, and
// the library audit. They agreed only as long as the rule stayed "every ROM the
// DAT lists must be in the set's own zip", which is the *non-merged* layout and
// nothing else. FBNeo DATs mark 85 % of Neo Geo ROMs and 56 % of arcade ROMs
// with merge= : in a *split* collection those live in the parent's zip (or the
// BIOS's), and a set that lacks them is complete, not broken.
//
// So the rule now takes the collection's style and, for inherited ROMs, a way
// to look at other archives. Callers plug in where those archives come from
// (a freshly read zip, the zip_contents cache, the audit's index); the rule
// itself never touches the disk or the database.
#pragma once

#include "DatabaseManager.h"
#include "Game.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace RomResolve {

// How the collection is laid out. Merged (a clone has no zip of its own; its
// ROMs sit in the parent's) is a later phase and deliberately not listed yet :
// an enum value with no behaviour behind it would be a promise the code cannot
// keep.
enum class SetStyle { NonMerged, Split };

SetStyle    style_from_string(const std::string& s);   // unknown → NonMerged
std::string to_string(SetStyle s);
// The style of the collection : that of the DAT group the Library audits
// against (rom_manager.library_group, else the first group), which carries
// set_style. Missing or unknown → NonMerged, which is exactly the behaviour
// every scan had before the setting existed.
SetStyle    load_style();
// The style of the collection of one emulator : that of the group describing
// it (DatSource::group_for : the library group when it is that emulator's).
// A FinalBurn Neo scan must not start judging its sets as split because the
// Library happens to be showing a split MAME group.
SetStyle    load_style(const std::string& emulator);

// The folder a set's archive is expected in : its raw DAT header ("MAME ROMs
// (split)"), else the same header rebuilt from the emulator and the system.
// One DAT, one folder, as for FinalBurn Neo.
std::string expected_folder(const Game& g);

// Two verdicts on parts of one set (its zip and its CHDs) as one, by the rule
// a single archive follows : anything absent → "missing", else anything wrong
// → "incorrect", else "available". An empty status (no part to judge) yields
// the other one.
std::string combine_status(const std::string& a, const std::string& b);

// What one archive holds, keyed the way the scanner keys it: every entry name
// under its raw spelling *and* under RomScanner::normalize_name(), so ':' vs
// '-' never makes a set look wrong to one consumer and fine to another.
struct Archive {
    std::string path;                                            // empty for an archive not on disk
    std::unordered_map<std::string, unsigned long> crc_by_name;
    std::unordered_map<unsigned long, std::string> name_by_crc;  // first entry wins
    std::vector<std::string> entries;                            // raw names, one per real entry
    void add(const std::string& entry_name, unsigned long crc);
};

// Where inherited ROMs are looked for. Returns null when that set has no
// archive in the collection. The chain is followed through Game::romof, so the
// second callback has to resolve a set name within a system.
using ArchiveLookup = std::function<const Archive*(const Game& game)>;
using GameLookup    = std::function<Game(const std::string& name, const std::string& system)>;

enum class RomState {
    Present,    // right data, under the right name
    WrongName,  // right data, stored under another name
    Corrupt,    // the expected name is there, but the data is wrong
    Absent,     // nowhere it is allowed to be
};

struct RomVerdict {
    std::string   name;
    unsigned long crc  = 0;
    uint64_t      size = 0;
    RomState      state = RomState::Absent;
    std::string   found_as;        // entry name, when it differs from `name`
    unsigned long found_crc = 0;   // CRC of the entry that answered (differs from `crc` when Corrupt)
    std::string   found_in;        // archive path, when another set's archive provided it
    // Set only when an inherited ROM was satisfied by an ancestor's archive
    // rather than the set's own : which set (parent or BIOS short name)
    // provided it. This is what keeps a split audit explainable: "present,
    // from mslug2" rather than a bare "present" that the set's own zip cannot
    // account for.
    std::string   inherited_from;
    bool          inherited = false;  // the DAT marks this ROM merge=
};

struct Verdict {
    std::string status;               // "available" | "incorrect" | "missing" | "" (no ROMs)
    std::vector<RomVerdict> roms;     // every non-nodump ROM, in DAT order
};

// `own` is the set's own archive (null when none was found : every ROM then
// counts as absent, save inherited ones an ancestor can still provide). In
// NonMerged the lookups are never called and the outcome is byte-for-byte the
// rule the scanner always applied.
Verdict evaluate(const Game& game, const Archive* own, SetStyle style,
                 const ArchiveLookup& archive_for, const GameLookup& game_for);

// Convenience: status only, same rule.
std::string status_of(const Game& game, const Archive* own, SetStyle style,
                      const ArchiveLookup& archive_for, const GameLookup& game_for);

// ── CHDs ────────────────────────────────────────────────────────────────────
//
// A MAME disk image is a CHD file next to the zips, in a folder named after its
// set : <root>/<set>/<disk>.chd (or <root>/<DAT header>/<set>/<disk>.chd when
// the root holds one folder per DAT, as RomVault lays them out). CHDs weigh
// hundreds of megabytes : they are never read, never cached in zip_contents,
// never moved. Their header declares the SHA1 of their content, which is what
// the DAT lists : comparing the two is the whole verdict.

// The content SHA1 a CHD's header declares, lower-case hex ; empty when the
// file is not a CHD (v3, v4 or v5) or cannot be read. Reads the header only.
std::string chd_header_sha1(const std::string& path);

struct DiskVerdict {
    std::string name;          // disk name as the DAT gives it, without ".chd"
    std::string sha1;          // expected
    RomState    state = RomState::Absent;   // Present, Corrupt or Absent
    std::string path;          // the file that answered, when one did
    std::string found_sha1;    // what its header declares (differs when Corrupt)
};

struct DiskResult {
    std::string status;               // "available" | "incorrect" | "missing" | "" (no disk)
    std::string folder;               // the set folder the first found disk sits in
    std::vector<DiskVerdict> disks;
};

// Every disk of `game`, looked for under each of `roots`. A good copy anywhere
// wins over a wrong one ; status : all present → available, any absent →
// missing, else incorrect. CHDs are merged whatever the style of the ROMs :
// a clone's disks, its own included, may sit in its parent's folder, and a
// BIOS's in the BIOS's ; `game_for` lets the search climb the whole
// cloneof / romof chain (without it : the parent and romof only).
DiskResult evaluate_disks(const Game& game, const std::vector<std::string>& roots,
                          const GameLookup& game_for = {});

// ── The zip_contents cache as a source of archives ──────────────────────────
//
// Built once from DatabaseManager::getAllZipContents(); indexes every cached
// archive by the lower-cased stem of its filename. Several archives can share
// a stem (the same short name under two systems, or an outbox someone added to
// roms_paths next to the library) : `for_game` scores each candidate by how
// many of the set's own ROMs it satisfies and takes the best, breaking a tie
// by the folder whose name matches the set's DAT header. Same tie-break as the
// audit, so the two never claim different archives for the same set.
class CacheIndex {
public:
    // Whether each archive is checked on disk before being indexed. Scans
    // forget the archives that disappeared (RomScanner::prune_zip_cache), so
    // the cache can be trusted as it stands ; the ROM manager still checks,
    // because its own moves (Fix, Quarantine) land before the scan that
    // follows them.
    enum class OnDisk { Verify, Trust };

    // `roots` restricts the index to archives under the configured ROM
    // directories; empty accepts everything the cache knows. With Verify,
    // archives that no longer exist on disk are skipped too.
    CacheIndex(std::shared_ptr<DatabaseManager> db, const std::vector<std::string>& roots,
               OnDisk on_disk = OnDisk::Verify);

    const Archive* for_game(const Game& game) const;
    const Archive* by_path(const std::string& path) const;
    const std::unordered_map<std::string, Archive>& all() const { return m_archives; }
    size_t         size() const { return m_archives.size(); }
    bool           empty() const { return m_archives.empty(); }

private:
    std::unordered_map<std::string, Archive>                  m_archives;   // path → contents
    std::unordered_map<std::string, std::vector<std::string>> m_by_stem;    // lower stem → paths
    std::unordered_set<std::string>                           m_headers;    // every DAT header the database knows
};

// Re-derive the status of every set that inherits at least one ROM (style
// Split), from the cache alone : no disk I/O. Sets whose own archive is not
// in the cache are left untouched: the cache cannot say anything about them,
// and a stale cache must not turn a real set into a missing one.
//
// `touched` narrows the pass after an incremental scan: the lower-cased short
// names of the sets whose archives were just (re)read. A set is re-derived
// when its own name is in there, or any ancestor's is : a rescanned parent
// changes what its clones can inherit. Empty means every set.
//
// Returns the number of statuses that actually changed.
int resolve_inherited_from_cache(std::shared_ptr<DatabaseManager> db,
                                 const std::vector<std::string>& roots,
                                 SetStyle style,
                                 const std::unordered_set<std::string>& touched = {},
                                 const std::string& emulator = "fbneo");

// Re-derive the status of EVERY set of one emulator from the cache alone, by
// the rule the audit applies : the set's own archive (CacheIndex), then its
// romof chain for inherited ROMs. Unlike the pass above, a set the cache has
// no archive for becomes "missing" : this is the whole verdict for that
// emulator, not a correction on top of a per-file scan. Only rows whose
// status changes are written. Used by the MAME scan (ROMScanDialog), whose
// files are read into zip_contents first, and after a DAT update.
//
// `progress(done, total)` returning false stops the pass; what was decided
// so far is kept.
struct CacheResolveResult {
    // CHDs are evaluated too, by their headers (evaluate_disks) : no cache
    // holds them. A set with both a zip and CHDs (single-folder MAME DAT) gets
    // one verdict for the two (combine_status). They count in the totals.
    int evaluated = 0, available = 0, incorrect = 0, missing = 0;
    int changed = 0;
    bool cancelled = false;
};
CacheResolveResult resolve_all_from_cache(std::shared_ptr<DatabaseManager> db,
                                          const std::vector<std::string>& roots,
                                          SetStyle style,
                                          const std::string& emulator,
                                          const std::function<bool(size_t, size_t)>& progress = {});

// After a DAT update : judge only the sets that are new or whose ROM
// definition changed (`changed`, keys "name\x1fsystem"), from the cache as it
// stands, without touching the disk (CacheIndex::OnDisk::Trust). A set whose
// parent or BIOS changed is judged again too, since what it inherits may
// have, but only when the cache holds its own archive : otherwise its status
// from the last scan stands. Same counters and progress as above.
CacheResolveResult resolve_changed_from_cache(std::shared_ptr<DatabaseManager> db,
                                              const std::vector<std::string>& roots,
                                              SetStyle style,
                                              const std::string& emulator,
                                              const std::unordered_set<std::string>& changed,
                                              const std::function<bool(size_t, size_t)>& progress = {});

// ── Devices ─────────────────────────────────────────────────────────────────
//
// A MAME machine needs its devices (Game::devices : a sound chip, a
// protection CPU...) to start, each kept in its own zip like a set of its
// own. The set is only as available as they are. Found among one emulator's
// sets by name, the machine's system first (a device may sit in another DAT
// of the same emulator, Pleasuredome's "bios-devices"), and closed over the
// devices' own devices. A name no set carries has nothing to verify.
class DeviceIndex {
public:
    explicit DeviceIndex(const std::vector<Game>& games);
    // Indexes into `games` of every device set `g` needs, each once.
    std::vector<size_t> needed(const Game& g) const;
    bool empty() const { return m_any == false; }

private:
    const std::vector<Game>& m_games;
    std::unordered_map<std::string, std::vector<size_t>> m_by_name;
    bool m_any = false;   // some set needs a device at all
};

// Whether `path` lies under one of `roots`, compared as written (no disk
// access) : both sides are expected canonical, as zip_contents stores them.
bool under_any_root(const std::string& path, const std::vector<std::string>& roots);

} // namespace RomResolve
