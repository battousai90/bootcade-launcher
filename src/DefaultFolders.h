// src/DefaultFolders.h : le dossier Bootcade et son arborescence par defaut.
#pragma once

#include <string>

/* Au premier lancement, Bootcade cree son dossier dans le dossier personnel
 * du joueur, avec tout ce qu'il y range, et y fait pointer ses reglages :
 *
 *   Bootcade/
 *     Artworks/  FBNeo/{previews,titles}   Mame/{previews,titles}
 *     Support/   DAT/{FBNeo,Mame}          Catver/{FBNeo,Mame}
 *     Roms/      Inbox  Outbox  Quarantine  Library/{FBNeo,Mame}
 *     Emulators/ FBNeo
 *
 * Seuls les reglages VIDES sont remplis : un joueur qui a deja choisi ses
 * dossiers ne voit rien changer, et chacun peut ensuite en designer d'autres
 * dans les reglages. */
namespace DefaultFolders {

// Le dossier de donnees de Bootcade : le dossier de travail quand il y en a un
// (choisi par le joueur, ou portable), sinon default_root().
std::string root();
// ~/Bootcade (ou <BOOTCADE_CONFIG_DIR>/Bootcade pour un build de chantier).
std::string default_root();
// Premier lancement : aucune installation existante, aucun dossier choisi.
// Jamais pour un build de chantier (BOOTCADE_CONFIG_DIR) ; BOOTCADE_ASK_WORK_DIR=1 le force.
bool is_first_run();
// Un sous-dossier de root(), ex. sub("Support/DAT/FBNeo").
std::string sub(const std::string& relative);

// Cree l'arborescence et remplit les reglages vides de config.json. Sans
// effet apres la premiere fois (marque « default_folders » dans config.json).
void apply();

}  // namespace DefaultFolders
