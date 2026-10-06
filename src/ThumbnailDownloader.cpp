// src/ThumbnailDownloader.cpp
#include "ThumbnailDownloader.h"
#include "AppContext.h"
#include "SystemPrefix.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>

namespace ArtworkSources {

/* Les sources proposees d'office.
 *
 * Le serveur Bootcade d'abord : la collection de l'auteur, la plus complete,
 * enrichie de ses propres captures. FinalBurn Neo y est range comme
 * FBNeo-extras (previews/, titles/), MAME comme progettoSNAPS (snap/,
 * titles/, nom court du jeu).
 *
 * FBNeo-extras ensuite : c'est le depot du projet FinalBurn Neo, il couvre
 * aussi les consoles, sous le nom court du jeu. libretro-thumbnails ensuite :
 * des depots GitHub publics, maintenus par la communaute libretro, rangees
 * sous le titre complet du jeu ; ils comblent les trous du premier et
 * couvrent MAME, qui n'avait aucune source.
 *
 * ArcadeDB (adb.arcadeitalia.net) n'y figure pas, bien qu'il reponde : son
 * robots.txt interdit /media aux programmes, et un telechargement de masse
 * est precisement ce qu'il refuse. */
std::vector<std::string> defaults_for(const std::string& emulator) {
    if (emulator.empty() || emulator == "fbneo")
        return {"https://files.bootcade.duckdns.org/artwork/fbneo/",
                "https://raw.githubusercontent.com/finalburnneo/FBNeo-extras/main/",
                "https://raw.githubusercontent.com/libretro-thumbnails/FBNeo_-_Arcade_Games/master/"
                "{Named_Snaps|Named_Titles}/{desc}.png"};
    // MAME et FinalBurn Neo donnent le MEME nom court a un jeu d'arcade :
    // FBNeo-extras comble donc aussi les trous de libretro pour MAME, dont
    // les noms de fichiers suivent parfois un ancien titre du jeu.
    if (emulator == "mame")
        return {"https://files.bootcade.duckdns.org/artwork/mame/{snap|titles}/{rom}.png",
                "https://raw.githubusercontent.com/libretro-thumbnails/MAME/master/"
                "{Named_Snaps|Named_Titles}/{desc}.png",
                "https://raw.githubusercontent.com/finalburnneo/FBNeo-extras/main/"};
    return {};
}

std::vector<std::string> with_new_defaults(std::vector<std::string> list,
                                           const std::string& emulator,
                                           int seen_version) {
    if (seen_version >= kDefaultsVersion) return list;
    // Chaque source nouvelle prend sa place dans l'ordre des valeurs par
    // defaut, pas la derniere : le serveur Bootcade passe ainsi devant
    // FBNeo-extras. Les sources du joueur gardent leur ordre entre elles.
    const auto defaults = defaults_for(emulator);
    size_t at = 0;
    for (const auto& d : defaults) {
        auto it = std::find(list.begin(), list.end(), d);
        if (it == list.end()) {
            list.insert(list.begin() + std::min(at, list.size()), d);
            ++at;
        } else {
            at = static_cast<size_t>(it - list.begin()) + 1;
        }
    }
    return list;
}

std::vector<std::string> load_for(const std::string& emulator) {
    const std::string id = emulator.empty() ? std::string("fbneo") : emulator;

    nlohmann::json j;
    std::ifstream in(AppContext::get_config_path());
    if (in) { try { in >> j; } catch (...) { j = nlohmann::json{}; } }

    std::vector<std::string> out;
    if (j.contains("emulators") && j["emulators"].is_object() &&
        j["emulators"].contains(id) && j["emulators"][id].is_object()) {
        const auto& e = j["emulators"][id];
        if (e.contains("artwork_sources") && e["artwork_sources"].is_array()) {
            for (const auto& v : e["artwork_sources"]) {
                // Une chaine aujourd'hui ; un objet le jour ou une source
                // portera autre chose que son adresse. Accepter les deux des
                // maintenant evite qu'un fichier ecrit par une version plus
                // recente vide la liste au lieu de la lire.
                std::string url;
                if (v.is_string())                                 url = v.get<std::string>();
                else if (v.is_object() && v.contains("url") &&
                         v["url"].is_string())                     url = v["url"].get<std::string>();
                if (!url.empty()) out.push_back(url);
            }
            // La cle existe et elle est vide : c'est un choix, pas un oubli.
            // Y remettre les valeurs d'usine irait contre ce que le joueur a
            // demande en retirant la derniere source. Seules les sources
            // apparues depuis la derniere fois qu'il a enregistre s'ajoutent.
            return with_new_defaults(std::move(out), id, e.value("artwork_defaults", 1));
        }
    }
    return defaults_for(id);
}

namespace {
// Les noms de fichiers libretro : le titre du jeu, ou ces caracteres
// deviennent « _ » (ils sont interdits dans un nom de fichier quelque part).
std::string libretro_name(const std::string& description) {
    std::string out = description;
    for (char& c : out)
        if (std::string("&*/:`<>?\\|\"").find(c) != std::string::npos) c = '_';
    return out;
}

std::string url_escape(const std::string& text) {
    char* e = curl_easy_escape(nullptr, text.c_str(), static_cast<int>(text.size()));
    if (!e) return text;
    std::string out(e);
    curl_free(e);
    return out;
}
}  // namespace

std::string artwork_url(const std::string& source, bool titles,
                        const std::string& encoded_rom, const std::string& description) {
    if (source.find('{') == std::string::npos) {
        std::string root = source;
        while (!root.empty() && root.back() == '/') root.pop_back();
        return root + "/" + (titles ? "titles" : "previews") + "/" + encoded_rom + ".png";
    }
    std::string out;
    for (size_t i = 0; i < source.size();) {
        if (source[i] != '{') { out += source[i++]; continue; }
        const size_t end = source.find('}', i);
        if (end == std::string::npos) { out += source.substr(i); break; }
        const std::string key = source.substr(i + 1, end - i - 1);
        if (key == "rom") {
            out += encoded_rom;
        } else if (key == "desc") {
            if (description.empty()) return {};
            out += url_escape(libretro_name(description));
        } else if (const size_t bar = key.find('|'); bar != std::string::npos) {
            out += titles ? key.substr(bar + 1) : key.substr(0, bar);
        } else {
            out += source.substr(i, end - i + 1);   // inconnu : laisse tel quel
        }
        i = end + 1;
    }
    return out;
}

}  // namespace ArtworkSources

// libcurl is initialised once, in main(), before any thread exists : this
// downloader used to init it in its constructor and clean it up in its
// destructor, while the hiscore probe and sync threads (and the DAT client)
// share the same library and may still be running when MainWindow dies.
ThumbnailDownloader::ThumbnailDownloader() = default;

ThumbnailDownloader::~ThumbnailDownloader() {
    cancel_download();
    if (m_download_thread.joinable()) {
        m_download_thread.join();
    }
}

// Structure pour passer les données à curl
struct DownloadData {
    std::ofstream* file;
    size_t total_size;
    size_t downloaded_size;
};

// Callback pour écrire les données reçues - écrit immédiatement sur disque
static size_t write_callback(void* contents, size_t size, size_t nmemb, DownloadData* data) {
    size_t total_size = size * nmemb;
    data->file->write(static_cast<char*>(contents), total_size);
    data->file->flush();  // Force l'écriture immédiate sur disque
    data->downloaded_size += total_size;
    return total_size;
}

/* Telecharger toutes les images qui manquent, en une passe.
 *
 * Les deux types d'image a la fois, et chaque emulateur dans SES dossiers
 * avec SES sources : le bouton qu'il remplace ne traitait qu'un type, et
 * versait les images de tous les jeux, MAME compris, dans le dossier de
 * l'emulateur affiche a ce moment-la dans les reglages.
 *
 * La liste du travail est faite AVANT de telecharger quoi que ce soit : les
 * images deja presentes et les emulateurs sans source n'y entrent pas. La
 * progression compte donc ce qui reste vraiment a chercher, et la passe ne
 * perd plus une pause de politesse sur chacun des jeux MAME sans source. */
void ThumbnailDownloader::start_missing_download(const std::vector<Game>& games,
                                                 const std::map<std::string, Folders>& folders,
                                                 ProgressCallback progress_callback) {
    if (m_is_downloading.load()) {
        std::cout << "[WARNING] Download already in progress" << std::endl;
        return;
    }
    m_cancel_requested.store(false);
    if (m_download_thread.joinable()) m_download_thread.join();
    // Pose AVANT le fil : sinon un second clic, juste apres le premier,
    // trouvait encore « rien en cours ».
    m_is_downloading.store(true);
    m_download_thread = std::thread(&ThumbnailDownloader::missing_worker, this,
                                    games, folders, progress_callback);
}

ThumbnailDownloader::Summary ThumbnailDownloader::last_summary() const {
    std::lock_guard<std::mutex> lock(m_summary_mutex);
    return m_summary;
}

void ThumbnailDownloader::cancel_download() {
    m_cancel_requested.store(true);
    if (m_download_thread.joinable()) {
        m_download_thread.join();
    }
}

bool ThumbnailDownloader::is_downloading() const {
    return m_is_downloading.load();
}

void ThumbnailDownloader::missing_worker(const std::vector<Game> games,
                                         const std::map<std::string, Folders> folders,
                                         ProgressCallback progress_callback) {
    struct Item { const Game* game; std::string dir; ArtworkType type; const std::vector<std::string>* sources; };
    std::map<std::string, std::vector<std::string>> sources;
    std::vector<Item> work;
    Summary sum;

    for (const auto& game : games) {
        if (m_cancel_requested.load()) break;
        const std::string emu = game.emulator.empty() ? std::string("fbneo") : game.emulator;
        auto f = folders.find(emu);
        if (f == folders.end()) continue;
        auto known = sources.find(emu);
        if (known == sources.end())
            known = sources.emplace(emu, ArtworkSources::load_for(emu)).first;
        if (known->second.empty()) { ++sum.no_source; continue; }

        const std::string file = get_system_prefix(game.system) + game.name + ".png";
        for (auto [dir, type] : {std::pair{f->second.previews, ArtworkType::Previews},
                                 std::pair{f->second.titles,   ArtworkType::Titles}}) {
            if (dir.empty()) continue;
            std::error_code ec;
            const std::string path = dir + "/" + file;
            if (std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) > 0 && !ec) {
                ++sum.already_there;
                continue;
            }
            std::filesystem::create_directories(dir, ec);
            work.push_back({&game, dir, type, &known->second});
        }
    }

    const int total = static_cast<int>(work.size());
    std::cout << "[INFO] Missing artwork: " << total << " image(s) to look for" << std::endl;

    /* Plusieurs images a la fois : l'essentiel du temps se passe a attendre
     * la reponse du serveur, pas a recevoir des octets. Six fils suffisent a
     * passer de deux images par seconde a plusieurs dizaines, sans
     * multiplier les connexions au point de se faire ralentir. */
    std::atomic<int> next{0}, done{0}, found{0}, missing{0};
    std::mutex cb_mutex;
    auto worker = [&] {
        for (;;) {
            if (m_cancel_requested.load()) return;
            const int i = next.fetch_add(1);
            if (i >= total) return;
            const Item& item = work[i];
            const bool ok = download_single_file(item.game->name, item.game->description,
                                                 item.game->system,
                                                 item.game->emulator, item.dir, item.type,
                                                 *item.sources);
            if (ok) ++found;
            else if (!m_cancel_requested.load()) ++missing;
            const int n = ++done;
            if (progress_callback) {
                std::lock_guard<std::mutex> lock(cb_mutex);
                progress_callback(item.game->description.empty() ? item.game->name
                                                                  : item.game->description,
                                  n, total, std::min(99.9, 100.0 * n / std::max(total, 1)));
            }
        }
    };
    {
        std::vector<std::thread> pool;
        for (int t = 0; t < 6; ++t) pool.emplace_back(worker);
        for (auto& t : pool) t.join();
    }
    sum.downloaded = found;
    sum.not_found  = missing;
    sum.cancelled = m_cancel_requested.load();
    {
        std::lock_guard<std::mutex> lock(m_summary_mutex);
        m_summary = sum;
    }
    std::cout << "[INFO] Missing artwork: " << sum.downloaded << " downloaded, "
              << sum.not_found << " not found, " << sum.already_there << " already there, "
              << sum.no_source << " game(s) without a source" << std::endl;
    m_is_downloading.store(false);
    if (progress_callback && !sum.cancelled)
        progress_callback("", total, total, 100.0);
}

bool ThumbnailDownloader::download_single_file(const std::string& rom_name,
                                              const std::string& description,
                                              const std::string& system,
                                              const std::string& emulator,
                                              const std::string& artwork_dir,
                                              ArtworkType artwork_type,
                                              const std::vector<std::string>& sources) {
    // Déterminer le dossier selon le type d'artwork
    const char* folder = (artwork_type == ArtworkType::Previews) ? "previews" : "titles";

    // Obtenir le préfixe système
    std::string system_prefix = get_system_prefix(system);

    // Construire le chemin de destination pour FBNeo-extras avec préfixe système
    std::string filename_with_prefix = system_prefix + rom_name;
    std::string filepath = artwork_dir + "/" + filename_with_prefix + ".png";

    // Ne rien retélécharger si l'artwork est déjà présent (fichier non vide).
    // C'est le garde-fou commun aux deux chemins : téléchargement en masse ET
    // bouton « Download Art » pour un seul jeu.
    {
        std::error_code ec;
        if (std::filesystem::exists(filepath, ec) &&
            std::filesystem::file_size(filepath, ec) > 0 && !ec) {
            return true;
        }
    }

    /* Les sources, dans l'ordre, jusqu'a ce qu'une reponde.
     *
     * Une image absente d'un depot est le cas ORDINAIRE, pas une panne :
     * aucun depot ne couvre un catalogue entier. C'est pourquoi on passe a
     * la suivante en silence, et qu'on ne rend false qu'apres les avoir
     * toutes essayees. */
    const std::string encoded = url_encode(filename_with_prefix);
    for (const auto& base : sources) {
        if (base.empty()) continue;
        if (m_cancel_requested.load()) return false;

        const std::string url = ArtworkSources::artwork_url(
            base, artwork_type == ArtworkType::Titles, encoded, description);
        if (url.empty()) continue;   // modele par titre, jeu sans titre connu

        /* Une poignee par fil, gardee d'une image a l'autre : la connexion
         * au serveur reste ouverte. En ouvrir une neuve a chaque image (et
         * refaire la poignee de main TLS) coutait plus que l'image elle-meme. */
        struct Handle {
            CURL* c = curl_easy_init();
            ~Handle() { if (c) curl_easy_cleanup(c); }
        };
        thread_local Handle handle;
        CURL* curl = handle.c;
        if (!curl) return false;
        curl_easy_reset(curl);

        std::ofstream file(filepath, std::ios::binary);
        if (!file.is_open()) return false;

        DownloadData data;
        data.file = &file;
        data.total_size = 0;
        data.downloaded_size = 0;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);  // 30 secondes timeout
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "FBNeo-Launcher/1.0");
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);  // Fail sur HTTP errors
        // Annuler n'attend plus la fin d'une image lente (jusqu'a 30 s).
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
            +[](void* p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
                return static_cast<std::atomic<bool>*>(p)->load() ? 1 : 0;
            });
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &m_cancel_requested);

        CURLcode res = curl_easy_perform(curl);

        long response_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

        file.close();

        if (res == CURLE_OK && response_code == 200 && data.downloaded_size > 0)
            return true;

        // Un fichier vide ou partiel ferait passer la source suivante pour
        // deja servie : la bibliotheque afficherait alors une vignette vide.
        std::error_code ec;
        std::filesystem::remove(filepath, ec);
    }

    (void)emulator;
    return false;
}

void ThumbnailDownloader::download_single_artwork(const std::string& game_name,
                                                 const std::string& game_description,
                                                 const std::string& game_system,
                                                 const std::string& artwork_dir,
                                                 ArtworkType artwork_type,
                                                 ProgressCallback progress_callback,
                                                 const std::string& emulator) {
    if (m_is_downloading.load()) {
        std::cout << "[WARNING] Download already in progress" << std::endl;
        return;
    }
    
    // Créer le répertoire s'il n'existe pas
    try {
        std::filesystem::create_directories(artwork_dir);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to create artwork directory: " << e.what() << std::endl;
        return;
    }
    
    m_cancel_requested.store(false);
    m_is_downloading.store(true);
    
    // Use ROM name directly for FBNeo-extras
    const char* artwork_type_str = (artwork_type == ArtworkType::Previews) ? "preview" : "title";
    
    if (progress_callback) {
        progress_callback("Downloading " + std::string(artwork_type_str) + " for " + game_name, 1, 1, 0.0);
    }
    
    // Download the single artwork using ROM name with system info
    bool success = download_single_file(game_name, game_description, game_system, emulator, artwork_dir,
                                       artwork_type,
                                       ArtworkSources::load_for(emulator));
    
    if (success) {
        std::cout << "[SUCCESS] Downloaded " << artwork_type_str << " for: " << game_name << std::endl;
    } else {
        std::cout << "[WARNING] Failed to download " << artwork_type_str << " for: " << game_name << std::endl;
    }
    
    m_is_downloading.store(false);
    
    // Final callback
    if (progress_callback && !m_cancel_requested.load()) {
        std::string status = success ? "Download completed!" : "Download failed!";
        progress_callback(status, 1, 1, 100.0);
    }
}

std::string ThumbnailDownloader::url_encode(const std::string& text) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return text;
    }
    
    char* encoded = curl_easy_escape(curl, text.c_str(), text.length());
    if (!encoded) {
        curl_easy_cleanup(curl);
        return text;
    }
    
    std::string result(encoded);
    curl_free(encoded);
    curl_easy_cleanup(curl);
    
    return result;
}

std::string ThumbnailDownloader::get_system_prefix(const std::string& system) {
    return get_fbneo_system_prefix(system);
}
