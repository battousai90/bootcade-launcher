// src/GameListSetup.h : la liste des jeux de FinalBurn Neo, sans rien demander.
#pragma once

#include <gtkmm.h>
#include <string>

/* Une installation neuve n'a ni dossier DAT ni DAT : la base restait vide,
 * et le joueur devait deviner qu'il fallait un « Update DAT » cache dans un
 * menu, qui s'arretait lui-meme sur « No DAT path configured ».
 *
 * Ici, le dossier se choisit seul (celui du groupe FinalBurn Neo, sinon
 * ~/Bootcade/DAT/FBNeo, a cote de l'emulateur que Bootcade installe) et se
 * remplit seul : depuis le serveur Bootcade, la source par defaut du groupe,
 * et a defaut depuis le FinalBurn Neo installe. */
namespace GameListSetup {

// ~/Bootcade/DAT/FBNeo.
std::string default_fbneo_folder();

// Le dossier des DAT de FinalBurn Neo, fixe une fois pour toutes : celui du
// groupe « fbneo » s'il en a un, sinon dat_path, sinon le dossier par defaut.
// Il est ecrit dans le groupe ET dans dat_path, qu'« Update DAT » lit encore.
std::string adopt_fbneo_folder();

// Le dossier contient-il au moins un DAT ?
bool has_dats(const std::string& folder);

// Remplit `folder` : serveur Bootcade d'abord, `fbneo -dat` s'il ne repond
// pas. Une seule fenetre de progression, aucune question. Vrai si le dossier
// contient des DAT a la fin ; sinon un message a deja dit pourquoi.
bool fill(Gtk::Window& parent, const std::string& folder, const std::string& fbneo_executable);

}  // namespace GameListSetup
