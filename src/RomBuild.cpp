// src/RomBuild.cpp
#include "RomBuild.h"
#include "FileCopy.h"
#include "RomArchive.h"
#include "RomScanner.h"
#include "TorrentZip.h"

#include <archive.h>
#include <archive_entry.h>
#include <zip.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <unordered_map>

namespace fs = std::filesystem;

namespace RomBuild {
namespace {

const char* const kRepairs[] = {
    "swap16", "swap32", "halves", "first_half", "second_half",
    "even", "odd", "mirror", "pad00", "padff", "trim",
};

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

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

// Temporary directory holding entries pulled out of non-ZIP sources, removed
// whatever the outcome. libzip needs the files to survive until zip_close().
struct Staging {
    fs::path dir;
    explicit Staging(const std::string& base) : dir(base + ".staging") {}
    ~Staging() { std::error_code ec; fs::remove_all(dir, ec); }
    Staging(const Staging&) = delete;
    Staging& operator=(const Staging&) = delete;
};

// The pieces that are not entries of a ZIP nor loose files, drained into
// `staging` : one pass per source archive, and checksum repairs written out
// once transformed and checked. `staged` maps a piece's index to its file.
bool stage_pieces(const std::vector<Piece>& pieces, Staging& staging,
                  std::unordered_map<size_t, std::string>& staged, std::string& error) {
    std::error_code ec;
    // 7z and rar cannot be streamed entry-by-entry into a ZIP the way libzip
    // streams from another ZIP, and 7z's solid blocks make random access
    // pathological : pulling N entries one at a time re-decompresses the whole
    // block N times. So each non-ZIP source is drained once, in a single pass.
    std::unordered_map<std::string, std::vector<std::string>> by_container;
    for (const auto& p : pieces) {
        if (!p.repair.empty()) continue;
        // A loose file needs no extraction step at all : it *is* its own
        // one and only piece, read straight off disk.
        if (!RomArchive::is_zip(p.container) && RomArchive::looks_like_archive(p.container))
            by_container[p.container].push_back(p.entry);
    }
    // Each source gets its own staging subdirectory: extract() numbers its
    // output files from zero, so sharing one directory between two archives
    // would silently overwrite the first one's pieces with the second's.
    std::unordered_map<std::string, std::string> files_of;   // "container\x1fentry" → file
    size_t slot = 0;
    for (const auto& [container, entries] : by_container) {
        std::string dir = (staging.dir / std::to_string(slot++)).string();
        std::unordered_map<std::string, std::string> files;
        if (!RomArchive::extract(container, entries, dir, files, error))
            return false;
        for (auto& [entry, file] : files)
            files_of[container + '\x1f' + entry] = file;
    }

    size_t repaired_slot = 0;
    for (size_t i = 0; i < pieces.size(); ++i) {
        const Piece& p = pieces[i];
        if (!p.repair.empty()) {
            // Transformed, then checked once more : only the DAT's dump is
            // ever written under its name.
            Bytes data, fixed;
            if (!read_bytes(p.container, p.entry, data, error)) return false;
            if (!transform(p.repair, data, p.size, fixed) || crc_of(fixed) != p.crc) {
                error = "checksum repair of " + p.entry + " no longer gives the DAT's CRC";
                return false;
            }
            fs::create_directories(staging.dir, ec);
            const fs::path file = staging.dir / ("repaired-" + std::to_string(repaired_slot++));
            std::ofstream o(file, std::ios::binary);
            o.write(reinterpret_cast<const char*>(fixed.data()), (std::streamsize)fixed.size());
            o.close();
            if (!o) { error = "cannot write " + file.string(); return false; }
            staged[i] = file.string();
            continue;
        }
        auto it = files_of.find(p.container + '\x1f' + p.entry);
        if (it != files_of.end()) staged[i] = it->second;
    }
    return true;
}

using Cancelled = std::function<bool()>;

bool asked_to_stop(const Cancelled& cancelled) { return cancelled && cancelled(); }

bool write_zip(const std::string& tmp_path, const std::vector<Piece>& pieces, std::string& error,
               const Cancelled& cancelled = {}) {
    std::error_code ec;
    Staging staging(tmp_path);
    std::unordered_map<size_t, std::string> staged;
    if (!stage_pieces(pieces, staging, staged, error)) return false;
    if (asked_to_stop(cancelled)) { error = "cancelled"; return false; }

    SourceArchives sources;

    int zerr = 0;
    zip_t* out = zip_open(tmp_path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &zerr);
    if (!out) {
        error = "cannot create " + tmp_path;
        return false;
    }
    // libzip writes everything at zip_close() : asked there too, between
    // two blocks, so a big set does not hold a Cancel for minutes.
    if (cancelled)
        zip_register_cancel_callback_with_state(out, [](zip_t*, void* state) {
            return (*static_cast<const Cancelled*>(state))() ? 1 : 0;
        }, nullptr, const_cast<Cancelled*>(&cancelled));

    for (size_t i = 0; i < pieces.size(); ++i) {
        const Piece& p = pieces[i];
        zip_source_t* zs = nullptr;

        auto st = staged.find(i);
        if (p.container.empty()) {
            // An empty ROM : nothing to read it from, nothing to write.
            zs = zip_source_buffer(out, nullptr, 0, 0);
        } else if (st != staged.end()) {
            zs = zip_source_file(out, st->second.c_str(), 0, -1);
        } else if (RomArchive::is_zip(p.container)) {
            zip_t* src = nullptr;
            zip_int64_t src_idx = 0;
            if (!sources.locate(p.container, p.entry, src, src_idx)) {
                error = "source vanished: " + p.entry + " in " + p.container;
                zip_discard(out);
                fs::remove(tmp_path, ec);
                return false;
            }
            // flags = 0 → libzip decompresses the source and recompresses on write,
            // so the output is uniformly deflated whatever the source archive used.
            // It streams, so a 700 MB Neo Geo set never has to fit in memory.
            zs = FBNEO_ZIP_SOURCE_FROM_ZIP(out, src, (zip_uint64_t)src_idx);
        } else if (!RomArchive::looks_like_archive(p.container)) {
            // Loose file: the piece's bytes are the whole file, nothing to stage.
            zs = zip_source_file(out, p.container.c_str(), 0, -1);
        }

        if (!zs) {
            error = "cannot read " + p.entry + " from " + p.container;
            zip_discard(out);
            fs::remove(tmp_path, ec);
            return false;
        }

        zip_int64_t added = zip_file_add(out, p.name.c_str(), zs, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE);
        if (added < 0) {
            zip_source_free(zs);
            error = "cannot add " + p.name + ": " + zip_strerror(out);
            zip_discard(out);
            fs::remove(tmp_path, ec);
            return false;
        }
        zip_set_file_compression(out, (zip_uint64_t)added, ZIP_CM_DEFLATE, 0);
    }

    if (zip_close(out) != 0) {
        error = asked_to_stop(cancelled) ? std::string("cancelled") : std::string("cannot finalize archive: ") + zip_strerror(out);
        zip_discard(out);
        fs::remove(tmp_path, ec);
        return false;
    }
    return true;
}

// Copies everything `read` gives into the current 7z entry.
template <typename Read>
bool pump(archive* out, Read read, std::string& error) {
    std::vector<char> buf(1 << 16);
    for (;;) {
        const int64_t n = read(buf.data(), buf.size());
        if (n < 0) { error = "read error"; return false; }
        if (n == 0) return true;
        if (archive_write_data(out, buf.data(), (size_t)n) != n) {
            error = std::string("cannot write: ") + archive_error_string(out);
            return false;
        }
    }
}

bool write_7z(const std::string& tmp_path, const std::vector<Piece>& pieces, std::string& error,
              const Cancelled& cancelled = {}) {
    std::error_code ec;
    Staging staging(tmp_path);
    std::unordered_map<size_t, std::string> staged;
    if (!stage_pieces(pieces, staging, staged, error)) return false;

    // Sorted by name ignoring case, as a TorrentZip is : the same set gives
    // the same listing whatever order its pieces were found in.
    std::vector<size_t> order(pieces.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const std::string la = lower(pieces[a].name), lb = lower(pieces[b].name);
        return la != lb ? la < lb : pieces[a].name < pieces[b].name;
    });

    archive* out = archive_write_new();
    archive_write_set_format_7zip(out);
    // LZMA2 where libarchive was built with it ; its default (LZMA) otherwise.
    archive_write_set_format_option(out, "7zip", "compression", "lzma2");
    if (archive_write_open_filename(out, tmp_path.c_str()) != ARCHIVE_OK) {
        error = std::string("cannot create ") + tmp_path + ": " + archive_error_string(out);
        archive_write_free(out);
        return false;
    }

    SourceArchives sources;
    bool ok = true;
    for (size_t i : order) {
        if (asked_to_stop(cancelled)) { error = "cancelled"; ok = false; break; }
        const Piece& p = pieces[i];
        archive_entry* e = archive_entry_new();
        archive_entry_set_pathname(e, p.name.c_str());
        archive_entry_set_filetype(e, AE_IFREG);
        archive_entry_set_perm(e, 0644);
        archive_entry_set_size(e, (la_int64_t)p.size);
        // TorrentZip's fixed date : two people holding the same set hold the
        // same file.
        archive_entry_set_mtime(e, 851470320, 0);   // 1996-12-24 23:32 UTC
        if (archive_write_header(out, e) != ARCHIVE_OK) {
            error = "cannot add " + p.name + ": " + archive_error_string(out);
            archive_entry_free(e);
            ok = false;
            break;
        }
        archive_entry_free(e);

        if (p.container.empty()) continue;   // an empty ROM : its header is all of it
        auto st = staged.find(i);
        const std::string file = st != staged.end() ? st->second
                               : !RomArchive::looks_like_archive(p.container) ? p.container : std::string();
        if (!file.empty()) {
            std::ifstream f(file, std::ios::binary);
            if (!f) { error = "cannot read " + file; ok = false; break; }
            ok = pump(out, [&](char* b, size_t n) -> int64_t {
                f.read(b, (std::streamsize)n);
                return f.bad() ? -1 : (int64_t)f.gcount();
            }, error);
        } else if (RomArchive::is_zip(p.container)) {
            zip_t* src = nullptr;
            zip_int64_t src_idx = 0;
            zip_file_t* zf = sources.locate(p.container, p.entry, src, src_idx)
                           ? zip_fopen_index(src, (zip_uint64_t)src_idx, 0) : nullptr;
            if (!zf) { error = "source vanished: " + p.entry + " in " + p.container; ok = false; break; }
            ok = pump(out, [&](char* b, size_t n) -> int64_t { return zip_fread(zf, b, n); }, error);
            zip_fclose(zf);
        } else {
            error = "cannot read " + p.entry + " from " + p.container;
            ok = false;
        }
        if (!ok) break;
    }

    if (archive_write_close(out) != ARCHIVE_OK && ok) {
        error = std::string("cannot finalize archive: ") + archive_error_string(out);
        ok = false;
    }
    archive_write_free(out);
    if (!ok) fs::remove(tmp_path, ec);
    return ok;
}

} // namespace

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

std::string find_repair(const Bytes& data, unsigned long crc, uint64_t size) {
    Bytes out;
    for (const char* k : kRepairs)
        if (transform(k, data, size, out) && crc_of(out) == crc) return k;
    return {};
}

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

const char* extension(Format format) {
    return format == Format::SevenZip ? ".7z" : ".zip";
}

namespace {

// Re-read a freshly written archive and check every piece. This is the only
// thing standing between a silent libzip/IO failure and a corrupt set
// landing at its destination, so a rebuild is not considered done until this
// passes. Read as `format`, whatever the file is called.
bool verify_as(const std::string& archive, Format format, const std::vector<Piece>& pieces, std::string& error) {
    std::vector<RomArchive::Entry> produced;
    bool read;
    if (format == Format::Zip) {
        std::vector<RomScanner::ZipEntry> z;
        read = RomScanner::read_zip_entries(archive, z);
        for (auto& e : z) produced.push_back({std::move(e.name), e.crc, e.size});
    } else {
        read = RomArchive::read_entries(archive, produced);
    }
    if (!read) {
        error = "cannot reopen the rebuilt archive";
        return false;
    }
    std::unordered_map<std::string, const RomArchive::Entry*> by_name;
    for (const auto& e : produced) by_name[e.name] = &e;

    for (const auto& p : pieces) {
        auto it = by_name.find(p.name);
        if (it == by_name.end()) {
            error = "missing entry after rebuild: " + p.name;
            return false;
        }
        if (it->second->crc != p.crc) {
            error = "CRC mismatch after rebuild: " + p.name;
            return false;
        }
        if (p.size && it->second->size != p.size) {
            error = "size mismatch after rebuild: " + p.name;
            return false;
        }
    }
    if (produced.size() != pieces.size()) {
        error = "unexpected entry count after rebuild";
        return false;
    }
    return true;
}

} // namespace

bool verify(const std::string& archive, Format format, const std::vector<Piece>& pieces, std::string& error) {
    return verify_as(archive, format, pieces, error);
}

bool to_torrentzip(const std::string& zip_path, const std::vector<Piece>& pieces) {
    const std::string tz_path = zip_path + ".tz";
    std::string err;
    std::error_code ec;
    if (!TorrentZip::rezip(zip_path, tz_path, err) || !verify_as(tz_path, Format::Zip, pieces, err)) {
        fs::remove(tz_path, ec);
        return false;
    }
    fs::rename(tz_path, zip_path, ec);
    if (ec) { fs::remove(tz_path, ec); return false; }
    return true;
}

bool write_archive(const std::string& dest_path, const std::vector<Piece>& pieces, Format format, std::string& error) {
    const std::string tmp_path = dest_path + ".tmp";
    std::error_code ec;
    fs::remove(tmp_path, ec);

    const bool written = format == Format::SevenZip ? write_7z(tmp_path, pieces, error)
                                                    : write_zip(tmp_path, pieces, error);
    if (!written) return false;

    if (!verify_as(tmp_path, format, pieces, error)) {
        fs::remove(tmp_path, ec);
        return false;
    }
    // Au format TorrentZip, celui de RomVault et clrmamepro, qui sinon
    // recompressaient chaque zip produit ici. Un zip qui ne s'y prete pas
    // (deux noms egaux a la casse pres) reste tel que libzip l'a ecrit.
    if (format == Format::Zip) to_torrentzip(tmp_path, pieces);

    // Only now is the real destination touched.
    fs::rename(tmp_path, dest_path, ec);
    if (ec) {
        fs::remove(dest_path, ec);
        fs::rename(tmp_path, dest_path, ec);
        if (ec) {
            error = "cannot move the rebuilt archive into place: " + ec.message();
            fs::remove(tmp_path, ec);
            return false;
        }
    }
    return true;
}

bool build_archive(const std::string& tmp_path, const std::vector<Piece>& pieces, Format format,
                   const std::function<bool()>& cancelled, std::string& error) {
    std::error_code ec;
    fs::remove(tmp_path, ec);
    const bool written = format == Format::SevenZip ? write_7z(tmp_path, pieces, error, cancelled)
                                                    : write_zip(tmp_path, pieces, error, cancelled);
    if (!written) { fs::remove(tmp_path, ec); return false; }
    if (!verify_as(tmp_path, format, pieces, error)) { fs::remove(tmp_path, ec); return false; }
    if (format == Format::Zip && !TorrentZip::is_torrentzip(tmp_path)) {
        std::string why;
        const std::string tz_path = tmp_path + ".tz";
        if (!TorrentZip::rezip(tmp_path, tz_path, why) || !verify_as(tz_path, Format::Zip, pieces, why)
            || !TorrentZip::is_torrentzip(tz_path)) {
            fs::remove(tz_path, ec);
            fs::remove(tmp_path, ec);
            error = "cannot be written as a TorrentZip: " + why;
            return false;
        }
        fs::rename(tz_path, tmp_path, ec);
        if (ec) { error = "cannot write " + tmp_path + ": " + ec.message(); fs::remove(tz_path, ec); fs::remove(tmp_path, ec); return false; }
    }
    return true;
}

bool normalise_zip(const std::string& source, const std::string& tmp_path, const std::vector<Piece>& pieces,
                   std::string& error) {
    std::error_code ec;
    fs::remove(tmp_path, ec);
    if (!TorrentZip::rezip(source, tmp_path, error)) { fs::remove(tmp_path, ec); return false; }
    if (!verify_as(tmp_path, Format::Zip, pieces, error) || !TorrentZip::is_torrentzip(tmp_path)) {
        if (error.empty()) error = "the normalised archive is not a TorrentZip";
        fs::remove(tmp_path, ec);
        return false;
    }
    return true;
}

namespace {

bool same_bytes(const std::string& a, const std::string& b) {
    std::ifstream fa(fs::path(a), std::ios::binary), fb(fs::path(b), std::ios::binary);
    if (!fa || !fb) return false;
    std::vector<char> ba(1 << 16), bb(1 << 16);
    for (;;) {
        fa.read(ba.data(), (std::streamsize)ba.size());
        fb.read(bb.data(), (std::streamsize)bb.size());
        if (fa.gcount() != fb.gcount()) return false;
        if (fa.gcount() == 0) return fa.eof() && fb.eof();
        if (!std::equal(ba.begin(), ba.begin() + fa.gcount(), bb.begin())) return false;
    }
}

} // namespace

bool publish(const std::string& tmp_path, const std::string& dest_path, const std::string& keep_old_at,
             std::string& error) {
    std::error_code ec;
    // Empty `keep_old_at` : the old archive is already kept as it is.
    const bool had_old = fs::exists(fs::path(dest_path), ec) && !keep_old_at.empty();
    if (had_old) {
        // The archive in place stays whole until the very end : a second
        // name for it first (no data copied), a verified copy where the two
        // folders cannot share a file.
        fs::create_directories(fs::path(keep_old_at).parent_path(), ec);
        ec.clear();
        fs::create_hard_link(fs::path(dest_path), fs::path(keep_old_at), ec);
        if (ec) {
            ec.clear();
            copy_file_robust(dest_path, keep_old_at, true, ec);
            if (ec || !same_bytes(dest_path, keep_old_at)) {
                error = "cannot keep the previous archive in " + keep_old_at + (ec ? ": " + ec.message() : std::string());
                fs::remove(fs::path(keep_old_at), ec);
                fs::remove(fs::path(tmp_path), ec);
                return false;
            }
        }
    }
    // One rename : the destination is the old archive, then the new one,
    // never missing nor half written.
    fs::rename(fs::path(tmp_path), fs::path(dest_path), ec);
    if (ec) {
        error = "cannot put the new archive in place: " + ec.message();
        std::error_code rec;
        fs::remove(fs::path(tmp_path), rec);
        if (had_old) fs::remove(fs::path(keep_old_at), rec);   // the old one never left
        return false;
    }
    return true;
}

bool disk_full(const std::string& dir) {
    std::error_code ec;
    const fs::space_info si = fs::space(fs::path(dir), ec);
    // Less than what a block of the writers needs : nothing more fits.
    return !ec && si.available < (1u << 20);
}

// rename(), falling back to copy+delete when the two folders live on
// different filesystems : which is the normal case when ROMs sit on an
// external drive.
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

fs::path weak_canonical(const std::string& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(fs::path(p), ec);
    return ec ? fs::path(p) : c;
}

bool is_under(const fs::path& path, const fs::path& root) {
    if (root.empty()) return false;
    auto it_r = root.begin(), end_r = root.end();
    auto it_p = path.begin(), end_p = path.end();
    for (; it_r != end_r; ++it_r, ++it_p) {
        if (it_p == end_p || *it_p != *it_r) return false;
    }
    return true;
}

} // namespace RomBuild
