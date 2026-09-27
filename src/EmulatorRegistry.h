// src/EmulatorRegistry.h
//
// La liste des emulateurs que Bootcade sait presenter.
//
// Elle existe pour que l'interface n'ait jamais a nommer « fbneo » ou « mame »
// en dur : le selecteur, la modale et les compteurs parcourent ce registre.
// Ajouter un emulateur, c'est ajouter une entree ici et une source de
// catalogue ; aucun ecran n'a besoin d'etre retouche.
#pragma once

#include <string>
#include <vector>

struct EmulatorInfo {
    std::string id;        // la valeur portee par Game::emulator
    std::string name;      // ce que l'utilisateur lit
    std::string logo;      // chemin sous assets/icons/, marque de l'emulateur
    std::string tagline;   // une ligne : ce que cet emulateur couvre
};

namespace EmulatorRegistry {

// Toutes les entrees connues, dans l'ordre d'affichage.
const std::vector<EmulatorInfo>& all();

// L'entree d'un identifiant, ou nullptr. Rendre nullptr est un cas normal :
// une base peut porter un emulateur qu'une version plus ancienne ignore.
const EmulatorInfo* find(const std::string& id);

// Le nom lisible, ou l'identifiant lui-meme en dernier recours, pour qu'un
// libelle ne soit jamais vide.
std::string display_name(const std::string& id);

}  // namespace EmulatorRegistry
