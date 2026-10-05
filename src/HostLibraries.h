// src/HostLibraries.h : ce qu'il manque a la machine pour lancer un emulateur.
#pragma once

#include <string>
#include <vector>

/* FinalBurn Neo et MAME sont des programmes de la machine, lies a ses
 * bibliotheques (SDL2, SDL2_image...). Une Ubuntu fraichement installee n'en a
 * pas toujours : le programme est la, executable, et meurt au chargement avant
 * d'ouvrir la moindre fenetre. Le joueur ne voyait rien se passer.
 *
 * `ldd` les liste toutes d'un coup, la ou le chargeur s'arrete a la premiere.
 * Il est execute sur la machine (hors du bac a sable Flatpak), la ou
 * l'emulateur tournera. */
namespace HostLibraries {

// Les bibliotheques introuvables pour `executable`, par nom de fichier
// (« libSDL2_image-2.0.so.0 »). Vide si tout se resout, ou si ldd n'a pas pu
// repondre : dans le doute, on laisse le lancement se faire.
std::vector<std::string> missing(const std::string& executable);

// Le message a montrer au joueur : la liste, puis la commande a copier pour
// Ubuntu et Debian quand toutes les bibliotheques y ont un paquet connu.
std::string explain(const std::vector<std::string>& libraries);

}  // namespace HostLibraries
