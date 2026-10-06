// src/FbneoUpdateCheck.h
//
// Checks whether battousai90/FBNeo's "latest" release (our SDL2/Linux fork,
// synced from upstream daily by its own CI : see the landing page's FBNeo
// section) has moved past whatever build the user last downloaded through
// this launcher. Blocking network call; callers run it off the GTK thread.
#pragma once
#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace FbneoUpdateCheck {

struct Result {
    bool ok = false;          // false on any network/parse failure
    std::string sha;          // target_commitish : the exact commit this build is
    std::string tag;          // release tag_name, e.g. "latest"
    std::string published_at; // ISO 8601, for display
    std::string error;        // set when ok == false
    // Les fichiers joints a la release (nom, adresse de telechargement) :
    // la mise a jour automatique y choisit celui de son mode d'installation.
    std::vector<std::pair<std::string, std::string>> assets;
};

// GET https://api.github.com/repos/battousai90/FBNeo/releases/tags/latest
Result fetch_latest();

    // La derniere version publiee du lanceur lui-meme. Le flatpak est
    // distribue en bundle et non par un depot, l'AppImage et l'archive n'ont
    // aucun mecanisme de mise a jour : sans cette verification, un utilisateur
    // reste sur sa version pour toujours sans jamais l'apprendre.
    Result fetch_launcher_latest();

// Parses a GitHub-style UTC timestamp ("2026-08-18T02:11:31Z"). Returns -1 on
// a malformed string, which callers should treat as "can't tell, don't guess".
std::time_t parse_iso8601(const std::string& s);

}
