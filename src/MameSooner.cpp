// src/MameSooner.cpp
#include "MameSooner.h"
#include "Platform.h"

#include "AppContext.h"
#include "DatSource.h"
#include "DefaultFolders.h"
#include "MameCatalog.h"

#include "CurlInit.h"
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
#ifdef _WIN32
constexpr const char* kExe = "mame.exe";
constexpr const char* kChdman = "chdman.exe";
#else
constexpr const char* kExe = "mame";
constexpr const char* kChdman = "chdman";
#endif

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
    // Sous Windows, la fiche donne l'adresse entiere (nightly.link).
    if (info.file.rfind("https://", 0) == 0) return info.file;
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

#ifdef _WIN32
// GET sur l'API GitHub (sans jeton : 60 appels par heure, bien assez).
bool get_json(const std::string& url, nlohmann::json& out, std::string& error) {
    CURL* curl = curl_easy_init();
    if (!curl) { error = "curl"; return false; }
    std::string body;
    struct curl_slist* headers = curl_slist_append(nullptr, "Accept: application/vnd.github+json");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) { error = curl_easy_strerror(rc); return false; }
    if (status != 200)  { error = "HTTP " + std::to_string(status); return false; }
    try { out = nlohmann::json::parse(body); } catch (const std::exception& e) { error = e.what(); return false; }
    return true;
}

/* Le serveur Bootcade ne republie que le Sooner Linux. Sous Windows, on prend
 * directement le dernier passage reussi de la CI Windows de MAMEdev (branche
 * master) : son artefact mame-windows-gcc-x64 est un MAME complet, sans DLL,
 * que nightly.link sert sans jeton. Ni taille ni SHA-256 connus d'avance :
 * l'executable extrait doit repondre a -version pour etre garde. */
Info fetch_windows_build() {
    Info info;
    constexpr const char* api = "https://api.github.com/repos/mamedev/mame";
    nlohmann::json runs, arts;
    if (!get_json(std::string(api) + "/actions/workflows/ci-windows.yml/runs?branch=master&event=push&per_page=30",
                  runs, info.error))
        return info;
    // Le plus recent qui a reussi, choisi ici : le filtre status=success de
    // l'API n'est pas fiable.
    nlohmann::json best;
    for (const auto& r : runs.value("workflow_runs", nlohmann::json::array()))
        if (r.value("status", "") == "completed" && r.value("conclusion", "") == "success"
            && (best.is_null() || r.value("created_at", "") > best.value("created_at", "")))
            best = r;
    if (best.is_null()) { info.error = "no successful Windows build"; return info; }
    const std::string run_id = std::to_string(best.value("id", (std::uint64_t)0));
    if (!get_json(std::string(api) + "/actions/runs/" + run_id + "/artifacts", arts, info.error)) return info;
    for (const auto& a : arts.value("artifacts", nlohmann::json::array())) {
        const std::string name = a.value("name", std::string());
        if (name.rfind("mame-windows-gcc-x64-", 0) != 0 || a.value("expired", true)) continue;
        info.commit = best.value("head_sha", std::string());
        info.date   = best.value("created_at", std::string());
        info.file   = "https://nightly.link/mamedev/mame/actions/runs/" + run_id + "/" + name + ".zip";
        info.ok     = !info.commit.empty();
        return info;
    }
    info.error = "no Windows build in the latest MAME CI run";
    return info;
}
#endif

} // namespace

Info fetch_info() {
#ifdef _WIN32
    return fetch_windows_build();
#endif
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

std::string default_install_dir() {
    // A cote de FinalBurn Neo (Emulators/FBNeo), dans le dossier Bootcade.
    return DefaultFolders::sub("Emulators/MAME Sooner");
}

std::string legacy_install_dir() {
    if (const char* isolated = std::getenv("BOOTCADE_CONFIG_DIR"); isolated && *isolated)
        return std::string(isolated) + "/mame-sooner";
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.local/share/bootcade/mame-sooner";
}

std::string install_dir() {
    const std::string chosen = read_config().value("mame_sooner_dir", std::string());
    if (!chosen.empty()) return chosen;
    // Un Sooner installe avant ce dossier par defaut reste ou il est : le
    // joueur n'a rien a refaire.
    std::error_code ec;
    if (fs::is_regular_file(legacy_install_dir() + "/" + kExe, ec)) return legacy_install_dir();
    return default_install_dir();
}

bool set_install_dir(const std::string& dir, std::string& error) {
    const std::string from = install_dir();
    std::error_code ec;
    if (fs::path(dir).lexically_normal() == fs::path(from).lexically_normal()) return true;
    fs::create_directories(dir, ec);
    if (ec) { error = ec.message(); return false; }
    // Ce qui est deja installe suit : seulement nos trois fichiers, le reste
    // du dossier n'est pas a nous.
    for (const char* name : {kExe, kChdman, "installed.json"}) {
        const fs::path src = fs::path(from) / name, dest = fs::path(dir) / name;
        if (!fs::exists(src, ec)) continue;
        fs::rename(src, dest, ec);
        if (ec) {
            // Un autre disque : copier, puis retirer l'original.
            ec.clear();
            fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
            if (ec) { error = ec.message(); return false; }
            fs::permissions(dest, fs::status(src).permissions(), ec);
            fs::remove(src, ec);
        }
    }
    auto j = read_config();
    j["mame_sooner_dir"] = dir == default_install_dir() ? std::string() : dir;
    std::ofstream fo(AppContext::get_config_path());
    if (!fo) { error = "cannot write " + AppContext::get_config_path(); return false; }
    fo << j.dump(4);
    return true;
}

std::string executable() {
    const std::string exe = install_dir() + "/" + kExe;
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
    if (info.size && fs::file_size(zip_path, ec) != info.size) return fail("size mismatch");
    if (!info.sha256.empty() && DatSource::sha256_of(zip_path) != info.sha256) return fail("SHA-256 mismatch");

    if (progress) progress(0.95, "unzip");
    int zerr = 0;
    zip_t* za = zip_open(zip_path.c_str(), ZIP_RDONLY, &zerr);
    if (!za) return fail("not a zip archive");
    const std::string fresh = dir + "/.new";
    fs::remove_all(fresh, ec);
    fs::create_directories(fresh, ec);
    const bool ok = extract(za, kExe, fresh + "/" + kExe);
    // chdman n'est pas indispensable : son absence n'empeche pas de jouer.
    extract(za, kChdman, fresh + "/" + kChdman);
    zip_close(za);
    if (!ok) return fail("no mame executable in the archive");

    // Sans fiche (Windows), le build se lit sur l'executable lui-meme : un
    // fichier qui ne repond pas n'est pas un MAME.
    std::string build = info.build;
    if (build.empty()) {
        build = MameCatalog::installed_build(fresh + "/" + kExe);
        if (build.empty()) return fail("the downloaded MAME does not start");
    }
    nlohmann::json note = {{"build", build}, {"commit", info.commit}, {"date", info.date}};
    { std::ofstream o(fresh + "/installed.json"); o << note.dump(2) << "\n"; }
    for (const char* name : {kExe, kChdman, "installed.json"}) {
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
