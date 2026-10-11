// src/FbneoPaths.h
//
// Ou FinalBurn Neo range ses fichiers : sa config, ses scores, ses captures.
//
//   Linux    le build SDL2 ecrit dans ~/.local/share/fbneo.
//   Windows  le build officiel travaille dans SON dossier : config/, support/ et
//            screenshots/ sont a cote de l'exe, et le fichier de config porte
//            le nom de l'exe (fbneo64.ini).
//
// Le dossier de l'exe se lit dans config.json (« fbneo_executable ») a chaque
// appel : ces chemins ne servent qu'au lancement d'un jeu et a la lecture des
// scores, jamais dans une boucle.
#pragma once
#include <string>

namespace FbneoPaths {

// La racine : tous les autres chemins en partent.
std::string data_dir();

std::string config_dir();      // <racine>/config
std::string ini_path();        // le fichier de config principal de FinalBurn Neo
std::string games_dir();       // <racine>/config/games   (.fs des jeux, .unsaved)
std::string hiscores_dir();    // <racine>/support/hiscores
std::string screenshots_dir(); // <racine>/screenshots
std::string dat_lists_dir();   // ou FinalBurn Neo ecrit ses DAT par defaut

// Pose « <cle> <valeur> » dans le fichier de config de FinalBurn Neo (la ligne
// est remplacee si elle existe, ajoutee sinon, le fichier cree au besoin).
// Rend vrai si le fichier a ete ecrit.
bool set_ini_value(const std::string& key, const std::string& value);

// Un dossier de ROMs, tel que FinalBurn Neo le veut dans son ini : avec un
// separateur final.
std::string with_trailing_separator(std::string path);

}  // namespace FbneoPaths
