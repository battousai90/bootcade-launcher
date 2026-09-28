// src/RomDownload.h
//
// Downloading a missing set from Bootcade's own ROM server, with the player's
// account : the same rules as the website (account signed in, e-mail
// verified, a daily quota of distinct ROMs), enforced by the server. FinalBurn
// Neo only : MAME has no online feature yet.
//
// Every call is blocking and MUST run off the GTK thread.
#pragma once

#include <atomic>
#include <functional>
#include <string>

namespace RomDownload {

// What the server says about this account before anything is downloaded.
struct Quota {
    bool        answered = false;   // the service replied at all
    bool        can_download = false;
    int         remaining = 0;      // ROMs still allowed in the rolling window
    int         quota = 0;
    std::string reason;             // "not_signed_in", "unverified", "no_role", "blocked", "quota", ...
    std::string error;              // transport problem, when !answered
};
Quota fetch_quota();

// Whether the server holds /roms/<dat_header>/<name>.zip. A HEAD request :
// the server counts only a GET against the quota, so asking costs nothing.
// Unknown : no answer, or a refusal that hides the answer (quota, account).
enum class Presence { Present, Absent, Unknown };
Presence presence(const std::string& dat_header, const std::string& name);

// One set, /roms/<dat_header>/<name>.zip, written to <dest_dir>/<name>.zip
// (through a .part file, so Import never sees half a zip).
struct Result {
    bool        ok = false;
    long        http_status = 0;    // 0 : nothing answered
    std::string path;               // the zip written, when ok
    std::string reason;             // why not, in the server's words when it gave some
};
Result download(const std::string& dat_header, const std::string& name,
                const std::string& dest_dir, const std::atomic<bool>& cancelled,
                const std::function<void(double)>& progress);

// Human-readable, translated explanation of a quota refusal.
std::string explain(const std::string& reason);

}  // namespace RomDownload
