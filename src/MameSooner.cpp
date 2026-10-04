// src/MameSooner.cpp
#include "MameSooner.h"

#include "AppContext.h"
#include "DatSource.h"
#include "MameCatalog.h"

#include <curl/curl.h>
#include <glibmm.h>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <zip.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace MameSooner {

namespace {

constexpr const char* kUserAgent = "Bootcade (MAME Sooner)";

size_t to_string(char* p, size_t size, size_t n, void* out) {
    static_cast<std::string*>(out)->append(p, size * n);
    return size * n;
}

size_t to_file(char* p, size_t size, size_t n, void* out) {
    return std::fwrite(p, size, n, static_cast<std::FILE*>(out)) * size;
}

struct Transfer {
    const std::function<void(double, const std::string&)>* progress = nullptr;
    const std::function<bool()>* cancelled = nullptr;
};

int on_progress(void* data, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
    auto* t = static_cast<Transfer*>(data);
    if (t->cancelled && *t->cancelled && (*t->cancelled)()) return 1;
    if (t->progress && *t->progress && total > 0)
        (*t->progress)(0.9 * (double)now / (double)total,
                       std::to_string(now >> 20) + " / " + std::to_string(total >> 20) + " MB");
    return 0;
}

std::string info_url() { return kInfoUrl; }

nlohmann::json read_config() {
    nlohmann::json j;
    std::ifstream in(AppContext::get_config_path());
    if (in) { try { in >> j; } catch (...) { j = nlohmann::json::object(); } }
    return j.is_object() ? j : nlohmann::json::object();
}

// L'URL du zip : a cote de la fiche, sous le nom qu'elle donne.
std::string zip_url(const Info& info) {
    std::string base = info_url();
    base = base.substr(0, base.rfind('/') + 1);
    return base + (info.file.empty() ? std::string("mame-sooner.zip") : info.file);
}

// Extrait une entree du zip vers `dest`. false si elle manque ou ne s'ecrit pas.
bool extract(zip_t* za, const char* name, const std::string& dest) {
    zip_file_t* zf = zip_fopen(za, name, 0);
    if (!zf) return false;
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    char buf[1 << 16];
    zip_int64_t n;
    bool ok = (bool)out;
    while (ok && (n = zip_fread(zf, buf, sizeof buf)) > 0) {
        out.write(buf, n);
        ok = (bool)out;
    }
    zip_fclose(zf);
    out.close();
    if (!ok || !out) return false;
    ::chmod(dest.c_str(), 0755);
    return true;
}

} // namespace

Info fetch_info() {
    Info info;
    CURL* curl = curl_easy_init();
    if (!curl) { info.error = "curl"; return info; }
    std::string body;
    const std::string url = info_url();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) { info.error = curl_easy_strerror(rc); return info; }
    if (status != 200) { info.error = "HTTP " + std::to_string(status); return info; }
    try {
        const auto j = nlohmann::json::parse(body);
        if (j.value("schema_version", 0) != 1) {
            info.error = "unknown schema_version";
            return info;
        }
        info.build  = j.value("build", std::string());
        info.commit = j.value("commit", std::string());
        info.date   = j.value("date", std::string());
        info.file   = j.value("file", std::string("mame-sooner.zip"));
        info.sha256 = j.value("sha256", std::string());
        info.size   = j.value("size", (std::uint64_t)0);
        info.ok     = !info.commit.empty() && !info.sha256.empty() && info.size > 0;
        if (!info.ok) info.error = "incomplete mame-sooner.json";
    } catch (const std::exception& e) {
        info.error = e.what();
    }
    return info;
}

std::string channel() {
    const auto j = read_config();
    const std::string c = j.value("mame_channel", std::string(kChannelRelease));
    return c == kChannelSooner ? kChannelSooner : kChannelRelease;
}

std::string install_dir() {
    if (const char* isolated = std::getenv("BOOTCADE_CONFIG_DIR"); isolated && *isolated)
        return std::string(isolated) + "/mame-sooner";
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.local/share/bootcade/mame-sooner";
}

std::string executable() {
    const std::string exe = install_dir() + "/mame";
    std::error_code ec;
    return fs::is_regular_file(exe, ec) ? exe : std::string();
}

Installed installed() {
    Installed out;
    std::ifstream in(install_dir() + "/installed.json");
    if (!in) return out;
    try {
        nlohmann::json j;
        in >> j;
        out.build  = j.value("build", std::string());
        out.commit = j.value("commit", std::string());
        out.date   = j.value("date", std::string());
    } catch (...) {}
    if (executable().empty()) out = Installed{};
    return out;
}

bool install(const Info& info, std::string& error,
             const std::function<void(double, const std::string&)>& progress,
             const std::function<bool()>& cancelled) {
    const std::string dir = install_dir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) { error = ec.message(); return false; }

    // Le zip passe par un nom cache ; l'ancienne installation reste en place
    // jusqu'a ce que la nouvelle soit verifiee et extraite a cote.
    const std::string zip_path = dir + "/.mame-sooner.zip.part";
    std::FILE* f = std::fopen(zip_path.c_str(), "wb");
    if (!f) { error = "cannot write " + zip_path; return false; }
    CURL* curl = curl_easy_init();
    if (!curl) { std::fclose(f); error = "curl"; return false; }
    Transfer t{&progress, &cancelled};
    const std::string url = zip_url(info);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, to_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    std::fclose(f);
    auto fail = [&](const std::string& why) {
        error = why;
        fs::remove(zip_path, ec);
        fs::remove_all(dir + "/.new", ec);
        return false;
    };
    if (rc == CURLE_ABORTED_BY_CALLBACK) return fail("cancelled");
    if (rc != CURLE_OK) return fail(curl_easy_strerror(rc));

    if (progress) progress(0.92, "SHA-256");
    if (fs::file_size(zip_path, ec) != info.size) return fail("size mismatch");
    if (DatSource::sha256_of(zip_path) != info.sha256) return fail("SHA-256 mismatch");

    if (progress) progress(0.95, "unzip");
    int zerr = 0;
    zip_t* za = zip_open(zip_path.c_str(), ZIP_RDONLY, &zerr);
    if (!za) return fail("not a zip archive");
    const std::string fresh = dir + "/.new";
    fs::remove_all(fresh, ec);
    fs::create_directories(fresh, ec);
    const bool ok = extract(za, "mame", fresh + "/mame");
    // chdman n'est pas indispensable : son absence n'empeche pas de jouer.
    extract(za, "chdman", fresh + "/chdman");
    zip_close(za);
    if (!ok) return fail("no mame executable in the archive");

    nlohmann::json note = {{"build", info.build}, {"commit", info.commit}, {"date", info.date}};
    { std::ofstream o(fresh + "/installed.json"); o << note.dump(2) << "\n"; }
    for (const char* name : {"mame", "chdman", "installed.json"}) {
        if (!fs::exists(fresh + "/" + name, ec)) continue;
        fs::rename(fresh + "/" + name, dir + "/" + name, ec);
        if (ec) return fail(ec.message());
    }
    fs::remove_all(fresh, ec);
    fs::remove(zip_path, ec);
    if (progress) progress(1.0, "");
    return true;
}

std::string missing_libraries(const std::string& exe) {
    if (exe.empty()) return {};
    std::string out, err;
    int status = 0;
    try {
        Glib::spawn_sync("", AppContext::host_command({"ldd", exe}),
                         Glib::SPAWN_SEARCH_PATH, {}, &out, &err, &status);
    } catch (const Glib::Error&) {
        return {};
    }
    std::string missing;
    std::istringstream lines(out);
    for (std::string line; std::getline(lines, line);) {
        if (line.find("not found") == std::string::npos) continue;
        const auto b = line.find_first_not_of(" \t");
        const auto e = line.find(' ', b);
        if (b == std::string::npos) continue;
        if (!missing.empty()) missing += ' ';
        missing += line.substr(b, e == std::string::npos ? std::string::npos : e - b);
    }
    return missing;
}

std::string resolve_executable(const std::string& chan, const std::string& typed) {
    if (chan == kChannelSooner) {
        const std::string sooner = executable();
        if (!sooner.empty()) return sooner;
    }
    if (!typed.empty()) return typed;
    return MameCatalog::find_executable();
}

std::string resolve_executable() {
    const auto j = read_config();
    return resolve_executable(channel(), j.value("mame_executable", std::string()));
}

} // namespace MameSooner
