// src/AppUpdater.cpp
#include "AppUpdater.h"

#include "AppContext.h"
#include "DatSource.h"

#include <curl/curl.h>
#include <glibmm.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace AppUpdater {
namespace {

const char* const kAppId = "io.github.battousai90.Bootcade";

// Le chemin de l'executable en cours. Lu une fois, AVANT la mise a jour :
// une fois le fichier remplace, /proc/self/exe se termine par « (deleted) ».
const std::string& own_executable() {
    static const std::string path = [] {
        std::error_code ec;
        std::string p = fs::read_symlink("/proc/self/exe", ec).string();
        const std::string deleted = " (deleted)";
        if (p.size() > deleted.size() && p.compare(p.size() - deleted.size(), deleted.size(), deleted) == 0)
            p.erase(p.size() - deleted.size());
        return p;
    }();
    return path;
}

// Une commande, sur l'hote quand Bootcade est en Flatpak. Rend son code de
// sortie (-1 si elle n'a pas pu demarrer) et, au besoin, sa sortie.
int run(const std::vector<std::string>& args, std::string* out = nullptr) {
    std::string so, se;
    int status = 0;
    try {
        Glib::spawn_sync("", AppContext::host_command(args),
                         Glib::SPAWN_SEARCH_PATH, Glib::SlotSpawnChildSetup(),
                         &so, &se, &status);
    } catch (const Glib::Error&) {
        return -1;
    }
    if (out) *out = so.empty() ? se : so;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

size_t to_file(char* p, size_t size, size_t n, void* f) {
    return std::fwrite(p, size, n, static_cast<std::FILE*>(f)) * size;
}
size_t to_string(char* p, size_t size, size_t n, void* s) {
    static_cast<std::string*>(s)->append(p, size * n);
    return size * n;
}

struct Transfer { const Progress* progress; };
int on_progress(void* data, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
    auto* t = static_cast<Transfer*>(data);
    if (t->progress && *t->progress && total > 0)
        (*t->progress)(0.85 * (double)now / (double)total,
                       std::to_string(now >> 20) + " / " + std::to_string(total >> 20) + " MB");
    return 0;
}

// Un telechargement vers un fichier (ou une chaine si `file` est vide).
std::string download(const std::string& url, const std::string& file,
                     std::string* body, const Progress* progress) {
    CURL* curl = curl_easy_init();
    if (!curl) return "curl";
    std::FILE* f = nullptr;
    if (!file.empty()) {
        f = std::fopen(file.c_str(), "wb");
        if (!f) { curl_easy_cleanup(curl); return "cannot write " + file; }
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, to_file);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
    } else {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, to_string);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
    }
    Transfer t{progress};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "bootcade");
    if (progress) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
    }
    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (f) std::fclose(f);
    return rc == CURLE_OK ? std::string() : curl_easy_strerror(rc);
}

bool ends_with(const std::string& s, const std::string& end) {
    return s.size() >= end.size() && s.compare(s.size() - end.size(), end.size(), end) == 0;
}

// Le fichier de la release qui correspond au mode d'installation. Choisi par
// sa terminaison : les noms portent la version, et le Flatpak un « v » de plus.
const std::pair<std::string, std::string>* pick(const Assets& assets, Install how) {
    const char* end = nullptr;
    switch (how) {
        case Install::AppImage: end = "-x86_64.AppImage"; break;
        case Install::Flatpak:  end = "-x86_64.flatpak";  break;
        case Install::Deb:      end = "_amd64.deb";       break;
        case Install::Tarball:  end = "-x86_64.tar.gz";   break;
        case Install::Unknown:  return nullptr;
    }
    for (const auto& a : assets) if (ends_with(a.first, end)) return &a;
    return nullptr;
}

// L'empreinte attendue, lue dans SHA256SUMS (« empreinte  nom »).
std::string expected_sha256(const Assets& assets, const std::string& name) {
    for (const auto& a : assets) {
        if (a.first != "SHA256SUMS") continue;
        std::string body;
        if (!download(a.second, "", &body, nullptr).empty()) return {};
        std::istringstream lines(body);
        std::string line;
        while (std::getline(lines, line)) {
            std::istringstream words(line);
            std::string hash, file;
            words >> hash >> file;
            if (!file.empty() && file[0] == '*') file.erase(0, 1);
            if (file == name) return hash;
        }
    }
    return {};
}

std::string install_appimage(const std::string& file) {
    const char* env = std::getenv("APPIMAGE");
    const std::string target = env ? env : "";
    if (target.empty()) return "AppImage path unknown";
    // Meme dossier, puis renommage : l'ancienne AppImage n'est remplacee que
    // par une copie complete, jamais par un fichier a moitie ecrit.
    const std::string staged = target + ".update";
    std::error_code ec;
    fs::copy_file(file, staged, fs::copy_options::overwrite_existing, ec);
    if (ec) return "cannot write next to " + target + " : " + ec.message();
    fs::permissions(staged, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                            fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace, ec);
    fs::rename(staged, target, ec);
    if (ec) { fs::remove(staged, ec); return "cannot replace " + target; }
    return {};
}

std::string install_flatpak(const std::string& file) {
    std::string out;
    // Installe pour l'utilisateur ou pour toute la machine : on reinstalle au
    // meme endroit. Le second cas demande le mot de passe du systeme.
    const bool user = run({"flatpak", "info", "--user", kAppId}) == 0;
    std::string origin;
    run({"flatpak", "info", "--show-origin", kAppId}, &origin);
    while (!origin.empty() && (origin.back() == '\n' || origin.back() == ' ')) origin.pop_back();
    // Venu d'un vrai depot (Flathub) : c'est lui qui fait foi.
    if (origin == "flathub") {
        if (run({"flatpak", "update", user ? "--user" : "--system", "-y", "--noninteractive", kAppId}, &out) != 0)
            return out;
        return {};
    }
    // --reinstall : le paquet d'une release remplace celui d'une autre ; les
    // donnees du joueur (~/.var/app) ne sont pas touchees.
    if (run({"flatpak", "install", user ? "--user" : "--system", "--reinstall", "-y",
             "--noninteractive", "--bundle", file}, &out) != 0)
        return out;
    return {};
}

std::string install_deb(const std::string& file) {
    std::string out;
    // Un paquet systeme ne s'installe jamais sans le mot de passe : pkexec
    // ouvre la fenetre d'authentification du bureau.
    if (run({"pkexec", "apt-get", "install", "-y", "--allow-downgrades", file}, &out) != 0)
        return out.empty() ? "apt-get failed" : out;
    return {};
}

std::string install_tarball(const std::string& file) {
    // L'archive contient « bootcade-X.Y.Z-x86_64/bin/bootcade » : son dossier
    // racine est donc celui qui contient bin/.
    const fs::path root = fs::path(own_executable()).parent_path().parent_path();
    if (root.empty()) return "install folder unknown";
    std::vector<std::string> cmd{"tar", "-xzf", file, "--strip-components=1", "-C", root.string()};
    // Un dossier du systeme (/opt, /usr/local) : il faut le mot de passe.
    if (::access(root.c_str(), W_OK) != 0) cmd.insert(cmd.begin(), "pkexec");
    std::string out;
    if (run(cmd, &out) != 0) return out.empty() ? "tar failed" : out;
    return {};
}

}  // namespace

Install detect() {
    if (AppContext::in_flatpak()) return Install::Flatpak;
    if (const char* a = std::getenv("APPIMAGE"); a && *a) return Install::AppImage;
    const std::string& exe = own_executable();
    if (exe.empty()) return Install::Unknown;
    // dpkg connait-il ce fichier ? Alors il vient du .deb.
    std::string out;
    if (run({"dpkg", "-S", exe}, &out) == 0 && out.rfind("bootcade:", 0) == 0) return Install::Deb;
    // Sinon, la disposition de l'archive : <racine>/bin/bootcade et
    // <racine>/share/bootcade a cote. Un build de developpement n'a pas
    // cette forme et ne se met pas a jour tout seul.
    const fs::path root = fs::path(exe).parent_path().parent_path();
    std::error_code ec;
    if (fs::path(exe).parent_path().filename() == "bin" && fs::is_directory(root / "share" / "bootcade", ec))
        return Install::Tarball;
    return Install::Unknown;
}

std::string install(const Assets& assets, const Progress& progress) {
    own_executable();   // lu maintenant, tant que le fichier est encore le bon
    const Install how = detect();
    const auto* asset = pick(assets, how);
    if (!asset) return "no matching file in the release";

    const std::string expected = expected_sha256(assets, asset->first);
    if (expected.empty()) return "SHA256SUMS unavailable";

    const std::string dir = Glib::get_user_cache_dir() + "/bootcade/update";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (ec) return ec.message();
    const std::string file = dir + "/" + asset->first;

    if (progress) progress(0.0, asset->first);
    if (auto err = download(asset->second, file, nullptr, &progress); !err.empty()) return err;

    if (progress) progress(0.88, "SHA-256");
    if (DatSource::sha256_of(file) != expected) return "SHA-256 mismatch";

    if (progress) progress(0.92, "install");
    std::string err;
    switch (how) {
        case Install::AppImage: err = install_appimage(file); break;
        case Install::Flatpak:  err = install_flatpak(file);  break;
        case Install::Deb:      err = install_deb(file);      break;
        case Install::Tarball:  err = install_tarball(file);  break;
        case Install::Unknown:  err = "unknown install";      break;
    }
    // Le paquet reste dans le cache tant que l'installation n'est pas faite :
    // inutile ensuite.
    if (err.empty()) fs::remove_all(dir, ec);
    if (progress && err.empty()) progress(1.0, "");
    return err;
}

void restart_after_exit() {
    const Install how = detect();
    std::vector<std::string> cmd;
    if (how == Install::Flatpak) {
        // Le PID du bac a sable ne dit rien a l'hote : on attend que plus
        // aucune instance de Bootcade ne tourne, puis on relance.
        const std::string script = std::string(
            "while flatpak ps --columns=application 2>/dev/null | grep -qx ") + kAppId +
            "; do sleep 0.3; done; exec flatpak run " + kAppId;
        cmd = AppContext::host_command({"sh", "-c", script});
    } else {
        std::string target = own_executable();
        if (how == Install::AppImage) {
            if (const char* a = std::getenv("APPIMAGE")) target = a;
        }
        if (target.empty()) return;
        // Les variables posees par l'AppRun de l'ancienne AppImage pointent
        // vers son point de montage, qui disparait avec elle.
        const std::string script =
            "while kill -0 " + std::to_string(::getpid()) + " 2>/dev/null; do sleep 0.2; done; "
            "exec env -u APPDIR -u APPIMAGE -u ARGV0 -u OWD -u LD_LIBRARY_PATH \"$0\"";
        cmd = {"sh", "-c", script, target};
    }
    try {
        Glib::spawn_async("", cmd, Glib::SPAWN_SEARCH_PATH);
    } catch (const Glib::Error&) {}
}

}  // namespace AppUpdater
