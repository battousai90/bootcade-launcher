// src/Workspace.h : changer le dossier ou Bootcade range ses fichiers.
//
// Le dossier de travail porte config.json, games.db et le dossier Bootcade
// (DAT, images, ROMs de travail, emulateurs). Le deplacer demande que la base
// soit fermee : Reglages demande le changement (request), le launcher redemarre
// seul, et le deplacement se fait au demarrage suivant, avant que la base ne
// s'ouvre (run_pending). Au premier lancement le joueur choisit son dossier
// (ask_first_run) et le deplacement est immediat : rien n'est encore ouvert.
#pragma once

#include <functional>
#include <string>

namespace Workspace {

using Say = std::function<void(const std::string&)>;

// Un changement demande par les Reglages attend le prochain demarrage.
bool pending();

// Verifie le dossier voulu et retient la demande. Rend "" si tout va bien,
// sinon la phrase a montrer au joueur.
std::string request(const std::string& to);

// Deplace config.json, games.db et le dossier Bootcade vers `to`, reecrit les
// chemins de config.json, retient le nouvel emplacement. Reprenable : ce qui est
// deja arrive n'est pas refait. Rend "" ou la phrase d'erreur.
std::string relocate(const std::string& to, const Say& say);

// Execute la demande en attente, s'il y en a une.
std::string run_pending(const Say& say);

// Le deplacement a eu lieu pendant CE lancement : les chemins que la base garde
// (cache des ROMs) ne valent plus.
bool relocated_this_run();

// Fenetre du premier lancement : rend le dossier choisi ("" : celui par defaut).
std::string ask_first_run();

// Fenetre « deplacement en cours » autour de run_pending, puis du retour.
void run_pending_with_window();

}  // namespace Workspace
