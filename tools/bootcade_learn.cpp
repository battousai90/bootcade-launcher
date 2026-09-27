// tools/bootcade_learn.cpp
//
// OUTIL DE DEVELOPPEMENT, jamais livre aux joueurs : construit seulement avec
// -DBOOTCADE_DEV_TOOLS=ON, absent du Flatpak et des releases.
//
// Le bouton « Apprendre » de l'admin du service de scores ouvre un lien
//   bootcade-learn://learn?game=<rom>&token=<jeton>
// Ce programme le recoit (declare sur le poste de l'administrateur par
// tools/bootcade-learn.desktop), lance le jeu dans FBNeo, attend qu'on le
// quitte, puis envoie au service les captures F6 prises pendant la partie et
// le fichier de scores que l'emulateur vient d'ecrire. Chaque etape est
// signalee au service, qui l'affiche dans le journal de la page.
//
// Le jeton ne vaut que pour ce jeu et trente minutes ; l'adresse du service
// vient de la configuration de Bootcade, jamais du lien.
#include "AppContext.h"

#include <curl/curl.h>
#include <sqlite3.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace {

struct Link { std::string game, token, mode; };

bool parse_link(const std::string& url, Link& out) {
    static const std::regex re(R"(^bootcade-learn://learn/?\?(.*)$)");
    std::smatch m;
    if (!std::regex_match(url, m, re)) return false;
    static const std::regex kv(R"(([a-z]+)=([A-Za-z0-9_\-]+))");
    const std::string query = m[1];
    for (std::sregex_iterator it(query.begin(), query.end(), kv), end; it != end; ++it) {
        if ((*it)[1] == "game")  out.game  = (*it)[2];
        if ((*it)[1] == "token") out.token = (*it)[2];
        if ((*it)[1] == "mode")  out.mode  = (*it)[2];
    }
    return std::regex_match(out.game, std::regex("[a-z0-9_]{1,32}")) && out.token.size() >= 16;
}

std::string fbneo_data_dir() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "") + "/.local/share/fbneo";
}

size_t sink(char* p, size_t s, size_t n, void* out) {
    static_cast<std::string*>(out)->append(p, s * n);
    return s * n;
}

class Service {
public:
    Service(std::string base, Link link) : base_(std::move(base)), link_(std::move(link)) {}

    // Une ligne du journal de la page. Un echec ici ne doit rien arreter :
    // le jeu et l'envoi comptent plus que la trace.
    void event(const std::string& name, const std::string& detail = "") {
        post("event", [&](curl_mime* mime) {
            add_field(mime, "event", name);
            if (!detail.empty()) add_field(mime, "detail", detail);
        });
    }

    // Le hiscore.dat que sert le service, entrees apprises comprises : sans
    // lui, un jeu tout juste ajoute depuis la page n'ecrirait toujours rien.
    // -> contenu, vide en cas d'echec.
    // La sauvegarde marquee (faux records) du mode « mark », pour ce type de
    // fichier : vide si le service n'en a pas.
    std::string marked(const std::string& kind) {
        return get("/api/learn/" + link_.game + "/marked?kind=" + kind + "&token=" + link_.token);
    }

    std::string hiscore_dat() { return get("/api/hiscore-dat"); }

private:
    std::string get(const std::string& path) {
        CURL* curl = curl_easy_init();
        if (!curl) return {};
        std::string body;
        const std::string url = base_ + path;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sink);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        CURLcode res = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(curl);
        return res == CURLE_OK && status == 200 ? body : std::string();
    }

public:
    bool upload(const std::vector<fs::path>& shots, const fs::path& hi, const fs::path& fsave) {
        return post("upload", [&](curl_mime* mime) {
            for (const auto& s : shots) add_file(mime, "shot", s);
            if (!hi.empty())    add_file(mime, "hi", hi);
            if (!fsave.empty()) add_file(mime, "fs", fsave);
        });
    }

private:
    template <typename Fill>
    bool post(const std::string& what, Fill fill) {
        CURL* curl = curl_easy_init();
        if (!curl) return false;
        curl_mime* mime = curl_mime_init(curl);
        add_field(mime, "token", link_.token);
        fill(mime);
        std::string body;
        const std::string url = base_ + "/api/learn/" + link_.game + "/" + what;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sink);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        CURLcode res = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_mime_free(mime);
        curl_easy_cleanup(curl);
        const bool ok = res == CURLE_OK && status == 200;
        std::cout << "[bootcade-learn] " << what << " -> "
                  << (res == CURLE_OK ? "HTTP " + std::to_string(status) : curl_easy_strerror(res))
                  << std::endl;
        return ok;
    }

    static void add_field(curl_mime* mime, const char* name, const std::string& value) {
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, name);
        curl_mime_data(part, value.c_str(), CURL_ZERO_TERMINATED);
    }

    static void add_file(curl_mime* mime, const char* name, const fs::path& path) {
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, name);
        curl_mime_filedata(part, path.c_str());
    }

    std::string base_;
    Link link_;
};

// La ROM est-elle dans la bibliotheque de Bootcade ? Sans elle, FBNeo
// s'ouvrirait sur une erreur : autant le dire tout de suite a la page.
bool rom_present(const std::string& game) {
    const std::string db_path = AppContext::get_user_config_dir() + "/games.db";
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return true;   // bibliotheque illisible : on laisse FBNeo en juger
    }
    bool present = false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT status FROM games WHERE emulator='fbneo' AND name=?",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, game.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char* s = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
            if (s && std::string(s) != "missing") present = true;
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return present;
}

// Les captures F6 de CETTE partie : FBNeo les nomme <rom>-MM-JJ-HHMMSS.png.
std::vector<fs::path> shots_since(const std::string& game, fs::file_time_type since) {
    std::vector<fs::path> out;
    std::error_code ec;
    const fs::path dir = fbneo_data_dir() + "/screenshots";
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind(game + "-", 0) != 0 || e.path().extension() != ".png") continue;
        if (fs::last_write_time(e.path(), ec) < since) continue;
        out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    if (out.size() > 10) out.erase(out.begin(), out.end() - 10);
    return out;
}

/* Mode « factory » : le pack d'images montre la table d'USINE. Pour que le
 * jeu ecrive la meme, les records du joueur sont mis de cote le temps de la
 * seance, puis remis a l'identique. Le dossier de sauvegarde vit a cote des
 * fichiers (meme disque, un renommage) ; s'il reste d'une seance
 * interrompue, il est restaure avant toute chose. */
class AsideRecords {
public:
    explicit AsideRecords(const std::string& game) : game_(game) {
        const fs::path base = fbneo_data_dir();
        files_ = {base / "support/hiscores" / (game + ".hi"),
                  base / "config/games" / (game + ".fs")};
        keep_ = base / "learn-backup" / game;
        restore();                                   // restes d'une seance interrompue
        std::error_code ec;
        fs::create_directories(keep_, ec);
        for (const auto& f : files_)
            if (fs::exists(f, ec)) fs::rename(f, keep_ / f.filename(), ec);
    }
    ~AsideRecords() { restore(); }
    void restore() {
        std::error_code ec;
        if (!fs::exists(keep_, ec)) return;
        for (const auto& f : files_) {
            const fs::path saved = keep_ / f.filename();
            if (fs::exists(saved, ec)) {
                fs::remove(f, ec);                   // la table d'usine de la seance
                fs::rename(saved, f, ec);
            } else {
                fs::remove(f, ec);                   // absent avant : absent apres
            }
        }
        fs::remove_all(keep_, ec);
        fs::remove(keep_.parent_path(), ec);         // learn-backup, s'il est vide
    }
private:
    std::string game_;
    std::vector<fs::path> files_;
    fs::path keep_;
};

fs::path if_exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec) ? p : fs::path();
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "usage: bootcade-learn 'bootcade-learn://learn?game=<rom>&token=<jeton>'\n";
        return 2;
    }
    Link link;
    if (!parse_link(argv[1], link)) {
        std::cerr << "lien invalide : " << argv[1] << "\n";
        return 2;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);

    nlohmann::json cfg = nlohmann::json::object();
    try {
        std::ifstream f(AppContext::get_config_path());
        if (f) f >> cfg;
    } catch (...) {}
    std::string base = cfg.value("hiscore_url", std::string("https://scores.bootcade.duckdns.org"));
    while (!base.empty() && base.back() == '/') base.pop_back();
    Service service(base, link);

    const std::string fbneo = cfg.value("fbneo_executable", std::string());
    if (fbneo.empty() || ::access(fbneo.c_str(), X_OK) != 0) {
        service.event("no_emulator", fbneo.empty() ? "chemin non configuré" : fbneo);
        return 1;
    }
    if (!rom_present(link.game)) {
        service.event("rom_missing", link.game);
        return 1;
    }

    // Le hiscore.dat du poste, remplace seulement par un fichier complet :
    // une reponse tronquee laisserait l'emulateur sans aucune entree.
    {
        const std::string dat = service.hiscore_dat();
        const fs::path target = fbneo_data_dir() + "/support/hiscores/hiscore.dat";
        if (dat.size() > 10000 && dat.find(':') != std::string::npos) {
            std::error_code ec;
            fs::create_directories(target.parent_path(), ec);
            const fs::path tmp = target.string() + ".learn-tmp";
            std::ofstream(tmp, std::ios::binary) << dat;
            fs::rename(tmp, target, ec);
            const bool listed = std::regex_search(dat, std::regex("(^|\\n)" + link.game + ":"));
            service.event("dat_synced", std::string("entrée ") + (listed ? "présente" : "absente")
                                        + " pour " + link.game);
        } else {
            service.event("dat_synced", "impossible de le récupérer, celui du poste est gardé");
        }
    }

    // Memes options d'affichage que Bootcade ; rien d'autre.
    std::vector<std::string> args{fbneo};
    if (cfg.value("launch_fullscreen", false))   args.push_back("-fullscreen");
    if (cfg.value("launch_integerscale", false)) args.push_back("-integerscale");
    args.push_back(link.game);

    // Une seconde de marge : l'horloge des fichiers et celle-ci peuvent
    // differer d'un arrondi.
    std::optional<AsideRecords> aside;
    if (link.mode == "factory" || link.mode == "mark") {
        aside.emplace(link.game);
        service.event("records_aside", link.game);
    }
    // Mode « mark » : la sauvegarde aux faux records prend la place de la
    // vraie, le temps de la seance (AsideRecords la retire en rendant la vraie).
    if (link.mode == "mark") {
        const fs::path base_dir = fbneo_data_dir();
        bool placed = false;
        for (const auto& [kind, target] : {std::pair<std::string, fs::path>{"hi", base_dir / "support/hiscores" / (link.game + ".hi")},
                                           std::pair<std::string, fs::path>{"saveram", base_dir / "config/games" / (link.game + ".fs")}}) {
            const std::string data = service.marked(kind);
            if (data.empty()) continue;
            std::error_code ec;
            fs::create_directories(target.parent_path(), ec);
            std::ofstream(target, std::ios::binary) << data;
            placed = true;
        }
        if (!placed) {
            service.event("error", "sauvegarde marquée introuvable");
            return 1;
        }
    }

    const auto since = fs::file_time_type::clock::now() - std::chrono::seconds(1);
    const auto started = std::chrono::steady_clock::now();
    pid_t pid = fork();
    if (pid == 0) {
        std::vector<char*> cargs;
        for (auto& a : args) cargs.push_back(a.data());
        cargs.push_back(nullptr);
        execv(cargs[0], cargs.data());
        _exit(127);
    }
    if (pid < 0) {
        service.event("error", "lancement de FBNeo impossible");
        return 1;
    }
    service.event("launched", link.game);
    int status = 0;
    waitpid(pid, &status, 0);
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started).count();
    service.event("exited", "après " + std::to_string(secs) + " s");

    const auto shots = shots_since(link.game, since);
    const fs::path base_dir = fbneo_data_dir();
    const fs::path hi = if_exists(base_dir / "support/hiscores" / (link.game + ".hi"));
    const fs::path fsave = if_exists(base_dir / "config/games" / (link.game + ".fs"));
    std::cout << "[bootcade-learn] " << shots.size() << " capture(s), .hi "
              << (hi.empty() ? "absent" : "present") << ", .fs "
              << (fsave.empty() ? "absent" : "present") << std::endl;
    const bool ok = service.upload(shots, hi, fsave);
    if (aside) {
        aside.reset();                               // records du joueur remis
        service.event("records_back", link.game);
    }
    curl_global_cleanup();
    return ok ? 0 : 1;
}
