// src/RetroRoms.h
//
// RetroRoms (bda.retroroms.info), source de telechargement avec compte.
//
// Le site est un index de dossiers que tout le monde peut parcourir ; seul le
// telechargement demande d'etre connecte (403 sinon). La connexion passe par
// le forum www.retroroms.info (OAuth2) : Bootcade remplit le formulaire de
// connexion du forum avec l'identifiant et le mot de passe que le joueur tape,
// suit la redirection, et ne garde que la session (un fichier de cookies dans
// le dossier de configuration, droits 600). Le mot de passe n'est jamais ecrit.
//
//   FinalBurn Neo  downloads/FBneo/ROMs_Nightly/<systeme>/<jeu>.zip
//   MAME           downloads/mame/currentroms/<jeu>.zip
//   CHD de MAME    downloads/mame/CHDs/<jeu>/<disque>.chd
//
// Quota du site : 100 jeux par 24 h.
//
// Appels bloquants : hors du fil GTK.
#pragma once

#include "DownloadSources.h"

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace RetroRoms {

struct Status {
    bool        answered = false;
    bool        signed_in = false;
    std::string user;          // le nom affiche par le site
    std::string banner;        // la ligne d'en-tete du site, texte brut (quota s'il le dit)
    int         used = -1, limit = -1;   // quota lu dans la ligne, -1 : inconnu
};
Status status();

// Connexion avec l'identifiant (ou e-mail) et le mot de passe du forum.
// `why` dit ce qui a coince.
bool sign_in(const std::string& id, const std::string& password, std::string& why);
void sign_out();

std::string folder_for(const std::string& emulator, const std::string& system);

class Catalog {
public:
    // Comme LolRoms::Catalog : une page par dossier, `date` en AAAA-MM-JJ.
    bool has(const std::string& emulator, const std::string& system, const std::string& name,
             bool is_chd, bool& known, std::string* date = nullptr, std::string* size = nullptr);
private:
    struct Listing { bool ok = false; std::map<std::string, std::string> names, sizes; };
    std::map<std::string, Listing> m_dirs;
};

DownloadSources::Result download(const std::string& emulator, const std::string& system, const std::string& name,
                                 bool is_chd, const std::string& dest_dir, const std::atomic<bool>& cancelled,
                                 const std::function<void(double)>& progress);

}
