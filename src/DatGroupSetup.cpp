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
    // Le groupe de cette version : un groupe qui la porte en toutes lettres,
    // l'actif d'abord, puis un qui genere ses DAT depuis l'emulateur, puis
    // n'importe lequel. Un groupe MAME ancien n'est jamais pris : sa version
    // se lisait dans le canal de Settings, et une mise a jour de Sooner
    // reprenait ainsi le groupe et le dossier des DAT de la version officielle.
    size_t at = DatSource::kNoGroup;
    for (int pass = 0; pass < 3 && at == DatSource::kNoGroup; ++pass)
        for (size_t i = 0; i < groups.size(); ++i) {
            const auto& g = groups[i];
            if (g.emulator != emulator || DatSource::stable_variant(g) != variant) continue;
            if (pass == 0 && !g.active) continue;
            if (pass == 1 && g.source != DatSource::Kind::Emulator) continue;
            at = i;
            break;
        }
    if (at == DatSource::kNoGroup) {
        DatSource::Group g;
        for (const auto& v : EmulatorVariants::all())
            if (v.emulator == emulator && v.variant == variant) g.name = v.group_name;
        g.id       = DatSource::make_id(g.name, groups);
        g.folder   = DatSource::free_folder(groups, EmulatorVariants::default_folder(emulator, variant));
        g.emulator = emulator;
        g.variant  = variant;
        g.source   = DatSource::Kind::Emulator;
        groups.push_back(std::move(g));
        at = groups.size() - 1;
    }
    auto& g = groups[at];
    // Un groupe FinalBurn Neo ancien recoit la version qu'il decrivait deja.
    g.variant = variant;
    g.source  = DatSource::Kind::Emulator;
    // Chaque version a ses DAT dans son dossier : un dossier partage avec un
    // groupe d'une autre version verrait ses DAT ecrases par cette generation.
    if (g.folder.empty() || !DatSource::folder_shared_with(groups, at).empty()) {
        std::cout << "[DAT] " << emulator << "/" << variant << " : group '" << g.name << "' shared '"
                  << g.folder << "' with another version, moved to its own folder" << std::endl;
        g.folder    = DatSource::free_folder(groups, EmulatorVariants::default_folder(emulator, variant), at);
        g.all_files = true;
        g.files.clear();
        g.rules.clear();
    }
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
    now[again].variant   = variant;
    now[again].source    = DatSource::Kind::Emulator;
    now[again].folder    = g.folder;
    now[again].all_files = g.all_files;
    now[again].files     = g.files;
    now[again].rules     = g.rules;
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
