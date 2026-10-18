// src/EmulatorVariants.h
//
// Les versions de chaque emulateur que Bootcade sait installer et faire
// tourner : un groupe de DAT decrit l'une d'elles, et c'est son executable
// qui produit ses DAT.
//
//   fbneo / bootcade   FinalBurn Neo Nightly : le fork Bootcade, reconstruit chaque jour
//   fbneo / official   FinalBurn Neo officiel (release « latest » de finalburnneo)
//   mame  / release    MAME officiel, la version publiee chaque mois
//   mame  / sooner     MAME Sooner : le MAME en developpement, qui prepare la suivante
//
// Un groupe ancien n'a pas de version ecrite : FinalBurn Neo y est la version
// Bootcade (ses DAT viennent du fork), MAME le canal choisi dans les reglages.
#pragma once

#include <string>
#include <vector>

namespace EmulatorVariants {

struct Variant {
    std::string emulator;   // "fbneo" | "mame"
    std::string variant;    // "bootcade" | "official" | "release" | "sooner"
    std::string label;      // ce que la liste affiche
    std::string group_name; // nom par defaut d'un groupe cree pour elle
    std::string short_name; // « FinalBurn Neo Nightly », pour une ligne de texte
};

const std::vector<Variant>& all();

// Le nom court d'une version (« FinalBurn Neo Nightly »).
std::string short_name(const std::string& emulator, const std::string& variant);

// "fbneo:bootcade" et retour.
std::string key(const std::string& emulator, const std::string& variant);
bool        split(const std::string& key, std::string& emulator, std::string& variant);

// La version d'un groupe qui n'en porte pas (groupe ancien).
std::string resolve(const std::string& emulator, const std::string& variant);

// L'executable de cette version, installe et configure, ou "".
std::string executable(const std::string& emulator, const std::string& variant);

// Ce qu'on affiche a cote du nom du groupe : le commit upstream (finalburnneo)
// pour FinalBurn Neo,
// « 0.289 » pour MAME, « 0.290-8aeb286 » pour MAME Sooner. "" si inconnu.
std::string version(const std::string& emulator, const std::string& variant);

// Le dossier par defaut de ses DAT : Support/DAT/fbneo_nightly,
// Support/DAT/fbneo_official, Support/DAT/Mame_0.289_official,
// Support/DAT/Mame_0.290_sooner.
std::string default_folder(const std::string& emulator, const std::string& variant);

}
