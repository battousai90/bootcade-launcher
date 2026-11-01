// src/RomArchive.cpp
#include "RomArchive.h"
#include "RomScanner.h"

#include <archive.h>
#include <archive_entry.h>
#include <zlib.h>
#include <zip.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unordered_set>

namespace fs = std::filesystem;

namespace RomArchive {
namespace {

constexpr size_t kBuf = 65536;

std::string lower_ext(const std::string& path) {
    std::string e = fs::path(path).extension().string();
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return e;
}

// RAII for a libarchive read handle.
struct Reader {
    struct archive* a = nullptr;
    explicit Reader(const std::string& path) {
        a = archive_read_new();
        archive_read_support_filter_all(a);
        archive_read_support_format_all(a);
        if (archive_read_open_filename(a, path.c_str(), kBuf) != ARCHIVE_OK) {
            archive_read_free(a);
            a = nullptr;
        }
    }
    ~Reader() { if (a) archive_read_free(a); }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    explicit operator bool() const { return a != nullptr; }
};

} // namespace

bool is_zip(const std::string& path) { return lower_ext(path) == ".zip"; }

bool looks_like_archive(const std::string& path) {
    static const std::unordered_set<std::string> exts = {
        ".zip", ".7z", ".rar", ".tar", ".tgz", ".gz", ".bz2", ".xz",
        ".cab", ".lha", ".lzh", ".arj", ".zst",
    };
    return exts.count(lower_ext(path)) > 0;
}

bool is_sidecar(const std::string& path) {
    static const std::unordered_set<std::string> exts = {
        ".txt", ".nfo", ".diz", ".dsc", ".cue", ".ccd", ".sub", ".m3u",
        ".ips", ".bps", ".ups", ".xdelta", ".dat", ".xml", ".json",
        ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".pdf",
        ".md5", ".sha1", ".sfv", ".cfg", ".ini", ".url", ".log",
    };
    return exts.count(lower_ext(path)) > 0;
}

bool read_any(const std::string& path, std::vector<Entry>& out) {
    out.clear();
    if (looks_like_archive(path)) return read_entries(path, out);
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) return false;
    Entry e;
    e.name = fs::path(path).filename().string();
    e.crc  = crc32(0L, Z_NULL, 0);
    std::vector<char> buf(kBuf);
    while (f.read(buf.data(), buf.size()) || f.gcount() > 0) {
        e.crc = crc32(e.crc, (const Bytef*)buf.data(), (uInt)f.gcount());
        e.size += (uint64_t)f.gcount();
    }
    out.push_back(std::move(e));
    return true;
}

bool read_entries(const std::string& path, std::vector<Entry>& out) {
    if (is_zip(path)) {
        std::vector<RomScanner::ZipEntry> z;
        if (!RomScanner::read_zip_entries(path, z)) return false;
        out.reserve(out.size() + z.size());
        for (auto& e : z) out.push_back({std::move(e.name), e.crc, e.size});
        return true;
    }

    Reader r(path);
    if (!r) return false;

    std::vector<char> buf(kBuf);
    struct archive_entry* ae = nullptr;
    while (archive_read_next_header(r.a, &ae) == ARCHIVE_OK) {
        if (archive_entry_filetype(ae) != AE_IFREG) continue;
        const char* name = archive_entry_pathname(ae);
        if (!name || !*name) continue;

        uLong crc = crc32(0L, Z_NULL, 0);
        uint64_t total = 0;
        la_ssize_t n;
        while ((n = archive_read_data(r.a, buf.data(), buf.size())) > 0) {
            crc = crc32(crc, (const Bytef*)buf.data(), (uInt)n);
            total += (uint64_t)n;
        }
        if (n < 0) return false;   // truncated or corrupt
        out.push_back({name, (unsigned long)crc, total});
    }
    return true;
}

bool read_listed(const std::string& path, std::vector<Entry>& out) {
    if (!is_zip(path)) return read_any(path, out);
    out.clear();
    int err = 0;
    zip_t* z = zip_open(path.c_str(), ZIP_RDONLY, &err);
    if (!z) return false;
    const zip_int64_t n = zip_get_num_entries(z, 0);
    for (zip_uint64_t i = 0; i < (zip_uint64_t)n; ++i) {
        zip_stat_t sb;
        zip_stat_init(&sb);
        if (zip_stat_index(z, i, 0, &sb) != 0) continue;
        std::string name = sb.name ? sb.name : "";
        if (name.empty() || name.back() == '/') continue;
        if (!(sb.valid & ZIP_STAT_CRC) || !(sb.valid & ZIP_STAT_SIZE)) {
            // A central directory that does not say : read as read_entries does.
            zip_close(z);
            return read_entries(path, out);
        }
        out.push_back({std::move(name), (unsigned long)sb.crc, (uint64_t)sb.size});
    }
    zip_close(z);
    return true;
}

bool list_names(const std::string& path, std::vector<std::string>& out) {
    if (is_zip(path)) {
        int err = 0;
        zip_t* z = zip_open(path.c_str(), ZIP_RDONLY, &err);
        if (!z) return false;
        zip_int64_t n = zip_get_num_entries(z, 0);
        for (zip_uint64_t i = 0; i < (zip_uint64_t)n; ++i) {
            zip_stat_t sb;
            if (zip_stat_index(z, i, 0, &sb) != 0) continue;
            std::string name = sb.name ? sb.name : "";
            if (name.empty() || name.back() == '/') continue;
            out.push_back(std::move(name));
        }
        zip_close(z);
        return true;
    }

    Reader r(path);
    if (!r) return false;
    struct archive_entry* ae = nullptr;
    while (archive_read_next_header(r.a, &ae) == ARCHIVE_OK) {
        if (archive_entry_filetype(ae) != AE_IFREG) continue;
        const char* name = archive_entry_pathname(ae);
        if (name && *name) out.push_back(name);
        archive_read_data_skip(r.a);
    }
    return true;
}

bool extract(const std::string& archive_path,
             const std::vector<std::string>& wanted,
             const std::string& dest_dir,
             std::unordered_map<std::string, std::string>& out_files,
             std::string& error) {
    std::unordered_set<std::string> todo(wanted.begin(), wanted.end());
    if (todo.empty()) return true;

    std::error_code ec;
    fs::create_directories(dest_dir, ec);
    if (ec) { error = "cannot create staging directory: " + ec.message(); return false; }

    Reader r(archive_path);
    if (!r) { error = "cannot open " + archive_path; return false; }

    std::vector<char> buf(kBuf);
    size_t index = 0;
    struct archive_entry* ae = nullptr;

    // One forward pass. Entries we do not need are skipped rather than read, and we
    // stop as soon as everything wanted has been seen.
    while (!todo.empty() && archive_read_next_header(r.a, &ae) == ARCHIVE_OK) {
        if (archive_entry_filetype(ae) != AE_IFREG) continue;
        const char* name = archive_entry_pathname(ae);
        if (!name || !todo.count(name)) { archive_read_data_skip(r.a); continue; }

        std::string entry = name;
        std::string out_path = (fs::path(dest_dir) / ("p" + std::to_string(index++) + ".bin")).string();
        std::ofstream of(out_path, std::ios::binary);
        if (!of) { error = "cannot write " + out_path; return false; }

        la_ssize_t n;
        while ((n = archive_read_data(r.a, buf.data(), buf.size())) > 0)
            of.write(buf.data(), n);
        of.close();
        if (n < 0) { error = "read error on " + entry + " in " + archive_path; return false; }

        out_files[entry] = out_path;
        todo.erase(entry);
    }

    if (!todo.empty()) {
        error = "entry not found in " + archive_path + ": " + *todo.begin();
        return false;
    }
    return true;
}

} // namespace RomArchive
