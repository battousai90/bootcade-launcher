// src/EmulatorRegistry.cpp
#include "EmulatorRegistry.h"

#include "i18n.h"

namespace EmulatorRegistry {

const std::vector<EmulatorInfo>& all() {
    // Construit une fois, a la premiere demande : les libelles passent par la
    // traduction, qui n'est prete qu'apres l'initialisation de i18n.
    static const std::vector<EmulatorInfo> registry = {
        {"fbneo", "FinalBurn Neo", "emulators/fbneo.svg",
         _("Arcade boards and a few home systems")},
        {"mame",  "MAME",          "emulators/mame.svg",
         _("Arcade, and just about every machine ever built")},
    };
    return registry;
}

const EmulatorInfo* find(const std::string& id) {
    for (const auto& e : all())
        if (e.id == id) return &e;
    return nullptr;
}

std::string display_name(const std::string& id) {
    if (const EmulatorInfo* e = find(id)) return e->name;
    return id;
}

}  // namespace EmulatorRegistry
