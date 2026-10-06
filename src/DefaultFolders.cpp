// src/DefaultFolders.cpp
#include "DefaultFolders.h"

#include "AppContext.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace DefaultFolders {
namespace {

// Ecrit une seule fois : un joueur qui supprime ensuite un de ces dossiers,
// ou vide un reglage, ne le voit pas revenir au lancement suivant.
constexpr int kVersion = 1;

const char* const kTree[] = {
    "Artworks/FBNeo/previews", "Artworks/FBNeo/titles",
    "Artworks/Mame/previews",  "Artworks/Mame/titles",
    "Support/DAT/FBNeo",       "Support/DAT/Mame",
    "Support/Catver/FBNeo",    "Support/Catver/Mame",
    "Roms/Inbox", "Roms/Outbox", "Roms/Quarantine",
    "Roms/Library/FBNeo",      "Roms/Library/Mame",
    "Emulators/FBNeo",
};

bool empty_string(const nlohmann::json& o, const char* key) {
    return !o.contains(key) || !o[key].is_string() || o[key].get<std::string>().empty();
}
bool empty_list(const nlohmann::json& o, const char* key) {
    if (!o.contains(key) || !o[key].is_array()) return true;
    for (const auto& v : o[key]) if (v.is_string() && !v.get<std::string>().empty()) return false;
    return true;
}

}  // namespace

std::string root() {
    if (const char* isolated = std::getenv("BOOTCADE_CONFIG_DIR"); isolated && *isolated)
        return (fs::path(isolated) / "Bootcade").string();
    const char* home = std::getenv("HOME");
    return (fs::path(home ? home : ".") / "Bootcade").string();
}

std::string sub(const std::string& relative) {
    return (fs::path(root()) / relative).string();
}

void apply() {
    const std::string path = AppContext::get_config_path();
    nlohmann::json j;
    { std::ifstream in(path); if (in) { try { in >> j; } catch (...) { return; } } }
    if (!j.is_object()) return;
    if (j.value("default_folders", 0) >= kVersion) return;

    std::error_code ec;
    for (const char* d : kTree) {
        fs::create_directories(sub(d), ec);
        if (ec) std::cerr << "[WARN] cannot create " << sub(d) << " : " << ec.message() << std::endl;
    }

    // ── ROM Management : Import, Outbox, Quarantine ──────────────────────
    if (!j.contains("rom_manager") || !j["rom_manager"].is_object()) j["rom_manager"] = nlohmann::json::object();
    auto& rm = j["rom_manager"];
    if (empty_string(rm, "inbox_path"))      rm["inbox_path"]      = sub("Roms/Inbox");
    if (empty_string(rm, "outbox_path"))     rm["outbox_path"]     = sub("Roms/Outbox");
    if (empty_string(rm, "quarantine_path")) rm["quarantine_path"] = sub("Roms/Quarantine");

    // ── Les DAT de FinalBurn Neo ─────────────────────────────────────────
    // Le groupe FinalBurn Neo se construit sur dat_path tant qu'aucun groupe
    // n'est enregistre (DatSource::load_groups).
    if (empty_string(j, "dat_path")) j["dat_path"] = sub("Support/DAT/FBNeo");

    // ── Bibliotheque et images, par emulateur ────────────────────────────
    /* Un fichier d'avant les reglages par emulateur porte encore les cles a
     * plat de FinalBurn Neo : SettingsPanel ne les reprend que tant que
     * "emulators/fbneo" n'existe pas. On complete donc ces cles-la plutot que
     * de creer l'entree, qui les rendrait invisibles. */
    const bool has_emulators = j.contains("emulators") && j["emulators"].is_object();
    const bool fbneo_legacy = !has_emulators &&
        (!empty_list(j, "roms_paths") || !empty_string(j, "roms_path") ||
         !empty_string(j, "previews_path") || !empty_string(j, "titles_path"));
    if (!has_emulators) j["emulators"] = nlohmann::json::object();
    auto& emus = j["emulators"];

    struct Emu { const char* id; const char* folder; };
    for (Emu e : {Emu{"fbneo", "FBNeo"}, Emu{"mame", "Mame"}}) {
        const std::string base = std::string("Artworks/") + e.folder;
        const std::string library = sub(std::string("Roms/Library/") + e.folder);
        if (std::string(e.id) == "fbneo" && fbneo_legacy) {
            if (empty_string(j, "previews_path")) j["previews_path"] = sub(base + "/previews");
            if (empty_string(j, "titles_path"))   j["titles_path"]   = sub(base + "/titles");
            if (empty_list(j, "roms_paths") && empty_string(j, "roms_path"))
                j["roms_paths"] = nlohmann::json::array({library});
            continue;
        }
        if (!emus.contains(e.id) || !emus[e.id].is_object()) {
            emus[e.id] = nlohmann::json::object();
            // Les dossiers MAME d'avant les reglages par emulateur.
            if (std::string(e.id) == "mame" && !empty_string(j, "mame_rompaths")) {
                std::vector<std::string> paths;
                std::istringstream in(j["mame_rompaths"].get<std::string>());
                for (std::string p; std::getline(in, p, ';');) if (!p.empty()) paths.push_back(p);
                emus[e.id]["roms_paths"] = paths;
            }
        }
        auto& lib = emus[e.id];
        if (empty_string(lib, "previews_path")) lib["previews_path"] = sub(base + "/previews");
        if (empty_string(lib, "titles_path"))   lib["titles_path"]   = sub(base + "/titles");
        if (empty_list(lib, "roms_paths"))      lib["roms_paths"]    = nlohmann::json::array({library});
    }

    j["default_folders"] = kVersion;
    std::ofstream out(path, std::ios::trunc);
    if (out) out << j.dump(4) << std::endl;
    std::cout << "[INFO] Default folders ready under " << root() << std::endl;
}

}  // namespace DefaultFolders
