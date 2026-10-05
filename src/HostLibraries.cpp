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

std::string explain(const std::vector<std::string>& libraries) {
    // Les paquets Ubuntu et Debian des bibliotheques des builds SDL2 de
    // FinalBurn Neo. Une bibliotheque absente de cette table fait taire la
    // commande : une commande incomplete laisserait croire que tout est regle.
    static const std::map<std::string, std::string> packages = {
        {"libSDL2-2.0.so.0",       "libsdl2-2.0-0"},
        {"libSDL2_image-2.0.so.0", "libsdl2-image-2.0-0"},
        {"libsamplerate.so.0",     "libsamplerate0"},
        {"libGL.so.1",             "libgl1"},
        {"libasound.so.2",         "libasound2t64"},
        {"libpulse.so.0",          "libpulse0"},
    };

    std::string list;
    std::string command = "sudo apt install";
    bool all_known = true;
    for (const auto& lib : libraries) {
        list += "\n• " + lib;
        auto it = packages.find(lib);
        if (it == packages.end()) all_known = false;
        else if (command.find(" " + it->second) == std::string::npos) command += " " + it->second;
    }

    std::string text = _("It needs libraries that are not installed on this computer:") + list;
    if (all_known)
        text += "\n\n" + std::string(_("On Ubuntu or Debian, install them with:")) + "\n" + command;
    else
        text += "\n\n" + std::string(_("Install them with your package manager, then try again."));
    return text;
}

}  // namespace HostLibraries
