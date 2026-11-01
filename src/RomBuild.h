// src/RomBuild.h
//
// Writing a set : how Import and Rebuild turn pieces found here and there
// into the archive a DAT asks for.
//
// A piece is one entry of the archive to write and the place its bytes are
// read from : an entry of a ZIP (streamed by libzip, never held in memory),
// an entry of any other archive (7z, rar… drained once into a staging
// folder), or a loose file. The archive is written next to its destination,
// read back and checked piece by piece, then renamed into place : a set that
// fails the check never lands, and whatever was there stays.
//
// Nothing here knows about DATs, merge modes or where the pieces were found :
// the caller has decided all of that, and says it with the list of pieces.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace RomBuild {

using Bytes = std::vector<unsigned char>;

// ── Checksum repair ─────────────────────────────────────────────────────────
// clrmamepro's "ROM checksum analysis" : a file that is not the DAT's dump
// may still hold it, in the wrong byte order, doubled, split or padded. Each
// transform is tried, and kept only when its result has exactly the size and
// the CRC the DAT asks for : a repair is never a guess.
bool          transform(const std::string& kind, const Bytes& in, uint64_t size, Bytes& out);
unsigned long crc_of(const Bytes& b);
// The transform that turns `data` into the dump of that CRC and size, or "".
std::string   find_repair(const Bytes& data, unsigned long crc, uint64_t size);
// One entry's bytes, whatever holds it : a loose file, or any archive.
bool          read_bytes(const std::string& container, const std::string& entry, Bytes& out, std::string& error);

// One entry of the archive to write.
struct Piece {
    std::string   name;        // its name in the written archive
    unsigned long crc  = 0;
    uint64_t      size = 0;
    std::string   container;   // a ZIP, another archive, or a loose file ; "" for an empty ROM
    std::string   entry;       // inside `container` ; unused for a loose file
    std::string   repair;      // the checksum repair it needs, "" for none
};

enum class Format { Zip, SevenZip };

// The extension a written archive takes : ".zip" or ".7z".
const char* extension(Format format);

// True when `archive`, read as `format` whatever its name, holds exactly
// `pieces` : every name, with its CRC and size, and nothing else.
bool verify(const std::string& archive, Format format, const std::vector<Piece>& pieces, std::string& error);

// Replaces `zip_path` with its TorrentZip, checked against `pieces`. False
// leaves `zip_path` as it was.
bool to_torrentzip(const std::string& zip_path, const std::vector<Piece>& pieces);

// Writes `dest_path` from `pieces` : Import's way. A ZIP is deflated then
// turned into a TorrentZip (kept as libzip wrote it when TorrentZip refuses
// it : two names equal but for their case) ; a 7z is LZMA, entries sorted by
// name ignoring case. Either is read back and checked before it replaces
// `dest_path`.
bool write_archive(const std::string& dest_path, const std::vector<Piece>& pieces, Format format, std::string& error);

// The Rebuilder's way, in two steps, so that a destination is replaced only
// once its replacement is complete :
//
// build_archive writes `tmp_path` from `pieces` and checks it. A ZIP must
// come out a TorrentZip (ZIP64 included) or it is a failure, with `error`
// saying why. `cancelled`, when given, is asked while writing ; a cancelled
// or failed archive leaves nothing behind.
bool build_archive(const std::string& tmp_path, const std::vector<Piece>& pieces, Format format,
                   const std::function<bool()>& cancelled, std::string& error);
// The TorrentZip of `source`, a ZIP that already holds exactly `pieces`, at
// `tmp_path` and checked : the same content, normalised.
bool normalise_zip(const std::string& source, const std::string& tmp_path, const std::vector<Piece>& pieces,
                   std::string& error);
// Puts `tmp_path` at `dest_path`. An archive already there is first kept at
// `keep_old_at` (a hard link, else a copy checked byte for byte), then one
// rename replaces it ; an empty `keep_old_at` says it is kept already. On
// failure `tmp_path` is removed and `dest_path` is what it was.
bool publish(const std::string& tmp_path, const std::string& dest_path, const std::string& keep_old_at,
             std::string& error);
// True when the filesystem of `dir` has no room left : a write that failed
// there will fail for every archive after it.
bool disk_full(const std::string& dir);

// rename(), falling back to copy + delete across filesystems.
bool move_file(const std::string& from, const std::string& to, std::string& error);

// A DAT name made safe as one path component : no separator, never "." or "..".
std::string sanitize_component(const std::string& s);

// `p` made canonical as far as it exists ; `p` itself when that fails.
std::filesystem::path weak_canonical(const std::string& p);
// True when `path` sits inside `root` (both canonical-ish).
bool is_under(const std::filesystem::path& path, const std::filesystem::path& root);

} // namespace RomBuild
