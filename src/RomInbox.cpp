// src/RomInbox.cpp
#include "RomInbox.h"
#include "FileCopy.h"
#include "i18n.h"
#include "RomArchive.h"
#include "RomManifest.h"
#include "RomScanner.h"

#include <zip.h>
#include <zlib.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace RomInbox {
namespace {

unsigned long parse_crc_hex(const std::string& hex) {
    if (hex.empty()) return 0;
    unsigned long crc = 0;
    std::stringstream ss;
    ss << std::hex << hex;
    ss >> crc;
    return crc;
}

bool is_cancelled(const Callbacks& cb) { return cb.cancelled && cb.cancelled(); }

void report(const Callbacks& cb, double pct, const std::string& msg) {
    if (cb.progress) cb.progress(pct, msg);
}

void log(const Callbacks& cb, const std::string& msg) {
    if (cb.log) cb.log(msg);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// A ROM dumped loose, with no archive around it at all, is just as valid an
// inbox source as one already zipped : DAT roms are matched by name/CRC either
// way. Only sidecar files that are clearly never ROM data are excluded, rather
// than trying to enumerate every possible ROM extension across every system.
bool is_junk_sidecar(const std::string& path) {
    static const std::unordered_set<std::string> exts = {
        ".txt", ".nfo", ".diz", ".dsc", ".cue", ".ccd", ".sub", ".m3u",
        ".ips", ".bps", ".ups", ".xdelta", ".dat", ".xml", ".json",
        ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".pdf",
        ".md5", ".sha1", ".sfv", ".cfg", ".ini", ".url", ".log",
    };
    auto p = fs::path(path);
    return exts.count(lower(p.extension().string())) > 0;
}

// CRC32 of a whole file : the "archive" reading path for a loose ROM, which
// has exactly one entry: itself.
bool compute_file_crc(const std::string& path, unsigned long& crc, uint64_t& size) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    crc = crc32(0L, Z_NULL, 0);
    size = 0;
    std::vector<char> buf(65536);
    while (f.read(buf.data(), buf.size()) || f.gcount() > 0) {
        auto n = (uInt)f.gcount();
        crc = crc32(crc, (const Bytef*)buf.data(), n);
        size += n;
    }
    return true;
}

// ── Checksum repair ─────────────────────────────────────────────────────────
// clrmamepro's "ROM checksum analysis" : a file that is not the DAT's dump
// may still hold it, in the wrong byte order, doubled, split or padded. Each
// transform is tried, and kept only when its result has exactly the size and
// the CRC the DAT asks for : a repair is never a guess.
using Bytes = std::vector<unsigned char>;

const char* const kRepairs[] = {
    "swap16", "swap32", "halves", "first_half", "second_half",
    "even", "odd", "mirror", "pad00", "padff", "trim",
};

bool transform(const std::string& kind, const Bytes& in, uint64_t size, Bytes& out) {
    const uint64_t n = in.size();
    out.clear();
    if (kind == "swap16" || kind == "swap32") {
        const uint64_t w = kind == "swap16" ? 2 : 4;
        if (n != size || n % w) return false;
        out = in;
        for (uint64_t i = 0; i < n; i += w) std::reverse(out.begin() + i, out.begin() + i + w);
    } else if (kind == "halves") {
        if (n != size || n % 2) return false;
        out.insert(out.end(), in.begin() + n / 2, in.end());
        out.insert(out.end(), in.begin(), in.begin() + n / 2);
    } else if (kind == "first_half" || kind == "second_half") {
        if (n != 2 * size) return false;
        auto from = in.begin() + (kind == "first_half" ? 0 : size);
        out.assign(from, from + size);
    } else if (kind == "even" || kind == "odd") {
        if (n != 2 * size) return false;
        out.reserve(size);
        for (uint64_t i = kind == "even" ? 0 : 1; i < n; i += 2) out.push_back(in[i]);
    } else if (kind == "mirror") {
        if (2 * n != size) return false;
        out = in;
        out.insert(out.end(), in.begin(), in.end());
    } else if (kind == "pad00" || kind == "padff") {
        if (n == 0 || n >= size) return false;
        out = in;
        out.resize(size, kind == "pad00" ? 0x00 : 0xFF);
    } else if (kind == "trim") {
        if (n <= size) return false;
        out.assign(in.begin(), in.begin() + size);
    } else {
        return false;
    }
    return out.size() == size;
}

unsigned long crc_of(const Bytes& b) {
    return crc32(crc32(0L, Z_NULL, 0), b.data(), (uInt)b.size());
}

// The transform that turns `data` into the dump of that CRC and size, or "".
std::string find_repair(const Bytes& data, unsigned long crc, uint64_t size) {
    Bytes out;
    for (const char* k : kRepairs)
        if (transform(k, data, size, out) && crc_of(out) == crc) return k;
    return {};
}

// One entry's bytes, whatever holds it : a loose file, or any archive.
bool read_bytes(const std::string& container, const std::string& entry, Bytes& out, std::string& error) {
    std::string file = container;
    fs::path tmp;
    if (RomArchive::looks_like_archive(container)) {
        tmp = fs::temp_directory_path() / ("bootcade-repair-" + std::to_string(std::hash<std::string>{}(container + entry)));
        std::unordered_map<std::string, std::string> files;
        if (!RomArchive::extract(container, {entry}, tmp.string(), files, error)) {
            std::error_code ec; fs::remove_all(tmp, ec);
            return false;
        }
        auto it = files.find(entry);
        if (it == files.end()) { error = "no " + entry + " in " + container; std::error_code ec; fs::remove_all(tmp, ec); return false; }
        file = it->second;
    }
    std::ifstream f(file, std::ios::binary);
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    const bool ok = (bool)f || f.eof();
    if (!tmp.empty()) { std::error_code ec; fs::remove_all(tmp, ec); }
    if (!ok) error = "cannot read " + file;
    return ok;
}

// Replace anything that would escape the intended directory. DAT headers and game
// names are tame in practice, but they end up as filesystem paths so they get
// sanitized rather than trusted.
std::string sanitize_component(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += (c == '/' || c == '\\') ? '_' : c;
    if (out == "." || out == "..") out = "_";
    return out.empty() ? std::string("_") : out;
}

// True when `path` sits inside `root`. Both are expected already canonical-ish;
// used to keep the inbox and the outbox out of the "existing library" pool.
bool is_under(const fs::path& path, const fs::path& root) {
    if (root.empty()) return false;
    auto it_r = root.begin(), end_r = root.end();
    auto it_p = path.begin(), end_p = path.end();
    for (; it_r != end_r; ++it_r, ++it_p) {
        if (it_p == end_p || *it_p != *it_r) return false;
    }
    return true;
}

fs::path weak_canonical(const std::string& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(fs::path(p), ec);
    return ec ? fs::path(p) : c;
}

// A single ZIP sitting in the inbox, with its recomputed contents.
struct InboxArchive {
    std::string path;
    std::vector<RomArchive::Entry> entries;
};

// CRC → every place that CRC can be read from.
using PoolIndex = std::unordered_multimap<unsigned long, PieceSource>;

int action_rank(Action a) {
    switch (a) {
        case Action::Move:             return 4;
        case Action::Rebuild:          return 3;
        case Action::AlreadyInLibrary: return 2;
        case Action::Incomplete:       return 1;
    }
    return 0;
}

// ── Rebuild helpers ──────────────────────────────────────────────────────────

// Keeps every source archive open for the lifetime of one rebuild: libzip streams
// from the source when the destination is written, so the handles must outlive
// zip_close() on the target.
class SourceArchives {
public:
    ~SourceArchives() {
        for (auto& [_, z] : m_open) if (z) zip_close(z);
    }

    // Returns the open archive and the index of `entry` inside it, or false.
    bool locate(const std::string& container, const std::string& entry,
                zip_t*& out_zip, zip_int64_t& out_index) {
        zip_t* z = open(container);
        if (!z) return false;
        zip_int64_t idx = zip_name_locate(z, entry.c_str(), 0);
        if (idx < 0) return false;
        out_zip = z;
        out_index = idx;
        return true;
    }

private:
    zip_t* open(const std::string& path) {
        auto it = m_open.find(path);
        if (it != m_open.end()) return it->second;
        int err = 0;
        zip_t* z = zip_open(path.c_str(), ZIP_RDONLY, &err);
        m_open[path] = z;
        return z;
    }
    std::unordered_map<std::string, zip_t*> m_open;
};

// Re-read a freshly written archive and check every piece against the DAT.
// This is the only thing standing between a silent libzip/IO failure and a
// corrupt set landing in the outbox, so a rebuild is not considered done until
// this passes.
bool verify_against_plan(const std::string& zip_path, const SetPlan& plan, std::string& error) {
    std::vector<RomScanner::ZipEntry> produced;
    if (!RomScanner::read_zip_entries(zip_path, produced)) {
        error = "cannot reopen the rebuilt archive";
        return false;
    }
    std::unordered_map<std::string, const RomScanner::ZipEntry*> by_name;
    for (const auto& e : produced) by_name[e.name] = &e;

    for (const auto& p : plan.pieces) {
        // A partial set holds only what was found : the rest is not written.
        if (!p.resolved && plan.partial) continue;
        auto it = by_name.find(p.target_name);
        if (it == by_name.end()) {
            error = "missing entry after rebuild: " + p.target_name;
            return false;
        }
        if (it->second->crc != p.crc) {
            error = "CRC mismatch after rebuild: " + p.target_name;
            return false;
        }
        if (p.size && it->second->size != p.size) {
            error = "size mismatch after rebuild: " + p.target_name;
            return false;
        }
    }
    const size_t expected = (size_t)std::count_if(plan.pieces.begin(), plan.pieces.end(),
        [&](const PiecePlan& p) { return p.resolved || !plan.partial; });
    if (produced.size() != expected) {
        error = "unexpected entry count after rebuild";
        return false;
    }
    return true;
}

// Stream one entry of an already-open ZIP straight into the archive being written,
// without ever holding it in memory.
//
// libzip renamed this call: zip_source_zip() gained a password argument and became
// zip_source_zip_file(), the old name surviving as a deprecated alias. Distributions
// are all over the place on which one they ship, so CMake probes for the symbol at
// configure time (HAVE_ZIP_SOURCE_ZIP_FILE) rather than us guessing from version
// numbers. Using the modern spelling where it exists also avoids the deprecation
// warning.
#ifdef HAVE_ZIP_SOURCE_ZIP_FILE
#  define FBNEO_ZIP_SOURCE_FROM_ZIP(dst, src, idx) \
       zip_source_zip_file((dst), (src), (idx), 0, 0, -1, nullptr)
#else
#  define FBNEO_ZIP_SOURCE_FROM_ZIP(dst, src, idx) \
       zip_source_zip((dst), (src), (idx), 0, 0, -1)
#endif

// Maps an inbox path a Move has already relocated to its new outbox path.
using Relocations = std::unordered_map<std::string, std::string>;

// Temporary directory holding entries pulled out of non-ZIP sources, removed
// whatever the outcome. libzip needs the files to survive until zip_close().
struct Staging {
    fs::path dir;
    explicit Staging(const std::string& base) : dir(base + ".staging") {}
    ~Staging() { std::error_code ec; fs::remove_all(dir, ec); }
    Staging(const Staging&) = delete;
    Staging& operator=(const Staging&) = delete;
};

bool rebuild_set(const SetPlan& plan, const Relocations& relocated, std::string& error) {
    const std::string tmp_path = plan.dest_path + ".tmp";
    // A piece nobody could supply has nothing to write.
    auto wanted = [&](const PiecePlan& p) { return p.resolved; };

    std::error_code ec;
    fs::remove(tmp_path, ec);

    // Resolve each piece's container up front: a Move may already have relocated
    // the archive a clone borrows from.
    auto container_of = [&](const PiecePlan& p) {
        auto moved = relocated.find(p.src.container);
        return moved != relocated.end() ? moved->second : p.src.container;
    };

    // 7z and rar cannot be streamed entry-by-entry into a ZIP the way libzip
    // streams from another ZIP, and 7z's solid blocks make random access
    // pathological : pulling N entries one at a time re-decompresses the whole
    // block N times. So each non-ZIP source is drained once, in a single pass.
    Staging staging(plan.dest_path);
    std::unordered_map<std::string, std::string> staged;   // "container\x1fentry" → file
    {
        std::unordered_map<std::string, std::vector<std::string>> by_container;
        for (const auto& p : plan.pieces) {
            if (!wanted(p) || !p.repair.empty()) continue;
            std::string c = container_of(p);
            // A loose file needs no extraction step at all : it *is* its own
            // one and only piece, read straight off disk further down.
            if (!RomArchive::is_zip(c) && RomArchive::looks_like_archive(c))
                by_container[c].push_back(p.src.entry);
        }
        // Each source gets its own staging subdirectory: extract() numbers its
        // output files from zero, so sharing one directory between two archives
        // would silently overwrite the first one's pieces with the second's.
        size_t slot = 0;
        for (const auto& [container, entries] : by_container) {
            std::string dir = (staging.dir / std::to_string(slot++)).string();
            std::unordered_map<std::string, std::string> files;
            if (!RomArchive::extract(container, entries, dir, files, error))
                return false;
            for (auto& [entry, file] : files)
                staged[container + '\x1f' + entry] = file;
        }
    }

    SourceArchives sources;

    int zerr = 0;
    zip_t* out = zip_open(tmp_path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &zerr);
    if (!out) {
        error = "cannot create " + tmp_path;
        return false;
    }

    size_t repaired_slot = 0;
    for (const auto& p : plan.pieces) {
        if (!wanted(p)) continue;
        const std::string container = container_of(p);
        zip_source_t* zs = nullptr;

        if (!p.repair.empty()) {
            // Transformed, then checked once more : only the DAT's dump is
            // ever written under its name.
            Bytes data, fixed;
            if (!read_bytes(container, p.src.entry, data, error)) {
                zip_discard(out); fs::remove(tmp_path, ec); return false;
            }
            if (!transform(p.repair, data, p.size, fixed) || crc_of(fixed) != p.crc) {
                error = "checksum repair of " + p.src.entry + " no longer gives the DAT's CRC";
                zip_discard(out); fs::remove(tmp_path, ec); return false;
            }
            fs::create_directories(staging.dir, ec);
            const fs::path file = staging.dir / ("repaired-" + std::to_string(repaired_slot++));
            std::ofstream o(file, std::ios::binary);
            o.write(reinterpret_cast<const char*>(fixed.data()), (std::streamsize)fixed.size());
            o.close();
            if (!o) { error = "cannot write " + file.string(); zip_discard(out); fs::remove(tmp_path, ec); return false; }
            zs = zip_source_file(out, file.string().c_str(), 0, -1);
        } else if (RomArchive::is_zip(container)) {
            zip_t* src = nullptr;
            zip_int64_t src_idx = 0;
            if (!sources.locate(container, p.src.entry, src, src_idx)) {
                error = "source vanished: " + p.src.entry + " in " + container;
                zip_discard(out);
                fs::remove(tmp_path, ec);
                return false;
            }
            // flags = 0 → libzip decompresses the source and recompresses on write,
            // so the output is uniformly deflated whatever the source archive used.
            // It streams, so a 700 MB Neo Geo set never has to fit in memory.
            zs = FBNEO_ZIP_SOURCE_FROM_ZIP(out, src, (zip_uint64_t)src_idx);
        } else if (!RomArchive::looks_like_archive(container)) {
            // Loose file: the piece's bytes are the whole file, nothing to stage.
            zs = zip_source_file(out, container.c_str(), 0, -1);
        } else {
            auto it = staged.find(container + '\x1f' + p.src.entry);
            if (it != staged.end())
                zs = zip_source_file(out, it->second.c_str(), 0, -1);
        }

        if (!zs) {
            error = "cannot read " + p.src.entry + " from " + container;
            zip_discard(out);
            fs::remove(tmp_path, ec);
            return false;
        }

        zip_int64_t added = zip_file_add(out, p.target_name.c_str(), zs, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE);
        if (added < 0) {
            zip_source_free(zs);
            error = "cannot add " + p.target_name + ": " + zip_strerror(out);
            zip_discard(out);
            fs::remove(tmp_path, ec);
            return false;
        }
        zip_set_file_compression(out, (zip_uint64_t)added, ZIP_CM_DEFLATE, 0);
    }

    if (zip_close(out) != 0) {
        error = std::string("cannot finalize archive: ") + zip_strerror(out);
        zip_discard(out);
        fs::remove(tmp_path, ec);
        return false;
    }

    if (!verify_against_plan(tmp_path, plan, error)) {
        fs::remove(tmp_path, ec);
        return false;
    }

    // Only now is the real destination touched.
    fs::rename(tmp_path, plan.dest_path, ec);
    if (ec) {
        fs::remove(plan.dest_path, ec);
        fs::rename(tmp_path, plan.dest_path, ec);
        if (ec) {
            error = "cannot move the rebuilt archive into place: " + ec.message();
            fs::remove(tmp_path, ec);
            return false;
        }
    }
    return true;
}

// rename(), falling back to copy+delete when inbox and outbox live on different
// filesystems : which is the normal case when ROMs sit on an external drive.
bool move_file(const std::string& from, const std::string& to, std::string& error) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec) return true;

    ec.clear();
    copy_file_robust(from, to, true, ec);
    if (ec) {
        error = "cannot copy to " + to + ": " + ec.message();
        return false;
    }
    fs::remove(from, ec);
    if (ec) {
        error = "copied to " + to + " but the source could not be removed: " + ec.message();
        return false;
    }
    return true;
}

} // namespace

// ── Public helpers ───────────────────────────────────────────────────────────

const char* repair_label(const std::string& repair) {
    if (repair == "swap16")      return N_("byte order swapped (16-bit)");
    if (repair == "swap32")      return N_("byte order swapped (32-bit)");
    if (repair == "halves")      return N_("halves swapped");
    if (repair == "first_half")  return N_("first half kept");
    if (repair == "second_half") return N_("second half kept");
    if (repair == "even")        return N_("even bytes kept");
    if (repair == "odd")         return N_("odd bytes kept");
    if (repair == "mirror")      return N_("mirrored to full size");
    if (repair == "pad00")       return N_("padded with 0x00");
    if (repair == "padff")       return N_("padded with 0xFF");
    if (repair == "trim")        return N_("trimmed to size");
    return "";
}

const char* action_label(Action a) {
    switch (a) {
        case Action::Move:             return "Complete";
        case Action::Rebuild:          return "Rebuild";
        case Action::Incomplete:       return "Incomplete";
        case Action::AlreadyInLibrary: return "In library";
    }
    return "?";
}

// ── analyze ──────────────────────────────────────────────────────────────────

Report analyze(const std::string& inbox_dir,
               const std::string& outbox_dir,
               std::shared_ptr<DatabaseManager> db,
               const Options& options,
               const Callbacks& cb) {
    Report rep;
    rep.outbox_dir = outbox_dir;
    rep.inbox_dir  = inbox_dir;
    rep.options    = options;
    const bool recursive = options.recursive;

    std::error_code ec;
    if (inbox_dir.empty() || !fs::is_directory(inbox_dir, ec)) {
        log(cb, "Inbox directory is not readable: " + inbox_dir);
        return rep;
    }

    // ── 1. Enumerate the inbox ───────────────────────────────────────────────
    report(cb, 2.0, _("Listing inbox…"));
    std::vector<std::string> zip_paths;
    std::vector<std::string> chd_paths;   // disk images : matched by the SHA1 their header declares
    auto classify = [&](const fs::directory_entry& de) {
        if (!de.is_regular_file(ec)) return;
        std::string p = de.path().string();
        if (fs::path(p).filename().string().rfind(".bootcade", 0) == 0) return;  // our own manifests
        if (lower(fs::path(p).extension().string()) == ".chd") { chd_paths.push_back(p); return; }
        // Any archive format libarchive can open is accepted; whether it can
        // actually be read is settled when we try, below. A file that is not
        // an archive at all is still accepted as a loose, single-ROM source // only the handful of extensions that are clearly never ROM data
        // (readmes, cue sheets, patches, checksums…) are skipped, and counted.
        if (RomArchive::looks_like_archive(p)) {
            if (options.include_archives) zip_paths.push_back(p);
        } else if (is_junk_sidecar(p)) {
            rep.ignored.push_back(p);
        } else if (options.include_loose) {
            zip_paths.push_back(p);
        }
    };
    if (recursive) {
        for (auto it = fs::recursive_directory_iterator(inbox_dir, ec);
             it != fs::recursive_directory_iterator(); ++it) {
            if (is_cancelled(cb)) { rep.cancelled = true; return rep; }
            // What a previous run already used is not a source any more.
            if (it->is_directory(ec) && it->path().filename() == kProcessedSubdir) {
                it.disable_recursion_pending();
                continue;
            }
            classify(*it);
        }
    } else {
        for (auto it = fs::directory_iterator(inbox_dir, ec);
             it != fs::directory_iterator(); ++it) {
            if (is_cancelled(cb)) { rep.cancelled = true; return rep; }
            classify(*it);
        }
    }
    std::sort(zip_paths.begin(), zip_paths.end());
    std::sort(chd_paths.begin(), chd_paths.end());

    log(cb, "Inbox: " + std::to_string(zip_paths.size()) + " archive(s)" +
            (chd_paths.empty() ? "" : ", " + std::to_string(chd_paths.size()) + " CHD(s)") +
            (rep.ignored.empty() ? "" : ", " + std::to_string(rep.ignored.size()) + " non-ROM file(s) ignored") + ".");

    if (zip_paths.empty() && chd_paths.empty()) {
        report(cb, 100.0, _("Nothing to analyze."));
        return rep;
    }

    // ── 2. Index the inbox ───────────────────────────────────────────────────
    std::vector<InboxArchive> archives;
    PoolIndex inbox_pool;
    // Every entry of the import folder by its file name, lowercased : a DAT
    // ROM found nowhere by CRC may still sit there under its own name, in
    // another build.
    struct InboxNamed { PieceSource src; unsigned long crc; };
    std::unordered_multimap<std::string, InboxNamed> inbox_by_name;
    // Checksum repairs already tried : "container\x1fentry\x1fcrc" → transform.
    std::unordered_map<std::string, std::string> repairs_tried;
    archives.reserve(zip_paths.size());
    for (size_t i = 0; i < zip_paths.size(); ++i) {
        if (is_cancelled(cb)) { rep.cancelled = true; return rep; }
        double pct = 5.0 + 35.0 * (double)i / (double)zip_paths.size();
        report(cb, pct, _("Reading ") + fs::path(zip_paths[i]).filename().string());

        InboxArchive a;
        a.path = zip_paths[i];

        if (RomArchive::looks_like_archive(a.path)) {
            // An archive with no readable entry is either corrupt or a format
            // libarchive was not built for; either way it is not something we can
            // act on, and saying so is more useful than "no DAT entry matches".
            if (!RomArchive::read_entries(a.path, a.entries) || a.entries.empty()) {
                log(cb, "  cannot read (corrupt or unsupported format): " +
                            fs::path(a.path).filename().string());
                rep.unsupported.push_back(a.path);
                continue;
            }
        } else {
            // Loose file: its one and only "entry" is itself.
            unsigned long crc = 0; uint64_t size = 0;
            if (!compute_file_crc(a.path, crc, size)) {
                log(cb, "  cannot read: " + fs::path(a.path).filename().string());
                rep.unsupported.push_back(a.path);
                continue;
            }
            a.entries.push_back({fs::path(a.path).filename().string(), crc, size});
        }
        for (const auto& e : a.entries) {
            inbox_pool.emplace(e.crc, PieceSource{a.path, e.name, true});
            inbox_by_name.emplace(lower(fs::path(e.name).filename().string()), InboxNamed{{a.path, e.name, true}, e.crc});
        }
        auto& held = rep.inbox_contents[a.path];
        for (const auto& e : a.entries) held.emplace_back(e.name, e.crc);
        archives.push_back(std::move(a));
    }

    // ── 3. Index the existing library (read-only pool) ───────────────────────
    report(cb, 42.0, _("Indexing the existing library…"));
    PoolIndex lib_pool;
    if (options.use_library) {
        std::vector<DatabaseManager::ZipContentRow> rows;
        db->getAllZipContents(rows);

        const fs::path inbox_c  = weak_canonical(inbox_dir);
        const fs::path outbox_c = outbox_dir.empty() ? fs::path() : weak_canonical(outbox_dir);
        std::vector<fs::path> roots_c;
        for (const auto& r : options.roms_paths) if (!r.empty()) roots_c.push_back(weak_canonical(r));

        std::unordered_map<std::string, bool> usable;  // per distinct archive path
        for (const auto& r : rows) {
            auto it = usable.find(r.filepath);
            if (it == usable.end()) {
                fs::path p = weak_canonical(r.filepath);
                // Stale cache rows, plus anything already inside the inbox or the
                // outbox : neither is "the library". Nor is an archive outside
                // every configured ROM directory (a folder since removed).
                bool ok = fs::exists(p, ec) && !is_under(p, inbox_c) &&
                          (outbox_c.empty() || !is_under(p, outbox_c));
                if (ok && !roots_c.empty()) {
                    ok = false;
                    for (const auto& root : roots_c) if (is_under(p, root)) { ok = true; break; }
                }
                it = usable.emplace(r.filepath, ok).first;
            }
            if (it->second) {
                lib_pool.emplace(r.crc, PieceSource{r.filepath, r.entry_name, false});
                rep.library_crcs.insert(r.crc);
            }
        }
    }
    rep.library_pool_empty = options.use_library && lib_pool.empty();
    if (options.use_library) {
        log(cb, "Library pool: " + std::to_string(lib_pool.size()) + " indexed piece(s).");
        if (rep.library_pool_empty)
            log(cb, "  WARNING: empty pool : run a ROM scan first, or sets will look incomplete.");
    } else {
        log(cb, "Library pool: not used (pieces come from the import folder only).");
    }
    log(cb, "Output : every set as its DAT, read with that DAT's merge mode, lays it out.");

    // ── 4. Resolve candidates ────────────────────────────────────────────────
    // What each DAT says its archives hold, read with its own merge mode.
    RomResolve::LayoutBook book(db, options.emulator);
    // The library as the audit sees it : where a set's archive already sits,
    // correct, in its DAT's folder.
    std::unique_ptr<RomResolve::CacheIndex> lib_index;
    if (options.use_library) lib_index = std::make_unique<RomResolve::CacheIndex>(db, options.roms_paths);

    std::unordered_map<std::string, SetPlan> best;  // "name\x1fsystem" → best plan

    // An incomplete set written with the pieces found, when the player asked
    // for it : at least one piece, or there is nothing to write.
    auto make_partial = [&](SetPlan& plan) {
        if (!options.rebuild_incomplete || plan.action != Action::Incomplete) return;
        bool any = false;
        for (const auto& pc : plan.pieces) if (pc.resolved) { any = true; break; }
        if (!any) return;
        plan.action  = Action::Rebuild;
        plan.partial = true;
    };

    // Memo for the content-discovery axis: shared BIOS and PROM dumps make the same
    // CRC come up for archive after archive, and each miss would otherwise be a
    // fresh database round-trip.
    std::unordered_map<unsigned long, std::vector<std::pair<std::string, std::string>>> crc_games;
    auto games_for_crc = [&](unsigned long crc)
        -> const std::vector<std::pair<std::string, std::string>>& {
        auto it = crc_games.find(crc);
        if (it == crc_games.end()) {
            std::vector<std::pair<std::string, std::string>> v;
            for (const auto& g : db->getGamesByRomCrc(crc, options.emulator)) v.emplace_back(g.name, g.system);
            it = crc_games.emplace(crc, std::move(v)).first;
        }
        return it->second;
    };

    // The CRCs a file holds, for the player : one, or the first few of an
    // archive. Deduplicated, because a BIOS repeated in a set says nothing twice.
    auto crc_list_of = [](const InboxArchive& arc) {
        std::vector<unsigned long> seen; std::string out;
        for (const auto& e : arc.entries) {
            if (std::find(seen.begin(), seen.end(), e.crc) != seen.end()) continue;
            seen.push_back(e.crc);
            if (seen.size() > 3) { out += ", …"; break; }
            char hex[16]; snprintf(hex, sizeof(hex), "%08lx", e.crc);
            out += (out.empty() ? "" : ", ") + std::string(hex);
        }
        return out;
    };

    for (size_t ai = 0; ai < archives.size(); ++ai) {
        if (is_cancelled(cb)) { rep.cancelled = true; return rep; }
        const InboxArchive& arc = archives[ai];
        double pct = 45.0 + 53.0 * (double)ai / (double)archives.size();
        report(cb, pct, _("Matching ") + fs::path(arc.path).filename().string());

        // Entries of this archive, by name and by CRC.
        std::unordered_map<std::string, const RomArchive::Entry*> arc_by_name;
        std::unordered_multimap<unsigned long, const RomArchive::Entry*> arc_by_crc;
        for (const auto& e : arc.entries) {
            arc_by_name[e.name] = &e;
            arc_by_crc.emplace(e.crc, &e);
        }

        // Candidate sets, along two axes.
        // `by_rom` : found by a ROM's name alone (axis 3), never by content.
        struct Candidate { std::string name, system; bool by_name; bool by_rom = false; };
        std::vector<Candidate> candidates;
        std::unordered_set<std::string> seen;
        auto add_candidate = [&](const std::string& n, const std::string& s, bool by_name, bool by_rom = false) {
            if (seen.insert(n + '\x1f' + s).second) candidates.push_back({n, s, by_name, by_rom});
        };

        // Axis 1 : the archive's own name.
        std::string stem = fs::path(arc.path).stem().string();
        for (const auto& g : db->getAllGamesWithName(stem, options.emulator)) add_candidate(g.name, g.system, true);

        // Axis 2 : the archive's content, ALWAYS, not merely as a fallback. An
        // archive is not only "the set it is named after": a ZIP called
        // sxevious.zip may also carry the six hack ROMs that its clone hyxevious
        // is short of, and stopping at the name match would silently leave that
        // set broken. It is also what rescues "Metal Slug (1996).zip".
        //
        // FBNeo DATs inline every shared BIOS ROM into each game (all 656
        // ColecoVision sets carry coleco.rom, every Neo Geo set carries the full
        // BIOS), so a CRC common to hundreds of games identifies nothing and would
        // otherwise turn one archive into the entire catalogue. Only CRCs that
        // actually discriminate are used as seeds.
        {
            constexpr size_t kMaxGamesPerSeedCrc = 32;
            std::unordered_set<unsigned long> distinct_crcs;
            for (const auto& e : arc.entries) distinct_crcs.insert(e.crc);
            for (unsigned long crc : distinct_crcs) {
                if (is_cancelled(cb)) { rep.cancelled = true; return rep; }
                const auto& games = games_for_crc(crc);
                if (games.size() > kMaxGamesPerSeedCrc) continue;  // shared BIOS dump
                for (const auto& [gn, gs] : games) add_candidate(gn, gs, false);
            }
        }

        // Axis 3 : neither its name nor its content, but its entries' names
        // and sizes are those of a DAT ROM. Another build of that ROM (a
        // loose 29lv160.u3 that is not the dump the DAT lists) : the set it
        // belongs to is shown incomplete, with both CRCs, rather than the
        // file as unknown. Only when nothing else matched, and only names
        // that point at a handful of sets, since "1.bin" points at hundreds.
        if (candidates.empty()) {
            constexpr size_t kMaxGamesPerRomName = 32;
            for (const auto& e : arc.entries) {
                if (is_cancelled(cb)) { rep.cancelled = true; return rep; }
                const std::string fname = lower(fs::path(e.name).filename().string());
                const auto games = db->getGamesByRomName(fname, options.emulator);
                if (games.size() > kMaxGamesPerRomName) continue;
                for (const auto& g : games) {
                    bool same_size = false;
                    for (const auto& r : g.roms)
                        if (lower(fs::path(r.name).filename().string()) == fname && (uint64_t)r.size == e.size) { same_size = true; break; }
                    if (same_size) add_candidate(g.name, g.system, false, true);
                }
            }
        }

        if (candidates.empty()) {
            {
                std::unordered_set<unsigned long> distinct_crcs;
                for (const auto& e : arc.entries) distinct_crcs.insert(e.crc);
                std::string dbg = "[INBOX-DEBUG] unrecognized " + fs::path(arc.path).filename().string()
                                 + " stem=\"" + stem + "\" entries=" + std::to_string(arc.entries.size());
                for (unsigned long crc : distinct_crcs) {
                    char hex[16]; snprintf(hex, sizeof(hex), "%08lx", crc);
                    dbg += " | crc=" + std::string(hex) + " games=" + std::to_string(games_for_crc(crc).size());
                }
                log(cb, dbg);
            }
            rep.unrecognized.push_back(arc.path);
            rep.unrecognized_crcs.push_back(crc_list_of(arc));
            continue;
        }

        // Some DAT set holds one of this archive's CRCs : the content speaks, so a
        // same-named set that takes nothing from the archive is only a filename
        // coincidence (berserk.zip carrying the MSX berzerk ROM is not the NES
        // berserk). Without a content match, the name is all there is to go on.
        bool content_matched = false;
        for (const auto& c : candidates) if (!c.by_name) { content_matched = true; break; }

        bool produced_any = false;
        bool saw_already_in_library = false; // a content-only match that turned out redundant
        // Content-only matches left incomplete : set aside, and reported only
        // if the archive matched nothing better. A file that belongs to a set
        // still short of a ROM is that set's piece, not an unknown file.
        std::vector<SetPlan> incomplete_only;
        std::string dbg_trace; // built only when this archive ends up unrecognized

        for (const auto& [cand_name, cand_system, by_name, by_rom] : candidates) {
            const Game* found = book.game(cand_name, cand_system);
            std::string folder;
            const DatLayout::Archive* layout = found ? book.archive_for(*found, &folder) : nullptr;
            // A set its DAT expects no archive for (a split clone with no ROM
            // of its own), or only CHDs, which Import never builds.
            if (!layout || layout->entries.empty()) {
                dbg_trace += " | candidate " + cand_name + "/" + cand_system + " -> no archive expected by its DAT";
                continue;
            }
            const Game* owner_set = book.game(layout->name, cand_system);

            SetPlan plan;
            plan.game_name       = layout->name;
            plan.system          = cand_system;
            plan.dat_header      = sanitize_component(folder);
            plan.description     = owner_set ? owner_set->description : found->description;
            plan.trigger_archive = arc.path;

            bool all_resolved       = true;
            bool all_from_trigger   = true;
            std::unordered_set<std::string> used_from_trigger;

            for (const auto& entry : layout->entries) {
                PiecePlan piece;
                piece.target_name = entry.name;
                piece.crc         = entry.crc;
                piece.size        = entry.size;
                piece.owner       = entry.owner;

                auto size_ok = [&](uint64_t got) { return entry.size == 0 || got == entry.size; };

                // 1. right name, right content, already in the trigger archive
                auto byname = arc_by_name.find(entry.name);
                if (byname == arc_by_name.end()) byname = arc_by_name.find(DatLayout::entry_path(entry.name));
                if (byname != arc_by_name.end() && byname->second->crc == entry.crc &&
                    size_ok(byname->second->size)) {
                    piece.src = {arc.path, byname->second->name, true};
                    piece.resolved = true;
                    used_from_trigger.insert(byname->second->name);
                }
                // 2. right content under another name in the trigger archive
                if (!piece.resolved) {
                    auto range = arc_by_crc.equal_range(entry.crc);
                    for (auto it = range.first; it != range.second; ++it) {
                        if (!size_ok(it->second->size)) continue;
                        piece.src = {arc.path, it->second->name, true};
                        piece.resolved = true;
                        plan.renamed_entries++;
                        used_from_trigger.insert(it->second->name);
                        break;
                    }
                }
                // 3. elsewhere in the inbox
                if (!piece.resolved) {
                    auto range = inbox_pool.equal_range(entry.crc);
                    if (range.first != range.second) {
                        piece.src = range.first->second;
                        piece.resolved = true;
                        all_from_trigger = false;
                    }
                }
                // 4. the existing library, read-only
                if (!piece.resolved) {
                    auto range = lib_pool.equal_range(entry.crc);
                    if (range.first != range.second) {
                        piece.src = range.first->second;
                        piece.resolved = true;
                        all_from_trigger = false;
                        plan.pieces_from_library++;
                    }
                }
                if (!piece.resolved) {
                    // Found nowhere by its CRC : a file of the import folder
                    // under its name is another build of it, worth both CRCs.
                    // The trigger archive's own entry first.
                    if (byname != arc_by_name.end()) {
                        piece.wrong     = true;
                        piece.wrong_crc = byname->second->crc;
                        piece.wrong_src = {arc.path, byname->second->name, true};
                    } else {
                        auto range = inbox_by_name.equal_range(lower(fs::path(entry.name).filename().string()));
                        if (range.first != range.second) {
                            piece.wrong     = true;
                            piece.wrong_crc = range.first->second.crc;
                            piece.wrong_src = range.first->second.src;
                        }
                    }
                }
                // That other build may be the dump itself, transformed.
                if (!piece.resolved && piece.wrong) {
                    char hex[16]; snprintf(hex, sizeof(hex), "%08lx", entry.crc);
                    const std::string key = piece.wrong_src.container + '\x1f' + piece.wrong_src.entry + '\x1f' + hex;
                    auto tried = repairs_tried.find(key);
                    if (tried == repairs_tried.end()) {
                        Bytes data; std::string err;
                        std::string found = read_bytes(piece.wrong_src.container, piece.wrong_src.entry, data, err)
                                          ? find_repair(data, entry.crc, entry.size) : std::string();
                        if (!err.empty()) log(cb, "  checksum repair : " + err);
                        if (!found.empty())
                            log(cb, "  " + piece.wrong_src.entry + " → " + entry.name + " : repaired by " + repair_label(found));
                        tried = repairs_tried.emplace(key, found).first;
                    }
                    if (!tried->second.empty()) {
                        piece.repair   = tried->second;
                        piece.src      = piece.wrong_src;
                        piece.resolved = true;
                        plan.repaired_entries++;
                        all_from_trigger = false;
                        if (piece.src.container == arc.path) used_from_trigger.insert(piece.src.entry);
                    }
                }
                if (!piece.resolved) {
                    all_resolved = false;
                    plan.missing.push_back({entry.name, entry.crc, entry.size});
                }
                plan.pieces.push_back(std::move(piece));
            }

            if (by_name && content_matched && used_from_trigger.empty()) {
                dbg_trace += " | candidate " + cand_name + "/" + cand_system +
                             " -> name only, the archive holds none of its ROMs";
                continue;
            }

            for (const auto& e : arc.entries)
                if (!used_from_trigger.count(e.name)) plan.extra_entries.push_back({e.name, e.crc, e.size});

            // Does the library already hold this archive, exactly as its DAT
            // lays it out : named after its set, in its DAT's folder, every
            // entry under its name with its CRC? Answered on content rather
            // than on a stored status, which can be stale.
            bool library_has_set = false;
            if (lib_index)
                for (const RomResolve::Archive* cand : lib_index->in_folder(folder, layout->name)) {
                    bool all = true;
                    for (const auto& entry : layout->entries)
                        if (RomResolve::probe_rom(cand, entry.name, entry.crc) != RomResolve::RomState::Present) { all = false; break; }
                    if (all) { library_has_set = true; break; }
                }

            if (library_has_set) {
                plan.action = Action::AlreadyInLibrary;
            } else if (!all_resolved) {
                plan.action = Action::Incomplete;
            } else if (all_from_trigger && plan.renamed_entries == 0 && plan.extra_entries.empty()
                       && RomArchive::is_zip(arc.path)) {
                // A perfect set can only be relocated as-is when it already is a
                // ZIP. A 7z or rar always has to be rewritten, because that is the
                // only container the emulators load.
                plan.action = Action::Move;
            } else {
                plan.action = Action::Rebuild;
            }
            // Normalising: a set that is fine as it sits is rewritten anyway
            // (DAT entry names, deflate, nothing the DAT does not list).
            if (options.rebuild_correct && plan.action == Action::Move)
                plan.action = Action::Rebuild;

            // Content discovery casts a wide net: a single shared PROM links an
            // archive to dozens of unrelated sets. Those are only worth reporting
            // when this archive actually improves them : otherwise the list fills
            // with "already in library" noise about games the user never asked
            // about. Sets matched by filename are always reported, since that is
            // the archive the user deliberately downloaded.
            // RomVault's partial fix : what was found is written all the same.
            // Only for the set the archive is named after ; a content match
            // left incomplete waits below, so a shared PROM does not write
            // dozens of sets it is a sliver of.
            if (by_name) make_partial(plan);
            bool actionable = (plan.action == Action::Move || plan.action == Action::Rebuild);
            if (!by_name && !actionable) {
                const char* an = plan.action == Action::AlreadyInLibrary ? "AlreadyInLibrary"
                                : plan.action == Action::Incomplete       ? "Incomplete" : "?";
                dbg_trace += " | candidate " + cand_name + "/" + cand_system +
                             " -> action=" + an + " by_name=0, skipped as non-actionable content-only match";
                // A duplicate only when the content says so : a ROM name is
                // shared by several boards (29lv160.u3), and another set
                // complete in the library proves nothing about this file.
                if (plan.action == Action::AlreadyInLibrary && !by_rom) saw_already_in_library = true;
                if (plan.action == Action::Incomplete) incomplete_only.push_back(std::move(plan));
                continue;
            }

            plan.dest_path = (fs::path(outbox_dir) / plan.dat_header /
                              (sanitize_component(plan.game_name) + ".zip")).string();
            plan.selected = actionable;

            produced_any = true;
            std::string key = plan.game_name + '\x1f' + plan.system;
            auto it = best.find(key);
            if (it == best.end() || action_rank(plan.action) > action_rank(it->second.action))
                best[key] = std::move(plan);
        }

        if (!produced_any && !saw_already_in_library && !incomplete_only.empty()) {
            // The sets this archive gives the most of, from the import folder :
            // a shared PROM also feeds sets it is a sliver of. A piece found
            // by its CRC outweighs any number found by name only : a ROM name
            // like 29lv160.u3 is shared by several boards, the dumps are not.
            auto from_inbox = [](const SetPlan& p) {
                int n = 0;
                for (const auto& pc : p.pieces) n += (pc.resolved && pc.src.from_inbox) ? 1000 : pc.wrong ? 1 : 0;
                return n;
            };
            int top = 0;
            for (const auto& p : incomplete_only) top = std::max(top, from_inbox(p));
            for (auto& plan : incomplete_only) {
                if (from_inbox(plan) < top) continue;
                make_partial(plan);
                plan.dest_path = (fs::path(outbox_dir) / plan.dat_header /
                                  (sanitize_component(plan.game_name) + ".zip")).string();
                plan.selected = plan.partial;
                std::string key = plan.game_name + '\x1f' + plan.system;
                if (!best.count(key)) best[key] = std::move(plan);
                produced_any = true;
            }
        }

        if (!produced_any) {
            if (saw_already_in_library) {
                rep.already_have.push_back(arc.path);
            } else {
                std::string cand_list;
                for (const auto& [n, s, bn, br] : candidates)
                    cand_list += " " + n + "/" + s + (bn ? "(name)" : "(crc)");
                log(cb, "[INBOX-DEBUG] unrecognized " + fs::path(arc.path).filename().string() +
                        " stem=\"" + stem + "\" candidates:" + cand_list + dbg_trace);
                rep.unrecognized.push_back(arc.path);
                rep.unrecognized_crcs.push_back(crc_list_of(arc));
            }
        }
    }

    for (auto& [_, plan] : best) rep.sets.push_back(std::move(plan));

    // ── 5. CHDs : the disks the DATs list, by the SHA1 of their header ───────
    // Every disk every DAT expects, where its layout puts it (the folder of
    // its set's archive, the parent's in merged). One CHD can be expected in
    // several places (two DATs, or a clone keeping its parent's disk in a
    // non-merged DAT) : one plan each, the first taking the file and the
    // next ones copying it (apply()).
    if (!chd_paths.empty()) {
        report(cb, 98.0, _("Matching CHDs…"));
        struct DiskTarget { std::string folder, set, system, description; DatLayout::DiskEntry disk; };
        std::unordered_map<std::string, std::vector<DiskTarget>> by_sha1;
        std::unordered_set<std::string> seen_target;
        for (const auto& g : book.games()) {
            if (g.disks.empty()) continue;
            std::string folder;
            const DatLayout::Archive* arc = book.archive_for(g, &folder);
            if (!arc) continue;
            for (size_t di : book.layout_of(g.dat_source).disks_of(g.name)) {
                const DatLayout::DiskEntry& d = arc->disks[di];
                if (!seen_target.insert(folder + '\x1f' + arc->name + '\x1f' + d.name).second) continue;
                const Game* owner = book.game(arc->name, g.system);
                by_sha1[d.sha1].push_back({folder, arc->name, g.system, owner ? owner->description : g.description, d});
            }
        }
        for (const auto& path : chd_paths) {
            if (is_cancelled(cb)) { rep.cancelled = true; return rep; }
            const std::string sha1 = RomResolve::chd_header_sha1(path);
            if (sha1.empty()) {
                log(cb, "  not a readable CHD (v3, v4 or v5 header): " + fs::path(path).filename().string());
                rep.unsupported.push_back(path);
                continue;
            }
            auto hit = by_sha1.find(sha1);
            if (hit == by_sha1.end()) {
                rep.unrecognized.push_back(path);
                rep.unrecognized_crcs.push_back("SHA1 " + sha1);
                continue;
            }
            for (const auto& t : hit->second) {
                SetPlan plan;
                plan.disk            = true;
                plan.game_name       = t.set;
                plan.system          = t.system;
                plan.dat_header      = sanitize_component(t.folder);
                plan.description     = t.description;
                plan.trigger_archive = path;
                plan.dest_path       = (fs::path(outbox_dir) / plan.dat_header / sanitize_component(t.set)
                                        / (t.disk.name + ".chd")).string();
                PiecePlan piece;
                piece.target_name = t.disk.name + ".chd";
                piece.sha1        = sha1;
                piece.size        = (uint64_t)fs::file_size(path, ec);
                piece.src         = {path, fs::path(path).filename().string(), true};
                piece.resolved    = true;
                plan.pieces.push_back(std::move(piece));
                if (fs::path(path).stem().string() != t.disk.name) plan.renamed_entries = 1;
                const bool in_library = options.use_library &&
                    RomResolve::evaluate_layout_disks(t.set, {t.disk}, options.roms_paths, t.folder).status == "available";
                plan.action   = in_library ? Action::AlreadyInLibrary : Action::Move;
                plan.selected = !in_library;
                log(cb, "  CHD " + fs::path(path).filename().string() + " → " + t.folder + "/" + t.set + "/"
                        + t.disk.name + ".chd" + (in_library ? " (already in the library)" : ""));
                rep.sets.push_back(std::move(plan));
            }
        }
    }
    std::sort(rep.sets.begin(), rep.sets.end(), [](const SetPlan& a, const SetPlan& b) {
        if (a.system != b.system) return a.system < b.system;
        return a.game_name < b.game_name;
    });

    for (const auto& s : rep.sets) {
        switch (s.action) {
            case Action::Move:             rep.complete++;   break;
            case Action::Rebuild:          rep.fixable++;    break;
            case Action::Incomplete:       rep.incomplete++; break;
            case Action::AlreadyInLibrary: rep.already++;    break;
        }
    }

    report(cb, 100.0, _("Analysis complete."));
    log(cb, "Result: " + std::to_string(rep.complete) + " complete, " +
                std::to_string(rep.fixable) + " to rebuild, " +
                std::to_string(rep.incomplete) + " incomplete, " +
                std::to_string(rep.already) + " already in library, " +
                std::to_string(rep.unrecognized.size()) + " unrecognized.");
    return rep;
}

// ── apply ────────────────────────────────────────────────────────────────────

ApplyResult apply(const Report& report_in, const Callbacks& cb) {
    ApplyResult res;
    std::error_code ec;
    const Options& options = report_in.options;

    std::vector<const SetPlan*> todo;
    for (const auto& s : report_in.sets) {
        if (!s.selected) continue;
        if (s.action != Action::Move && s.action != Action::Rebuild) continue;
        todo.push_back(&s);
    }

    // Rebuilds run before moves. A parent set is frequently a perfect Move while
    // its clones are Rebuilds sourcing pieces from that same archive; moving the
    // parent out of the inbox first would pull the ground from under them.
    // (Relocations are also tracked below, so either order stays correct.)
    std::stable_partition(todo.begin(), todo.end(),
                          [](const SetPlan* s) { return s->action == Action::Rebuild; });
    // Nothing to build is not nothing to do: the rejects below still leave.
    if (todo.empty()) log(cb, "No set selected to move or rebuild.");

    // (archive, entry) pairs actually consumed, so an inbox archive is only deleted
    // once every single one of its entries has landed somewhere. Anything holding
    // unclaimed data is left alone.
    std::unordered_set<std::string> consumed_entries;
    // The content those sets now hold : an identical copy elsewhere in the
    // import folder (the same zip downloaded twice, then unpacked) has
    // nothing left to give either.
    std::unordered_set<unsigned long> written_crcs;
    std::unordered_set<std::string> moved_archives;
    Relocations relocated;

    // What each produced file is and where it came from, written next to the
    // files themselves so the Outbox can still explain them next week.
    RomManifest::Manifest manifest = RomManifest::Manifest::load(report_in.outbox_dir);
    auto describe = [&](const SetPlan& plan, const char* action) {
        RomManifest::Entry e;
        e.file       = manifest.relative(plan.dest_path);
        e.game       = plan.game_name;
        e.system     = plan.system;
        e.dat_header = plan.dat_header;
        e.origin     = plan.trigger_archive;
        e.action     = action;
        if (plan.action == Action::Move) {
            e.details.push_back("complete set, relocated as-is");
        } else {
            int from_import = 0;
            std::vector<std::string> library_sources;
            for (const auto& p : plan.pieces) {
                if (!p.resolved) continue;
                if (p.src.from_inbox) { ++from_import; continue; }
                std::string name = fs::path(p.src.container).filename().string();
                if (std::find(library_sources.begin(), library_sources.end(), name) == library_sources.end())
                    library_sources.push_back(name);
            }
            size_t written = 0;
            for (const auto& p : plan.pieces) if (p.resolved) ++written;
            e.details.push_back("rebuilt from " + std::to_string(written) + " piece(s)");
            if (from_import)
                e.details.push_back(std::to_string(from_import) + " from the import folder");
            if (plan.pieces_from_library) {
                std::string line = std::to_string(plan.pieces_from_library) + " from the library: ";
                for (size_t k = 0; k < library_sources.size() && k < 5; ++k)
                    line += (k ? ", " : "") + library_sources[k];
                if (library_sources.size() > 5) line += ", …";
                e.details.push_back(line);
            }
            if (plan.renamed_entries)
                e.details.push_back(std::to_string(plan.renamed_entries) + " entry(ies) renamed to the DAT name");
            for (const auto& p : plan.pieces)
                if (!p.repair.empty())
                    e.details.push_back(p.target_name + ": repaired (" + repair_label(p.repair) + ")");
            if (plan.partial)
                e.details.push_back("incomplete : " + std::to_string(plan.missing.size()) + " ROM(s) still missing");
            if (!plan.extra_entries.empty()) {
                // A left-out entry named like a missing piece is a WRONG BUILD,
                // not a surplus : said so, with both numbers, or the player
                // cannot tell a stray file from the very ROM they were after.
                int wrong = 0, extra = 0;
                for (const auto& x : plan.extra_entries) {
                    bool named_like_missing = false;
                    for (const auto& pc : plan.pieces)
                        if (!pc.resolved && lower(pc.target_name) == lower(x.name)) {
                            char f[16], w[16]; snprintf(f, sizeof(f), "%08lx", x.crc); snprintf(w, sizeof(w), "%08lx", pc.crc);
                            e.details.push_back(x.name + ": wrong CRC (found " + f + ", expected " + w + ")");
                            named_like_missing = true; break;
                        }
                    if (named_like_missing) wrong++; else extra++;
                }
                if (extra) e.details.push_back(std::to_string(extra) + " entry(ies) not needed by the DAT left out");
                (void)wrong;
            }
            if (!RomArchive::is_zip(plan.trigger_archive))
                e.details.push_back("converted to ZIP from " + fs::path(plan.trigger_archive).extension().string());
        }
        manifest.add(std::move(e));
    };

    for (size_t i = 0; i < todo.size(); ++i) {
        if (is_cancelled(cb)) { res.cancelled = true; break; }
        const SetPlan& plan = *todo[i];
        double pct = 100.0 * (double)i / (double)todo.size();
        report(cb, pct, plan.game_name + " (" + plan.system + ")");

        fs::create_directories(fs::path(plan.dest_path).parent_path(), ec);
        if (ec) {
            res.failed++;
            res.errors.push_back(plan.game_name + ": cannot create the destination folder: " + ec.message());
            ec.clear();
            continue;
        }

        std::string error;
        if (plan.action == Action::Move) {
            // The same archive can be a perfect set for two DATs at once (every
            // Neo Geo game is also in the Arcade DAT). The first Move takes the
            // file; the next ones copy it from where it landed.
            auto already = relocated.find(plan.trigger_archive);
            bool ok;
            if (already != relocated.end()) {
                std::error_code cec;
                copy_file_robust(already->second, plan.dest_path, true, cec);
                ok = !cec;
                if (!ok) error = "cannot copy from " + already->second + ": " + cec.message();
            } else {
                ok = move_file(plan.trigger_archive, plan.dest_path, error);
            }
            if (ok) {
                res.moved++;
                moved_archives.insert(plan.trigger_archive);
                for (const auto& p : plan.pieces) if (p.resolved) written_crcs.insert(p.crc);
                if (already == relocated.end()) relocated[plan.trigger_archive] = plan.dest_path;
                describe(plan, RomManifest::action::Moved);
                log(cb, "moved   " + plan.game_name + " → " + plan.dat_header);
            } else {
                res.failed++;
                res.errors.push_back(plan.game_name + ": " + error);
                log(cb, "FAILED  " + plan.game_name + ": " + error);
            }
        } else {
            if (rebuild_set(plan, relocated, error)) {
                res.rebuilt++;
                describe(plan, RomManifest::action::Rebuilt);
                // Every piece the plan resolved counts as used.
                for (const auto& p : plan.pieces)
                    if (p.resolved)
                    {
                        consumed_entries.insert(p.src.container + '\x1f' + p.src.entry);
                        written_crcs.insert(p.crc);
                    }
                std::string extra = plan.pieces_from_library
                                        ? " (" + std::to_string(plan.pieces_from_library) + " from library)"
                                        : "";
                log(cb, "rebuilt " + plan.game_name + " → " + plan.dat_header + extra);
            } else {
                res.failed++;
                res.errors.push_back(plan.game_name + ": " + error);
                log(cb, "FAILED  " + plan.game_name + ": " + error);
            }
        }
    }

    // Recorded even after a cancel: the files that did land are real.
    if (res.moved + res.rebuilt > 0 && !manifest.save())
        log(cb, "WARNING: could not write the outbox manifest (" + std::string(RomManifest::kFileName) + ")");

    // A source every piece of which has been used is done with : deleted,
    // parked under _processed (keeping its place relative to the inbox), or
    // left alone, as the user chose.
    auto retire_source = [&](const std::string& archive, const char* verb) -> bool {
        std::error_code rec;
        switch (options.processed) {
            case Options::Processed::Keep:
                log(cb, std::string(verb) + " " + fs::path(archive).filename().string() + " (kept)");
                return true;
            case Options::Processed::Delete:
                fs::remove(archive, rec);
                if (rec) { log(cb, "could not delete " + archive + ": " + rec.message()); return false; }
                log(cb, std::string(verb) + " " + fs::path(archive).filename().string() + " (deleted)");
                return true;
            default: {
                fs::path rel = fs::relative(archive, report_in.inbox_dir, rec);
                if (rec || rel.empty() || *rel.begin() == "..") rel = fs::path(archive).filename();
                fs::path dest = fs::path(report_in.inbox_dir) / kProcessedSubdir / rel;
                fs::create_directories(dest.parent_path(), rec);
                std::string err;
                if (!move_file(archive, dest.string(), err)) { log(cb, "could not park " + archive + ": " + err); return false; }
                log(cb, std::string(verb) + " " + fs::path(archive).filename().string() + " → " + kProcessedSubdir + "/");
                return true;
            }
        }
    };

    // Delete inbox archives whose every entry was consumed by a successful rebuild.
    if (!res.cancelled) {
        report(cb, 99.0, _("Cleaning up the inbox…"));
        std::unordered_map<std::string, std::vector<std::string>> inbox_entries;
        auto candidate = [&](const std::string& archive) {
            if (moved_archives.count(archive) || inbox_entries.count(archive)) return;
            std::vector<std::string> names;
            if (!RomArchive::looks_like_archive(archive))
                names.push_back(fs::path(archive).filename().string());
            else if (!RomArchive::list_names(archive, names))
                return;
            inbox_entries[archive] = std::move(names);
        };
        // An entry the library already holds, or this run just wrote, by content.
        auto in_library = [&](const std::string& archive, const std::string& entry) {
            auto it = report_in.inbox_contents.find(archive);
            if (it == report_in.inbox_contents.end()) return false;
            for (const auto& [name, crc] : it->second)
                if (name == entry) return report_in.library_crcs.count(crc) > 0 || written_crcs.count(crc) > 0;
            return false;
        };
        // Every other import file that copies what was just written : the
        // file the analysis listed is one of them, the others got no row of
        // their own. One written entry at least, so a file the library
        // merely already holds stays a duplicate for the quarantine below.
        {
            std::unordered_set<std::string> rejected(report_in.already_have.begin(), report_in.already_have.end());
            rejected.insert(report_in.unrecognized.begin(), report_in.unrecognized.end());
            for (const auto& [archive, held] : report_in.inbox_contents) {
                if (rejected.count(archive)) continue;
                bool wrote_any = false;
                for (const auto& e : held) if (written_crcs.count(e.second)) { wrote_any = true; break; }
                if (wrote_any) candidate(archive);
            }
        }
        // The archive that surfaced each set, and every import file a rebuilt
        // set took a piece from : a loose ROM that completed a set is as used
        // as the archive named after it.
        std::unordered_set<std::string> applied;
        for (const SetPlan* s : todo) applied.insert(s->game_name + '\x1f' + s->system);
        // A file another set still draws on stays, whatever this run took
        // from it : that set was left unselected, incomplete or failed.
        std::unordered_set<std::string> still_needed;
        for (const auto& s : report_in.sets) {
            // A set the library already has asks nothing of any file.
            if (s.action == Action::AlreadyInLibrary) continue;
            const bool done = applied.count(s.game_name + '\x1f' + s.system) &&
                              std::none_of(res.errors.begin(), res.errors.end(), [&](const std::string& e) {
                                  return e.rfind(s.game_name + ": ", 0) == 0; });
            for (const auto& p : s.pieces) {
                const bool from_inbox = p.resolved && p.src.from_inbox;
                if (done) { if (from_inbox) candidate(p.src.container); continue; }
                if (from_inbox) still_needed.insert(p.src.container);
                if (p.wrong && p.wrong_src.from_inbox) still_needed.insert(p.wrong_src.container);
            }
            if (!done) { still_needed.insert(s.trigger_archive); continue; }
            candidate(s.trigger_archive);
        }
        for (const auto& n : still_needed) inbox_entries.erase(n);
        for (const auto& [archive, names] : inbox_entries) {
            // Every entry used, or already in the library : what is left of
            // the file is a copy of something the collection holds.
            bool fully_consumed = !names.empty();
            for (const auto& n : names) {
                if (consumed_entries.count(archive + '\x1f' + n) || in_library(archive, n)) continue;
                fully_consumed = false;
                break;
            }
            if (!fully_consumed) continue;
            if (retire_source(archive, "consumed")) res.consumed_archives++;
        }
    }

    // ── Rejects : what the analysis could do nothing with ────────────────────
    // Moved out of the inbox into the quarantine, each with its reason, so
    // the inbox only ever holds what still needs a decision.
    if (!res.cancelled && options.quarantine_rejects && !options.quarantine_dir.empty()) {
        RomManifest::Manifest qm = RomManifest::Manifest::load(options.quarantine_dir);
        fs::path dest_dir = fs::path(options.quarantine_dir) / kRejectedSubdir;
        fs::create_directories(dest_dir, ec);
        auto reject = [&](const std::string& path, const char* reason, const char* why) {
            fs::path src(path);
            // Deja en quarantaine sous ce nom : le meme fichier n'a plus a
            // rester dans l'import, un autre prend un nom libre a cote.
            bool same = false;
            fs::path dest = RomManifest::free_destination(dest_dir.string(), src.string(), same);
            if (same) {
                std::error_code rec;
                fs::remove(src, rec);
                if (!rec) {
                    res.quarantined++;
                    log(cb, src.filename().string() + " : the same file is already in quarantine, copy removed");
                }
                return;
            }
            std::string err;
            if (!move_file(src.string(), dest.string(), err)) {
                log(cb, "could not quarantine " + src.filename().string() + ": " + err);
                return;
            }
            RomManifest::Entry e;
            e.file   = qm.relative(dest.string());
            e.game   = src.stem().string();
            e.reason = reason;
            e.origin = path;
            e.action = RomManifest::action::Moved;
            e.details.push_back(why);
            qm.add(std::move(e));
            res.quarantined++;
            log(cb, std::string("quarantined (") + reason + ") " + src.filename().string());
        };
        for (const auto& p : report_in.unrecognized)
            reject(p, RomManifest::reason::Unknown, "recognised by neither name nor content in the DAT group");
        for (const auto& p : report_in.unsupported)
            reject(p, RomManifest::reason::Unsupported, "no reader could open this archive");
        // A duplicate is an archive the library already holds as a complete
        // set and that can build nothing else : the ones matched by content
        // only are listed in already_have, the ones matched by name sit in
        // `sets` with that action. A same-named set of another system that
        // this archive leaves incomplete does not make it any more useful.
        std::unordered_set<std::string> duplicates(report_in.already_have.begin(), report_in.already_have.end());
        {
            struct Seen { bool already = false, useful = false; };
            std::unordered_map<std::string, Seen> by_trigger;
            for (const auto& s : report_in.sets) {
                Seen& v = by_trigger[s.trigger_archive];
                if (s.action == Action::AlreadyInLibrary) v.already = true;
                if (s.action == Action::Move || s.action == Action::Rebuild) v.useful = true;
            }
            for (const auto& [archive, v] : by_trigger)
                if (v.already && !v.useful && !moved_archives.count(archive) && fs::exists(archive, ec))
                    duplicates.insert(archive);
        }
        for (const auto& p : duplicates)
            reject(p, RomManifest::reason::Duplicate, "the library already holds this set, complete and correct");
        if (res.quarantined > 0 && !qm.save())
            log(cb, "WARNING: could not write the quarantine manifest");
    }

    report(cb, 100.0, _("Done."));
    log(cb, "Applied: " + std::to_string(res.moved) + " moved, " +
                std::to_string(res.rebuilt) + " rebuilt, " +
                std::to_string(res.failed) + " failed, " +
                std::to_string(res.consumed_archives) + " source archive(s) consumed.");
    return res;
}

} // namespace RomInbox
