// src/FileCopy.h : copy a file even where the kernel's fast copy refuses.
#pragma once

#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

/* std::filesystem::copy_file asks the kernel to copy (copy_file_range /
 * sendfile), which some file systems and device pairs refuse with "Invalid
 * argument" or EXDEV : exFAT, NTFS or FUSE volumes, a copy across two disks.
 * A move across devices ends in a copy, so it failed too. When the fast copy
 * fails, read and write the file in blocks, which every file system accepts.
 * A partial destination is removed on failure. */
inline bool copy_file_robust(const std::filesystem::path& from, const std::filesystem::path& to,
                             bool overwrite, std::error_code& ec) {
    namespace fs = std::filesystem;
    ec.clear();
    fs::copy_file(from, to, overwrite ? fs::copy_options::overwrite_existing : fs::copy_options::none, ec);
    if (!ec) return true;
    // A refusal to overwrite is an answer, not a failure of the fast copy.
    if (ec == std::errc::file_exists) return false;

    std::ifstream in(from, std::ios::binary);
    if (!in) { ec = std::make_error_code(std::errc::no_such_file_or_directory); return false; }
    if (!overwrite && fs::exists(to)) { ec = std::make_error_code(std::errc::file_exists); return false; }
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!out) { ec = std::make_error_code(std::errc::io_error); return false; }
    std::vector<char> buf(1u << 20);
    while (in) {
        in.read(buf.data(), (std::streamsize)buf.size());
        const std::streamsize got = in.gcount();
        if (got > 0) out.write(buf.data(), got);
        if (!out) break;
    }
    out.close();
    if (!out || in.bad()) {
        std::error_code rm;
        fs::remove(to, rm);
        ec = std::make_error_code(std::errc::io_error);
        return false;
    }
    ec.clear();
    return true;
}
