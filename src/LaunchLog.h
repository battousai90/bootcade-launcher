// src/LaunchLog.h
//
// Ce que l'emulateur a dit pendant le dernier lancement.
//
// MAME et FinalBurn Neo expliquent tres bien pourquoi ils refusent un jeu
// (« dvp-0003b NOT FOUND », « is not supported »), mais sur leur sortie : un
// launcher qui la jette laisse le joueur devant une fenetre qui ne s'ouvre
// pas, sans un mot. Chaque lancement ecrit donc cette sortie dans un fichier
// du dossier de configuration, et un arret anormal la montre au joueur avec
// une explication en clair.
#pragma once

#include <string>

namespace Gtk { class Window; }

namespace LaunchLog {

// <config>/logs/<emulateur>-last.log : un fichier par emulateur, ecrase a
// chaque lancement. Le dossier est cree au besoin.
std::string path_for(const std::string& emulator);

// Vide le fichier et y ecrit l'en-tete (date, commande). Le processus lance
// y ajoute ensuite sa propre sortie.
void begin(const std::string& path, const std::string& emulator_label,
           const std::string& command_line);

// Comment le processus s'est termine, sous une forme commune a Linux et
// Windows.
struct Exit {
    bool signaled = false;   // tue par un signal ou un plantage
    int  code = 0;           // code de sortie, ou numero du signal
    int  seconds = 0;        // duree de la partie
};

#ifndef _WIN32
// Depuis le `status` de waitpid.
Exit from_wait_status(int status, int seconds);
#else
// Depuis le code rendu par WinProc::wait.
Exit from_exit_code(int code, int seconds);
#endif

// Ajoute au journal comment le processus s'est termine. Appele par le fil
// qui attend l'emulateur, une fois celui-ci ferme.
void finish(const std::string& path, const Exit& e);

// Vrai quand l'arret merite d'etre explique : une erreur, un plantage, ou une
// fermeture si rapide qu'aucune partie n'a pu avoir lieu.
bool looks_failed(const Exit& e);

// Ce qu'on sait du jeu au moment du lancement, pour completer le message.
struct Game {
    std::string title;          // nom affiche
    std::string emulator;       // "mame" ou "fbneo"
    std::string driver_status;  // good | imperfect | preliminary (MAME)
};

// La boite : la cause en clair, les dernieres lignes de l'emulateur, et de
// quoi ouvrir ou copier le journal complet. A appeler sur le fil graphique.
void show_report(Gtk::Window& parent, const Game& game, const Exit& exit,
                 const std::string& log_path);

// La meme boite, ouverte a la main pour le dernier lancement de cet
// emulateur, meme s'il s'est bien passe.
void show_last(Gtk::Window& parent, const std::string& emulator);

}  // namespace LaunchLog
