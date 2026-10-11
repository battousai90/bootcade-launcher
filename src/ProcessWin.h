// src/ProcessWin.h
//
// Lancer un programme sous Windows (CreateProcess). L'equivalent de ce que le
// code Linux fait avec fork/execvp/waitpid/pipe, derriere une API aussi petite
// que possible. Uniquement compile sous Windows.
#pragma once
#ifdef _WIN32

#include <functional>
#include <string>
#include <vector>

namespace WinProc {

// `env` : « NOM=valeur » ajoutes a l'environnement de CE seul processus.
// Rend l'identifiant du processus, ou -1 s'il n'a pas pu demarrer. La poignee
// reste ouverte jusqu'a wait() : c'est elle qui permet d'en lire le code de
// sortie meme apres la fin du processus.
int spawn(const std::vector<std::string>& args, const std::vector<std::string>& env = {});

// Bloque jusqu'a la fin du processus lance par spawn(), rend son code de sortie
// (-1 si inconnu) et libere la poignee.
int wait(int pid);

// Vrai tant que le processus tourne.
bool running(int pid);

// Lance `args`, donne sa sortie standard a `sink` par blocs (rend false pour
// s'arreter) et attend sa fin. `timeout_ms` > 0 : au-dela, le processus est
// tue et *timed_out passe a vrai. Pas de fenetre console. Rend false si le
// processus n'a pas pu demarrer.
bool run_streaming(const std::vector<std::string>& args, const std::vector<std::string>& env,
                   const std::function<bool(const char*, size_t)>& sink,
                   int timeout_ms = 0, bool* timed_out = nullptr);

}  // namespace WinProc

#endif
