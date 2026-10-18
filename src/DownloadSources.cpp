// src/DownloadSources.cpp
#include "DownloadSources.h"

#include "AppContext.h"
#include "CurlInit.h"   // sous Windows : les certificats livres avec Bootcade
#include "i18n.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

namespace DownloadSources {
namespace {

constexpr const char* kUserAgent = "Mozilla/5.0 (Bootcade)";

nlohmann::json read_config() {
    nlohmann::json j;
    std::ifstream fi(AppContext::get_config_path());
    if (fi) { try { fi >> j; } catch (...) { j = nlohmann::json::object(); } }
    if (!j.is_object()) j = nlohmann::json::object();
    return j;
}
void write_config(const nlohmann::json& j) {
    std::ofstream fo(AppContext::get_config_path());
    if (fo) fo << j.dump(4);
}
nlohmann::json& rom_manager(nlohmann::json& j) {
    if (!j.contains("rom_manager") || !j["rom_manager"].is_object()) j["rom_manager"] = nlohmann::json::object();
    return j["rom_manager"];
}

std::string secrets_path() {
    return (fs::path(AppContext::get_user_config_dir()) / "download-secrets.json").string();
}

std::vector<Source> defaults() {
    Source b; b.id = b.kind = "bootcade"; b.name = "Bootcade";
    Source l; l.id = l.kind = "lolroms";  l.name = "LolRoms";  l.emulators = "fbneo,mame";
    Source r; r.id = r.kind = "retroroms"; r.name = "RetroRoms"; r.emulators = "fbneo,mame"; r.daily_quota = 100;
    return {b, l, r};
}

// Les en-tetes de reponse qui parlent de quota ou de limite.
size_t on_header(char* p, size_t size, size_t n, void* out) {
    std::string h(p, size * n);
    std::string low = h;
    for (auto& c : low) c = (char)std::tolower((unsigned char)c);
    if (low.find("quota") != std::string::npos || low.find("limit") != std::string::npos
        || low.find("remaining") != std::string::npos) {
        while (!h.empty() && (h.back() == '\r' || h.back() == '\n')) h.pop_back();
        auto* dst = static_cast<std::string*>(out);
        if (!dst->empty()) *dst += "; ";
        *dst += h;
    }
    return size * n;
}

size_t to_file(char* p, size_t size, size_t n, void* out) {
    return std::fwrite(p, size, n, static_cast<std::FILE*>(out)) * size;
}
struct Transfer {
    const std::atomic<bool>* cancelled;
    const std::function<void(double)>* progress;
};
int on_progress(void* data, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
    auto* t = static_cast<Transfer*>(data);
    if (t->cancelled && t->cancelled->load()) return 1;
    if (t->progress && *t->progress && total > 0) (*t->progress)((double)now / (double)total);
    return 0;
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size())) s.replace(p, from.size(), to);
    return s;
}
std::string url_encode_path(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

} // namespace

bool Source::serves(const std::string& emulator) const {
    if (kind == "bootcade") return emulator == "fbneo";
    return ("," + emulators + ",").find("," + emulator + ",") != std::string::npos;
}

std::vector<Source> load() {
    auto j = read_config();
    std::vector<Source> out = defaults();
    const auto& rm = rom_manager(j);
    if (!rm.contains("download_sources") || !rm["download_sources"].is_array()) return out;
    for (const auto& e : rm["download_sources"]) {
        if (!e.is_object()) continue;
        Source s;
        s.id          = e.value("id", std::string());
        s.kind        = e.value("kind", std::string("custom"));
        s.name        = e.value("name", s.id);
        s.url         = e.value("url", std::string());
        s.user        = e.value("user", std::string());
        s.emulators   = e.value("emulators", std::string("fbneo"));
        s.enabled     = e.value("enabled", true);
        s.daily_quota = e.value("daily_quota", 0);
        if (s.id.empty()) continue;
        bool merged = false;
        for (auto& d : out) {
            if (d.id != s.id) continue;
            // Une source predefinie garde sa nature : seuls l'etat et le quota
            // se reglent.
            d.enabled = s.enabled;
            if (e.contains("daily_quota")) d.daily_quota = s.daily_quota;
            merged = true;
        }
        if (!merged && s.kind == "custom") out.push_back(s);
    }
    return out;
}

void save(const std::vector<Source>& sources) {
    auto j = read_config();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& s : sources)
        arr.push_back({{"id", s.id}, {"kind", s.kind}, {"name", s.name}, {"url", s.url}, {"user", s.user},
                       {"emulators", s.emulators}, {"enabled", s.enabled}, {"daily_quota", s.daily_quota}});
    rom_manager(j)["download_sources"] = arr;
    write_config(j);
}

Source find(const std::string& id) {
    for (auto& s : load()) if (s.id == id) return s;
    return {};
}

std::string password(const std::string& id) {
    nlohmann::json j;
    std::ifstream fi(secrets_path());
    if (fi) { try { fi >> j; } catch (...) { return {}; } }
    return j.is_object() ? j.value(id, std::string()) : std::string();
}

void set_password(const std::string& id, const std::string& pw) {
    nlohmann::json j = nlohmann::json::object();
    { std::ifstream fi(secrets_path()); if (fi) { try { fi >> j; } catch (...) { j = nlohmann::json::object(); } } }
    if (!j.is_object()) j = nlohmann::json::object();
    if (pw.empty()) j.erase(id); else j[id] = pw;
    { std::ofstream fo(secrets_path()); if (fo) fo << j.dump(2); }
    std::error_code ec;
    fs::permissions(secrets_path(), fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
}

void record_download(const std::string& id) {
    auto j = read_config();
    auto& log = rom_manager(j)["download_log"];
    if (!log.is_object()) log = nlohmann::json::object();
    auto& list = log[id];
    if (!list.is_array()) list = nlohmann::json::array();
    const long long now = (long long)std::time(nullptr);
    nlohmann::json kept = nlohmann::json::array();
    for (const auto& t : list) if (t.is_number() && now - t.get<long long>() < 86400) kept.push_back(t);
    kept.push_back(now);
    list = kept;
    write_config(j);
}

int used_last_24h(const std::string& id) {
    auto j = read_config();
    const auto& rm = rom_manager(j);
    if (!rm.contains("download_log") || !rm["download_log"].is_object()) return 0;
    const auto& log = rm["download_log"];
    if (!log.contains(id) || !log[id].is_array()) return 0;
    const long long now = (long long)std::time(nullptr);
    int n = 0;
    for (const auto& t : log[id]) if (t.is_number() && now - t.get<long long>() < 86400) ++n;
    return n;
}

void remember_wrong(const std::string& id, const std::string& key, const std::string& site_date) {
    auto j = read_config();
    auto& w = rom_manager(j)["download_wrong"];
    if (!w.is_object()) w = nlohmann::json::object();
    if (!w[id].is_object()) w[id] = nlohmann::json::object();
    w[id][key] = site_date;
    write_config(j);
}

bool known_wrong(const std::string& id, const std::string& key, const std::string& site_date) {
    auto j = read_config();
    const auto& rm = rom_manager(j);
    if (!rm.contains("download_wrong") || !rm["download_wrong"].is_object()) return false;
    const auto& w = rm["download_wrong"];
    if (!w.contains(id) || !w[id].is_object() || !w[id].contains(key)) return false;
    return w[id][key].get<std::string>() == site_date;
}

std::string download_target() {
    auto j = read_config();
    const auto& rm = rom_manager(j);
    std::string t = rm.value("download_target", std::string());
    if (t.empty()) t = rm.value("download_folder", std::string()).empty() ? "inbox" : "folder";
    return t;
}
void set_download_target(const std::string& target) {
    auto j = read_config();
    rom_manager(j)["download_target"] = target;
    write_config(j);
}

std::string download_folder() {
    auto j = read_config();
    return rom_manager(j).value("download_folder", std::string());
}
void set_download_folder(const std::string& folder) {
    auto j = read_config();
    rom_manager(j)["download_folder"] = folder;
    write_config(j);
}
std::string chosen_source() {
    auto j = read_config();
    return rom_manager(j).value("download_source", std::string("bootcade"));
}
void set_chosen_source(const std::string& id) {
    auto j = read_config();
    rom_manager(j)["download_source"] = id;
    write_config(j);
}

std::string custom_url(const Source& s, const std::string& system, const std::string& name) {
    std::string u = s.url;
    if (u.find("{name}") == std::string::npos) {
        while (!u.empty() && u.back() == '/') u.pop_back();
        u += "/{name}.{ext}";
    }
    u = replace_all(u, "{system}", url_encode_path(system));
    u = replace_all(u, "{name}", url_encode_path(name));
    return u;
}

bool fetch_file(const std::string& url, const std::string& target, const Fetch& opt,
                const std::atomic<bool>& cancelled, const std::function<void(double)>& progress,
                Result& r) {
    const std::string part = target + ".part";
    std::FILE* f = std::fopen(part.c_str(), "wb");
    if (!f) { r.reason = "cannot write " + part; return false; }
    CURL* curl = curl_easy_init();
    if (!curl) { std::fclose(f); r.reason = "curl"; return false; }
    Transfer t{&cancelled, &progress};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, to_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &r.quota_header);
    if (!opt.cookie_jar.empty()) {
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, opt.cookie_jar.c_str());
        curl_easy_setopt(curl, CURLOPT_COOKIEJAR, opt.cookie_jar.c_str());
    }
    if (!opt.userpwd.empty()) {
        curl_easy_setopt(curl, CURLOPT_USERPWD, opt.userpwd.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_ANY);
    }
    const CURLcode rc = curl_easy_perform(curl);
    char* ctype = nullptr;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.http_status);
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ctype);
    const std::string type = ctype ? ctype : "";
    curl_easy_cleanup(curl);
    std::fclose(f);
    if (!r.quota_header.empty()) std::cerr << "[DOWNLOAD] " << url << " quota headers: " << r.quota_header << std::endl;
    std::error_code ec;
    if (rc == CURLE_ABORTED_BY_CALLBACK) { fs::remove(part, ec); r.reason = _("cancelled"); return false; }
    if (rc != CURLE_OK) {
        fs::remove(part, ec);
        r.reason = curl_easy_strerror(rc);
        r.site_fault = rc == CURLE_COULDNT_CONNECT || rc == CURLE_COULDNT_RESOLVE_HOST || rc == CURLE_OPERATION_TIMEDOUT;
        return false;
    }
    if (r.http_status >= 500) { fs::remove(part, ec); r.reason = "HTTP " + std::to_string(r.http_status); r.site_fault = true; return false; }
    if (r.http_status == 401 || r.http_status == 403) { fs::remove(part, ec); r.reason = _("refused by the site (sign in, or daily quota reached)"); return false; }
    if (r.http_status != 200 || type.rfind("text/html", 0) == 0) {
        fs::remove(part, ec);
        r.reason = r.http_status == 404 || r.http_status == 200 ? std::string(_("not available on this source"))
                                                                : "HTTP " + std::to_string(r.http_status);
        return false;
    }
    r.transferred = true;
    fs::rename(part, target, ec);
    if (ec) { r.reason = ec.message(); return false; }
    r.ok = true;
    r.path = target;
    return true;
}

Result download_custom(const Source& s, const std::string& system, const std::string& name,
                       const std::string& dest_dir, const std::atomic<bool>& cancelled,
                       const std::function<void(double)>& progress) {
    Result r;
    Fetch opt;
    if (!s.user.empty()) opt.userpwd = s.user + ":" + password(s.id);
    // Le modele dit l'extension, ou on essaie zip puis 7z.
    for (const char* ext : {"zip", "7z"}) {
        const std::string url = replace_all(custom_url(s, system, name), "{ext}", ext);
        const std::string target = (fs::path(dest_dir) / (name + "." + ext)).string();
        r = Result{};
        if (fetch_file(url, target, opt, cancelled, progress, r)) return r;
        if (cancelled || r.site_fault || r.http_status == 401 || r.http_status == 403) return r;
        if (s.url.find("{ext}") == std::string::npos && s.url.find("{name}") != std::string::npos) return r;
    }
    return r;
}

}
