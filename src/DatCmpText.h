// src/DatCmpText.h
#pragma once
#include <string>

namespace pugi { class xml_document; }

// The ClrMamePro text DAT format :
//
//   clrmamepro ( name "..." description "..." version ... forcemerging split )
//   game ( name x description "..." cloneof y romof y
//          rom ( name a.bin size 123 crc 1234abcd merge a.bin status nodump )
//          disk ( name d sha1 ... merge d ) device_ref z sample s.wav )
//
// Blocks are `key ( ... )`, values are bare tokens or "quoted strings". The
// reader builds the very tree a Logiqx XML datafile gives (<datafile>, its
// <header>, <game>/<machine> with their <rom>, <disk>, <device_ref> ...), so
// every reader of a datafile reads both formats the same way.
namespace DatCmpText {

// True when `text` starts, past a byte-order mark and blanks, with one of
// the format's top-level blocks : `clrmamepro (`, `game (`, `machine (` or
// `resource (`. Content only : the file name is never looked at.
bool looks_like(const std::string& text);

// looks_like() on the first kilobytes of a file.
bool is_file(const std::string& path);

// The whole file (or text) as a Logiqx datafile tree in `doc`. False, with
// `error` set when given, when the text is not in the format or is cut short.
bool load_file(const std::string& path, pugi::xml_document& doc, std::string* error = nullptr);
bool load_string(const std::string& text, pugi::xml_document& doc, std::string* error = nullptr);

} // namespace DatCmpText
