// src/MameOfficial.h
//
// Le MAME officiel sous Windows : MAMEdev publie chaque version sur GitHub
// (mamedev/mame) en archive auto-extractible, mame0xxxb_x64.exe. Bootcade la
// telecharge, en extrait tout le contenu (7-Zip, lu par libarchive) dans son
// propre dossier, et designe mame.exe dans Settings › Emulator.
//
// Sous Linux il n'y a pas de binaire officiel : MAME vient de la distribution.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace MameOfficial {

struct Release {
    bool          ok = false;
    std::string   tag;            // « mame0289 »
    std::string   url;            // l'archive x64
    std::string   sha256;         // calcule par GitHub, vide sur une release ancienne
    std::uint64_t size = 0;
    std::string   error;          // renseigne quand ok == false
};

// La derniere release de mamedev/mame. Appel reseau bloquant : hors du fil GTK.
Release fetch_latest();

std::string install_dir();        // <donnees Bootcade>/mame-official
std::string executable();         // mame.exe installe, ou ""
std::string installed_tag();      // la version installee, ou ""

// Telecharge, verifie (taille, SHA-256), extrait a cote puis remplace
// l'installation precedente. Rien n'est remplace tant que tout n'est pas
// extrait. `progress` recoit 0..1 et un message ; `cancelled` interrompt.
bool install(const Release& release, std::string& error,
             const std::function<void(double, const std::string&)>& progress = nullptr,
             const std::function<bool()>& cancelled = nullptr);

}
