// src/LolRoms.h
//
// LolRoms (lolroms.com), seconde source de telechargement du ROM Manager, a
// cote du serveur Bootcade. Le site est un simple index de dossiers :
//
//   FinalBurn Neo  Arcade/FBNeo/<systeme>/<jeu>.7z   (arcade, megadrive, nes...)
//   MAME           Arcade/MAME/<jeu>.7z
//   CHD de MAME    Arcade/MAME/chd/<jeu>/<disque>.chd
//
// Un fichier se telecharge par https://lolroms.com/?file=<chemin>. Un fichier
// absent rend une page HTML avec le code 200 : c'est le type du contenu qui dit
// si l'on a recu un fichier. Pas de compte ni de quota. Import verifie ensuite
// chaque archive contre le DAT et la range, comme pour Bootcade.
//
// Appels bloquants : hors du fil GTK.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <set>
#include <vector>
#include <string>

namespace LolRoms {

// Le dossier du site pour un systeme de la base, ou "" s'il n'y en a pas.
std::string folder_for(const std::string& emulator, const std::string& system);

struct Result {
    bool        ok = false;
    std::string path;          // l'archive (ou le dossier des CHD) ecrit
    std::string reason;        // pourquoi pas
    bool        site_fault = false;   // le site ne repond pas : inutile de continuer
};

// Ce que le site propose, lu une fois par dossier (une page par systeme, pas
// une requete par jeu). Pour un audit : savoir d'avance quels jeux il a.
class Catalog {
public:
    // Vrai si le site a ce jeu ; faux s'il ne l'a pas ; `known` faux si le
    // site n'a pas repondu (on ne sait pas : rien n'est affirme).
    // `date` recoit la date du fichier sur le site (AAAA-MM-JJ), quand il y est.
    bool has(const std::string& emulator, const std::string& system, const std::string& name,
             bool is_chd, bool& known, std::string* date = nullptr, std::string* size = nullptr);
private:
    struct Listing { bool ok = false; std::map<std::string, std::string> names, sizes; };   // jeu -> date, taille
    std::map<std::string, Listing> m_dirs;   // dossier du site -> ce qu'il contient
};

// L'archive recue contient-elle tous ces CRC ? Le site garde parfois une
// ancienne version sous le meme nom : seul le contenu le dit.
bool has_all_crcs(const std::string& archive, const std::vector<unsigned long>& crcs);

// Un jeu, ecrit dans dest_dir : <jeu>.7z, ou <jeu>/<disque>.chd pour un CHD.
Result download(const std::string& emulator, const std::string& system, const std::string& name,
                bool is_chd, const std::string& dest_dir, const std::atomic<bool>& cancelled,
                const std::function<void(double)>& progress);

}
