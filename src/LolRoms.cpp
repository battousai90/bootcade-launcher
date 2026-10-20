// src/LolRoms.cpp
#include "LolRoms.h"

#include "CurlInit.h"   // sous Windows : les certificats livres avec Bootcade
#include "i18n.h"
#include "RomArchive.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>
#include <vector>

namespace fs = std::filesystem;

namespace LolRoms {
namespace {

constexpr const char* kSite = "https://lolroms.com/";
constexpr const char* kUserAgent = "Mozilla/5.0 (Bootcade)";

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

size_t to_string(char* p, size_t size, size_t n, void* out) {
    static_cast<std::string*>(out)->append(p, size * n);
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

// Un fichier du site vers `target` (par un .part). Faux et `why` si ce n'est
// pas un fichier : page d'erreur, absence, coupure.
bool fetch_file(const std::string& site_path, const std::string& target, const std::atomic<bool>& cancelled,
                const std::function<void(double)>& progress, std::string& why, bool& site_fault) {
    const std::string part = target + ".part";
    std::FILE* f = std::fopen(part.c_str(), "wb");
    if (!f) { why = "cannot write " + part; return false; }
    CURL* curl = curl_easy_init();
    if (!curl) { std::fclose(f); why = "curl"; return false; }
    Transfer t{&cancelled, &progress};
    const std::string url = std::string(kSite) + "?file=" + url_encode(site_path);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, to_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    char* ctype = nullptr;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ctype);
    const std::string type = ctype ? ctype : "";
    curl_easy_cleanup(curl);
    std::fclose(f);
    std::error_code ec;
    if (rc == CURLE_ABORTED_BY_CALLBACK) { fs::remove(part, ec); why = "cancelled"; return false; }
    if (rc != CURLE_OK) {
        fs::remove(part, ec);
        why = curl_easy_strerror(rc);
        site_fault = rc == CURLE_COULDNT_CONNECT || rc == CURLE_COULDNT_RESOLVE_HOST || rc == CURLE_OPERATION_TIMEDOUT;
        return false;
    }
    if (status >= 500) { fs::remove(part, ec); why = "HTTP " + std::to_string(status); site_fault = true; return false; }
    // Un fichier absent rend une page HTML (code 200) : pas un telechargement.
    if (status != 200 || type.rfind("text/html", 0) == 0) {
        fs::remove(part, ec);
        why = _("not available on LolRoms");
        return false;
    }
    fs::rename(part, target, ec);
    if (ec) { why = ec.message(); return false; }
    return true;
}

// Les fichiers d'un dossier du site (ses liens « ?file= »).
std::vector<std::string> list_files(const std::string& site_dir, std::string& why,
                                    std::map<std::string, std::string>* dated = nullptr,
                                    std::map<std::string, std::string>* sized = nullptr) {
    std::vector<std::string> out;
    CURL* curl = curl_easy_init();
    if (!curl) { why = "curl"; return out; }
    std::string body;
    const std::string url = std::string(kSite) + "?dir=" + url_encode(site_dir);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) { why = curl_easy_strerror(rc); return out; }
    const std::string key = "href=\"?file=";
    for (size_t p = body.find(key); p != std::string::npos; p = body.find(key, p + 1)) {
        const size_t s = p + key.size();
        const size_t e = body.find('"', s);
        if (e == std::string::npos) break;
        std::string enc = body.substr(s, e - s), dec;
        for (size_t i = 0; i < enc.size(); ++i) {
            if (enc[i] == '%' && i + 2 < enc.size()) { dec += (char)std::stoi(enc.substr(i + 1, 2), nullptr, 16); i += 2; }
            else if (enc[i] == '+') dec += ' ';
            else dec += enc[i];
        }
        out.push_back(dec);
        // « <span class="file-meta">2026-10-15 • 1.45 MB</span> » suit le lien.
        if (dated) {
            const std::string base = fs::path(dec).filename().string();
            std::string day;
            const size_t meta = body.find("file-meta\">", e);
            const size_t next = body.find(key, e);
            std::string size;
            if (meta != std::string::npos && (next == std::string::npos || meta < next)) {
                day = body.substr(meta + 11, 10);
                // « 2026-10-15 • 1.45 MB » : la taille suit la puce.
                const size_t bullet = body.find("\u2022", meta);
                const size_t end = body.find('<', meta);
                if (bullet != std::string::npos && bullet < end) size = body.substr(bullet + 3, end - bullet - 3);
                while (!size.empty() && size.front() == ' ') size.erase(0, 1);
                while (!size.empty() && size.back() == ' ') size.pop_back();
            }
            if (base.size() > 3 && base.compare(base.size() - 3, 3, ".7z") == 0) {
                (*dated)[base.substr(0, base.size() - 3)] = day;
                if (sized) (*sized)[base.substr(0, base.size() - 3)] = size;
            }
        }
    }
    return out;
}

// Les sous-dossiers d'un dossier du site (ses liens « ?dir= »), noms seuls.
std::vector<std::string> list_dirs(const std::string& site_dir, bool& ok) {
    std::vector<std::string> out;
    ok = false;
    CURL* curl = curl_easy_init();
    if (!curl) return out;
    std::string body;
    const std::string url = std::string(kSite) + "?dir=" + url_encode(site_dir);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK || status != 200) return out;
    ok = true;
    const std::string prefix = "href=\"?dir=" + url_encode(site_dir) + "%2F";
    for (size_t p = body.find(prefix); p != std::string::npos; p = body.find(prefix, p + 1)) {
        const size_t s = p + prefix.size();
        const size_t e = body.find_first_of("\"&", s);
        if (e == std::string::npos) break;
        out.push_back(body.substr(s, e - s));
    }
    return out;
}

} // namespace

bool Catalog::has(const std::string& emulator, const std::string& system, const std::string& name,
                  bool is_chd, bool& known, std::string* date, std::string* size) {
    known = false;
    const std::string folder = folder_for(emulator, system);
    if (folder.empty()) { known = true; return false; }
    const std::string dir = is_chd ? folder + "/chd" : folder;
    auto it = m_dirs.find(dir);
    if (it == m_dirs.end()) {
        Listing l;
        if (is_chd) {
            for (auto& d : list_dirs(dir, l.ok)) l.names.emplace(d, std::string());
        } else {
            std::string why;
            const auto files = list_files(dir, why, &l.names, &l.sizes);
            l.ok = why.empty();
        }
        it = m_dirs.emplace(dir, std::move(l)).first;
    }
    if (!it->second.ok) return false;
    known = true;
    auto n = it->second.names.find(name);
    if (n == it->second.names.end()) return false;
    if (date) *date = n->second;
    if (size) { auto z = it->second.sizes.find(name); if (z != it->second.sizes.end()) *size = z->second; }
    return true;
}

bool has_all_crcs(const std::string& archive, const std::vector<unsigned long>& crcs) {
    std::vector<RomArchive::Entry> entries;
    if (!RomArchive::read_entries(archive, entries)) return false;
    std::set<unsigned long> got;
    for (const auto& e : entries) got.insert(e.crc);
    for (unsigned long c : crcs) if (!got.count(c)) return false;
    return true;
}

std::string folder_for(const std::string& emulator, const std::string& system) {
    if (emulator == "mame") return "Arcade/MAME";
    if (emulator != "fbneo") return {};
    static const std::map<std::string, std::string> fbneo = {
        {"Arcade", "arcade"}, {"Neo Geo", "arcade"},
        {"Astrocade Home Computer", "astrocade"}, {"ColecoVision", "coleco"},
        {"FDS", "fds"}, {"Fairchild Channel F", "channelf"}, {"GBA", "gba"},
        {"Game Gear", "gamegear"}, {"MSX 1", "msx"}, {"Master System", "sms"},
        {"Megadrive", "megadrive"}, {"NES", "nes"}, {"Neo Geo Pocket", "ngp"},
        {"PC-Engine", "pce"}, {"SNES", "snes"}, {"Sega SG-1000", "sg1000"},
        {"SuprGrafx", "sgx"}, {"TurboGrafx 16", "tg16"}, {"ZX Spectrum", "spectrum"},
    };
    auto it = fbneo.find(system);
    return it == fbneo.end() ? std::string() : "Arcade/FBNeo/" + it->second;
}

Result download(const std::string& emulator, const std::string& system, const std::string& name,
                bool is_chd, const std::string& dest_dir, const std::atomic<bool>& cancelled,
                const std::function<void(double)>& progress) {
    Result r;
    const std::string folder = folder_for(emulator, system);
    if (folder.empty()) { r.reason = _("not available on LolRoms"); return r; }

    if (is_chd) {
        // Un CHD a ses disques dans un dossier a son nom.
        std::string why;
        const std::string dir = folder + "/chd/" + name;
        std::vector<std::string> files;
        for (const auto& f : list_files(dir, why))
            if (f.size() > 4 && f.compare(f.size() - 4, 4, ".chd") == 0) files.push_back(f);
        if (files.empty()) { r.reason = why.empty() ? _("not available on LolRoms") : why; return r; }
        const std::string out_dir = (fs::path(dest_dir) / name).string();
        std::error_code ec;
        fs::create_directories(out_dir, ec);
        for (size_t i = 0; i < files.size(); ++i) {
            const std::string target = (fs::path(out_dir) / fs::path(files[i]).filename()).string();
            if (!fetch_file(files[i], target, cancelled,
                            [&](double f) { if (progress) progress((i + f) / files.size()); },
                            r.reason, r.site_fault))
                return r;
        }
        r.ok = true;
        r.path = out_dir;
        return r;
    }

    const std::string target = (fs::path(dest_dir) / (name + ".7z")).string();
    if (!fetch_file(folder + "/" + name + ".7z", target, cancelled, progress, r.reason, r.site_fault)) return r;
    r.ok = true;
    r.path = target;
    return r;
}

}
