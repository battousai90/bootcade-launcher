// src/DatLayout.h
//
// What one DAT says each archive of its folder holds.
//
// A DAT is the contract. Its sets, read with the DAT's merge mode (see
// DatSource::effective_merge), give the exact list of entries every archive
// (zip) and every folder of CHDs must hold. Everything that judges or builds
// a set (scan, audit, Import, Rebuild, Fix) starts from this list, and from
// nothing else : a set is never judged by what another archive holds.
//
//   ""          : a DAT without links, read as it is : one archive per set,
//                 every ROM and disk it lists ;
//   split       : one archive per set, holding its ROMs and disks without
//                 merge= (those belong to the parent or the BIOS) ; a set left
//                 with nothing has no archive ;
//   non-merged  : one archive per set, holding every ROM and disk it lists,
//                 merge= ones included, and the ROMs of the devices it uses
//                 (device_ref, followed through the devices' own) whose name
//                 the set does not already use ;
//   merged      : one archive per parent (a set without cloneof), holding
//                 its split content, then each clone's, clones in natural
//                 order, a clone's ROMs under "<clone>/<name>" and skipped
//                 when an entry with the same CRC is already there ; CHDs
//                 side by side in the parent's folder, skipped when the name
//                 is already there. Clones have no archive of their own.
//
// Only the DAT's own sets are looked at : a parent, BIOS or device named by
// a link is looked for in the same DAT. Nothing depends on where the DAT
// comes from. Two exceptions are asked for, never assumed (Options) : the
// Rebuilder hands over the other DATs it read (`linked`), where a non-merged
// set finds the devices a DAT of their own describes ; those sets get no
// archive, they only lend their ROMs. And it reads the DAT alone
// (`strict_dat`), without the one rule taken from an emulator : an FBNeo
// clone repeating its parent's disk without merge= (disk_is_parents).
//
// check() says what a mode needs that a DAT does not say, before anything is
// laid out : a DAT without links, ROMs inherited but not marked, a link to a
// set no DAT holds. What it reports is never guessed instead.
#pragma once

#include "Game.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace DatLayout {

struct Entry {
    std::string   name;       // inside the archive ; "<clone>/<rom>" in a merged parent
    unsigned long crc = 0;
    uint64_t      size = 0;
    std::string   owner;      // the set whose ROM it is, when not the archive's own set
    bool          merge = false;   // the DAT marks it merge= : a ROM of the parent, listed here as it is
};

struct DiskEntry {
    std::string name;         // file name without ".chd"
    std::string sha1;         // lower case
    std::string owner;        // the set whose disk it is, when not the archive's own set
};

struct Archive {
    std::string            name;     // the archive's set : "<name>.zip", "<name>/" for CHDs
    std::vector<Entry>     entries;
    std::vector<DiskEntry> disks;
};

struct Options {
    // Sets of other DATs a device_ref may name, looked for when `sets` has no
    // set of that name.
    std::vector<const Game*> linked;
    // The DAT alone : no disk_is_parents.
    bool strict_dat = false;
};

class Layout {
public:
    // `sets` : every set of ONE DAT. `mode` : "", split, non-merged, merged.
    Layout(const std::vector<const Game*>& sets, const std::string& mode,
           const Options& options = Options());

    const std::string& mode() const { return m_mode; }
    const std::vector<Archive>& archives() const { return m_archives; }

    // The archive that holds `set`'s content : its own, or its parent's in
    // merged. Null when the DAT expects nothing for it (a split clone with no
    // ROM of its own, a set with nothing to verify).
    const Archive* archive_of(const std::string& set) const;
    // What `set` needs in that archive : indexes into its entries and disks.
    // Everything the archive holds, except for a merged clone (its own ROMs,
    // wherever the parent's archive already has them).
    const std::vector<size_t>& entries_of(const std::string& set) const;
    const std::vector<size_t>& disks_of(const std::string& set) const;

private:
    std::string m_mode;
    std::vector<Archive> m_archives;
    std::unordered_map<std::string, size_t> m_archive_of;             // set → archive index
    std::unordered_map<std::string, std::vector<size_t>> m_entries_of;
    std::unordered_map<std::string, std::vector<size_t>> m_disks_of;
};

// ── What a mode needs that the DAT does not say ─────────────────────────────
enum class Missing {
    NoLinks,             // no cloneof, romof nor device_ref anywhere : only "" can be laid out
    MergeNotMarked,      // a ROM the set shares with a parent or BIOS, not marked merge=
    ParentRomsNotListed, // the DAT marks no ROM merge= and the set lists none of its parent's
    LinkTargetMissing,   // cloneof or device_ref names a set no DAT read holds
    NoDeviceRefs,        // devices described, but no device_ref : reported by the caller, which counts them
};

struct Problem {
    Missing     kind;
    std::string set;      // empty for the whole DAT
    std::string detail;   // the ROM, or the set named
};

struct CheckResult {
    bool                 whole_dat = false;   // the mode is impossible for every set
    std::vector<Problem> problems;
};

// What laying `sets` (one DAT, `linked` the others read with it) out in
// `mode` would have to guess. "" never does.
CheckResult check(const std::vector<const Game*>& sets, const std::vector<const Game*>& linked,
                  const std::string& mode);

// "a9" before "a10" : how merged archives order clones and their ROMs.
bool natural_less(const std::string& a, const std::string& b);

// A DAT's own spelling of a path inside an archive ('\' in merged DATs) and
// an archive's ('/') name the same entry.
std::string entry_path(const std::string& name);

} // namespace DatLayout
