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

namespace ArtworkSources {

std::vector<std::string> defaults_for(const std::string& emulator) {
    if (emulator.empty() || emulator == "fbneo")
        return {"https://raw.githubusercontent.com/finalburnneo/FBNeo-extras/main/"};
    return {};
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
            // demande en retirant la derniere source.
            return out;
        }
    }
    return defaults_for(id);
}

std::string artwork_url(const std::string& base, const std::string& folder,
                        const std::string& encoded_name) {
    std::string root = base;
    while (!root.empty() && root.back() == '/') root.pop_back();
    return root + "/" + folder + "/" + encoded_name + ".png";
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

void ThumbnailDownloader::start_download(const std::vector<Game>& games,
                                        const std::string& artwork_dir,
                                        ArtworkType artwork_type,
                                        ProgressCallback progress_callback) {
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
    
    // Lancer le thread de téléchargement
    if (m_download_thread.joinable()) {
        m_download_thread.join();
    }
    
    m_download_thread = std::thread(&ThumbnailDownloader::download_worker, this, 
                                   games, artwork_dir, artwork_type, progress_callback);
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

void ThumbnailDownloader::download_worker(const std::vector<Game> games,
                                         const std::string artwork_dir,
                                         ArtworkType artwork_type,
                                         ProgressCallback progress_callback) {
    m_is_downloading.store(true);
    
    std::map<std::string, std::vector<std::string>> sources;
    int total_games = games.size();
    int current_index = 0;
    int successful_downloads = 0;
    int skipped_existing = 0;
    
    const char* artwork_type_str = (artwork_type == ArtworkType::Previews) ? "previews" : "titles";
    std::cout << "[INFO] Starting " << artwork_type_str << " download for " << total_games << " games" << std::endl;
    
    for (const auto& game : games) {
        if (m_cancel_requested.load()) {
            std::cout << "[INFO] Download cancelled by user" << std::endl;
            break;
        }
        
        current_index++;
        double progress = static_cast<double>(current_index) / total_games * 100.0;
        
        // Mettre à jour le callback de progression
        if (progress_callback) {
            // Show progress every 100 games to reduce console spam
            if (current_index % 100 == 0 || current_index == total_games) {
                std::cout << "[INFO] Progress: " << current_index << "/" << total_games << " (" << std::fixed << std::setprecision(1) << progress << "%)" << std::endl;
            }
            progress_callback(game.name, current_index, total_games, progress);
        }
        
        // Vérifier si le fichier existe déjà - use ROM name with system prefix
        std::string system_prefix = get_system_prefix(game.system);
        std::string filename = system_prefix + game.name + ".png";
        std::string filepath = artwork_dir + "/" + filename;
        
        if (std::filesystem::exists(filepath)) {
            skipped_existing++;
            continue;
        }
        
        /* Les sources sont relues UNE fois par emulateur rencontre : les
         * lire a chaque jeu ouvrirait config.json trente mille fois pour y
         * trouver la meme chose. */
        auto known = sources.find(game.emulator);
        if (known == sources.end())
            known = sources.emplace(game.emulator,
                                    ArtworkSources::load_for(game.emulator)).first;

        // Télécharger le artwork - use ROM name with system prefix
        if (download_single_file(game.name, game.system, game.emulator, artwork_dir,
                                 artwork_type, known->second)) {
            successful_downloads++;
            std::cout << "[SUCCESS] Downloaded: " << filename << std::endl;
        } else {
            // Don't spam console with failed downloads - they're expected for many ROMs
            // std::cout << "[WARNING] Failed to download: " << filename << std::endl;
        }
        
        // Petit délai pour ne pas surcharger le serveur
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    m_is_downloading.store(false);
    
    int failed_downloads = total_games - successful_downloads - skipped_existing;
    double success_rate = (total_games > 0) ? (double(successful_downloads) / total_games * 100.0) : 0.0;
    
    std::cout << "[INFO] " << artwork_type_str << " download completed!" << std::endl;
    std::cout << "[INFO] Results: " << successful_downloads << " downloaded"
              << ", " << skipped_existing << " skipped (already exist)"
              << ", " << failed_downloads << " not available"
              << " (Success rate: " << std::fixed << std::setprecision(1) << success_rate << "%)" << std::endl;
    
    // Callback final
    if (progress_callback && !m_cancel_requested.load()) {
        progress_callback("Download completed!", total_games, total_games, 100.0);
    }
}

bool ThumbnailDownloader::download_single_file(const std::string& rom_name,
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

        const std::string url = ArtworkSources::artwork_url(base, folder, encoded);

        CURL* curl = curl_easy_init();
        if (!curl) return false;

        std::ofstream file(filepath, std::ios::binary);
        if (!file.is_open()) {
            curl_easy_cleanup(curl);
            return false;
        }

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

        CURLcode res = curl_easy_perform(curl);

        long response_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

        curl_easy_cleanup(curl);
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
    bool success = download_single_file(game_name, game_system, emulator, artwork_dir,
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
