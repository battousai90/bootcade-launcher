// src/RomRebuild.h
//
// The Rebuild tab's engine : clrmamepro's Rebuilder, in Bootcade's terms.
//
// The DATs say which sets exist and what they hold ; `mode` says how they are
// laid out in the destination (non-merged, split, merged : DatLayout), or
// "" for exactly as each DAT lists them. The DATs are a Bootcade group's, read
// from the database, or files chosen outside of any group, read in memory
// only : nothing is registered, no emulator is involved, and only what those
// DATs say decides what an archive must hold.
//
// Before anything is laid out, each DAT is checked for what the mode needs
// (DatLayout::check) : a mode the DAT cannot give is refused, a set it cannot
// tell is left unwritten, and both say what is missing. Nothing is guessed.
//
// Every file of the source folder is read and each of its entries known by
// its CRC and size (CHDs by the SHA1 of their header). Each archive takes its
// pieces wherever they are and is written next to its destination, checked
// (a ZIP must be a TorrentZip, ZIP64 included), then published in one rename,
// the archive it replaces kept in _replaced/. Nothing is ever deleted : not a
// source, not a destination file.
#pragma once

#include "DatLayout.h"
#include "DatabaseManager.h"
#include "RomBuild.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace RomRebuild {

enum class DatOrigin { Group, Files };

struct Options {
    DatOrigin                origin = DatOrigin::Group;
    std::string              group_id;   // Group : the DAT group (DatSource::Group::id), active or not
    // Files : every DAT file offered (a file, the files of a folder, or the
    // DATs inside an archive) ; the others besides `dats` only lend their
    // sets to links (BIOS, devices), like the other DATs of a group.
    std::vector<std::string> files;
    // The DATs to rebuild : file names of the group, or paths among `files`.
    std::vector<std::string> dats;
    std::string              source_dir;
    std::string              dest_dir;
    std::string              mode = "non-merged";   // DatSource::kMerge*, or "" : as the DAT lists
    RomBuild::Format         format = RomBuild::Format::Zip;
    bool recursive  = true;    // the source's subfolders too
    // Every archive written anew. Off, an archive that already conforms is
    // copied or kept as it is, and a ZIP with the right content but not a
    // TorrentZip is normalised.
    bool recompress = false;
    // An incomplete set written with the pieces found (RomVault's partial
    // fix, Import's rebuild_incomplete) : what an old collection still holds
    // is kept, and a later rebuild completes it. Off, it is only reported.
    bool write_incomplete = true;
};

// Archives replaced in the destination are kept here (under dest_dir).
constexpr const char* kReplacedSubdir = "_replaced";
constexpr const char* kReportFile     = "rebuild-report.txt";

enum class Status {
    Build,          // written from its pieces
    Copy,           // one source archive is that very archive, conforming : copied
    Normalise,      // one archive holds exactly that content, but is not a TorrentZip
    AlreadyThere,   // the destination already holds it, conforming
    Incomplete,     // some pieces found, not all : written with them (Options::write_incomplete), reported
    Impossible,     // not one piece found
    Undeterminable, // the DAT does not say what it must hold in this mode : not written
};

struct Piece {
    std::string   name;       // entry name in the archive
    unsigned long crc  = 0;
    uint64_t      size = 0;
    std::string   owner;      // the set whose ROM it is, when not the archive's own
    bool          found = false;
    std::string   container;  // where it is read from ; "" for an empty ROM
    std::string   entry;
    // Other copies of the same ROM (container, entry) : tried when the chosen
    // one does not read back as its CRC.
    std::vector<std::pair<std::string, std::string>> others;
};

struct DiskPiece {
    std::string name;         // without ".chd"
    std::string sha1;
    std::string owner;
    bool        found = false;
    std::string file;         // the source CHD
    bool        present = false;   // the destination already has it
};

// One archive of the destination, and its CHD folder.
struct ArchivePlan {
    std::string set;            // the archive's set (a merged clone's parent)
    std::string description;
    std::string dat;            // its DAT (file name, or path for a file chosen outside a group)
    std::string folder;         // the DAT's folder in the destination
    std::string dest_path;      // <dest>/<folder>/<set>.zip (or .7z) ; "" when it has no ROM
    std::string disk_dir;       // <dest>/<folder>/<set>/ for its CHDs
    Status      status = Status::Impossible;
    std::vector<Piece>     pieces;
    std::vector<DiskPiece> disks;
    std::string from;           // Copy, Normalise : the archive taken as it is
    bool        archive_ok = false;   // the archive at dest_path is already right : only CHDs to add
    bool        replaces = false;   // a different archive sits at dest_path
    // Incomplete, and written with what was found : false when the
    // destination already holds exactly that, or nothing was found to write.
    bool        partial = false;
    // The pieces the archive already at dest_path holds (CRC + size) : a new
    // archive replaces it only if it holds every one of them.
    std::vector<std::string> dest_keys;
    // Checked in the Rebuild tab : when any archive to write is, apply writes
    // those only ; when none is, every archive to write.
    bool        selected = false;
    std::string why;            // Undeterminable : what the DAT does not say
    // Dependencies another DAT holds that was not chosen : "neogeo (BIOS &
    // devices)". Rebuilt anyway : the set matches its DAT without them.
    std::vector<std::string> needs;
    int      found = 0, missing = 0;
    int      sources = 0;        // distinct files the pieces come from
    uint64_t bytes = 0;          // what the DAT says the ROMs weigh
};

// What one DAT is, and which modes it can give.
struct DatReport {
    std::string dat;            // as in ArchivePlan::dat
    std::string label;          // what to show : its header name, or the file name
    std::string version, build;
    size_t sets = 0, clones = 0, romofs = 0, merge_roms = 0, device_refs = 0, isdevice = 0, disks = 0;
    // "", split, non-merged, merged : possible for every set, or not at all.
    bool        possible[4] = {true, false, false, false};
    // The chosen mode : what the DAT does not say, for the whole DAT or set by set.
    bool                            whole = false;
    std::vector<DatLayout::Problem> problems;
    std::string                     advice;   // what would give the mode (empty when nothing is missing)
};

// The order of DatReport::possible.
const char* const kModes[4] = {"", "split", "non-merged", "merged"};

struct Report {
    Options                  options;
    std::vector<DatReport>   dats;
    std::vector<ArchivePlan> archives;
    std::vector<std::string> unreadable;     // source files no reader could open
    size_t                   source_files = 0;
    int build = 0, copy = 0, normalise = 0, already = 0, incomplete = 0, impossible = 0, undeterminable = 0;
    int partial = 0;                         // of the incomplete, those written with what was found
    // The chosen mode is impossible for at least one chosen DAT as a whole :
    // its sets are not laid out at all, and the DatReport says why.
    bool        refused = false;
    bool        cancelled = false;
    std::string error;                       // nothing could be analysed
};

struct Callbacks {
    std::function<void(double, const std::string&)> progress;  // percentage, message
    std::function<void(const std::string&)>         log;
    std::function<bool()>                           cancelled;
};

// The DAT files of one group : those the database holds for an active group,
// those its folder holds and it selects for an inactive one (read from the
// files, in memory). What the Rebuild tab offers.
std::vector<std::string> dats_of_group(std::shared_ptr<DatabaseManager> db, const std::string& group_id);
// The DAT files of a folder (not its subfolders), and the archives holding
// DATs : what "DAT folder" offers.
std::vector<std::string> dats_of_folder(const std::string& folder);

Report analyze(std::shared_ptr<DatabaseManager> db, const Options& options, const Callbacks& cb);

struct ApplyResult {
    int built = 0, copied = 0, normalised = 0, disks = 0, failed = 0;
    int partial = 0;             // incomplete sets written with what was found
    // ROMs whose every copy failed to read back as its CRC : left out, the
    // set written without them. "<folder>/<set> : <rom> (<crc>)".
    std::vector<std::string> unreadable;
    // Archives left in place : the new one would have lost some of their ROMs.
    int kept = 0;
    int replaced = 0;            // archives kept in _replaced/ before being replaced
    int cleaned = 0;             // leftovers of an interrupted run removed
    uint64_t bytes_written = 0;
    std::vector<std::string> errors;
    std::string report_path;
    bool cancelled = false;
    bool disk_full = false;      // stopped : the destination has no room left
};

// Whether apply writes `a` : something to write (Build, Copy, Normalise, or an
// incomplete set written with what was found), and checked, or none is.
bool writes(const ArchivePlan& a);
bool chosen(const Report& report, const ArchivePlan& a);

ApplyResult apply(const Report& report, const Callbacks& cb);

} // namespace RomRebuild
