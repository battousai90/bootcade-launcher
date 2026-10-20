// src/Platform.h
//
// Ce qui differe entre Linux et Windows, a un seul endroit. Le code Linux reste
// tel quel : sous Linux ce fichier n'ajoute rien, il ne fait que nommer ce que
// MSYS2/mingw-w64 ne fournit pas.
#pragma once

#ifdef _WIN32

#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <windows.h>
#include <io.h>

namespace Platform {

// Le dossier personnel : %USERPROFILE% la ou Linux lit $HOME. Pose dans HOME au
// demarrage (voir init()) pour que le code existant, qui lit HOME partout,
// fonctionne sans changement.
inline std::string home_dir() {
    if (const char* h = std::getenv("HOME"); h && *h) return h;
    if (const char* u = std::getenv("USERPROFILE"); u && *u) return u;
    return ".";
}

// Chemin complet de l'executable en cours (l'equivalent de /proc/self/exe).
inline std::string executable_path() {
    char buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameA(nullptr, buf, sizeof buf);
    return std::string(buf, n);
}

// Le dossier portable range les donnees de GTK a cote de l'exe (share/, etc/),
// et non dans un sous-dossier bin/ comme une installation MSYS2 : GLib les
// cherche alors au mauvais endroit (« No GSettings schemas are installed »,
// icones introuvables). On le lui dit.
inline void init() {
    _putenv_s("HOME", home_dir().c_str());
    std::string dir = executable_path();
    dir = dir.substr(0, dir.find_last_of("\\/"));
    if (!std::getenv("XDG_DATA_DIRS"))
        _putenv_s("XDG_DATA_DIRS", (dir + "\\share").c_str());
    if (!std::getenv("GSETTINGS_SCHEMA_DIR"))
        _putenv_s("GSETTINGS_SCHEMA_DIR", (dir + "\\share\\glib-2.0\\schemas").c_str());
    if (!std::getenv("FONTCONFIG_PATH"))
        _putenv_s("FONTCONFIG_PATH", (dir + "\\etc\\fonts").c_str());
}

} // namespace Platform

// POSIX absents de mingw-w64.
inline int setenv(const char* name, const char* value, int) { return _putenv_s(name, value); }
inline int unsetenv(const char* name) { return _putenv_s(name, ""); }

inline time_t timegm(struct tm* t) { return _mkgmtime(t); }

inline struct tm* gmtime_r(const time_t* t, struct tm* out) {
    return gmtime_s(out, t) == 0 ? out : nullptr;
}
inline struct tm* localtime_r(const time_t* t, struct tm* out) {
    return localtime_s(out, t) == 0 ? out : nullptr;
}

// Les deux seuls formats utilises : "%Y-%m-%dT%H:%M:%S" (suivi ou non d'un Z).
// Rend, comme strptime, un pointeur sur la fin de ce qui a ete lu, ou nullptr.
inline const char* strptime(const char* s, const char* fmt, struct tm* tm) {
    std::istringstream in(s);
    in >> std::get_time(tm, fmt);
    if (in.fail()) return nullptr;
    const auto pos = in.tellg();
    return s + (pos < 0 ? std::string(s).size() : static_cast<size_t>(pos));
}

// Statut de sortie d'un fils : GLib rend directement le code sous Windows.
#ifndef WIFEXITED
#define WIFEXITED(s)   (1)
#define WEXITSTATUS(s) (s)
#endif

// access(X_OK) n'existe pas : sous Windows, un fichier est executable par son
// extension, il suffit donc qu'il soit la.
// unistd.h est inclus ici d'abord, pour que sa propre definition de X_OK
// (1, que _access refuse) soit remplacee par 0 (« le fichier existe »).
#include <unistd.h>
#undef X_OK
#define X_OK 0

#else  // Linux et autres POSIX

#include <cstdlib>
#include <string>

namespace Platform {
inline std::string home_dir() {
    const char* h = std::getenv("HOME");
    return h ? h : ".";
}
inline void init() {}
} // namespace Platform

#endif
