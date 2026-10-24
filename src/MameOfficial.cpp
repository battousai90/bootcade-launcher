// src/MameOfficial.cpp
#include "MameOfficial.h"

#include "DatSource.h"
#include "MameSooner.h"

#include <archive.h>
#include <archive_entry.h>
#include "CurlInit.h"   // sous Windows : les certificats livres avec Bootcade
#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace MameOfficial {
namespace {

constexpr const char* kLatest = "https://api.github.com/repos/mamedev/mame/releases/latest";
constexpr const char* kUserAgent = "Bootcade (MAME)";
constexpr const char* kApi = "https://api.github.com/repos/mamedev/mame";
#ifdef _WIN32
constexpr const char* kExe = "mame.exe";
#else
constexpr const char* kExe = "mame";
#endif

size_t to_string(char* p, size_t size, size_t n, void* out) {
    static_cast<std::string*>(out)->append(p, size * n);
    return size * n;
}

size_t to_file(char* p, size_t size, size_t n, void* out) {
    return std::fwrite(p, size, n, static_cast<std::FILE*>(out)) * size;
}

struct Transfer {
    const std::function<void(double, const std::string&)>* progress;
    const std::function<bool()>* cancelled;
};

int on_progress(void* data, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
    auto* t = static_cast<Transfer*>(data);
    if (t->cancelled && *t->cancelled && (*t->cancelled)()) return 1;
    if (t->progress && *t->progress && total > 0)
        (*t->progress)(0.85 * (double)now / (double)total,
                       std::to_string(now / 1048576) + " / " + std::to_string(total / 1048576) + " MB");
    return 0;
}

// Extrait toute l'archive dans `dest`, sans jamais ecrire en dehors.
bool extract_all(const std::string& archive_path, const std::string& dest, std::string& error,
                 const std::function<bool()>& cancelled) {
    archive* in = archive_read_new();
    archive_read_support_filter_all(in);
    archive_read_support_format_all(in);
    archive* out = archive_write_disk_new();
    archive_write_disk_set_options(out, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_SECURE_NODOTDOT
                                        | ARCHIVE_EXTRACT_SECURE_SYMLINKS);
    archive_write_disk_set_standard_lookup(out);
    bool ok = archive_read_open_filename(in, archive_path.c_str(), 1 << 16) == ARCHIVE_OK;
    if (!ok) error = archive_error_string(in) ? archive_error_string(in) : "unreadable archive";
    archive_entry* e = nullptr;
    while (ok) {
        const int r = archive_read_next_header(in, &e);
        if (r == ARCHIVE_EOF) break;
        if (r != ARCHIVE_OK && r != ARCHIVE_WARN) { error = archive_error_string(in); ok = false; break; }
        if (cancelled && cancelled()) { error = "cancelled"; ok = false; break; }
        // Un chemin absolu ou un lecteur (« C: ») sortirait du dossier : refuse.
        const std::string inner = archive_entry_pathname(e) ? archive_entry_pathname(e) : "";
        if (inner.empty() || inner[0] == '/' || inner[0] == '\\' || inner.find(':') != std::string::npos) {
            error = "unsafe path in archive: " + inner; ok = false; break;
        }
        const std::string target = (fs::path(dest) / inner).string();
        archive_entry_set_pathname(e, target.c_str());
        if (archive_write_header(out, e) != ARCHIVE_OK) { error = archive_error_string(out); ok = false; break; }
        const void* buf; size_t len; la_int64_t off;
        int rr;
        while ((rr = archive_read_data_block(in, &buf, &len, &off)) == ARCHIVE_OK)
            if (archive_write_data_block(out, buf, len, off) != ARCHIVE_OK) { rr = ARCHIVE_FATAL; break; }
        if (rr != ARCHIVE_EOF) { error = archive_error_string(in) ? archive_error_string(in) : "write error"; ok = false; break; }
        archive_write_finish_entry(out);
    }
    archive_read_free(in);
    archive_write_free(out);
    return ok;
}

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
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
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

#ifndef _WIN32
/* Sous Linux, MAMEdev ne publie aucun executable. Leur CI Linux construit
 * pourtant le commit de chaque version (passage declenche par le tag) et en
 * laisse le MAME complet en artefact, que nightly.link sert sans jeton. Il
 * ne vit que 90 jours chez GitHub : passe ce delai, rien a telecharger. */
void find_linux_build(Release& r) {
    nlohmann::json runs, arts;
    if (!get_json(std::string(kApi) + "/actions/workflows/ci-linux.yml/runs?branch=" + r.tag
                      + "&event=push&per_page=10", runs, r.error))
        return;
    std::string run_id;
    for (const auto& run : runs.value("workflow_runs", nlohmann::json::array()))
        if (run.value("status", "") == "completed" && run.value("conclusion", "") == "success") {
            run_id = std::to_string(run.value("id", (std::uint64_t)0));
            break;
        }
    if (run_id.empty()) { r.error = "no Linux build of " + r.tag; return; }
    if (!get_json(std::string(kApi) + "/actions/runs/" + run_id + "/artifacts", arts, r.error)) return;
    for (const auto& a : arts.value("artifacts", nlohmann::json::array())) {
        const std::string name = a.value("name", std::string());
        // Le MAME complet construit par clang ; mametiny n'a qu'une poignee de pilotes.
        if (name.rfind("mame-linux-clang-", 0) != 0 || a.value("expired", true)) continue;
        r.url = "https://nightly.link/mamedev/mame/actions/runs/" + run_id + "/" + name + ".zip";
        return;
    }
    r.error = "no Linux build of " + r.tag;
}
#endif

} // namespace

Release fetch_latest() {
    Release r;
    CURL* curl = curl_easy_init();
    if (!curl) { r.error = "curl"; return r; }
    std::string body;
    struct curl_slist* headers = curl_slist_append(nullptr, "Accept: application/vnd.github+json");
    curl_easy_setopt(curl, CURLOPT_URL, kLatest);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) { r.error = curl_easy_strerror(rc); return r; }
    if (status != 200)  { r.error = "HTTP " + std::to_string(status); return r; }
    try {
        const auto j = nlohmann::json::parse(body);
        r.tag = j.value("tag_name", std::string());
        for (const auto& a : j.value("assets", nlohmann::json::array())) {
            const std::string name = a.value("name", std::string());
            // mame0289b_x64.exe : la version 64 bits pour PC (pas arm64, pas les sources).
            if (name.size() < 9 || name.compare(name.size() - 9, 9, "b_x64.exe") != 0) continue;
            r.url  = a.value("browser_download_url", std::string());
            r.size = a.value("size", (std::uint64_t)0);
            std::string digest = a.value("digest", std::string());
            if (digest.rfind("sha256:", 0) == 0) r.sha256 = digest.substr(7);
        }
    } catch (const std::exception& e) { r.error = e.what(); return r; }
#ifndef _WIN32
    r.url.clear();
    r.size = 0;
    r.sha256.clear();
    if (!r.tag.empty()) find_linux_build(r);
    r.ok = !r.tag.empty() && !r.url.empty();
#else
    r.ok = !r.tag.empty() && !r.url.empty();
    if (!r.ok) r.error = "no Windows x64 build in the latest MAME release";
#endif
    return r;
}

std::string install_dir() {
    // A cote du MAME Sooner : meme racine, meme isolement d'un build de chantier.
    return (fs::path(MameSooner::install_dir()).parent_path() / "mame-official").string();
}

std::string executable() {
    const std::string exe = install_dir() + "/current/" + kExe;
    std::error_code ec;
    return fs::is_regular_file(exe, ec) ? exe : std::string();
}

std::string installed_tag() {
    if (executable().empty()) return {};
    std::ifstream in(install_dir() + "/current/installed.json");
    if (!in) return {};
    try { nlohmann::json j; in >> j; return j.value("tag", std::string()); } catch (...) { return {}; }
}

std::string version_of(const std::string& tag) {
    std::string digits;
    for (char c : tag) if (c >= '0' && c <= '9') digits += c;
    if (digits.size() < 2 || tag.rfind("mame", 0) != 0) return tag;
    return digits.substr(0, 1) + "." + digits.substr(1);
}

bool install(const Release& release, std::string& error,
             const std::function<void(double, const std::string&)>& progress,
             const std::function<bool()>& cancelled) {
    const std::string dir = install_dir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) { error = ec.message(); return false; }

    const std::string part = dir + "/.download.part";
    const std::string fresh = dir + "/.new";
    auto fail = [&](const std::string& why) {
        error = why;
        fs::remove(part, ec);
        fs::remove_all(fresh, ec);
        return false;
    };

    std::FILE* f = std::fopen(part.c_str(), "wb");
    if (!f) { error = "cannot write " + part; return false; }
    CURL* curl = curl_easy_init();
    if (!curl) { std::fclose(f); return fail("curl"); }
    Transfer t{&progress, &cancelled};
    curl_easy_setopt(curl, CURLOPT_URL, release.url.c_str());
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
    if (rc == CURLE_ABORTED_BY_CALLBACK) return fail("cancelled");
    if (rc != CURLE_OK) return fail(curl_easy_strerror(rc));

    if (progress) progress(0.86, "SHA-256");
    if (release.size && fs::file_size(part, ec) != release.size) return fail("size mismatch");
    if (!release.sha256.empty() && DatSource::sha256_of(part) != release.sha256)
        return fail("SHA-256 mismatch");

    if (progress) progress(0.88, "Extracting...");
    fs::remove_all(fresh, ec);
    fs::create_directories(fresh, ec);
    if (!extract_all(part, fresh, error, cancelled)) return fail(error);
    std::error_code ec2;
    if (!fs::is_regular_file(fresh + "/" + kExe, ec2)) return fail(std::string("no ") + kExe + " in the archive");
#ifndef _WIN32
    // Un artefact GitHub ne garde pas les droits des fichiers.
    for (const char* name : {"mame", "chdman", "unidasm"})
        if (fs::exists(fresh + "/" + name, ec2))
            fs::permissions(fresh + "/" + name, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec
                                                    | fs::perms::others_read | fs::perms::others_exec, ec2);
#endif
    { std::ofstream o(fresh + "/installed.json"); o << nlohmann::json{{"tag", release.tag}}.dump(2) << "\n"; }

    // L'ancienne installation ne part qu'une fois la nouvelle complete.
    fs::remove_all(dir + "/old", ec);
    if (fs::exists(dir + "/current", ec)) fs::rename(dir + "/current", dir + "/old", ec);
    fs::rename(fresh, dir + "/current", ec);
    if (ec) return fail(ec.message());
    fs::remove_all(dir + "/old", ec);
    fs::remove(part, ec);
    if (progress) progress(1.0, "");
    return true;
}

}
