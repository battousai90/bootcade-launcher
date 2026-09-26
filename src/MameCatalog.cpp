// src/MameCatalog.cpp
#include "MameCatalog.h"

#include <sys/wait.h>
#include <unistd.h>

#include <curl/curl.h>
#include <pugixml.hpp>
#include <zip.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "AppContext.h"
#include "DatabaseManager.h"

namespace {

// Lance une commande et donne sa sortie standard a `sink`, par blocs, sans
// jamais la materialiser en entier : `mame -listxml` pese 320 Mo.
// Rend false si le processus n'a pas pu demarrer.
bool run_streaming(const std::vector<std::string>& args,
                   const std::function<bool(const char*, size_t)>& sink) {
    const std::vector<std::string> cmd = AppContext::host_command(args);

    int fds[2];
    if (pipe(fds) != 0) return false;

    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return false; }

    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        // MAME ecrit ses avertissements sur stderr : on les laisse passer au
        // journal plutot que de les melanger au XML.
        std::vector<char*> argv;
        argv.reserve(cmd.size() + 1);
        for (const auto& a : cmd) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(fds[1]);
    std::array<char, 1 << 16> buf;
    bool keep = true;
    ssize_t n;
    while (keep && (n = read(fds[0], buf.data(), buf.size())) > 0)
        keep = sink(buf.data(), static_cast<size_t>(n));
    close(fds[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    return true;
}

// Toute la sortie d'une commande courte, sans passer par un shell.
std::string run_capture(const std::vector<std::string>& args) {
    std::string out;
    run_streaming(args, [&out](const char* p, size_t n) {
        out.append(p, n);
        return out.size() < (1u << 20);   // garde-fou : ces commandes sont breves
    });
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

// Les lignes qu'on jette avant de parser : ce sont les plus nombreuses
// (781 216 <device_ref>, 371 752 <rom>) et l'interface n'en montre rien.
//
// On ne jette que des elements AUTO-FERMANTS. Retirer la ligne ouvrante d'un
// element qui a des enfants — <device>, <port>, <dipswitch> — laisserait sa
// balise fermante orpheline, le fragment ne serait plus du XML valide, et la
// machine entiere serait perdue. C'est exactement ce qui a fait tomber un
// premier essai de 50 368 machines a 5 425.
bool is_noise(const char* line, size_t len) {
    static const char* const skip[] = {
        "<device_ref", "<rom ", "<disk ", "<sample", "<dipvalue", "<diplocation",
        "<confsetting", "<conflocation", "<slotoption", "<ramoption", "<chip ",
        // « <display> » n'est plus jete : il porte rotate, width et height,
        // d'ou se deduisent l'orientation et la definition des machines MAME.
        // Une ligne de plus par machine sur 371 752 <rom> jetees : le cout ne
        // se mesure pas, et sans elle trois filtres restaient vides.
        "<sound", "<biosset", "<feature", "<adjuster", "<analog",
        "<instance", "<extension", "<softwarelist", "<control",
    };
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) ++i;

    size_t e = len;
    while (e > i && (line[e - 1] == '\r' || line[e - 1] == ' ')) --e;
    if (e - i < 2 || line[e - 2] != '/' || line[e - 1] != '>') return false;

    for (const char* s : skip) {
        const size_t sl = std::strlen(s);
        if (e - i >= sl && std::memcmp(line + i, s, sl) == 0) return true;
    }
    return false;
}

}  // namespace

namespace MameCatalog {

std::string find_executable() {
    // Les emplacements usuels d'abord : Debian et Fedora ne rangent pas MAME
    // au meme endroit, et `which` coute un processus de plus.
    for (const char* p : {"/usr/games/mame", "/usr/bin/mame", "/usr/local/bin/mame"}) {
        if (access(p, X_OK) == 0) return p;
    }
    const std::string found = run_capture({"which", "mame"});
    return found.find('/') == std::string::npos ? std::string() : found;
}

std::string installed_build(const std::string& mame_exe) {
    if (mame_exe.empty()) return {};
    return run_capture({mame_exe, "-version"});
}

std::string version_number(const std::string& raw) {
    size_t b = raw.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = b;
    while (e < raw.size() && (std::isdigit(static_cast<unsigned char>(raw[e])) ||
                              raw[e] == '.')) ++e;
    // Rien de numerique en tete : la sortie n'est pas celle qu'on attendait,
    // mieux vaut la rendre entiere que d'en inventer une lecture.
    if (e == b) return raw;
    std::string v = raw.substr(b, e - b);
    while (!v.empty() && v.back() == '.') v.pop_back();
    return v;
}

int compare_versions(const std::string& a, const std::string& b) {
    auto parts = [](const std::string& s) {
        std::vector<long> out;
        long cur = 0;
        bool any = false;
        for (char c : s) {
            if (std::isdigit(static_cast<unsigned char>(c))) { cur = cur * 10 + (c - '0'); any = true; }
            else if (c == '.') { out.push_back(any ? cur : 0); cur = 0; any = false; }
            else break;
        }
        if (any) out.push_back(cur);
        return out;
    };
    const std::vector<long> pa = parts(a), pb = parts(b);
    if (pa.empty() || pb.empty()) return 0;
    for (size_t i = 0; i < std::max(pa.size(), pb.size()); ++i) {
        const long x = i < pa.size() ? pa[i] : 0;
        const long y = i < pb.size() ? pb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

namespace {

size_t append_to_string(char* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

/* On ne s'accroche pas a UNE tournure de phrase, mais on ne ramasse pas non
 * plus n'importe quel numero.
 *
 * La page des sorties est ecrite a la main et change de forme a chaque
 * refonte du site : un seul point d'accroche, et la verification se met a
 * mentir le jour ou une balise bouge. On en garde donc deux, qui ne bougent
 * pas ensemble — la phrase d'annonce, et les noms d'archives « mame0289s.exe »
 * qui sont generes.
 *
 * Et on refuse tout le reste : la page porte, dans un commentaire HTML, un
 * « Version 1.11.0 » qui est celui de l'outil ayant fabrique le site. Pris
 * pour une version de MAME, il annoncait une mise a jour imaginaire — d'ou
 * les commentaires jetes d'abord, et la forme 0.NNN exigee ensuite.
 *
 * Aucun numero reconnu : on le DIT, au lieu de conclure que tout va bien.
 */
std::string strip_comments(const std::string& html) {
    std::string out;
    out.reserve(html.size());
    size_t i = 0;
    while (i < html.size()) {
        const size_t c = html.find("<!--", i);
        if (c == std::string::npos) { out.append(html, i, std::string::npos); break; }
        out.append(html, i, c - i);
        const size_t e = html.find("-->", c);
        if (e == std::string::npos) break;
        i = e + 3;
    }
    return out;
}

// La numerotation de MAME : 0 puis trois chiffres. Rien d'autre n'est une
// version de MAME, et s'en assurer coute moins cher qu'une fausse alerte.
bool looks_like_mame_version(const std::string& v) {
    return v.size() == 5 && v[0] == '0' && v[1] == '.' &&
           std::isdigit(static_cast<unsigned char>(v[2])) &&
           std::isdigit(static_cast<unsigned char>(v[3])) &&
           std::isdigit(static_cast<unsigned char>(v[4]));
}

std::string best_version_in(const std::string& html) {
    const std::string stripped = strip_comments(html);
    std::string low;
    low.reserve(stripped.size());
    for (char c : stripped) low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    std::string best;
    auto keep = [&best](const std::string& v) {
        if (!looks_like_mame_version(v)) return;
        if (best.empty() || compare_versions(best, v) < 0) best = v;
    };

    // « ... release is version 0.289. »
    for (size_t p = low.find("version "); p != std::string::npos;
         p = low.find("version ", p + 1)) {
        size_t i = p + 8, j = p + 8;
        while (j < low.size() && (std::isdigit(static_cast<unsigned char>(low[j])) ||
                                  low[j] == '.')) ++j;
        std::string v = low.substr(i, j - i);
        while (!v.empty() && v.back() == '.') v.pop_back();
        keep(v);
    }

    // « mame0289s.exe », « mame0289lx.zip » : 0289 se lit 0.289.
    for (size_t p = low.find("mame0"); p != std::string::npos;
         p = low.find("mame0", p + 1)) {
        const size_t i = p + 4;          // sur le '0'
        size_t j = i;
        while (j < low.size() && std::isdigit(static_cast<unsigned char>(low[j]))) ++j;
        if (j - i != 4) continue;
        keep(low.substr(i, 1) + "." + low.substr(i + 1, 3));
    }
    return best;
}

}  // namespace

LatestRelease fetch_latest_release() {
    LatestRelease r;

    CURL* curl = curl_easy_init();
    if (!curl) { r.error = "curl_easy_init failed"; return r; }

    std::string body;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "User-Agent: bootcade");

    curl_easy_setopt(curl, CURLOPT_URL, "https://www.mamedev.org/release.html");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    const CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) { r.error = curl_easy_strerror(res); return r; }
    if (status != 200)   { r.error = "HTTP " + std::to_string(status); return r; }

    r.version = best_version_in(body);
    r.ok = !r.version.empty();
    if (!r.ok) r.error = "no version found on the release page";
    return r;
}

std::string cached_build(const std::shared_ptr<DatabaseManager>& db) {
    return db ? db->getMetaString("mame_build") : std::string();
}

// Defini plus bas, avec le reste de la lecture de catver.ini : rebuild en a
// besoin pour reposer les genres qu'il vient d'effacer.
namespace { std::string configured_catver(); }

int rebuild(const std::shared_ptr<DatabaseManager>& db,
            const std::string& mame_exe,
            const std::function<void(int)>& progress) {
    if (!db || mame_exe.empty()) return -1;

    const std::string build = installed_build(mame_exe);
    if (build.empty()) {
        std::cerr << "[WARN] MAME did not report a version; catalog not rebuilt" << std::endl;
        return -1;
    }

    std::vector<MameMachine> batch;
    batch.reserve(4096);
    int total = 0;
    std::string pending;   // le fragment <machine>...</machine> en cours
    std::string carry;     // la ligne coupee entre deux blocs de lecture

    db->beginMameCatalogRebuild();

    // On decoupe le flux en lignes, on jette le bruit, et on accumule jusqu'a
    // </machine> : chaque fragment fait quelques kilo-octets et se parse seul,
    // ce qui evite un arbre DOM de 320 Mo.
    auto ends_machine = [](const std::string& s) {
        size_t e = s.find_last_not_of(" \t\r\n");
        return e != std::string::npos && e + 1 >= 10 &&
               s.compare(e + 1 - 10, 10, "</machine>") == 0;
    };

    auto handle_line = [&](const char* p, size_t n) {
        if (is_noise(p, n)) return;
        // Rien ne s'accumule avant la premiere <machine> : le flux commence par
        // la DTD et par <mame build=...>, et les coller devant le premier
        // fragment le rendait inexploitable — c'est ce qui faisait disparaitre
        // « 005 », la premiere machine du fichier.
        if (pending.empty() &&
            std::string(p, n).find("<machine ") == std::string::npos) return;
        pending.append(p, n);
        pending.push_back('\n');
        if (!ends_machine(pending)) return;

        pugi::xml_document doc;
        if (doc.load_buffer(pending.data(), pending.size())) {
            const pugi::xml_node m = doc.child("machine");
            if (m) {
                MameMachine mm;
                mm.name          = m.attribute("name").as_string();
                mm.sourcefile    = m.attribute("sourcefile").as_string();
                mm.cloneof       = m.attribute("cloneof").as_string();
                mm.romof         = m.attribute("romof").as_string();
                mm.is_bios       = m.attribute("isbios").as_bool(false);
                mm.is_device     = m.attribute("isdevice").as_bool(false);
                mm.is_mechanical = m.attribute("ismechanical").as_bool(false);
                mm.runnable      = m.attribute("runnable").as_bool(true);
                mm.description   = m.child_value("description");
                mm.year          = m.child_value("year");
                mm.manufacturer  = m.child_value("manufacturer");
                mm.driver_status = m.child("driver").attribute("status").as_string();
                /* Ce que MAME dit deja de lui-meme et qu'on jetait.
                 *
                 * `rotate` vaut 0, 90, 180 ou 270 : un quart de tour range la
                 * machine dans les verticales. On reprend le vocabulaire exact
                 * des DAT FinalBurn Neo, sinon le filtre « Orientation »
                 * afficherait deux listes de valeurs pour une seule notion.
                 *
                 * Les dimensions restent BRUTES, non tournees : elles servent
                 * a la fiche du jeu, qui doit montrer ce que MAME declare.
                 */
                if (const pugi::xml_node in = m.child("input"))
                    mm.players = in.attribute("players").as_int(0);
                if (const pugi::xml_node d = m.child("display")) {
                    const int rot = d.attribute("rotate").as_int(0);
                    mm.orientation = (rot == 90 || rot == 270) ? "vertical" : "horizontal";
                    mm.width  = d.attribute("width").as_string();
                    mm.height = d.attribute("height").as_string();
                }
                if (!mm.name.empty()) { batch.push_back(std::move(mm)); ++total; }
            }
        }
        pending.clear();

        if (batch.size() >= 4096) {
            db->insertMameMachines(batch);
            batch.clear();
            if (progress) progress(total);
        }
    };

    const bool started = run_streaming({mame_exe, "-listxml"},
                                       [&](const char* p, size_t n) {
        size_t start = 0;
        for (size_t i = 0; i < n; ++i) {
            if (p[i] != '\n') continue;
            if (carry.empty()) {
                handle_line(p + start, i - start);
            } else {
                carry.append(p + start, i - start);
                handle_line(carry.data(), carry.size());
                carry.clear();
            }
            start = i + 1;
        }
        // Le bloc s'arrete au milieu d'une ligne : on la garde pour le suivant,
        // sinon une machine sur quelques milliers se retrouve tronquee.
        if (start < n) carry.append(p + start, n - start);
        return true;
    });

    // Le flux peut s'arreter sans saut de ligne final : sans ceci, la derniere
    // machine reste dans le tampon et le catalogue en compte une de moins.
    if (!carry.empty()) { handle_line(carry.data(), carry.size()); carry.clear(); }

    if (!started) { db->abortMameCatalogRebuild(); return -1; }

    if (!batch.empty()) db->insertMameMachines(batch);
    db->commitMameCatalogRebuild(build);

    /* La regeneration vide la table, genres compris.
     *
     * Une mise a jour de MAME suffit a la declencher, et sans ceci le joueur
     * verrait le filtre « Genre » se vider tout seul un beau matin, sans
     * avoir rien fait. On repose donc ce que catver.ini sait encore dire des
     * machines qui viennent d'etre ecrites.
     */
    const std::string catver = catver_path(configured_catver());
    if (!catver.empty()) apply_catver(db, catver);

    if (progress) progress(total);
    std::cout << "[INFO] MAME catalog: " << total << " machines (build " << build << ")" << std::endl;
    return total;
}

int sync(const std::shared_ptr<DatabaseManager>& db,
         const std::string& mame_exe,
         const std::function<void(int)>& progress) {
    if (!db || mame_exe.empty()) return -1;
    const std::string installed = installed_build(mame_exe);
    if (!installed.empty() && installed == cached_build(db)) {
        return db->countMameMachines();
    }
    return rebuild(db, mame_exe, progress);
}

std::vector<std::string> rompaths_from_mame_ini() {
    std::vector<std::string> out;
    const char* home = std::getenv("HOME");
    if (!home) return out;

    std::ifstream ini(std::string(home) + "/.mame/mame.ini");
    if (!ini) return out;

    std::string line;
    while (std::getline(ini, line)) {
        // « rompath   "a;b;c" » ou « rompath a;b;c »
        const auto key = line.find("rompath");
        if (key != 0) continue;
        auto rest = line.substr(7);
        const auto first = rest.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        rest = rest.substr(first);
        if (!rest.empty() && rest.front() == '"') {
            rest.erase(0, 1);
            const auto close = rest.rfind('"');
            if (close != std::string::npos) rest.erase(close);
        }
        while (!rest.empty() && (rest.back() == '\r' || rest.back() == ' ')) rest.pop_back();

        size_t start = 0;
        while (start <= rest.size()) {
            const auto sep = rest.find(';', start);
            auto piece = rest.substr(start, sep == std::string::npos ? std::string::npos
                                                                     : sep - start);
            if (!piece.empty()) out.push_back(piece);
            if (sep == std::string::npos) break;
            start = sep + 1;
        }
        break;   // une seule ligne rompath fait foi
    }
    return out;
}

AuditResult audit(const std::shared_ptr<DatabaseManager>& db,
                  const std::string& mame_exe,
                  const std::vector<std::string>& rompaths,
                  const std::function<bool(int)>& progress) {
    AuditResult res;
    if (!db || mame_exe.empty()) return res;

    db->resetMameStatuses();

    std::vector<std::string> args{mame_exe};
    if (!rompaths.empty()) {
        // On passe les dossiers explicitement : le mame.ini de l'utilisateur
        // peut parfaitement designer des chemins qui n'existent plus, et MAME
        // ne dirait alors rien d'autre que « rien trouve ».
        std::string joined;
        for (const auto& p : rompaths) {
            if (!joined.empty()) joined += ';';
            joined += p;
        }
        args.push_back("-rompath");
        args.push_back(joined);
    }
    args.push_back("-verifyroms");

    std::vector<std::pair<std::string, std::string>> verdicts;
    verdicts.reserve(8192);
    std::string carry;
    bool cancelled = false;

    // « romset <nom> [<parent>] is <verdict> ». Le parent est facultatif, et
    // les lignes de detail (un fichier par ligne) ne nous interessent pas ici :
    // le statut du set suffit a colorer la bibliotheque.
    auto handle = [&](const std::string& line) {
        if (line.compare(0, 7, "romset ") != 0) return;
        auto rest = line.substr(7);
        const auto sp = rest.find(' ');
        if (sp == std::string::npos) return;
        const std::string name = rest.substr(0, sp);
        const std::string tail = rest.substr(sp + 1);

        std::string status;
        if (tail.find("is good") != std::string::npos)                { status = "available"; ++res.good; }
        else if (tail.find("is best available") != std::string::npos) { status = "available"; ++res.playable; }
        else if (tail.find("is bad") != std::string::npos)            { status = "incorrect"; ++res.bad; }
        else return;

        verdicts.emplace_back(name, status);
        if (verdicts.size() % 2048 == 0 && progress && !progress((int)verdicts.size()))
            cancelled = true;
    };

    run_streaming(args, [&](const char* p, size_t n) {
        size_t start = 0;
        for (size_t i = 0; i < n; ++i) {
            if (p[i] != '\n') continue;
            carry.append(p + start, i - start);
            while (!carry.empty() && carry.back() == '\r') carry.pop_back();
            handle(carry);
            carry.clear();
            start = i + 1;
        }
        if (start < n) carry.append(p + start, n - start);
        return !cancelled;
    });
    if (!carry.empty()) handle(carry);

    if (cancelled) return AuditResult{};

    db->setMameStatuses(verdicts);
    res.missing = db->countMameMachines() - (int)verdicts.size();
    std::cout << "[INFO] MAME audit: " << res.good << " good, " << res.playable
              << " playable, " << res.bad << " bad, " << res.missing << " missing"
              << std::endl;
    return res;
}

namespace {

// Echappement XML minimal. Les descriptions MAME contiennent des esperluettes
// (« Bally & Midway »), des apostrophes et des chevrons (« <unknown> ») : sans
// cela le DAT produit ne serait pas relisible par notre propre parseur.
std::string xml_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 16);
    for (char c : in) {
        switch (c) {
        case '&':  out += "&amp;";  break;
        case '<':  out += "&lt;";   break;
        case '>':  out += "&gt;";   break;
        case '"':  out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default:   out += c;
        }
    }
    return out;
}

// Ce que le generateur retient d'une machine de `mame -listxml` : de quoi
// ecrire les trois DAT, rien de plus. Les 48 000 machines tiennent ainsi en
// memoire (quelques dizaines de Mo), ce qu'exigent les deux DAT qui suivent
// les references d'une machine a l'autre.
struct LxRom {
    std::string name, crc, sha1;
    unsigned long long size = 0;
    std::string merge;            // attribut merge= : la ROM vient du parent ou du BIOS
    bool merged = false;
};
struct LxDisk {
    std::string name, sha1;
    std::string merge;            // attribut merge= : le disque du parent ou du BIOS
    bool merged = false;
};
struct LxMachine {
    std::string name, sourcefile, cloneof, romof, description, year, manufacturer;
    bool isbios = false, isdevice = false, runnable = true;
    std::vector<LxRom>  roms;     // sans les nodump ni les ROMs sans CRC
    std::vector<LxDisk> disks;    // sans les nodump
    std::vector<std::string> devices;   // <device_ref>, dans l'ordre de -listxml
};

// Un DAT en cours d'ecriture. Il s'ecrit sous un nom cache et temporaire, que
// rien ne prend pour un DAT (le chargeur ne lit que les .dat), et ne prend son
// vrai nom qu'une fois complet : une generation interrompue ne laisse jamais
// un demi-fichier que le prochain « Update DAT » chargerait.
struct DatWriter {
    std::ofstream out;
    std::string tmp_path, final_path;
    int machines = 0;

    bool open(const std::string& dir, const std::string& file, const std::string& header_name,
              const std::string& version, const std::string& date,
              const std::string& description = std::string()) {
        final_path = dir + "/" + file;
        tmp_path   = dir + "/." + file + ".tmp";
        out.open(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        // L'en-tete nomme la collection exactement comme les DAT de
        // Pleasuredome, et comme les dossiers que RomVault en tire : c'est ce
        // nom que le gestionnaire de ROMs attend comme dossier (RomResolve::
        // expected_folder), et c'est a lui que DatParser reconnait MAME.
        out << "<?xml version=\"1.0\"?>\n"
               "<!DOCTYPE datafile PUBLIC \"-//Logiqx//DTD ROM Management Datafile//EN\""
               " \"http://www.logiqx.com/Dats/datafile.dtd\">\n"
               "<datafile>\n\t<header>\n"
               "\t\t<name>" << xml_escape(header_name) << "</name>\n"
               "\t\t<description>" << xml_escape(description.empty() ? header_name : description)
            << "</description>\n"
               "\t\t<version>" << xml_escape(version) << "</version>\n"
               "\t\t<date>" << xml_escape(date) << "</date>\n"
               "\t\t<author>Bootcade</author>\n"
               "\t\t<homepage>https://www.mamedev.org/</homepage>\n"
               "\t</header>\n";
        return true;
    }

    void begin_machine(const LxMachine& m) {
        out << "\t<machine name=\"" << xml_escape(m.name) << "\"";
        if (!m.sourcefile.empty()) out << " sourcefile=\"" << xml_escape(m.sourcefile) << "\"";
        if (!m.cloneof.empty())    out << " cloneof=\"" << xml_escape(m.cloneof) << "\"";
        if (!m.romof.empty())      out << " romof=\"" << xml_escape(m.romof) << "\"";
        if (m.isbios)    out << " isbios=\"yes\"";
        if (m.isdevice)  out << " isdevice=\"yes\"";
        if (!m.runnable) out << " runnable=\"no\"";
        out << ">\n\t\t<description>" << xml_escape(m.description) << "</description>\n";
        if (!m.year.empty())         out << "\t\t<year>" << xml_escape(m.year) << "</year>\n";
        if (!m.manufacturer.empty()) out << "\t\t<manufacturer>" << xml_escape(m.manufacturer) << "</manufacturer>\n";
    }
    void rom(const LxRom& r) {
        out << "\t\t<rom name=\"" << xml_escape(r.name) << "\" size=\"" << r.size
            << "\" crc=\"" << xml_escape(r.crc) << "\"";
        if (!r.sha1.empty())  out << " sha1=\"" << xml_escape(r.sha1) << "\"";
        if (!r.merge.empty()) out << " merge=\"" << xml_escape(r.merge) << "\"";
        out << "/>\n";
    }
    void disk(const LxDisk& d) {
        out << "\t\t<disk name=\"" << xml_escape(d.name) << "\" sha1=\"" << xml_escape(d.sha1) << "\"";
        if (!d.merge.empty()) out << " merge=\"" << xml_escape(d.merge) << "\"";
        out << "/>\n";
    }
    void device_ref(const std::string& name) {
        out << "\t\t<device_ref name=\"" << xml_escape(name) << "\"/>\n";
    }
    void end_machine() { out << "\t</machine>\n"; ++machines; }

    // Ferme et donne au fichier son vrai nom. false : rien n'a ete remplace.
    bool commit() {
        out << "</datafile>\n";
        out.close();
        if (!out) { discard(); return false; }
        std::error_code ec;
        std::filesystem::rename(tmp_path, final_path, ec);
        if (ec) { discard(); return false; }
        return true;
    }
    void discard() {
        if (out.is_open()) out.close();
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
    }
};

std::string today_iso() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

// Un DAT que Bootcade a ecrit lui-meme : son en-tete le signe. C'est la seule
// condition pour qu'une generation se permette de supprimer un ancien fichier
// du dossier ; un DAT de Pleasuredome depose la par l'utilisateur ne porte
// pas cette signature et reste en place.
bool written_by_bootcade(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::string head(4096, '\0');
    in.read(&head[0], (std::streamsize)head.size());
    head.resize((size_t)in.gcount());
    const auto end = head.find("</header>");
    return end != std::string::npos &&
           head.substr(0, end).find("<author>Bootcade</author>") != std::string::npos;
}

}  // namespace

// La conversion elle-meme, quelle que soit la source du flux -listxml : la
// sortie de l'executable, ou un fichier telecharge (progettosnaps). `feed`
// pousse le flux par blocs dans le puits qu'on lui donne et rend false s'il
// n'a pas pu demarrer.
using ListxmlFeed = std::function<bool(const std::function<bool(const char*, size_t)>&)>;

// Tout -listxml, lu en flux et reduit a ce qu'un gestionnaire de ROMs
// verifie. La version vient de l'attribut build de la racine <mame>, que la
// sortie de l'executable et les fichiers publies portent tous deux. Rend le
// nombre de machines lues, -1 si le flux n'a pas pu etre lu ou a ete
// interrompu.
static int read_stream(const ListxmlFeed& feed, const std::function<bool(int)>& progress,
                       std::vector<LxMachine>& machines, std::string& version) {
    // Les references d'une machine vers d'autres (device_ref, romof,
    // cloneof) peuvent venir plus loin dans le flux : on lit tout d'abord.
    machines.reserve(50000);
    std::string pending, carry;
    int seen = 0;
    bool cancelled = false;

    auto flush_machine = [&]() {
        pugi::xml_document doc;
        if (!doc.load_buffer(pending.data(), pending.size())) { pending.clear(); return; }
        pending.clear();
        const pugi::xml_node m = doc.child("machine");
        if (!m) return;
        LxMachine x;
        x.name = m.attribute("name").as_string();
        if (x.name.empty()) return;
        x.sourcefile   = m.attribute("sourcefile").as_string();
        x.cloneof      = m.attribute("cloneof").as_string();
        x.romof        = m.attribute("romof").as_string();
        x.isbios       = m.attribute("isbios").as_bool(false);
        x.isdevice     = m.attribute("isdevice").as_bool(false);
        x.runnable     = m.attribute("runnable").as_bool(true);
        x.description  = m.child_value("description");
        x.year         = m.child_value("year");
        x.manufacturer = m.child_value("manufacturer");
        for (pugi::xml_node r = m.child("rom"); r; r = r.next_sibling("rom")) {
            // Une ROM jamais dumpee n'existe dans aucun zip, et une ROM sans
            // CRC n'est pas verifiable : ni l'une ni l'autre n'est ecrite.
            if (std::strcmp(r.attribute("status").as_string(), "nodump") == 0) continue;
            LxRom rom;
            rom.name = r.attribute("name").as_string();
            rom.crc  = r.attribute("crc").as_string();
            if (rom.name.empty() || rom.crc.empty()) continue;
            rom.sha1   = r.attribute("sha1").as_string();
            rom.size   = r.attribute("size").as_ullong();
            rom.merge  = r.attribute("merge").as_string();
            rom.merged = !rom.merge.empty();
            x.roms.push_back(std::move(rom));
        }
        for (pugi::xml_node d = m.child("disk"); d; d = d.next_sibling("disk")) {
            if (std::strcmp(d.attribute("status").as_string(), "nodump") == 0) continue;
            LxDisk disk;
            disk.name   = d.attribute("name").as_string();
            disk.sha1   = d.attribute("sha1").as_string();
            disk.merge  = d.attribute("merge").as_string();
            disk.merged = !disk.merge.empty();
            if (disk.name.empty() || disk.sha1.empty()) continue;
            x.disks.push_back(std::move(disk));
        }
        for (pugi::xml_node d = m.child("device_ref"); d; d = d.next_sibling("device_ref")) {
            std::string dev = d.attribute("name").as_string();
            if (dev.empty() || dev == x.name) continue;
            if (std::find(x.devices.begin(), x.devices.end(), dev) == x.devices.end())
                x.devices.push_back(std::move(dev));
        }
        machines.push_back(std::move(x));
        if (++seen % 2048 == 0 && progress && !progress(seen)) cancelled = true;
    };

    bool root_seen = false;
    auto handle_line = [&](const char* p, size_t n) {
        if (pending.empty()) {
            const std::string line(p, n);
            if (!root_seen) {
                const auto root = line.find("<mame");
                if (root != std::string::npos && root + 5 < line.size() &&
                    (line[root + 5] == ' ' || line[root + 5] == '>' || line[root + 5] == '\t')) {
                    root_seen = true;
                    const auto b = line.find("build=\"", root);
                    if (b != std::string::npos) {
                        const auto e = line.find('"', b + 7);
                        if (e != std::string::npos) version = version_number(line.substr(b + 7, e - b - 7));
                    }
                }
            }
            if (line.find("<machine ") == std::string::npos) return;
        }
        pending.append(p, n);
        pending.push_back('\n');
        size_t e = pending.find_last_not_of(" \t\r\n");
        if (e == std::string::npos || e + 1 < 10) return;
        if (pending.compare(e + 1 - 10, 10, "</machine>") != 0) return;
        flush_machine();
    };

    const bool started = feed([&](const char* p, size_t n) {
        size_t start = 0;
        for (size_t i = 0; i < n; ++i) {
            if (p[i] != '\n') continue;
            if (carry.empty()) {
                handle_line(p + start, i - start);
            } else {
                carry.append(p + start, i - start);
                handle_line(carry.data(), carry.size());
                carry.clear();
            }
            start = i + 1;
        }
        if (start < n) carry.append(p + start, n - start);
        return !cancelled;
    });
    if (!carry.empty()) handle_line(carry.data(), carry.size());
    if (cancelled || !started || machines.empty()) return -1;
    return seen;
}

// Ce qu'un gestionnaire de ROMs verifie de chaque machine, tel que -listxml
// le decrit : ses liens (cloneof, romof), ses ROMs et ses disques, merge=
// compris, et ses devices. Une machine n'a d'entree que si elle a quelque
// chose a verifier : une ROM, un disque, ou un device qui en a, directement
// ou par ses propres devices (un set non-merged porte les ROMs de ses
// devices) ; ou qu'elle soit le parent (cloneof, romof) d'une machine gardee,
// meme sans rien a elle : en merged, ses clones vont dans son archive.
// Un device_ref n'est garde que vers une machine qui a quelque chose a
// verifier : la chaine
// qui mene aux ROMs d'un device reste entiere. Deux lignes identiques (meme
// nom, meme CRC) dans un set n'en font qu'une. C'est la regle unique : le DAT
// de « Generate from MAME » l'ecrit, la lecture directe d'un -listxml la
// reproduit.
struct ListxmlSets {
    const std::vector<LxMachine>& machines;
    std::unordered_map<std::string, const LxMachine*> by_name;
    std::unordered_map<std::string, bool> reaches;   // memo : has something to verify
    std::unordered_set<std::string> parents;         // named by a kept machine's cloneof / romof
    explicit ListxmlSets(const std::vector<LxMachine>& m) : machines(m) {
        for (const auto& x : machines) by_name.emplace(x.name, &x);
        for (const auto& x : machines) verifiable(x.name);
        for (const auto& x : machines) {
            if (!reaches[x.name]) continue;
            // Up the chain : a parent's parent is kept too.
            const LxMachine* cur = &x;
            for (int depth = 0; depth < 8; ++depth) {
                bool more = false;
                for (const std::string* up : {&cur->cloneof, &cur->romof}) {
                    if (up->empty() || !parents.insert(*up).second) continue;
                    auto it = by_name.find(*up);
                    if (it != by_name.end()) { cur = it->second; more = true; }
                }
                if (!more) break;
            }
        }
    }
    bool verifiable(const std::string& name) {
        auto memo = reaches.find(name);
        if (memo != reaches.end()) return memo->second;
        auto it = by_name.find(name);
        if (it == by_name.end()) return false;
        const LxMachine& m = *it->second;
        reaches[name] = false;   // a cycle of devices adds nothing
        bool yes = !m.roms.empty() || !m.disks.empty();
        for (const auto& dev : m.devices)
            if (verifiable(dev)) yes = true;
        reaches[name] = yes;
        return yes;
    }
    bool has_entry(const LxMachine& m) const {
        auto it = reaches.find(m.name);
        return (it != reaches.end() && it->second) || parents.count(m.name);
    }
    template <class F> void roms(const LxMachine& m, F&& f) const {
        std::unordered_set<std::string> seen;
        for (const auto& r : m.roms)
            if (seen.insert(r.name + '\x1f' + r.crc).second) f(r);
    }
    template <class F> void disks(const LxMachine& m, F&& f) const {
        std::unordered_set<std::string> seen;
        for (const auto& d : m.disks)
            if (seen.insert(d.name).second) f(d);
    }
    template <class F> void devices(const LxMachine& m, F&& f) const {
        for (const auto& dev : m.devices) {
            auto it = reaches.find(dev);
            if (it != reaches.end() && it->second) f(dev);
        }
    }
};

static int convert_stream(const ListxmlFeed& feed, const std::string& dat_dir,
                   const std::function<bool(int)>& progress, bool replace_previous,
                   ConvertResult* result) {
    namespace fs = std::filesystem;
    if (dat_dir.empty()) return -1;
    std::error_code ec;
    fs::create_directories(dat_dir, ec);
    std::string version;
    std::vector<LxMachine> machines;
    const int seen = read_stream(feed, progress, machines, version);
    if (seen < 0) return -1;

    // Ecrit dans l'ordre des noms.
    std::vector<size_t> order(machines.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return machines[a].name < machines[b].name; });

    if (version.empty()) version = "unknown";
    const std::string date = today_iso();
    const std::string tag  = "MAME " + version;

    // ── Ce que les generations precedentes laissent derriere elles ─────────
    // Les deux fichiers de l'ancien format (un DAT par famille, semantique
    // brute de -listxml) : c'est Bootcade qui les ecrivait, sous ces noms-la
    // exactement. Et, s'ils portent notre signature, ceux d'une autre version
    // de MAME : laisses la, « Update DAT »
    // chargerait les memes machines une seconde fois. Un DAT depose par
    // l'utilisateur ne porte pas la signature et reste en place.
    auto remove_previous = [&](const std::vector<std::string>& written) {
        for (const char* old : {"MAME_-_Arcade.dat", "MAME_-_Mechanical.dat"})
            fs::remove(fs::path(dat_dir) / old, ec);
        auto ends_with = [](const std::string& f, const std::string& k) {
            return f.size() > k.size() && f.compare(f.size() - k.size(), k.size(), k) == 0;
        };
        for (auto it = fs::directory_iterator(dat_dir, ec); it != fs::directory_iterator(); it.increment(ec)) {
            const std::string file = it->path().filename().string();
            if (file.rfind("MAME ", 0) != 0 || it->path().extension() != ".dat") continue;
            bool keep = false;
            for (const auto& w : written) if (file == fs::path(w).filename().string()) keep = true;
            if (keep) continue;
            bool ours = false;
            for (const char* kind : {" ROMs (split).dat", " ROMs (bios-devices).dat", " CHDs (merged).dat"})
                if (ends_with(file, kind)) ours = true;
            // « MAME 0.289.dat » : le DAT unique, un seul mot apres la marque.
            const std::string middle = file.substr(5, file.size() - 5 - 4);
            if (!middle.empty() && middle.find(' ') == std::string::npos) ours = true;
            if (ours && written_by_bootcade(it->path())) fs::remove(it->path(), ec);
        }
    };

    // ── Un seul DAT, fidele a -listxml ─────────────────────────────────────
    // Chaque machine telle que MAME la decrit : ses liens (cloneof, romof), ses
    // ROMs et ses disques, merge= compris. C'est au gestionnaire de ROMs
    // d'appliquer le « Set style » du groupe (split, non-merged), exactement
    // comme pour un DAT FinalBurn Neo, et comme RomVault ou clrmamepro le font
    // avec le XML de MAME. Seul ce qui ne sert pas a verifier un set est laisse
    // de cote (entrees, DIP, ecrans, sons...). Une machine sans ROM ni disque
    // n'a rien a verifier et n'a pas d'entree.
    ListxmlSets sets(machines);
    DatWriter one;
    if (!one.open(dat_dir, tag + ".dat", kHeader, version, date, tag)) return -1;
    for (size_t i : order) {
        const LxMachine& m = machines[i];
        if (!sets.has_entry(m)) continue;
        one.begin_machine(m);
        sets.roms(m, [&](const LxRom& r) { one.rom(r); });
        sets.disks(m, [&](const LxDisk& d) { one.disk(d); });
        sets.devices(m, [&](const std::string& dev) { one.device_ref(dev); });
        one.end_machine();
    }
    const int n_sets = one.machines;
    if (!one.commit()) return -1;
    if (result) {
        result->version  = version;
        result->sets     = n_sets;
        result->machines = (int)machines.size();
        result->files    = {one.final_path};
    }
    if (progress) progress(seen);
    if (replace_previous) remove_previous({one.final_path});
    return 1;
}

int generate_dats(const std::string& mame_exe,
                  const std::string& dat_dir,
                  const std::function<bool(int)>& progress) {
    if (mame_exe.empty()) return -1;
    ConvertResult r;
    const int files = convert_stream(
        [&](const std::function<bool(const char*, size_t)>& sink) {
            return run_streaming({mame_exe, "-listxml"}, sink);
        },
        dat_dir, progress, /*replace_previous=*/true, &r);
    if (files > 0)
        std::cout << "[INFO] MAME DAT " << r.version << ": " << r.sets << " sets ("
                  << r.machines << " machines read)" << std::endl;
    return files;
}

static ListxmlFeed file_feed(const std::string& xml_path) {
    return [xml_path](const std::function<bool(const char*, size_t)>& sink) {
        std::ifstream in(xml_path, std::ios::binary);
        if (!in) return false;
        std::vector<char> buf(1 << 20);
        while (in) {
            in.read(buf.data(), (std::streamsize)buf.size());
            const std::streamsize n = in.gcount();
            if (n > 0 && !sink(buf.data(), (size_t)n)) break;
        }
        return true;
    };
}

int convert_listxml_file(const std::string& xml_path, const std::string& out_dir,
                         const std::function<bool(int)>& progress, ConvertResult* result) {
    return convert_stream(file_feed(xml_path), out_dir, progress, /*replace_previous=*/false, result);
}

int read_listxml_file(const std::string& xml_path, std::vector<Game>& out, std::string* version,
                      const std::function<bool(int)>& progress) {
    std::vector<LxMachine> machines;
    std::string build;
    if (read_stream(file_feed(xml_path), progress, machines, build) < 0) return -1;
    if (version) *version = build.empty() ? "unknown" : build;
    ListxmlSets sets(machines);
    out.reserve(out.size() + machines.size());
    for (const auto& m : machines) {
        if (!sets.has_entry(m)) continue;
        Game g;
        g.emulator     = "mame";
        g.name         = m.name;
        g.description  = m.description;
        g.year         = m.year;
        g.manufacturer = m.manufacturer;
        g.sourcefile   = m.sourcefile;
        g.cloneof      = m.cloneof;
        g.romof        = m.romof;
        g.is_bios      = m.isbios;
        sets.roms(m, [&](const LxRom& r) {
            Rom rom;
            rom.name  = r.name;
            rom.size  = r.size;
            rom.crc   = r.crc;
            rom.merge = r.merge;
            g.roms.push_back(std::move(rom));
        });
        sets.disks(m, [&](const LxDisk& d) {
            Disk disk;
            disk.name  = d.name;
            disk.sha1  = d.sha1;
            std::transform(disk.sha1.begin(), disk.sha1.end(), disk.sha1.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            disk.merge = d.merge;
            g.disks.push_back(std::move(disk));
        });
        sets.devices(m, [&](const std::string& dev) { g.devices.push_back(dev); });
        out.push_back(std::move(g));
    }
    return (int)out.size();
}


// ── catver.ini : le genre des machines MAME ─────────────────────────────────

namespace {

/* SQLite n'accepte que de l'UTF-8 valide.
 *
 * catver.ini est ecrit par des contributeurs sur trois systemes differents :
 * la plupart des lignes sont en ASCII pur, mais certaines portent des octets
 * hauts qui ne forment pas de l'UTF-8. Les inserer tels quels ferait echouer
 * l'UPDATE, et la machine concernee resterait sans genre sans qu'on sache
 * pourquoi. On valide donc, et a defaut on relit les octets comme du
 * Latin-1 : c'est ce que ces fichiers contiennent quand ils ne sont pas en
 * UTF-8, et cela ne peut pas echouer.
 */
std::string to_utf8(const std::string& in) {
    size_t i = 0;
    bool valid = true;
    while (i < in.size() && valid) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        size_t extra = 0;
        if      (c < 0x80)                 extra = 0;
        else if ((c & 0xE0) == 0xC0)       extra = 1;
        else if ((c & 0xF0) == 0xE0)       extra = 2;
        else if ((c & 0xF8) == 0xF0)       extra = 3;
        else                               { valid = false; break; }
        if (i + extra >= in.size()) { valid = false; break; }
        for (size_t k = 1; k <= extra; ++k)
            if ((static_cast<unsigned char>(in[i + k]) & 0xC0) != 0x80) { valid = false; break; }
        i += extra + 1;
    }
    if (valid) return in;

    std::string out;
    out.reserve(in.size() + 8);
    for (char ch : in) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x80) out += static_cast<char>(c);
        else { out += static_cast<char>(0xC0 | (c >> 6));
               out += static_cast<char>(0x80 | (c & 0x3F)); }
    }
    return out;
}

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Le chemin que les reglages ont enregistre. MameCatalog le relit lui-meme
// parce que rebuild() doit reposer les genres sans qu'aucun ecran ne soit
// ouvert : une mise a jour de MAME vide le catalogue, genres compris.
std::string configured_catver() {
    nlohmann::json j;
    std::ifstream in(AppContext::get_config_path());
    if (!in) return {};
    try { in >> j; } catch (...) { return {}; }
    return j.value("mame_catver_path", std::string());
}

// L'ecriture de curl, vers un fichier ouvert.
size_t write_to_file(void* ptr, size_t size, size_t nmemb, void* stream) {
    return std::fwrite(ptr, size, nmemb, static_cast<FILE*>(stream));
}

}  // namespace

std::string catver_url(const std::string& mame_version) {
    // « 0.289 » -> « pS_CatVer_289.zip ». On exige exactement trois chiffres
    // apres le point : rien d'autre n'est une version de MAME, et fabriquer
    // une adresse a partir d'autre chose ramenerait le fichier d'une version
    // qui n'est pas celle installee ici.
    const size_t dot = mame_version.find('.');
    if (dot == std::string::npos) return {};
    std::string digits;
    for (size_t i = dot + 1; i < mame_version.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(mame_version[i]))) break;
        digits += mame_version[i];
    }
    if (digits.size() != 3) return {};
    return "https://www.progettosnaps.net/download/?tipo=catver&file=pS_CatVer_"
           + digits + ".zip";
}

std::string catver_app_path() {
    return AppContext::get_user_config_dir() + "/catver.ini";
}

std::string catver_path(const std::string& configured) {
    // Le choix de l'utilisateur d'abord : s'il a deja un catver.ini, rien ne
    // justifie d'aller en telecharger un second.
    std::error_code ec;
    if (!configured.empty() && std::filesystem::is_regular_file(configured, ec))
        return configured;
    const std::string mine = catver_app_path();
    if (std::filesystem::is_regular_file(mine, ec)) return mine;
    return {};
}

CatverResult apply_catver(const std::shared_ptr<DatabaseManager>& db,
                          const std::string& path) {
    CatverResult r;
    r.path = path;
    if (!db)          { r.error = "no database"; return r; }
    if (path.empty()) { r.error = "no catver.ini selected"; return r; }

    std::ifstream in(path, std::ios::binary);
    if (!in) { r.error = "cannot read " + path; return r; }

    std::vector<std::pair<std::string, std::string>> genres;
    genres.reserve(60000);

    /* UNE seule section compte.
     *
     * Le fichier en porte quatre. [VerAdded] associe les MEMES noms de
     * machines a un numero de version (« pacman=0.1 ») : lue comme la
     * precedente, elle remplacerait chaque genre par un numero, et le filtre
     * « Genre » afficherait la liste des versions de MAME.
     */
    bool in_category = false;
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        const size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        if (line[b] == ';' || line[b] == '#') continue;
        if (line[b] == '[') {
            in_category = line.compare(b, 10, "[Category]") == 0;
            continue;
        }
        if (!in_category) continue;

        const size_t eq = line.find('=', b);
        if (eq == std::string::npos) continue;          // ligne sans « = » : ignoree
        const std::string name = trim(line.substr(b, eq - b));

        /* Le genre PRINCIPAL, pas le sous-genre.
         *
         * catver.ini ecrit « Maze / Collect » : la colonne des filtres
         * affiche deja « Maze » pour FinalBurn Neo, et garder le sous-genre
         * donnerait des centaines de categories a une machine pour une a
         * l'autre — deux vocabulaires dans une seule liste.
         */
        std::string genre = line.substr(eq + 1);

        /* « TTL * Shooter / Gallery » : le prefixe n'est pas un genre.
         *
         * catver.ini marque ainsi les machines a logique cablee, d'avant les
         * microprocesseurs. C'est une technologie, pas une categorie de jeu :
         * garde, il coupait 63 machines de leur genre et ajoutait six entrees
         * en double a la colonne des filtres (« Shooter » et « TTL * Shooter »).
         */
        if (genre.compare(0, 6, "TTL * ") == 0) genre = genre.substr(6);

        const size_t slash = genre.find('/');
        if (slash != std::string::npos) genre = genre.substr(0, slash);
        genre = trim(genre);

        if (name.empty() || genre.empty()) continue;
        genres.emplace_back(to_utf8(name), to_utf8(genre));
        ++r.entries;
    }

    if (genres.empty()) {
        r.error = "no [Category] section in " + path;
        return r;
    }

    const int applied = db->setMameGenres(genres);
    if (applied < 0) { r.error = "could not write the genres"; return r; }
    r.applied = applied;
    r.ok = true;
    std::cout << "[INFO] catver.ini: " << r.entries << " entries, "
              << r.applied << " machines classified" << std::endl;
    return r;
}

CatverResult download_catver(const std::string& url,
                             const std::string& dest_dir,
                             const std::function<bool(double)>& progress) {
    CatverResult r;
    if (url.empty())      { r.error = "no download address"; return r; }
    if (dest_dir.empty()) { r.error = "no destination folder"; return r; }

    std::error_code ec;
    std::filesystem::create_directories(dest_dir, ec);

    const std::string zip_path = dest_dir + "/catver-download.zip";
    const std::string out_path = dest_dir + "/catver.ini";

    CURL* curl = curl_easy_init();
    if (!curl) { r.error = "curl_easy_init failed"; return r; }

    FILE* fp = std::fopen(zip_path.c_str(), "wb");
    if (!fp) { curl_easy_cleanup(curl); r.error = "cannot write " + zip_path; return r; }

    // Le rappel de progression sert aussi d'interruption : rendre autre chose
    // que 0 abandonne le transfert au lieu de le laisser courir.
    struct Xfer { const std::function<bool(double)>* cb; } xfer{&progress};
    auto on_progress = [](void* p, curl_off_t dltotal, curl_off_t dlnow,
                          curl_off_t, curl_off_t) -> int {
        auto* x = static_cast<Xfer*>(p);
        if (!*x->cb) return 0;
        const double f = dltotal > 0 ? static_cast<double>(dlnow) / static_cast<double>(dltotal)
                                     : 0.0;
        return (*x->cb)(f) ? 0 : 1;
    };

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "bootcade/1.0");
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                     static_cast<curl_xferinfo_callback>(on_progress));
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &xfer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

    const CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    std::fclose(fp);

    if (res != CURLE_OK) {
        std::filesystem::remove(zip_path, ec);
        r.error = curl_easy_strerror(res);
        return r;
    }
    if (status != 200) {
        // progetto-SNAPS rend une page d'erreur en 200 pour une version qu'il
        // ne connait pas ; c'est l'ouverture du zip, plus bas, qui tranche.
        std::filesystem::remove(zip_path, ec);
        r.error = "HTTP " + std::to_string(status);
        return r;
    }

    /* L'archive porte aussi un dossier UI_files/ (genre.ini, catlist.ini,
     * mature.ini) et un mode d'emploi. On n'extrait QUE catver.ini : le
     * reste ne sert a rien ici, et deverser une arborescence entiere dans le
     * dossier de configuration ferait du desordre que personne n'a demande.
     */
    int err = 0;
    zip_t* z = zip_open(zip_path.c_str(), ZIP_RDONLY, &err);
    if (!z) {
        std::filesystem::remove(zip_path, ec);
        r.error = "the downloaded file is not a zip archive";
        return r;
    }

    bool written = false;
    const zip_int64_t n = zip_get_num_entries(z, 0);
    for (zip_int64_t i = 0; i < n && !written; ++i) {
        const char* name = zip_get_name(z, i, 0);
        if (!name) continue;
        std::string base(name);
        const size_t slash = base.find_last_of('/');
        if (slash != std::string::npos) base = base.substr(slash + 1);
        if (base != "catver.ini") continue;

        zip_file_t* f = zip_fopen_index(z, i, 0);
        if (!f) break;
        std::ofstream out(out_path, std::ios::binary);
        if (out) {
            char buf[1 << 16];
            zip_int64_t got;
            while ((got = zip_fread(f, buf, sizeof(buf))) > 0)
                out.write(buf, got);
            written = out.good();
        }
        zip_fclose(f);
    }
    zip_close(z);
    std::filesystem::remove(zip_path, ec);

    if (!written) { r.error = "no catver.ini inside the archive"; return r; }
    r.ok   = true;
    r.path = out_path;
    return r;
}

std::string catver_file_version(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    /* L'en-tete tient dans les toutes premieres lignes ; lire le fichier
     * entier (2,5 Mo) pour y trouver un commentaire serait payer cher une
     * information qui est toujours en tete. */
    std::string line;
    for (int i = 0; i < 40 && std::getline(in, line); ++i) {
        const size_t at = line.find("catver.ini");
        if (at == std::string::npos) continue;
        size_t p = at;
        while (p < line.size() && !std::isdigit(static_cast<unsigned char>(line[p]))) ++p;
        std::string v;
        while (p < line.size() &&
               (std::isdigit(static_cast<unsigned char>(line[p])) || line[p] == '.'))
            v += line[p++];
        while (!v.empty() && v.back() == '.') v.pop_back();
        if (v.find('.') != std::string::npos) return v;
    }
    return {};
}

namespace {

/* Ou se trouve le numero de version dans une adresse.
 *
 * Le DERNIER groupe d'exactement trois chiffres : l'adresse de
 * progetto-SNAPS en contient un seul, et une adresse personnalisee qui en
 * porterait plusieurs designe presque toujours sa version en dernier
 * (.../catver/289.zip). Rend false quand il n'y en a aucun, seul cas ou
 * l'on ne peut rien dire.
 */
bool url_version_span(const std::string& url, size_t& begin, size_t& len) {
    bool found = false;
    size_t i = 0;
    while (i < url.size()) {
        if (!std::isdigit(static_cast<unsigned char>(url[i]))) { ++i; continue; }
        size_t j = i;
        while (j < url.size() && std::isdigit(static_cast<unsigned char>(url[j]))) ++j;
        if (j - i == 3) { begin = i; len = 3; found = true; }
        i = j;
    }
    return found;
}

}  // namespace

int catver_url_version(const std::string& url) {
    size_t b = 0, n = 0;
    if (!url_version_span(url, b, n)) return -1;
    return std::stoi(url.substr(b, n));
}

std::string catver_url_with_version(const std::string& url, int version) {
    size_t b = 0, n = 0;
    if (!url_version_span(url, b, n)) return {};
    if (version < 0 || version > 999) return {};
    char buf[4];
    std::snprintf(buf, sizeof(buf), "%03d", version);
    return url.substr(0, b) + buf + url.substr(b + n);
}

UrlProbe probe_catver_url(const std::string& url) {
    if (url.empty()) return UrlProbe::Unreachable;

    CURL* curl = curl_easy_init();
    if (!curl) return UrlProbe::Unreachable;

    /* Quatre octets, pas une requete HEAD.
     *
     * Le serveur repond 200 avec une page d'erreur HTML pour certaines
     * versions qu'il ne connait pas : le code HTTP seul ment donc. Les
     * quatre premiers octets d'un zip valent « PK\x03\x04 » et tranchent
     * sans telecharger les 900 Ko.
     */
    std::string head;
    auto sink = +[](void* ptr, size_t size, size_t nmemb, void* user) -> size_t {
        auto* out = static_cast<std::string*>(user);
        out->append(static_cast<char*>(ptr), size * nmemb);
        return size * nmemb;
    };
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_RANGE, "0-3");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sink);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &head);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "bootcade/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    const CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return UrlProbe::Unreachable;
    if (status == 404 || status == 410) return UrlProbe::Absent;
    if (status != 200 && status != 206) return UrlProbe::Unreachable;
    return head.compare(0, 4, "PK\x03\x04") == 0 ? UrlProbe::Present
                                                 : UrlProbe::Absent;
}

CatverCheck check_catver(const std::string& url, const std::string& local_file) {
    CatverCheck c;
    if (url.empty()) { c.error = "no download address"; return c; }

    const std::string have = catver_file_version(local_file);
    if (!have.empty()) {
        const std::string digits = have.substr(have.find('.') + 1);
        if (digits.size() == 3) c.local = std::stoi(digits);
    }

    // A defaut de fichier lisible, l'adresse configuree sert de point de
    // depart : elle designe ce que le joueur considere comme sa version.
    const int from = c.local >= 0 ? c.local : catver_url_version(url);
    if (from < 0) { c.error = "no version number in the address"; return c; }
    c.newest = c.local;

    int misses = 0;
    for (int v = from + 1; v <= from + 12 && misses < 2; ++v) {
        const std::string candidate = catver_url_with_version(url, v);
        if (candidate.empty()) { c.error = "no version number in the address"; return c; }
        const UrlProbe p = probe_catver_url(candidate);
        if (p == UrlProbe::Unreachable) {
            // Une seule panne de reseau suffit a rendre la reponse fausse :
            // on ne conclut donc rien de ce qui a ete vu jusque-la.
            if (!c.asked) { c.error = "could not reach the server"; return c; }
            break;
        }
        c.asked = true;
        c.probed_to = v;
        if (p == UrlProbe::Present) { c.newest = v; c.url = candidate; misses = 0; }
        else                        { ++misses; }
    }
    if (!c.asked) c.error = "could not reach the server";
    return c;
}

std::vector<Game> load(const std::shared_ptr<DatabaseManager>& db,
                       bool include_mechanical) {
    return db ? db->getMameCatalog(include_mechanical) : std::vector<Game>{};
}

}  // namespace MameCatalog
