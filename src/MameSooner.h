// src/MameSooner.h
//
// MAME « Sooner » : le MAME en cours de developpement, au lieu d'attendre la
// version publiee chaque mois.
//
// MAMEdev construit MAME a chaque modification (leur CI Linux), mais ne laisse
// le resultat qu'en artefact GitHub, qui ne se telecharge qu'avec un jeton. Le
// serveur Bootcade (CT 105, fetch-mame-dats.sh) le recupere une fois par jour
// et le republie tel quel, avec sa fiche mame-sooner.json :
//
//   { "schema_version": 1, "build": "0.289 (mame0289-1306-g8aeb28649e1)",
//     "commit": "8aeb2864…", "ci_run": 37195481837, "date": "2026-10-04T12:55:27Z",
//     "file": "mame-sooner.zip", "size": 127149382, "sha256": "…" }
//
// Le meme serveur publie, pour chaque canal, les DAT de ce MAME (dat/mame/
// release/ et dat/mame/sooner/) : le groupe MAME dont la source est « Bootcade
// server » suit le canal choisi ici.
//
// Le canal se regle dans Settings › Emulator (cle « mame_channel » de
// config.json) : « release », le MAME installe ou designe par le joueur, ou
// « sooner », celui-ci une fois telecharge.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace MameSooner {

constexpr const char* kChannelRelease = "release";
constexpr const char* kChannelSooner  = "sooner";

// La fiche publiee par le serveur Bootcade.
constexpr const char* kInfoUrl =
    "https://files.bootcade.duckdns.org/emulators/mame/sooner/mame-sooner.json";

struct Info {
    bool          ok = false;
    std::string   build, commit, date, file, sha256;
    std::uint64_t size = 0;
    std::string   error;      // renseigne quand ok == false
};

// Lit la fiche du serveur. Appel reseau bloquant : hors du fil GTK.
Info fetch_info();

// Le canal enregistre dans config.json : « release » par defaut.
std::string channel();

// Le dossier ou le MAME Sooner est installe : ~/.local/share/bootcade/
// mame-sooner (sous BOOTCADE_CONFIG_DIR quand il est pose, pour qu'un build de
// chantier n'ecrase pas l'installation du joueur). Le meme chemin vu de
// l'hote et du bac a sable Flatpak : c'est l'hote qui lance MAME.
std::string install_dir();

// L'executable installe, ou "" s'il n'y en a pas.
std::string executable();

// Ce qui est installe, tel que l'installation l'a note (build vide : rien).
struct Installed {
    std::string build, commit, date;
};
Installed installed();

// Telecharge le zip de la fiche, verifie sa taille et son SHA-256, puis en
// extrait mame et chdman a la place de l'installation precedente. Rien n'est
// remplace tant que tout n'est pas verifie. `progress` recoit 0..1 et un
// message ; `cancelled` interrompt. Appel bloquant : hors du fil GTK.
bool install(const Info& info, std::string& error,
             const std::function<void(double, const std::string&)>& progress = nullptr,
             const std::function<bool()>& cancelled = nullptr);

// Les bibliotheques que l'executable ne trouve pas sur l'hote (« libQt6Widgets.so.6
// libSDL2_ttf-2.0.so.0 »), vide quand rien ne manque. Le MAME de leur CI est
// lie dynamiquement a SDL2 et Qt6 : sur une machine sans Qt6, il ne demarre pas.
std::string missing_libraries(const std::string& exe);

// L'executable de MAME que Bootcade utilise, de partout : le Sooner si c'est
// le canal choisi et qu'il est installe ; sinon le chemin designe par le
// joueur (`typed`, ou la cle « mame_executable » de config.json quand on ne le
// passe pas) ; sinon celui qu'on trouve sur le systeme.
std::string resolve_executable();
std::string resolve_executable(const std::string& channel, const std::string& typed);

} // namespace MameSooner
