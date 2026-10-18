// src/FbneoChannel.h
//
// Quel FinalBurn Neo Bootcade utilise : celui de Bootcade (le fork, par
// defaut) ou le FBNeo officiel (la release « latest » de finalburnneo/FBNeo).
//
// Les deux ne se melangent jamais : chacun a son dossier d'installation, son
// executable memorise a part, et son groupe de DAT. Passer de l'un a l'autre
// ne supprime rien ; on retrouve l'autre tel qu'on l'a laisse.
#pragma once
#include <string>

namespace FbneoChannel {

inline constexpr const char* kBootcade = "bootcade";
inline constexpr const char* kOfficial = "official";

std::string current();            // kBootcade (defaut) ou kOfficial, lu dans config.json
bool        official();

// Cle de config.json qui retient le commit telecharge, une par version.
const char* sha_key();
// Et celle du commit upstream (finalburnneo) de ce build : la version montree.
const char* upstream_key();
// Le zip Linux SDL2 de la version, et l'API de sa release « latest ».
std::string download_url();
std::string releases_url();
// Dossier d'installation propose, sous le dossier Emulators de Bootcade.
std::string install_subfolder();

// DAT : l'officiel n'a pas les memes jeux que le fork, donc son propre groupe.
std::string official_group_id();

struct Switched {
    std::string executable;       // celui de la version choisie ("" s'il n'est pas installe)
    std::string dat_folder;       // dossier du groupe FBNeo actif apres le changement
};
// Bascule. `current_executable` est celui qu'on quitte : il est retenu pour
// le retour. Ecrit tout de suite dans config.json.
Switched switch_to(const std::string& channel, const std::string& current_executable);

}
