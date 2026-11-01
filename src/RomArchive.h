// src/RomArchive.h
//
// Uniform read access to the archive formats users actually download ROMs in.
//
// ZIP keeps going through libzip: it is the format the emulator itself reads, the
// one the ROM manager writes, and libzip can stream an entry straight from one
// archive into another. Everything else (7z, rar, tar, …) is read through
// libarchive.
//
// Reading only. The ROM manager always *writes* ZIP, because that is what FinalBurn
// Neo loads : a 7z dropped in the inbox is converted, never passed through.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace RomArchive {

struct Entry {
    std::string   name;
    unsigned long crc  = 0;
    uint64_t      size = 0;
};

// Extension allow-list, so a stray .txt or .nfo in the inbox is not fed to
// libarchive just to be rejected.
bool looks_like_archive(const std::string& path);

// ZIP gets the libzip path; anything else goes through libarchive.
bool is_zip(const std::string& path);

// A file that is clearly never ROM data (readme, cue sheet, patch, checksum
// list, picture…), rather than an attempt at listing every ROM extension of
// every system : anything else may be a loose ROM.
bool is_sidecar(const std::string& path);

// Every regular-file entry with its recomputed CRC32. Directory entries are
// skipped. False when the file cannot be opened as an archive.
bool read_entries(const std::string& path, std::vector<Entry>& out);

// The ROM entries of any file : an archive's, or a loose ROM as its own one
// entry, named after the file.
bool read_any(const std::string& path, std::vector<Entry>& out);

// The ROM entries of any file, a ZIP's as its central directory states them :
// nothing decompressed, so a whole collection is listed in minutes. What it
// states is checked where it matters, when an entry is read. Other archives
// and loose files are read as read_any reads them.
bool read_listed(const std::string& path, std::vector<Entry>& out);

// Entry names only : no decompression. Used where the contents do not matter.
bool list_names(const std::string& path, std::vector<std::string>& out);

// Extract the wanted entries into dest_dir in a SINGLE sequential pass. That
// matters enormously for 7z: its default solid blocks mean pulling entries one at
// a time re-decompresses the whole block each time.
//
// Files are written under opaque numbered names to sidestep any path or collision
// question; out_files maps the archive entry name to the extracted file path.
bool extract(const std::string& archive_path,
             const std::vector<std::string>& wanted,
             const std::string& dest_dir,
             std::unordered_map<std::string, std::string>& out_files,
             std::string& error);

} // namespace RomArchive
