// src/ThumbnailDownloader.h
#pragma once

#include <string>
#include <vector>
#include <functional>
#include <map>
#include <thread>
#include <atomic>
#include "Game.h"

/* ── Ou les images des jeux se telechargent ───────────────────────────────
 *
 * Une seule adresse en dur ne pouvait servir qu'un emulateur : celle de
 * FBNeo-extras ne connait rien du catalogue MAME, et un joueur qui heberge
 * sa propre collection d'images n'avait aucun moyen de la designer. Les
 * sources sont donc une LISTE, propre a chaque emulateur, reglee page
 * Library et essayee dans l'ordre jusqu'a ce qu'une image reponde : c'est
 * tout l'interet d'en avoir plusieurs, la couverture de l'une comblant les
 * trous de l'autre.
 *
 * Une source est une adresse de BASE. Le telechargement y ajoute le
 * sous-dossier du type d'image (« previews/ » ou « titles/ ») puis le nom du
 * fichier, exactement comme le faisait l'adresse en dur : une source qui
 * range ses images autrement ne se decrit donc pas encore ici.
 */
namespace ArtworkSources {

// Ce que Bootcade propose quand la configuration ne dit rien. FinalBurn Neo
// garde son depot d'origine ; MAME n'a aucune source connue, et en inventer
// une ferait echouer chaque telechargement sans que personne sache pourquoi.
std::vector<std::string> defaults_for(const std::string& emulator);

// Les adresses que config.json declare pour cet emulateur, a defaut celles
// de defaults_for. Relu depuis le disque : le telechargement tourne dans un
// fil qui ne connait pas l'ecran des reglages.
std::vector<std::string> load_for(const std::string& emulator);

// L'adresse complete d'une image : base + sous-dossier + nom + « .png ».
std::string artwork_url(const std::string& base, const std::string& folder,
                        const std::string& encoded_name);

}  // namespace ArtworkSources

class ThumbnailDownloader {
public:
    // Callback: (current_file, current_index, total_count, progress_percentage)
    using ProgressCallback = std::function<void(const std::string&, int, int, double)>;
    
    ThumbnailDownloader();
    ~ThumbnailDownloader();
    
    // Type of artwork to download
    enum class ArtworkType {
        Previews,
        Titles
    };
    
    // Démarre le téléchargement en arrière-plan
    void start_download(const std::vector<Game>& games, 
                       const std::string& artwork_dir,
                       ArtworkType artwork_type,
                       ProgressCallback progress_callback = nullptr);
    
    // Download single artwork item
    // `emulator` dit dans quelle liste de sources chercher. Il a une valeur
    // par defaut parce que le seul appelant d'aujourd'hui ne connait que
    // FinalBurn Neo ; le telechargement en masse, lui, lit Game::emulator.
    void download_single_artwork(const std::string& game_name,
                                const std::string& game_system,
                                const std::string& artwork_dir,
                                ArtworkType artwork_type,
                                ProgressCallback progress_callback = nullptr,
                                const std::string& emulator = "fbneo");
    
    // Arrête le téléchargement
    void cancel_download();
    
    // Vérifie si un téléchargement est en cours
    bool is_downloading() const;
    
private:
    std::thread m_download_thread;
    std::atomic<bool> m_is_downloading{false};
    std::atomic<bool> m_cancel_requested{false};
    
    // Télécharge un seul artwork file, en essayant les sources dans l'ordre.
    bool download_single_file(const std::string& rom_name,
                             const std::string& system,
                             const std::string& emulator,
                             const std::string& artwork_dir,
                             ArtworkType artwork_type,
                             const std::vector<std::string>& sources);
    
    // URL encode pour les noms de fichiers
    std::string url_encode(const std::string& text);
    
    // Détermine le préfixe de fichier selon le système pour FBNeo-extras
    std::string get_system_prefix(const std::string& system);
    
    // Download worker thread
    void download_worker(const std::vector<Game> games, 
                        const std::string artwork_dir,
                        ArtworkType artwork_type,
                        ProgressCallback progress_callback);
};
