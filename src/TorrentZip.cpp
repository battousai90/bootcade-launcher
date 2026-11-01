// src/TorrentZip.cpp
#include "TorrentZip.h"

#include <zip.h>
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace TorrentZip {
namespace {

constexpr uint16_t kDosTime  = 0xBC00;   // 23:32:00
constexpr uint16_t kDosDate  = 0x2198;   // 1996-12-24
constexpr uint16_t kFlags    = 2;        // deflate, maximum compression
constexpr uint16_t kUtf8Flag = 1 << 11;
constexpr uint16_t kVersion  = 20;
constexpr const char* kCommentId = "TORRENTZIPPED-";

// CP437 as RomVault reads it : a name made only of these characters is
// written in CP437, without the UTF-8 flag ; any other, in UTF-8 with it.
const uint16_t kCp437[256] = {
    0x0000, 0x263A, 0x263B, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022, 0x25D8, 0x25CB, 0x25D9, 0x2642, 0x2640, 0x266A, 0x266B, 0x263C,
    0x25BA, 0x25C4, 0x2195, 0x203C, 0x00B6, 0x00A7, 0x25AC, 0x21A8, 0x2191, 0x2193, 0x2192, 0x2190, 0x221F, 0x2194, 0x25B2, 0x25BC,
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027, 0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037, 0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047, 0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057, 0x0058, 0x0059, 0x005A, 0x005B, 0x005C, 0x005D, 0x005E, 0x005F,
    0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067, 0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077, 0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0x007F,
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

std::u32string utf8_to_u32(const std::string& s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        char32_t cp = c;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (extra) cp = c & (0x3F >> extra);
        ++i;
        for (int k = 0; k < extra && i < s.size(); ++k, ++i) cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
        out.push_back(cp);
    }
    return out;
}

// The name as stored, and its flags : CP437 when every character has a
// CP437 byte, else UTF-8.
std::string stored_name(const std::string& utf8, uint16_t& flags) {
    std::string out;
    for (char32_t cp : utf8_to_u32(utf8)) {
        int byte = -1;
        for (int b = 0; b < 256 && byte < 0; ++b) if (kCp437[b] == cp) byte = b;
        if (byte < 0) { flags = kFlags | kUtf8Flag; return utf8; }
        out.push_back((char)byte);
    }
    flags = kFlags;
    return out;
}

// RomVault's Sorters.TrrntZipStringCompare : character by character, A-Z
// folded to a-z, nothing else.
int compare(const std::u32string& a, const std::u32string& b) {
    for (size_t i = 0;; ++i) {
        if (i == a.size()) return i == b.size() ? 0 : -1;
        if (i == b.size()) return 1;
        char32_t x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 0x20;
        if (y >= 'A' && y <= 'Z') y += 0x20;
        if (x != y) return x < y ? -1 : 1;
    }
}

void put16(std::vector<unsigned char>& b, uint16_t v) { b.push_back(v & 0xFF); b.push_back(v >> 8); }
void put32(std::vector<unsigned char>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xFF); }
void put64(std::vector<unsigned char>& b, uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back((v >> (8 * i)) & 0xFF); }
uint16_t get16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t get32(const unsigned char* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint64_t get64(const unsigned char* p) { return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32); }

// ZIP64, as RomVault writes it (Compress/ZipFile, ZipExtraFieldWrite.Zip64) :
// a value that reaches 0xFFFFFFFF is written as 0xFFFFFFFF and given in a
// 0x0001 extra field instead ; nothing of ZIP64 appears otherwise, so a zip
// under 4 GB is exactly what it always was.
constexpr uint32_t kMax32     = 0xFFFFFFFFu;
constexpr uint16_t kVersion64 = 45;   // version needed when an entry carries ZIP64

// The local header's ZIP64 field : both sizes, as soon as either needs it.
bool local_zip64(uint64_t usize, uint64_t csize) { return usize >= kMax32 || csize >= kMax32; }

// The central header's ZIP64 field : each value that needs it, in this order.
std::vector<unsigned char> central_zip64(uint64_t usize, uint64_t csize, uint64_t offset) {
    std::vector<unsigned char> values;
    if (usize  >= kMax32) put64(values, usize);
    if (csize  >= kMax32) put64(values, csize);
    if (offset >= kMax32) put64(values, offset);
    std::vector<unsigned char> field;
    if (values.empty()) return field;
    put16(field, 0x0001);
    put16(field, (uint16_t)values.size());
    field.insert(field.end(), values.begin(), values.end());
    return field;
}

uint32_t clamp32(uint64_t v) { return v >= kMax32 ? kMax32 : (uint32_t)v; }

struct Entry {
    zip_uint64_t   index = 0;
    std::string    name;     // UTF-8, as libzip gives it
    std::u32string key;      // for the order
    std::string    stored;   // bytes written
    uint16_t       flags = kFlags;
    uint32_t       crc = 0;
    uint64_t       csize = 0, usize = 0, offset = 0;
};

// Moves the bytes of [from, end) `by` bytes further, from the end backwards :
// room for a ZIP64 field the local header turned out to need.
bool shift_up(std::fstream& f, uint64_t from, uint64_t end, uint64_t by) {
    std::vector<char> buf(1 << 16);
    uint64_t pos = end;
    while (pos > from) {
        const uint64_t n = std::min<uint64_t>(buf.size(), pos - from);
        pos -= n;
        f.seekg((std::streamoff)pos);
        f.read(buf.data(), (std::streamsize)n);
        f.seekp((std::streamoff)(pos + by));
        f.write(buf.data(), (std::streamsize)n);
        if (!f) return false;
    }
    return true;
}

bool fail(std::string& error, const std::string& why, const std::string& out_zip, std::fstream* out = nullptr) {
    error = why;
    if (out) out->close();
    std::error_code ec;
    fs::remove(fs::path(out_zip), ec);
    return false;
}

}  // namespace

bool rezip(const std::string& in_zip, const std::string& out_zip, std::string& error) {
    int zerr = 0;
    zip_t* za = zip_open(in_zip.c_str(), ZIP_RDONLY, &zerr);
    if (!za) { error = "cannot open " + in_zip; return false; }
    struct Closer { zip_t* z; ~Closer() { zip_close(z); } } closer{za};

    const zip_int64_t n = zip_get_num_entries(za, 0);
    if (n < 0) { error = "cannot read " + in_zip; return false; }
    std::vector<Entry> entries;
    for (zip_int64_t i = 0; i < n; ++i) {
        zip_stat_t st;
        if (zip_stat_index(za, (zip_uint64_t)i, 0, &st) != 0 || !(st.valid & ZIP_STAT_NAME) || !(st.valid & ZIP_STAT_SIZE)) {
            error = "cannot read entry " + std::to_string(i);
            return false;
        }
        Entry e;
        e.index = (zip_uint64_t)i;
        e.name  = st.name;
        if (e.name.empty() || e.name.back() == '/') { error = "directory entry " + e.name; return false; }
        e.usize  = (uint64_t)st.size;
        e.key    = utf8_to_u32(e.name);
        e.stored = stored_name(e.name, e.flags);
        entries.push_back(std::move(e));
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return compare(a.key, b.key) < 0; });
    for (size_t i = 1; i < entries.size(); ++i)
        if (compare(entries[i - 1].key, entries[i].key) == 0) {
            error = "two entries differ only by case: " + entries[i].name;
            return false;
        }

    std::fstream out(fs::path(out_zip), std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!out) { error = "cannot write " + out_zip; return false; }

    std::vector<unsigned char> in_buf(1 << 16), out_buf(1 << 16);
    for (auto& e : entries) {
        e.offset = (uint64_t)out.tellp();
        // The uncompressed size is known : its ZIP64 field, if any, is in
        // the header from the start. The compressed one is only known once
        // written (below).
        bool zip64 = local_zip64(e.usize, 0);
        std::vector<unsigned char> head;
        put32(head, 0x04034b50);
        put16(head, zip64 ? kVersion64 : kVersion);
        put16(head, e.flags);
        put16(head, 8);
        put16(head, kDosTime);
        put16(head, kDosDate);
        put32(head, 0); put32(head, 0); put32(head, 0);   // crc, sizes : once the data is written
        put16(head, (uint16_t)e.stored.size());
        put16(head, zip64 ? 20 : 0);
        out.write((const char*)head.data(), (std::streamsize)head.size());
        out.write(e.stored.data(), (std::streamsize)e.stored.size());
        const uint64_t extra_at = (uint64_t)out.tellp();
        if (zip64) {
            std::vector<unsigned char> field;
            put16(field, 0x0001); put16(field, 16); put64(field, 0); put64(field, 0);
            out.write((const char*)field.data(), (std::streamsize)field.size());
        }

        zip_file_t* zf = zip_fopen_index(za, e.index, 0);
        if (!zf) return fail(error, "cannot read " + e.name, out_zip, &out);
        z_stream zs{};
        deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
        uLong crc = crc32(0L, Z_NULL, 0);
        uint64_t read_total = 0, written = 0;
        bool ok = true;
        for (;;) {
            const zip_int64_t got = zip_fread(zf, in_buf.data(), in_buf.size());
            if (got < 0) { ok = false; break; }
            crc = crc32(crc, in_buf.data(), (uInt)got);
            read_total += (uint64_t)got;
            zs.next_in  = in_buf.data();
            zs.avail_in = (uInt)got;
            const int flush = got == 0 ? Z_FINISH : Z_NO_FLUSH;
            int rc;
            do {
                zs.next_out  = out_buf.data();
                zs.avail_out = (uInt)out_buf.size();
                rc = deflate(&zs, flush);
                const size_t have = out_buf.size() - zs.avail_out;
                out.write((const char*)out_buf.data(), (std::streamsize)have);
                written += have;
            } while (zs.avail_out == 0);
            if (flush == Z_FINISH) { ok = rc == Z_STREAM_END; break; }
        }
        deflateEnd(&zs);
        zip_fclose(zf);
        if (!ok || read_total != e.usize) return fail(error, "cannot read " + e.name, out_zip, &out);
        if (!out) return fail(error, "cannot write " + out_zip, out_zip, &out);
        e.crc   = (uint32_t)crc;
        e.csize = written;

        uint64_t end = (uint64_t)out.tellp();
        if (!zip64 && local_zip64(e.usize, e.csize)) {
            // Data that grew past 4 GB once compressed : RomVault's
            // FixFileForZip64, room made for the field after the fact.
            if (!shift_up(out, extra_at, end, 20)) return fail(error, "cannot write " + out_zip, out_zip, &out);
            end += 20;
            zip64 = true;
            std::vector<unsigned char> fix;
            put16(fix, kVersion64);
            out.seekp((std::streamoff)(e.offset + 4));
            out.write((const char*)fix.data(), 2);
            fix.clear();
            put16(fix, 20);
            out.seekp((std::streamoff)(e.offset + 28));
            out.write((const char*)fix.data(), 2);
        }
        std::vector<unsigned char> sizes;
        put32(sizes, e.crc);
        put32(sizes, zip64 ? kMax32 : (uint32_t)e.csize);
        put32(sizes, zip64 ? kMax32 : (uint32_t)e.usize);
        out.seekp((std::streamoff)(e.offset + 14));
        out.write((const char*)sizes.data(), (std::streamsize)sizes.size());
        if (zip64) {
            std::vector<unsigned char> field;
            put16(field, 0x0001); put16(field, 16); put64(field, e.usize); put64(field, e.csize);
            out.seekp((std::streamoff)extra_at);
            out.write((const char*)field.data(), (std::streamsize)field.size());
        }
        out.seekp((std::streamoff)end);
        if (!out) return fail(error, "cannot write " + out_zip, out_zip, &out);
    }

    std::vector<unsigned char> cd;
    for (const auto& e : entries) {
        const std::vector<unsigned char> extra = central_zip64(e.usize, e.csize, e.offset);
        put32(cd, 0x02014b50);
        put16(cd, 0);                 // made by
        put16(cd, extra.empty() ? kVersion : kVersion64);
        put16(cd, e.flags);
        put16(cd, 8);
        put16(cd, kDosTime);
        put16(cd, kDosDate);
        put32(cd, e.crc); put32(cd, clamp32(e.csize)); put32(cd, clamp32(e.usize));
        put16(cd, (uint16_t)e.stored.size());
        put16(cd, (uint16_t)extra.size());
        put16(cd, 0); put16(cd, 0); put16(cd, 0);   // comment, disk, internal
        put32(cd, 0);                 // external attributes
        put32(cd, clamp32(e.offset));
        cd.insert(cd.end(), e.stored.begin(), e.stored.end());
        cd.insert(cd.end(), extra.begin(), extra.end());
    }
    const uint64_t cd_start = (uint64_t)out.tellp();
    const uint64_t cd_size  = cd.size();
    char comment[32];
    std::snprintf(comment, sizeof comment, "%s%08X", kCommentId, (unsigned)crc32(0L, cd.data(), (uInt)cd.size()));
    out.write((const char*)cd.data(), (std::streamsize)cd.size());

    // RomVault's EndOfCentralDirectoryWrite : the ZIP64 end records only when
    // the central directory's start, its size or the count needs them.
    const bool zip64_end = cd_start >= kMax32 || cd_size >= kMax32 || entries.size() >= 0xFFFF;
    if (zip64_end) {
        const uint64_t record_at = (uint64_t)out.tellp();
        std::vector<unsigned char> rec;
        put32(rec, 0x06064b50);
        put64(rec, 44);
        put16(rec, kVersion64); put16(rec, kVersion64);
        put32(rec, 0); put32(rec, 0);
        put64(rec, entries.size()); put64(rec, entries.size());
        put64(rec, cd_size); put64(rec, cd_start);
        put32(rec, 0x07064b50);
        put32(rec, 0);
        put64(rec, record_at);
        put32(rec, 1);
        out.write((const char*)rec.data(), (std::streamsize)rec.size());
    }
    const uint16_t count16 = entries.size() >= 0xFFFF ? 0xFFFF : (uint16_t)entries.size();
    std::vector<unsigned char> eocd;
    put32(eocd, 0x06054b50);
    put16(eocd, 0); put16(eocd, 0);
    put16(eocd, count16); put16(eocd, count16);
    put32(eocd, clamp32(cd_size));
    put32(eocd, clamp32(cd_start));
    put16(eocd, 22);
    eocd.insert(eocd.end(), comment, comment + 22);
    out.write((const char*)eocd.data(), (std::streamsize)eocd.size());
    out.close();
    if (!out) return fail(error, "cannot write " + out_zip, out_zip);
    return true;
}

bool is_torrentzip(const std::string& zip_path) {
    std::ifstream in(fs::path(zip_path), std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const auto size = (uint64_t)in.tellg();
    if (size < 44) return false;
    unsigned char eocd[44];
    in.seekg((std::streamoff)(size - 44));
    in.read((char*)eocd, 44);
    if (!in || get32(eocd) != 0x06054b50 || get16(eocd + 20) != 22) return false;
    if (std::string((const char*)eocd + 22, 14) != kCommentId) return false;
    uint64_t count = get16(eocd + 10), cd_size = get32(eocd + 12), cd_start = get32(eocd + 16);
    uint64_t cd_end = size - 44;   // where the central directory must stop

    // The ZIP64 end records, right before the end record, when and only when
    // RomVault would have written them.
    bool zip64_end = false;
    if (size >= 44 + 76) {
        unsigned char loc[20];
        in.seekg((std::streamoff)(size - 44 - 20));
        in.read((char*)loc, 20);
        zip64_end = in && get32(loc) == 0x07064b50;
        if (zip64_end) {
            const uint64_t record_at = get64(loc + 8);
            if (get32(loc + 4) != 0 || get32(loc + 16) != 1 || record_at != size - 44 - 20 - 56) return false;
            unsigned char rec[56];
            in.seekg((std::streamoff)record_at);
            in.read((char*)rec, 56);
            if (!in || get32(rec) != 0x06064b50 || get64(rec + 4) != 44) return false;
            if (get16(rec + 12) != kVersion64 || get16(rec + 14) != kVersion64 || get32(rec + 16) != 0 || get32(rec + 20) != 0) return false;
            if (get64(rec + 24) != get64(rec + 32)) return false;
            const uint64_t count64 = get64(rec + 32), size64 = get64(rec + 40), start64 = get64(rec + 48);
            if (count != (count64 >= 0xFFFF ? 0xFFFF : count64) || cd_size != clamp32(size64) || cd_start != clamp32(start64)) return false;
            if (count64 < 0xFFFF && size64 < kMax32 && start64 < kMax32) return false;   // not needed : not RomVault's
            count = count64; cd_size = size64; cd_start = start64;
            cd_end = record_at;
        }
    }
    if (!zip64_end && (count == 0xFFFF || cd_size == kMax32 || cd_start == kMax32)) return false;
    if (cd_start + cd_size != cd_end) return false;
    std::vector<unsigned char> cd(cd_size);
    in.seekg((std::streamoff)cd_start);
    in.read((char*)cd.data(), (std::streamsize)cd_size);
    if (!in) return false;
    char want[9];
    std::snprintf(want, sizeof want, "%08X", (unsigned)crc32(0L, cd.data(), (uInt)cd.size()));
    if (std::string((const char*)eocd + 36, 8) != want) return false;

    size_t p = 0;
    std::u32string last;
    for (uint64_t i = 0; i < count; ++i) {
        if (p + 46 > cd.size() || get32(cd.data() + p) != 0x02014b50) return false;
        const uint16_t flags = get16(cd.data() + p + 8);
        if ((flags & ~kUtf8Flag) != kFlags || get16(cd.data() + p + 10) != 8) return false;
        if (get16(cd.data() + p + 12) != kDosTime || get16(cd.data() + p + 14) != kDosDate) return false;
        const uint16_t nlen = get16(cd.data() + p + 28), xlen = get16(cd.data() + p + 30);
        if (get16(cd.data() + p + 32) != 0) return false;
        if (p + 46 + nlen + xlen > cd.size()) return false;
        const std::string raw((const char*)(cd.data() + p + 46), nlen);
        std::u32string key;
        if (flags & kUtf8Flag) key = utf8_to_u32(raw);
        else for (unsigned char c : raw) key.push_back(kCp437[c]);
        if (i > 0 && compare(last, key) >= 0) return false;
        last = key;

        // The sizes and the offset, from the ZIP64 field when the header
        // says so ; the field must then be exactly the one RomVault writes.
        uint64_t csize = get32(cd.data() + p + 20), usize = get32(cd.data() + p + 24), offset = get32(cd.data() + p + 42);
        const unsigned char* x = cd.data() + p + 46 + nlen;
        size_t at = 4;
        if (xlen) {
            if (xlen < 4 || get16(x) != 0x0001 || get16(x + 2) != xlen - 4) return false;
        }
        auto take = [&](uint64_t& v) {
            if (v != kMax32) return true;
            if (at + 8 > xlen) return false;
            v = get64(x + at);
            at += 8;
            return true;
        };
        if (!take(usize) || !take(csize) || !take(offset)) return false;
        if (central_zip64(usize, csize, offset) != std::vector<unsigned char>(x, x + xlen)) return false;
        if (get16(cd.data() + p + 6) != (xlen ? kVersion64 : kVersion)) return false;

        // L'en-tete local : meme date, et seulement le champ ZIP64 de ses tailles.
        const bool lz = local_zip64(usize, csize);
        unsigned char local[30];
        in.seekg((std::streamoff)offset);
        in.read((char*)local, 30);
        if (!in || get32(local) != 0x04034b50 || get16(local + 28) != (lz ? 20 : 0)) return false;
        if (get16(local + 10) != kDosTime || get16(local + 12) != kDosDate || get16(local + 6) != flags) return false;
        if (get16(local + 4) != (lz ? kVersion64 : kVersion)) return false;
        p += 46 + nlen + xlen;
    }
    return p == cd.size();
}

}  // namespace TorrentZip
