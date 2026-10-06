// src/HostLibraries.cpp
#include "HostLibraries.h"
#include "AppContext.h"
#include "i18n.h"

#include <glibmm.h>
#include <map>
#include <sstream>

namespace HostLibraries {

std::vector<std::string> missing(const std::string& executable) {
    std::vector<std::string> out;
    if (executable.empty()) return out;

    std::string stdout_text;
    int status = 0;
    try {
        Glib::spawn_sync("", AppContext::host_command({"ldd", executable}),
                         Glib::SPAWN_SEARCH_PATH | Glib::SPAWN_STDERR_TO_DEV_NULL,
                         Glib::SlotSpawnChildSetup(), &stdout_text, nullptr, &status);
    } catch (const Glib::Error&) {
        return out;
    }

    // « \tlibSDL2_image-2.0.so.0 => not found »
    std::istringstream lines(stdout_text);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.find("=> not found") == std::string::npos) continue;
        const auto first = line.find_first_not_of(" \t");
        const auto end = line.find(' ', first);
        if (first == std::string::npos || end == std::string::npos) continue;
        out.push_back(line.substr(first, end - first));
    }
    return out;
}

namespace {

// Lance `args` sur la machine et rend sa sortie ; vide en cas d'echec.
bool run_on_host(const std::vector<std::string>& args, std::string* out = nullptr) {
    std::string text;
    int status = 0;
    try {
        Glib::spawn_sync("", AppContext::host_command(args),
                         Glib::SPAWN_SEARCH_PATH | Glib::SPAWN_STDERR_TO_DEV_NULL,
                         Glib::SlotSpawnChildSetup(), &text, nullptr, &status);
    } catch (const Glib::Error&) {
        return false;
    }
    if (out) *out = text;
    return status == 0;
}

/* Le gestionnaire de paquets de la machine, deduit de son os-release.
 *
 * Lu sur la machine et non dans le bac a sable : le /etc/os-release d'un
 * Flatpak decrit son runtime, pas le systeme ou l'emulateur tournera. */
enum class Family { Unknown, Apt, Dnf, RpmOstree, Pacman, Zypper };

struct HostSystem {
    Family      family = Family::Unknown;
    std::string name;            // NAME= : « Bazzite », « Ubuntu »
};

std::string os_release_value(const std::string& text, const std::string& key) {
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.compare(0, key.size() + 1, key + "=") != 0) continue;
        std::string v = line.substr(key.size() + 1);
        if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front())
            v = v.substr(1, v.size() - 2);
        return v;
    }
    return {};
}

HostSystem detect_host() {
    HostSystem sys;
    std::string text;
    if (!run_on_host({"cat", "/etc/os-release"}, &text) || text.empty())
        if (!run_on_host({"cat", "/usr/lib/os-release"}, &text)) return sys;

    sys.name = os_release_value(text, "NAME");
    const std::string id = " " + os_release_value(text, "ID") + " "
                         + os_release_value(text, "ID_LIKE") + " ";
    auto is = [&](const char* word) {
        return id.find(std::string(" ") + word + " ") != std::string::npos;
    };

    if (is("debian") || is("ubuntu")) {
        sys.family = Family::Apt;
    } else if (is("fedora")) {
        /* Bazzite, Bluefin, Silverblue, Kinoite : le systeme est en lecture
         * seule, dnf n'y installe rien. rpm-ostree superpose le paquet, actif
         * au redemarrage suivant. /run/ostree-booted est le marqueur que pose
         * ostree au demarrage de ces systemes. */
        sys.family = run_on_host({"test", "-e", "/run/ostree-booted"})
                   ? Family::RpmOstree : Family::Dnf;
    } else if (is("suse") || is("opensuse")) {
        sys.family = Family::Zypper;
    } else if (is("arch") && !is("steamos")) {
        // SteamOS est une Arch en lecture seule, que chaque mise a jour
        // remplace : un pacman y serait efface au prochain update.
        sys.family = Family::Pacman;
    }
    return sys;
}

}  // namespace

std::string explain(const std::vector<std::string>& libraries) {
    // Les paquets des bibliotheques des builds SDL2 de FinalBurn Neo, par
    // famille de distribution. Une bibliotheque absente de cette table fait
    // taire la commande : une commande incomplete laisserait croire que tout
    // est regle.
    struct Packages { const char* apt; const char* rpm; const char* pacman; const char* zypper; };
    static const std::map<std::string, Packages> packages = {
        {"libSDL2-2.0.so.0",       {"libsdl2-2.0-0",       "SDL2",            "sdl2",          "libSDL2-2_0-0"}},
        {"libSDL2_image-2.0.so.0", {"libsdl2-image-2.0-0", "SDL2_image",      "sdl2_image",    "libSDL2_image-2_0-0"}},
        {"libsamplerate.so.0",     {"libsamplerate0",      "libsamplerate",   "libsamplerate", "libsamplerate0"}},
        {"libGL.so.1",             {"libgl1",              "libglvnd-glx",    "libglvnd",      "libGL1"}},
        {"libasound.so.2",         {"libasound2t64",       "alsa-lib",        "alsa-lib",      "libasound2"}},
        {"libpulse.so.0",          {"libpulse0",           "pulseaudio-libs", "libpulse",      "libpulse0"}},
    };

    const HostSystem host = detect_host();
    std::string command;
    switch (host.family) {
        case Family::Apt:       command = "sudo apt install";         break;
        case Family::Dnf:       command = "sudo dnf install";         break;
        case Family::RpmOstree: command = "sudo rpm-ostree install";  break;
        case Family::Pacman:    command = "sudo pacman -S";           break;
        case Family::Zypper:    command = "sudo zypper install";      break;
        case Family::Unknown:   break;
    }

    std::string list;
    bool all_known = !command.empty();
    for (const auto& lib : libraries) {
        list += "\n• " + lib;
        auto it = packages.find(lib);
        if (it == packages.end()) { all_known = false; continue; }
        const char* pkg = nullptr;
        switch (host.family) {
            case Family::Apt:       pkg = it->second.apt;    break;
            case Family::Dnf:
            case Family::RpmOstree: pkg = it->second.rpm;    break;
            case Family::Pacman:    pkg = it->second.pacman; break;
            case Family::Zypper:    pkg = it->second.zypper; break;
            case Family::Unknown:   break;
        }
        if (pkg && command.find(std::string(" ") + pkg) == std::string::npos)
            command += std::string(" ") + pkg;
    }

    std::string text = _("It needs libraries that are not installed on this computer:") + list;
    if (all_known) {
        const std::string system = host.name.empty() ? std::string("Linux") : host.name;
        text += "\n\n" + Glib::ustring::compose(_("On %1, install them with:"), system).raw()
              + "\n" + command;
        if (host.family == Family::RpmOstree)
            text += "\n\n" + std::string(_("Then restart the computer: on this system, new packages only become active after a restart."));
    } else {
        text += "\n\n" + std::string(_("Install them with your package manager, then try again."));
    }
    return text;
}

}  // namespace HostLibraries
