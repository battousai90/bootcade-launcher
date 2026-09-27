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
// comes from.
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

class Layout {
public:
    // `sets` : every set of ONE DAT. `mode` : "", split, non-merged, merged.
    Layout(const std::vector<const Game*>& sets, const std::string& mode);

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

// "a9" before "a10" : how merged archives order clones and their ROMs.
bool natural_less(const std::string& a, const std::string& b);

// A DAT's own spelling of a path inside an archive ('\' in merged DATs) and
// an archive's ('/') name the same entry.
std::string entry_path(const std::string& name);

} // namespace DatLayout
