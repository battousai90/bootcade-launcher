// src/AppUpdater.h : mettre Bootcade a jour sans quitter Bootcade.
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

/* Le joueur clique sur « Update », et tout le reste se fait seul : le bon
 * fichier de la release est telecharge, son empreinte comparee a SHA256SUMS,
 * il est installe a la place de l'ancien, puis Bootcade redemarre.
 *
 * « Le bon fichier » depend de la facon dont Bootcade a ete installe. Le
 * lanceur le reconnait lui-meme : une AppImage se remplace, un Flatpak se
 * reinstalle, un .deb passe par apt (et donc par le mot de passe du systeme),
 * une archive se decompresse par-dessus son dossier. */
namespace AppUpdater {

enum class Install { AppImage, Flatpak, Deb, Tarball, Unknown };

// Comment CETTE copie de Bootcade a ete installee.
Install detect();

using Assets = std::vector<std::pair<std::string, std::string>>;   // nom, adresse
using Progress = std::function<void(double fraction, const std::string& step)>;

// Telecharge, verifie et installe la release dont `assets` est la liste de
// fichiers. Bloquant : a appeler hors du fil graphique. Rend un message
// d'erreur lisible par le joueur, ou une chaine vide si tout s'est bien passe.
std::string install(const Assets& assets, const Progress& progress);

// Relance Bootcade une fois ce processus termine. A appeler juste avant de
// quitter : l'application est unique, un second exemplaire lance pendant que
// celui-ci vit encore se contenterait de le reveiller, puis disparaitrait.
void restart_after_exit();

}  // namespace AppUpdater
