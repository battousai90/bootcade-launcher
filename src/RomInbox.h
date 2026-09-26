// src/RomInbox.h
//
// Inbox → outbox ROM rebuilder, in the spirit of RomVault.
//
// The user drops downloaded archives into an *inbox*; analyze() recomputes every
// CRC32 in them, confronts them with the DAT-derived database, and produces a plan.
// apply() then writes the result into an *outbox* laid out like the DATs
// ("<outbox>/FinalBurn Neo - Arcade Games/mslug.zip").
//
// The existing ROM library is strictly READ-ONLY here: it only serves as a pool of
// pieces (via the zip_contents cache) from which incomplete sets can be rebuilt.
// Nothing in this module ever writes inside roms_paths, and nothing updates game
// statuses in the database : a rebuilt set becomes "available" only once the user
// points a normal scan at the outbox, which is the intended independent check.
#pragma once

#include "DatabaseManager.h"
#include "RomResolve.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace RomInbox {

// Where one ROM piece can be read from. `container` is always a ZIP path.
struct PieceSource {
    std::string container;
    std::string entry;
    bool        from_inbox = false;
};

// One ROM of a DAT set, and where it will be taken from.
struct PiecePlan {
    std::string   target_name;   // DAT rom name = entry name in the produced ZIP
    unsigned long crc  = 0;
    uint64_t      size = 0;
    PieceSource   src;
    bool          resolved = false;
    // The set the ROM belongs to when it is not the archive's own : a clone
    // in its parent's merged archive, a device in a non-merged set.
    std::string   owner;
    // A CHD's identity : the SHA1 its header declares (no CRC applies).
    std::string   sha1;
};

// A DAT ROM that could be found neither in the inbox nor in the library. Carries
// the DAT's own size/CRC so the report is enough to go hunting for the file.
struct MissingRom {
    std::string   name;
    unsigned long crc  = 0;
    uint64_t      size = 0;
};

enum class Action {
    Move,             // the inbox archive is already a perfect set: just relocate it
    Rebuild,          // every piece is available, but the ZIP must be recomposed
    Incomplete,       // at least one ROM is nowhere to be found
    AlreadyInLibrary, // the library already holds this set correctly; nothing to do
};

// One archive to produce, as the DAT of its set, read with that DAT's merge
// mode, lays it out (DatLayout) : `game_name` is the archive's set (a merged
// clone's parent), `pieces` every entry it holds.
struct SetPlan {
    std::string game_name;
    std::string system;
    std::string dat_header;      // the folder of its DAT = outbox subfolder name
    std::string description;
    Action      action = Action::Incomplete;
    std::string dest_path;       // <outbox>/<dat_header>/<game_name>.zip
    std::string trigger_archive; // inbox archive that surfaced this set

    std::vector<PiecePlan>   pieces;
    std::vector<MissingRom>  missing;        // unresolved ROMs
    // Entries of trigger_archive left out, WITH their CRC. The name alone
    // could not say why : a file carrying exactly the expected name but the
    // wrong content was announced as "not needed by the DAT", which reads as
    // a surplus when it is a mismatch, and hid the one number (the CRC) that
    // tells a wrong build from a stray file.
    struct ExtraEntry { std::string name; unsigned long crc = 0; uint64_t size = 0; };
    std::vector<ExtraEntry>  extra_entries;
    // A disk image (CHD) rather than an archive : `dest_path` is
    // <outbox>/<DAT folder>/<set>/<disk>.chd, `trigger_archive` the CHD file,
    // and the only action is Move (or AlreadyInLibrary). A CHD is never
    // rebuilt : its header's SHA1 is what the DAT lists, and it is either
    // that disk or not.
    bool disk = false;
    int  pieces_from_library = 0;            // how many pieces come from roms_paths
    int  renamed_entries     = 0;            // pieces whose source entry name differs
    bool selected = true;                    // UI checkbox
};

// What the user decided about how to analyse and what to do afterwards.
// Every field drives a real branch; none is decorative.
struct Options {
    bool recursive        = true;
    bool include_archives = true;   // zip, 7z, rar, …
    bool include_loose    = true;   // a bare ROM file is a one-piece source
    // Pieces may be borrowed from the existing library (read-only). Off, only
    // the inbox itself can supply them, and "already in library" is no longer
    // detected either.
    bool use_library      = true;
    // Rebuild even a set that is already perfect as it sits : normalises the
    // archive (DAT names, deflate, nothing extra) instead of relocating it.
    bool rebuild_correct  = false;
    // What becomes of an inbox file once every piece it held has been used.
    enum class Processed { Subfolder, Delete, Keep };
    Processed processed   = Processed::Subfolder;
    // Files the analysis could do nothing with (unrecognised, unreadable, or
    // duplicates of sets the library already holds) are moved to quarantine
    // with their reason, instead of lingering in the inbox.
    bool        quarantine_rejects = true;
    std::string quarantine_dir;
    // The library : the configured ROM directories. The scan cache can still
    // hold archives of a folder the user has since removed from Settings ;
    // those are not "the library" any more, neither as a source of pieces nor
    // as proof a set is already there. Empty accepts every cached archive.
    std::vector<std::string> roms_paths;
    // Whose sets the inbox is matched against : the library group's emulator.
    // An archive is only ever recognised as a set of that emulator, and the
    // library pool above is that emulator's ROM directories.
    std::string emulator = "fbneo";
};

// The subfolder processed sources are moved into; skipped when listing.
constexpr const char* kProcessedSubdir = "_processed";
constexpr const char* kRejectedSubdir  = "_rejected";   // under the quarantine root

struct Report {
    std::string              outbox_dir;    // where apply() writes, and keeps its manifest
    std::string              inbox_dir;
    Options                  options;       // as analysed : apply() follows the same
    std::vector<SetPlan>     sets;
    std::vector<std::string> unrecognized;  // inbox archives matching no DAT entry, by name or content
    // Parallel to `unrecognized` : the CRC of each entry the file holds, as
    // "efde5b4d" or "a, b, c" for an archive. Shown to the player : a file
    // that no DAT knows is only diagnosable by its CRC, and that number used
    // to live in the debug log alone.
    std::vector<std::string> unrecognized_crcs;
    std::vector<std::string> unsupported;   // .7z / .rar / anything libzip refuses
    std::vector<std::string> ignored;       // sidecars that are clearly not ROM data (readme, cue, …)
    // Archive's content was recognized (by CRC, not by its own filename) as a
    // game the library already has complete and correct elsewhere : a pure
    // duplicate with nothing left to do, not an unrecognized file.
    std::vector<std::string> already_have;
    int complete = 0, fixable = 0, incomplete = 0, already = 0;
    bool cancelled = false;
    // True when the library pool was empty, which makes "Incomplete" verdicts
    // unreliable : the UI surfaces this as a "run a ROM scan first" hint.
    bool library_pool_empty = false;
};

// Progress/log/cancel plumbing, so the engine stays free of any GTK dependency.
struct Callbacks {
    std::function<void(double, const std::string&)> progress;  // percentage, message
    std::function<void(const std::string&)>         log;
    std::function<bool()>                           cancelled;
};

Report analyze(const std::string& inbox_dir,
               const std::string& outbox_dir,
               std::shared_ptr<DatabaseManager> db,
               const Options& options,
               const Callbacks& cb);

struct ApplyResult {
    int moved = 0, rebuilt = 0, skipped = 0, failed = 0;
    int consumed_archives = 0;   // sources fully used : deleted or moved to _processed
    int quarantined = 0;         // rejects moved to quarantine
    std::vector<std::string> errors;
    bool cancelled = false;
};

ApplyResult apply(const Report& report, const Callbacks& cb);

const char* action_label(Action a);

} // namespace RomInbox
