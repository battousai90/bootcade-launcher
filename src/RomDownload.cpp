// src/RomDownload.cpp
#include "RomDownload.h"
#include "AppContext.h"
#include "BootcadeAuth.h"
#include "HiscoreClient.h"
#include "i18n.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>

namespace fs = std::filesystem;

namespace RomDownload {

namespace {

#ifdef BOOTCADE_VERSION
const char* kUserAgent = "bootcade-launcher/" BOOTCADE_VERSION;
#else
const char* kUserAgent = "bootcade-launcher";
#endif

// The ROM server, apart from the score service : config.json "roms_url".
std::string roms_url() {
    std::string url = "https://roms.bootcade.duckdns.org";
    try {
        std::ifstream f(AppContext::get_config_path());
        nlohmann::json j;
        if (f) f >> j;
        if (j.contains("roms_url") && j["roms_url"].is_string() && !j["roms_url"].get<std::string>().empty())
            url = j["roms_url"].get<std::string>();
    } catch (...) {}
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

size_t to_string(char* p, size_t s, size_t n, void* out) {
    static_cast<std::string*>(out)->append(p, s * n);
    return s * n;
}

struct Sink {
    FILE* file = nullptr;
    const std::atomic<bool>* cancelled = nullptr;
    const std::function<void(double)>* progress = nullptr;
};

size_t to_file(char* p, size_t s, size_t n, void* out) {
    auto* sink = static_cast<Sink*>(out);
    return fwrite(p, s, n, sink->file) * s;
}

int on_progress(void* data, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
    auto* sink = static_cast<Sink*>(data);
    if (sink->cancelled && sink->cancelled->load()) return 1;   // abort the transfer
    if (sink->progress && *sink->progress && total > 0) (*sink->progress)((double)now / (double)total);
    return 0;
}

std::string escape(CURL* curl, const std::string& part) {
    char* e = curl_easy_escape(curl, part.c_str(), (int)part.size());
    std::string out = e ? e : "";
    curl_free(e);
    return out;
}

// The DAT changes the site publishes : config.json "changes_url".
std::string changes_url() {
    std::string url = "https://files.bootcade.duckdns.org/dat/changes.json";
    try {
        std::ifstream f(AppContext::get_config_path());
        nlohmann::json j;
        if (f) f >> j;
        if (j.contains("changes_url") && j["changes_url"].is_string() && !j["changes_url"].get<std::string>().empty())
            url = j["changes_url"].get<std::string>();
    } catch (...) {}
    return url;
}

// A plain GET : the body and the status, 0 when nothing answered.
long get(const std::string& url, const std::string& token, std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) return 0;
    struct curl_slist* headers = nullptr;
    if (!token.empty()) headers = curl_slist_append(nullptr, ("Authorization: Bearer " + token).c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    const CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return res == CURLE_OK ? status : 0;
}

// The links of an nginx directory listing, decoded.
std::vector<std::string> listing_links(const std::string& html) {
    std::vector<std::string> out;
    static const std::regex href("href=\"([^\"]+)\"");
    CURL* curl = curl_easy_init();
    for (auto it = std::sregex_iterator(html.begin(), html.end(), href); it != std::sregex_iterator(); ++it) {
        const std::string raw = (*it)[1].str();
        int n = 0;
        char* d = curl ? curl_easy_unescape(curl, raw.c_str(), (int)raw.size(), &n) : nullptr;
        out.push_back(d ? std::string(d, (size_t)n) : raw);
        if (d) curl_free(d);
    }
    if (curl) curl_easy_cleanup(curl);
    return out;
}

}  // namespace

Quota fetch_quota() {
    Quota q;
    const std::string token = BootcadeAuth::access_token();
    if (token.empty()) { q.answered = true; q.reason = "not_signed_in"; return q; }
    const std::string base = HiscoreClient::base_url();
    if (base.empty()) { q.error = "no score service configured"; return q; }

    CURL* curl = curl_easy_init();
    if (!curl) { q.error = "curl"; return q; }
    std::string body;
    struct curl_slist* headers = curl_slist_append(nullptr, ("Authorization: Bearer " + token).c_str());
    curl_easy_setopt(curl, CURLOPT_URL, (base + "/api/me/roms").c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    const CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) { q.error = curl_easy_strerror(res); return q; }
    q.answered = true;
    if (status == 401) { q.reason = "not_signed_in"; return q; }
    if (status != 200) { q.reason = "unavailable"; return q; }
    try {
        const auto j = nlohmann::json::parse(body);
        q.can_download = j.value("can_download", false);
        q.remaining    = j.value("remaining", 0);
        q.quota        = j.value("quota", 0);
        if (j.contains("reason") && j["reason"].is_string()) q.reason = j["reason"].get<std::string>();
    } catch (...) {
        q.reason = "unavailable";
    }
    return q;
}

Presence presence(const std::string& dat_header, const std::string& name) {
    const std::string token = BootcadeAuth::access_token();
    if (token.empty()) return Presence::Unknown;
    CURL* curl = curl_easy_init();
    if (!curl) return Presence::Unknown;
    const std::string url = roms_url() + "/roms/" + escape(curl, dat_header) + "/" + escape(curl, name) + ".zip";
    struct curl_slist* headers = curl_slist_append(nullptr, ("Authorization: Bearer " + token).c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    const CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) return Presence::Unknown;
    if (status == 200) return Presence::Present;
    if (status == 404) return Presence::Absent;
    return Presence::Unknown;
}

void ServerView::load() {
    m_loaded = true;
    std::string body;
    if (get(changes_url(), "", body) == 200) {
        try {
            // Newest first : the first date met for a set is its latest.
            for (const auto& entry : nlohmann::json::parse(body)) {
                const std::string date = entry.value("generated", std::string()).substr(0, 10);
                if (date.size() != 10 || !entry.contains("systems")) continue;
                for (const auto& sys : entry["systems"]) {
                    if (sys.contains("added"))
                        for (const auto& g : sys["added"]) m_changed.emplace(g.value("n", std::string()), date);
                    // A new title alone leaves the zip as it was : only a
                    // change of ROMs asks the server for a new one.
                    if (sys.contains("modified"))
                        for (const auto& g : sys["modified"]) {
                            const auto& c = g.contains("changed") ? g["changed"] : nlohmann::json::array();
                            if (std::find(c.begin(), c.end(), "rom") != c.end())
                                m_changed.emplace(g.value("n", std::string()), date);
                        }
                }
            }
            m_changes_ok = true;
        } catch (...) { m_changed.clear(); }
    }
    body.clear();
    if (get(roms_url() + "/romfix/", BootcadeAuth::access_token(), body) == 200) {
        static const std::regex day("^\\d{4}-\\d{2}-\\d{2}/$");
        for (const auto& l : listing_links(body))
            if (std::regex_match(l, day)) m_dates.push_back(l.substr(0, 10));
        std::sort(m_dates.begin(), m_dates.end());
    }
}

const std::set<std::string>& ServerView::lot(const std::string& date, const std::string& dat_header) {
    const std::string key = date + "/" + dat_header;
    auto it = m_lots.find(key);
    if (it != m_lots.end()) return it->second;
    std::set<std::string> zips;
    std::string body;
    CURL* curl = curl_easy_init();
    const std::string url = roms_url() + "/romfix/" + date + "/" + (curl ? escape(curl, dat_header) : dat_header) + "/";
    if (curl) curl_easy_cleanup(curl);
    if (get(url, BootcadeAuth::access_token(), body) == 200)
        for (const auto& l : listing_links(body))
            if (l.size() > 4 && l.compare(l.size() - 4, 4, ".zip") == 0) zips.insert(l);
    return m_lots.emplace(key, std::move(zips)).first->second;
}

Offer ServerView::check(const std::string& dat_header, const std::string& name) {
    if (!m_loaded) load();
    auto on_server = [&] {
        switch (presence(dat_header, name)) {
            case Presence::Present: return Offer::Yes;
            case Presence::Absent:  return Offer::Absent;
            default:                return Offer::Unknown;
        }
    };
    const auto changed = m_changes_ok ? m_changed.find(name) : m_changed.end();
    if (changed == m_changed.end()) return on_server();
    for (const auto& date : m_dates)
        if (date >= changed->second && lot(date, dat_header).count(name + ".zip")) return Offer::Yes;
    // Changed, and no RomFix lot carries it since : the server's copy, if
    // any, is the version the DAT has moved away from.
    const Offer here = on_server();
    return here == Offer::Absent ? Offer::Absent : here == Offer::Yes ? Offer::Outdated : Offer::Unknown;
}

Result download(const std::string& dat_header, const std::string& name,
                const std::string& dest_dir, const std::atomic<bool>& cancelled,
                const std::function<void(double)>& progress) {
    Result r;
    const std::string token = BootcadeAuth::access_token();
    if (token.empty()) { r.reason = explain("not_signed_in"); return r; }

    // The server counts a GET against the quota before it looks for the
    // file : asking for a set it does not have spent a download for nothing.
    // A HEAD first costs nothing and settles it.
    if (presence(dat_header, name) == Presence::Absent) {
        r.http_status = 404;
        r.reason = _("not available on the Bootcade server");
        return r;
    }

    std::error_code ec;
    fs::create_directories(dest_dir, ec);
    const fs::path final_path = fs::path(dest_dir) / (name + ".zip");
    const fs::path part_path  = fs::path(dest_dir) / (name + ".zip.part");

    CURL* curl = curl_easy_init();
    if (!curl) { r.reason = "curl"; return r; }
    const std::string url = roms_url() + "/roms/" + escape(curl, dat_header) + "/" + escape(curl, name) + ".zip";

    Sink sink;
    sink.file = std::fopen(part_path.c_str(), "wb");
    if (!sink.file) { curl_easy_cleanup(curl); r.reason = "cannot write " + part_path.string(); return r; }
    sink.cancelled = &cancelled;
    sink.progress = &progress;

    struct curl_slist* headers = curl_slist_append(nullptr, ("Authorization: Bearer " + token).c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, to_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &sink);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    // No overall timeout : a large set at the server's 2 MB/s takes minutes.
    // A stalled transfer is caught instead : under 1 KB/s for 60 s.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    const CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.http_status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    std::fclose(sink.file);

    if (res != CURLE_OK || r.http_status != 200) {
        fs::remove(part_path, ec);
        if (cancelled.load())            r.reason = _("cancelled");
        else if (r.http_status == 401)   r.reason = explain("not_signed_in");
        else if (r.http_status == 403)   r.reason = _("refused by the server (quota or account)");
        else if (r.http_status == 404)   r.reason = _("not available on the Bootcade server");
        else if (r.http_status == 429 || r.http_status == 503) r.reason = _("the server is busy, try again later");
        else if (r.http_status)          r.reason = "HTTP " + std::to_string(r.http_status);
        else                             r.reason = curl_easy_strerror(res);
        return r;
    }
    fs::rename(part_path, final_path, ec);
    if (ec) { r.reason = ec.message(); return r; }
    r.ok = true;
    r.path = final_path.string();
    return r;
}

std::string explain(const std::string& reason) {
    if (reason == "not_signed_in") return _("Sign in to your Bootcade account (Settings) to download ROMs.");
    if (reason == "unverified")    return _("Verify your account's e-mail address to download ROMs.");
    if (reason == "no_role")       return _("Your account does not have access to ROM downloads.");
    if (reason == "blocked")       return _("ROM downloads are blocked for your account.");
    if (reason == "quota")         return _("Your daily download quota is used up.");
    if (reason == "unavailable")   return _("The Bootcade server is not available right now.");
    return reason.empty() ? std::string(_("Downloads are not possible right now.")) : reason;
}

}  // namespace RomDownload
