// src/MameControls.h
#pragma once
#include "ControllerConfig.h"
#include <string>

/* Les manettes de Bootcade, traduites pour MAME.
 *
 * FinalBurn Neo lit des fichiers qu'on reecrit ; MAME, lui, accepte un
 * « fichier controleur » (-ctrlrpath / -ctrlr) qui remplace ses reglages par
 * defaut sans toucher a ceux que le joueur a poses lui-meme dans son menu.
 * On le regenere avant chaque lancement, a partir du profil qui vaut pour la
 * machine lancee : une seule section « default » suffit donc, et c'est aussi
 * la seule dont MAME applique les <mapdevice>.
 *
 * Le clavier reste actif : chaque commande garde les touches d'origine de
 * MAME a cote de la manette. */
namespace MameControls {

// Cle d'un jeu MAME dans "game_controller_profiles" : le sf2 de MAME et
// celui de FinalBurn Neo ne partagent pas leur profil.
inline std::string profile_key(const std::string& machine) { return "mame:" + machine; }

// Nom passe a -ctrlr ; le fichier est <dossier>/bootcade.cfg.
constexpr const char* CTRLR_NAME = "bootcade";

// Dossier du fichier, dans la configuration de Bootcade (non cree).
std::string ctrlr_dir();

// Le contenu du fichier pour ce profil.
std::string ctrlr_xml(const ControllerConfig& cfg);

// Ecrit <dir>/bootcade.cfg de facon atomique. false si rien n'a pu etre
// ecrit : il ne faut alors PAS passer -ctrlr, MAME s'arrete net sur un
// fichier controleur absent.
bool write_ctrlr(const ControllerConfig& cfg, const std::string& dir);

// Un joueur au moins a une manette : les numeros de boutons et d'axes ne
// valent alors que sous le pilote SDL « joystick » brut (-joystickprovider
// sdljoy), le seul qui numerote comme /dev/input/js*.
bool uses_pad(const ControllerConfig& cfg);

// Nombre de commandes redefinies dans le <input> d'un .cfg de MAME (0 si
// aucune, -1 si le fichier est absent ou illisible). Elles passent devant
// le fichier controleur.
int input_overrides(const std::string& cfg_file);

// Retire le bloc <input> d'un .cfg de MAME, apres l'avoir copie en .bak.
// Le reste du fichier (compteurs, avertissements, reglages video) ne bouge
// pas. false si le fichier n'a pas pu etre relu ou reecrit.
bool reset_input(const std::string& cfg_file);

} // namespace MameControls
