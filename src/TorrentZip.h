// src/TorrentZip.h
//
// The zips Bootcade writes follow TorrentZip, the layout RomVault and
// clrmamepro keep their sets in : deflate at maximum compression, entries
// sorted by name ignoring case, a fixed date (1996-12-24 23:32), no extra
// field, and a closing comment "TORRENTZIPPED-<CRC of the central directory>".
// Two people holding the same set then hold the very same file, and RomVault
// recognises ours as already in order instead of recompressing every one.
//
// The rules are RomVault's own checks (StructuredZip.ValidateFileHeader and
// friends) : a zip that passes them is left alone by it.
#pragma once

#include <string>

namespace TorrentZip {

// True when `zip_path` already follows TorrentZip (comment, central directory
// CRC, fixed dates, deflate, order, and no extra field but the ZIP64 one
// RomVault writes where a size or an offset reaches 4 GB).
bool is_torrentzip(const std::string& zip_path);

// Writes `out_zip`, a TorrentZip of every entry of `in_zip`, same names, same
// data, in ZIP64 where 4 GB is reached, as RomVault does. False with `error`
// when it cannot : two names equal but for their case, a directory entry, a
// read or write failure. `out_zip` is removed on failure.
bool rezip(const std::string& in_zip, const std::string& out_zip, std::string& error);

}
