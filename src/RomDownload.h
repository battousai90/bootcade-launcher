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
#include <map>
#include <set>
#include <string>
#include <vector>

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

// Whether the server holds the version the DAT asks for, judged the way the
// server gets its corrected ROMs : a set added, or whose ROMs changed, in a
// DAT change (changes.json) is up to date there only once its zip sits in a
// RomFix lot of that date or later. A set no change mentions is judged on
// presence alone. One view per job : it reads changes.json and the RomFix
// listings once, and asks nothing that counts against the quota.
enum class Offer { Yes, Outdated, Absent, Unknown };
class ServerView {
public:
    Offer check(const std::string& dat_header, const std::string& name);
private:
    void load();
    const std::set<std::string>& lot(const std::string& date, const std::string& dat_header);
    bool m_loaded = false, m_changes_ok = false;
    std::map<std::string, std::string> m_changed;   // set name -> YYYY-MM-DD
    std::vector<std::string> m_dates;               // RomFix lots, oldest first
    std::map<std::string, std::set<std::string>> m_lots;   // "date/header" -> zip names
};

// One set, /roms/<dat_header>/<name>.zip, written to <dest_dir>/<name>.zip
// (through a .part file, so Import never sees half a zip).
struct Result {
    bool        ok = false;
    long        http_status = 0;    // 0 : nothing answered
    std::string path;               // the zip written, when ok
    std::string reason;             // why not, in the server's words when it gave some
    // Le serveur a refuse SANS dire pourquoi : ce n'est pas le compte ni le
    // quota (qui portent toujours une raison), c'est lui qui n'a pas pu
    // envoyer le fichier. Inutile d'essayer les suivants.
    bool        server_fault = false;
};
Result download(const std::string& dat_header, const std::string& name,
                const std::string& dest_dir, const std::atomic<bool>& cancelled,
                const std::function<void(double)>& progress);

// Human-readable, translated explanation of a quota refusal.
std::string explain(const std::string& reason);

}  // namespace RomDownload
