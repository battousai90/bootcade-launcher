// src/DatParser.cpp
#include "Game.h"
#include "DatParser.h"
#include "DatabaseManager.h"
#include "DatSource.h"
#include "MameCatalog.h"
#include <pugixml.hpp>
#include <fstream>
#include <stdlib.h>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <chrono>

namespace {

// Un set s'appelle <game> dans les DAT Logiqx de FinalBurn Neo, <machine>
// dans ceux de MAME (les notres comme ceux de Pleasuredome). Meme contenu.
bool is_set_node(const pugi::xml_node& n) {
    const char* tag = n.name();
    return std::strcmp(tag, "game") == 0 || std::strcmp(tag, "machine") == 0;
}

// Les CHD d'un set : le DAT « CHDs (merged) » de MAME n'a que cela. Un disque
// jamais dumpe, ou sans SHA1, n'a rien a verifier.
void read_disks(const pugi::xml_node& game_node, Game& game) {
    for (auto disk_node : game_node.children("disk")) {
        if (std::strcmp(disk_node.attribute("status").value(), "nodump") == 0) continue;
        Disk d;
        d.name = disk_node.attribute("name").value();
        d.sha1 = disk_node.attribute("sha1").value();
        d.merge = disk_node.attribute("merge").value();
        if (d.name.empty() || d.sha1.empty()) continue;
        std::transform(d.sha1.begin(), d.sha1.end(), d.sha1.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        game.disks.push_back(std::move(d));
    }
}

// Les devices dont une machine MAME a besoin : un zip chacun, a cote du sien.
void read_devices(const pugi::xml_node& game_node, Game& game) {
    for (auto dev : game_node.children("device_ref")) {
        std::string name = dev.attribute("name").value();
        if (name.empty() || name == game.name) continue;
        if (std::find(game.devices.begin(), game.devices.end(), name) == game.devices.end())
            game.devices.push_back(std::move(name));
    }
}

} // namespace

std::vector<Game> DatParser::parse(const std::string& filepath) {
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_file(filepath.c_str());

    if (!result) {
        std::cerr << "Erreur : impossible de charger " << filepath << " (" << result.description() << ")" << std::endl;
        return {};
    }

    std::vector<Game> games;
    auto games_node = doc.child("datafile");
    if (!games_node) {
        std::cerr << "Erreur : pas de <datafile> dans " << filepath << std::endl;
        return {};
    }

    for (auto game_node : games_node.children()) {
        if (!is_set_node(game_node)) continue;
        Game game;
        game.name = game_node.attribute("name").value();
        game.description = game_node.child("description").text().get();
        game.year = game_node.child("year").text().get();
        game.manufacturer = game_node.child("manufacturer").text().get();
        
        // Additional game attributes
        game.cloneof = game_node.attribute("cloneof").value();
        game.romof = game_node.attribute("romof").value();
        game.sourcefile = game_node.attribute("sourcefile").value();
        game.is_bios = std::string(game_node.attribute("isbios").value()) == "yes";
        game.comment = game_node.child("comment").text().get();

        // Video information
        auto video_node = game_node.child("video");
        if (video_node) {
            game.video_type = video_node.attribute("type").value();
            game.orientation = video_node.attribute("orientation").value();
            game.width = video_node.attribute("width").value();
            game.height = video_node.attribute("height").value();
            game.aspect_x = video_node.attribute("aspectx").value();
            game.aspect_y = video_node.attribute("aspecty").value();
        }
        
        // Driver information
        auto driver_node = game_node.child("driver");
        if (driver_node) {
            game.driver_status = driver_node.attribute("status").value();
        }

        // Champs ecrits par notre fork depuis la structure du pilote. Un DAT
        // d'amont ne les porte pas : les champs restent alors vides, ce qui
        // est exactement ce qu'on veut, plutot qu'une valeur inventee.
        game.genre = game_node.child("genre").text().get();
        game.family = game_node.child("family").text().get();
        game.players = game_node.child("players").text().as_int(0);
        game.fb_hiscore = game_node.child("hiscore").text().as_int(0) != 0;

        for (auto rom_node : game_node.children("rom")) {
            // Skip ROMs with status="nodump" (optional files without CRC)
            std::string status = rom_node.attribute("status").value();
            if (status == "nodump") {
                continue;
            }
            
            Rom rom;
            rom.name = rom_node.attribute("name").value();
            rom.size = rom_node.attribute("size").as_ullong();
            rom.crc = rom_node.attribute("crc").value();
            rom.merge = rom_node.attribute("merge").value();
            game.roms.push_back(rom);
        }
        read_disks(game_node, game);
        read_devices(game_node, game);

        // Extract system name from header
        auto header = games_node.child("header");
        if (header) {
            std::string headerName = header.child("name").text().get();
            game.system = extractSystemFromHeader(headerName);
            game.dat_header = headerName;
            game.emulator = emulatorFromHeader(headerName);
        } else {
            game.system = "Unknown";
        }

        game.status = "missing";  // tous les jeux commencent comme missing
        games.push_back(game);
    }

    return games;
}

std::vector<Game> DatParser::parseAllDats(const std::string& directory) {
    std::vector<Game> allGames;
    
    if (!std::filesystem::exists(directory) || !std::filesystem::is_directory(directory)) {
        std::cerr << "Erreur : répertoire DAT non trouvé : " << directory << std::endl;
        return allGames;
    }
    
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && datKind(entry.path().string()) != DatKind::None) {
            std::cout << "Chargement du fichier DAT : " << entry.path().filename() << std::endl;
            auto games = parse(entry.path().string());
            allGames.insert(allGames.end(), games.begin(), games.end());
        }
    }
    
    std::cout << "Total de jeux chargés : " << allGames.size() << std::endl;
    return allGames;
}

int DatParser::parseToDatabase(const std::string& filepath, std::shared_ptr<DatabaseManager> db,
                               std::string* note) {
    const std::string filename = std::filesystem::path(filepath).filename().string();
    int games_count = -1;

    if (datKind(filepath) == DatKind::MameListxml) {
        // Un fichier -listxml brut (celui de progettosnaps, ou la sortie de
        // l'executable enregistree) : pas un DAT Logiqx, mais il decrit les
        // memes sets, liens compris (cloneof, romof, merge=, device_ref). Il
        // est lu tel quel, sans DAT intermediaire, sous le nom de CE fichier.
        std::vector<Game> sets;
        std::string version;
        if (MameCatalog::read_listxml_file(filepath, sets, &version) < 0) return -1;
        for (auto& g : sets) {
            g.system     = extractSystemFromHeader(MameCatalog::kHeader);
            g.dat_header = MameCatalog::kHeader;
            g.dat_source = filename;
            g.status     = "missing";
        }
        games_count = insertGames(sets, db);
        if (games_count < 0) return -1;
        if (note) *note = "MAME -listxml " + version + ": " + std::to_string(games_count) + " sets";
    } else {
        games_count = importDatafile(filepath, db, filename, note);
        if (games_count < 0) return -1;
    }

    // Register the DAT file in the database
    auto ftime = std::filesystem::last_write_time(std::filesystem::path(filepath));
    time_t last_modified = std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch()).count();
    size_t file_size = std::filesystem::file_size(filepath);
    db->registerDatFile(filename, filepath, last_modified, file_size, games_count);

    std::cout << "Imported " << games_count << " games from " << filepath << std::endl;
    return games_count;
}

int DatParser::insertGames(const std::vector<Game>& games, std::shared_ptr<DatabaseManager> db) {
    if (!db->beginTransaction()) {
        std::cerr << "Erreur : impossible de démarrer la transaction" << std::endl;
        return -1;
    }
    for (const auto& g : games) {
        if (!db->insertGame(g)) {
            std::cerr << "Erreur insertion jeu: " << g.name << std::endl;
            db->rollbackTransaction();
            return -1;
        }
    }
    if (!db->commitTransaction()) {
        std::cerr << "Erreur : impossible de valider la transaction" << std::endl;
        return -1;
    }
    return (int)games.size();
}

int DatParser::importDatafile(const std::string& filepath, std::shared_ptr<DatabaseManager> db,
                              const std::string& dat_source, std::string* note) {
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_file(filepath.c_str());

    if (!result) {
        std::cerr << "Erreur : impossible de charger " << filepath << " (" << result.description() << ")" << std::endl;
        return -1;
    }

    auto games_node = doc.child("datafile");
    if (!games_node) {
        std::cerr << "Erreur : pas de <datafile> dans " << filepath << std::endl;
        return -1;
    }

    // Extract system name from header once. The raw header is kept alongside the
    // trimmed system name: it is what the ROM manager names its output folders
    // after, so a rebuilt tree matches a library laid out from the same DATs.
    std::string system = "Unknown";
    std::string dat_header;
    auto header = games_node.child("header");
    if (header) {
        dat_header = header.child("name").text().get();
        system = extractSystemFromHeader(dat_header);
    }
    // L'emulateur fait partie de l'identite d'un set : sans lui, les 28 203
    // machines de MAME ecraseraient leurs homonymes FinalBurn Neo, qui vivent
    // dans le meme system 'Arcade' (mslug existe des deux cotes).
    const std::string emulator = emulatorFromHeader(dat_header);
    const std::string& filename = dat_source;

    // Les listes de logiciels de MAME (cartouches, disquettes : « MAME
    // Software List … ») decrivent des medias, pas des machines : le
    // gestionnaire de ROMs ne sait pas encore ou les ranger ni comment les
    // auditer. Rien n'en est importe, et on le dit.
    if (isSoftwareListHeader(dat_header) || games_node.child("software")) {
        if (note) *note = "software list DAT: not handled by the ROM Manager yet, nothing imported";
        return 0;
    }

    // Begin transaction for batch insert - MASSIVE performance boost
    if (!db->beginTransaction()) {
        std::cerr << "Erreur : impossible de démarrer la transaction" << std::endl;
        return -1;
    }

    int games_count = 0;
    bool error_occurred = false;

    for (auto game_node : games_node.children()) {
        if (!is_set_node(game_node)) continue;
        Game game;
        game.name = game_node.attribute("name").value();
        game.description = game_node.child("description").text().get();
        game.year = game_node.child("year").text().get();
        game.manufacturer = game_node.child("manufacturer").text().get();
        game.system = system;

        // Additional game attributes
        game.cloneof = game_node.attribute("cloneof").value();
        game.romof = game_node.attribute("romof").value();
        game.sourcefile = game_node.attribute("sourcefile").value();
        game.is_bios = std::string(game_node.attribute("isbios").value()) == "yes";
        game.comment = game_node.child("comment").text().get();

        // Video information
        auto video_node = game_node.child("video");
        if (video_node) {
            game.video_type = video_node.attribute("type").value();
            game.orientation = video_node.attribute("orientation").value();
            game.width = video_node.attribute("width").value();
            game.height = video_node.attribute("height").value();
            game.aspect_x = video_node.attribute("aspectx").value();
            game.aspect_y = video_node.attribute("aspecty").value();
        }

        // Driver information
        auto driver_node = game_node.child("driver");
        if (driver_node) {
            game.driver_status = driver_node.attribute("status").value();
        }

        // Champs ecrits par notre fork depuis la structure du pilote. Un DAT
        // d'amont ne les porte pas : les champs restent alors vides, ce qui
        // est exactement ce qu'on veut, plutot qu'une valeur inventee.
        game.genre = game_node.child("genre").text().get();
        game.family = game_node.child("family").text().get();
        game.players = game_node.child("players").text().as_int(0);
        game.fb_hiscore = game_node.child("hiscore").text().as_int(0) != 0;

        // Parse ROMs
        for (auto rom_node : game_node.children("rom")) {
            Rom rom;
            rom.name = rom_node.attribute("name").value();
            rom.size = rom_node.attribute("size").as_ullong();
            rom.crc = rom_node.attribute("crc").value();
            rom.merge = rom_node.attribute("merge").value();
            game.roms.push_back(rom);
        }
        read_disks(game_node, game);
        read_devices(game_node, game);

        game.status = "missing";  // Default status
        game.dat_source = filename;  // Set the source DAT file
        game.dat_header = dat_header;
        game.emulator = emulator;

        if (!db->insertGame(game)) {
            std::cerr << "Erreur insertion jeu: " << game.name << std::endl;
            error_occurred = true;
            break;
        }

        games_count++;
    }

    // Commit or rollback transaction
    if (error_occurred) {
        db->rollbackTransaction();
        return -1;
    }

    if (!db->commitTransaction()) {
        std::cerr << "Erreur : impossible de valider la transaction" << std::endl;
        return -1;
    }
    return games_count;
}

bool DatParser::isSoftwareListHeader(const std::string& headerName) {
    std::string h = headerName;
    std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return h.find("software list") != std::string::npos;
}

DatParser::DatKind DatParser::datKind(const std::string& filepath) {
    std::string ext = std::filesystem::path(filepath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (ext != ".dat" && ext != ".xml") return DatKind::None;

    // La racine suffit, et elle est au debut : apres le prologue et la DTD
    // (160 lignes pour -listxml). « <!DOCTYPE mame » ou « <!ELEMENT mame »
    // ne contiennent pas « <mame », on peut donc chercher la balise telle
    // quelle.
    std::ifstream in(filepath, std::ios::binary);
    std::string head(64 * 1024, '\0');
    if (in) {
        in.read(&head[0], (std::streamsize)head.size());
        head.resize((size_t)in.gcount());
    } else {
        head.clear();
    }
    auto tag_at = [&](const char* tag) {
        const size_t len = std::strlen(tag);
        for (size_t p = head.find(tag); p != std::string::npos; p = head.find(tag, p + 1)) {
            const size_t q = p + len;
            if (q < head.size() && (head[q] == ' ' || head[q] == '>' || head[q] == '\t' ||
                                    head[q] == '\r' || head[q] == '\n'))
                return p;
        }
        return std::string::npos;
    };
    const size_t mame = tag_at("<mame"), datafile = tag_at("<datafile");
    if (mame != std::string::npos && (datafile == std::string::npos || mame < datafile))
        return DatKind::MameListxml;
    if (datafile != std::string::npos) return DatKind::Datafile;
    // Un .dat a toujours ete charge sans examen : il le reste, et c'est le
    // parseur qui dira s'il ne contient pas de <datafile>. Un .xml, lui,
    // n'est un DAT que si sa racine le dit : un dossier peut en contenir
    // d'autres.
    return ext == ".dat" ? DatKind::Datafile : DatKind::None;
}

bool DatParser::parseAllDatsToDatabase(const std::string& directory, std::shared_ptr<DatabaseManager> db) {
    if (!std::filesystem::exists(directory) || !std::filesystem::is_directory(directory)) {
        std::cerr << "Erreur : répertoire DAT non trouvé : " << directory << std::endl;
        return false;
    }

    // Clear existing data
    if (!db->clearAllData()) {
        std::cerr << "Erreur vidage base de données" << std::endl;
        return false;
    }

    int total_games = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && datKind(entry.path().string()) != DatKind::None) {
            std::cout << "Chargement du fichier DAT : " << entry.path().filename() << std::endl;
            int games_loaded = parseToDatabase(entry.path().string(), db);
            if (games_loaded >= 0) {
                total_games += games_loaded;
            } else {
                std::cerr << "Erreur chargement " << entry.path().filename() << std::endl;
                return false;
            }
        }
    }

    std::cout << "Tous les DAT ont été chargés dans la base de données (" << total_games << " jeux)" << std::endl;
    return true;
}

bool DatParser::synchronizeDatsToDatabase(const std::string& directory, std::shared_ptr<DatabaseManager> db) {
    if (!std::filesystem::exists(directory) || !std::filesystem::is_directory(directory)) {
        std::cerr << "Erreur : répertoire DAT non trouvé : " << directory << std::endl;
        return false;
    }
    
    std::cout << "[SYNC] Starting DAT ↔ DB synchronization..." << std::endl;
    
    // 1. Collecter tous les fichiers DAT existants
    std::vector<std::string> existing_dat_files;
    std::vector<std::string> outdated_files;
    
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && datKind(entry.path().string()) != DatKind::None) {
            std::string filename = entry.path().filename().string();
            existing_dat_files.push_back(filename);
            
            auto ftime = std::filesystem::last_write_time(entry);
            time_t last_modified = std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch()).count();
            size_t file_size = std::filesystem::file_size(entry);
            
            if (!db->isDatFileUpToDate(filename, last_modified, file_size)) {
                outdated_files.push_back(entry.path().string());
                std::cout << "[SYNC] DAT modifié détecté: " << filename << std::endl;
            }
        }
    }
    
    // 2. Supprimer les DAT qui n'existent plus sur le disque
    if (!db->removeUnreferencedDatFiles(existing_dat_files)) {
        std::cerr << "[SYNC] Erreur nettoyage DAT obsolètes" << std::endl;
    }
    
    // 3. Supprimer les jeux des DAT modifiés/supprimés
    for (const auto& filepath : outdated_files) {
        std::string filename = std::filesystem::path(filepath).filename().string();
        std::cout << "[SYNC] Suppression des jeux du DAT: " << filename << std::endl;
        db->removeGamesFromDat(filename);
    }
    
    // 4. Recharger les DAT modifiés
    int updated_count = 0;
    for (const auto& filepath : outdated_files) {
        std::cout << "[SYNC] Rechargement: " << std::filesystem::path(filepath).filename() << std::endl;
        int games_loaded = parseToDatabase(filepath, db);
        if (games_loaded >= 0) {
            updated_count++;
        } else {
            std::cerr << "[SYNC] Erreur rechargement: " << filepath << std::endl;
        }
    }
    
    if (outdated_files.empty()) {
        std::cout << "[SYNC] ✅ No synchronization needed - DB up to date" << std::endl;
    } else {
        std::cout << "[SYNC] ✅ Synchronization completed: " << updated_count << "/" << outdated_files.size() << " DAT files reloaded" << std::endl;
    }
    
    return true;
}

std::string DatParser::emulatorFromHeader(const std::string& headerName) {
    // On se fie a l'en-tete parce que c'est la seule chose que le fichier
    // porte lui-meme : le nom du fichier peut etre change et le dossier peut
    // etre partage par les deux emulateurs, alors que <header><name> est
    // ecrit par celui qui produit le DAT. MameCatalog::generate_dats ecrit
    // « MAME ROMs (split) », « MAME ROMs (bios-devices) », « MAME CHDs
    // (merged) », comme Pleasuredome, ou « MAME » seul pour une collection en
    // un seul dossier ; ses versions precedentes ecrivaient
    // « MAME - Arcade Games ». FinalBurn Neo ecrit « FinalBurn Neo - ... ».
    if (headerName == "MAME" || headerName.rfind("MAME ", 0) == 0) return "mame";
    // Tout le reste est traite comme FinalBurn Neo : c'est le seul catalogue
    // qui existait avant, et une base deja remplie doit garder son emulateur.
    return "fbneo";
}

std::string DatParser::extractSystemFromHeader(const std::string& headerName) {
    // Extract system name from header format: "FinalBurn Neo - System Games"
    size_t dashPos = headerName.find(" - ");
    if (dashPos != std::string::npos) {
        std::string systemPart = headerName.substr(dashPos + 3);
        // Remove " Games" suffix if present
        size_t gamesPos = systemPart.find(" Games");
        if (gamesPos != std::string::npos) {
            systemPart = systemPart.substr(0, gamesPos);
        }
        return systemPart;
    }
    // « MAME ROMs (split) » → « ROMs (split) » : ce qui suit la marque decrit
    // la collection, et c'est ce qui distingue le neogeo du DAT split (ses
    // seules ROMs) de celui du DAT bios-devices (tout ce qu'il lui faut). Un
    // numero de version eventuel (« MAME 0.289 ROMs (split) ») n'en fait pas
    // partie : il changerait l'identite de chaque set a chaque version.
    // « MAME » seul : le DAT ecrit depuis -listxml (MameCatalog::kHeader).
    if (headerName == "MAME") return "MAME";
    if (headerName.rfind("MAME ", 0) == 0) {
        std::string rest = headerName.substr(5);
        if (!rest.empty() && std::isdigit((unsigned char)rest[0])) {
            const size_t sp = rest.find(' ');
            rest = sp == std::string::npos ? std::string() : rest.substr(sp + 1);
        }
        if (!rest.empty()) return rest;
    }
    return "Unknown";
}
