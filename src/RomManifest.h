// src/RomManifest.h
//
// What Bootcade did to the files it manages, written down next to them.
//
// The outbox and the quarantine are plain folders, and a folder cannot say
// why a file is in it: what set it belongs to, where it came from, whether it
// was rebuilt from three sources or merely renamed, when it arrived. Until
// now that was known only for the session that did it and lost on exit. So
// each managed folder carries a `.bootcade-manifest.json` at its root, one
// entry per file Bootcade put there, and a bounded history of the entries
// that have since left it and how (moved into the library, restored, deleted).
//
// The manifest is a record, never an authority: the files on disk are what
// exists. A file with no entry is simply one Bootcade did not put there (a
// manual drop) and is shown as such; an entry whose file is gone is retired
// to the history on the next reconcile.
#pragma once

#include <string>
#include <vector>

namespace RomManifest {

constexpr int         kSchemaVersion = 1;
constexpr const char* kFileName      = ".bootcade-manifest.json";

// Why a file was put in quarantine. Each value is tied to one real code path;
// the outbox leaves it empty (nothing in the outbox is a rejection).
namespace reason {
    constexpr const char* Unknown     = "unknown";      // recognized by neither name nor content
    constexpr const char* BadCrc      = "bad_crc";      // wrong data, no good copy anywhere
    constexpr const char* Unsupported = "unsupported";  // an archive no reader could open
    constexpr const char* Duplicate   = "duplicate";    // the library already holds it correctly
    constexpr const char* ExtraFiles  = "extra_files";  // entries no DAT rom needs, taken out of a sound zip
    constexpr const char* Replaced    = "replaced";     // the previous library copy, overwritten by a move
}

// What produced the file.
namespace action {
    constexpr const char* Moved     = "moved";      // relocated as-is
    constexpr const char* Rebuilt   = "rebuilt";    // recomposed from pieces
    constexpr const char* Extracted = "extracted";  // pulled out of an archive
}

// How an entry left the folder.
namespace outcome {
    constexpr const char* MovedToLibrary = "moved_to_library";
    constexpr const char* Restored       = "restored";
    constexpr const char* Deleted        = "deleted";
    constexpr const char* Purged         = "purged";
    constexpr const char* Vanished       = "vanished";  // gone from disk, not through Bootcade
}

struct Entry {
    std::string file;         // path relative to the manifest's folder, '/' separated
    std::string game;         // DAT short name, when known
    std::string system;
    std::string dat_header;
    std::string reason;       // see reason::, empty in the outbox
    std::string origin;       // absolute path the data came from
    std::string action;       // see action::
    std::vector<std::string> details;   // human-readable, one fact per line
    std::string added_at;     // ISO-8601 UTC
    // Filled only for history entries.
    std::string removed_at;
    std::string outcome;      // see outcome::
};

class Manifest {
public:
    // Reads <root>/.bootcade-manifest.json. A missing or unreadable file
    // yields an empty manifest for that root : never an error, the folder is
    // still perfectly usable.
    static Manifest load(const std::string& root);

    // Atomic: written to a temporary name, then renamed over the old file.
    bool save() const;

    const std::string& root() const { return m_root; }
    const std::vector<Entry>& entries() const { return m_entries; }
    const std::vector<Entry>& history() const { return m_history; }

    // Adds or replaces the entry for `e.file`. `added_at` is stamped now when
    // left empty.
    void add(Entry e);
    const Entry* find(const std::string& rel_file) const;
    // Retires an entry to the history with the given outcome. No-op when the
    // file has no entry (a manual drop leaves no trace either way).
    void remove(const std::string& rel_file, const std::string& outcome);
    // Retires every entry whose file no longer exists on disk. Returns how
    // many were retired.
    int  reconcile();
    // Retires everything, e.g. after an "Empty quarantine".
    void retire_all(const std::string& outcome);

    // Path helpers, so callers never build the relative form by hand.
    std::string relative(const std::string& absolute_path) const;
    std::string absolute(const std::string& rel_file) const;

    // True for the manifest file (and its temporary), which folder listings
    // must not show as content.
    static bool is_manifest_file(const std::string& filename);

private:
    std::string        m_root;
    std::vector<Entry> m_entries;
    std::vector<Entry> m_history;
    // The history is bounded so a folder churned for years stays cheap to
    // read; the oldest entries go first.
    static constexpr size_t kMaxHistory = 5000;
};

// Where `src` goes in `dir` without overwriting anything. The free name is
// `dir/<name>`, else `dir/<stem> (2)<ext>`, `(3)`… ; `same` turns true, and
// the existing path comes back, when a file of that name already holds the
// very same bytes : the folder already keeps it, nothing more to write.
std::string free_destination(const std::string& dir, const std::string& src, bool& same);

// Current time as "YYYY-MM-DDTHH:MM:SSZ".
std::string now_iso();
// An added_at/removed_at stamp (UTC, ISO 8601) as "YYYY-MM-DD HH:MM" in the
// user's local time : what a screen shows. Anything unparseable comes back as is.
std::string local_time(const std::string& iso);

} // namespace RomManifest
