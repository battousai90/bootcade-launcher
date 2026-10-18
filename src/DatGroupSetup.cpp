// src/DatGroupSetup.cpp
#include "DatGroupSetup.h"

#include "DatSource.h"
#include "EmulatorVariants.h"
#include "GenerateDAT.h"

#include <filesystem>
#include <iostream>

namespace DatGroupSetup {

bool install_and_generate(Gtk::Window& parent, const std::string& emulator, const std::string& variant_in) {
    const std::string variant = EmulatorVariants::resolve(emulator, variant_in);
    const std::string exe = EmulatorVariants::executable(emulator, variant);
    if (exe.empty()) return false;

    auto groups = DatSource::load_groups();
    // Le groupe de cette version : de preference un groupe qui genere ses DAT
    // depuis l'emulateur, sinon n'importe lequel de cette version.
    size_t at = DatSource::kNoGroup;
    for (int pass = 0; pass < 2 && at == DatSource::kNoGroup; ++pass)
        for (size_t i = 0; i < groups.size(); ++i) {
            const auto& g = groups[i];
            if (g.emulator != emulator || EmulatorVariants::resolve(emulator, g.variant) != variant) continue;
            if (pass == 0 && g.source != DatSource::Kind::Emulator) continue;
            at = i;
            break;
        }
    if (at == DatSource::kNoGroup) {
        DatSource::Group g;
        for (const auto& v : EmulatorVariants::all())
            if (v.emulator == emulator && v.variant == variant) g.name = v.group_name;
        g.id       = DatSource::make_id(g.name, groups);
        g.folder   = EmulatorVariants::default_folder(emulator, variant);
        g.emulator = emulator;
        g.variant  = variant;
        g.source   = DatSource::Kind::Emulator;
        groups.push_back(std::move(g));
        at = groups.size() - 1;
    }
    auto& g = groups[at];
    // Un groupe ancien sans version ecrite la recoit : c'est celle-ci.
    g.variant = variant;
    g.source  = DatSource::Kind::Emulator;
    std::error_code ec;
    std::filesystem::create_directories(g.folder, ec);
    const std::string folder = g.folder;

    bool ok = false;
    if (emulator == "fbneo") {
        ok = GenerateDAT::execute(parent, exe, folder, nullptr, /*quiet=*/true);
    } else {
        GenerateDAT::execute_mame(parent, exe, folder, nullptr);
        ok = !DatSource::list_folder(folder).empty();
    }
    // Relu : la generation a pu toucher config.json (dossier de DAT de FBNeo).
    auto now = DatSource::load_groups();
    size_t again = DatSource::kNoGroup;
    for (size_t i = 0; i < now.size(); ++i) if (now[i].id == g.id) again = i;
    if (again == DatSource::kNoGroup) { now.push_back(g); again = now.size() - 1; }
    now[again].variant = variant;
    now[again].source  = DatSource::Kind::Emulator;
    if (ok) {
        now[again].version     = EmulatorVariants::version(emulator, variant);
        now[again].last_update = DatSource::now_iso();
    }
    // La version qu'on vient d'installer est celle qu'on joue : son groupe
    // devient le groupe actif de cet emulateur.
    DatSource::one_active_per_emulator(now, again);
    DatSource::save_groups(now);
    std::cout << "[DAT] " << emulator << "/" << variant << " : group '" << now[again].name << "' in " << folder
              << (ok ? ", DAT generated" : ", DAT generation failed") << std::endl;
    return ok;
}

}
