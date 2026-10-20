// src/FbneoChannel.cpp
#include "FbneoChannel.h"

#include "AppContext.h"
#include "DatSource.h"
#include "DefaultFolders.h"
#include "EmulatorVariants.h"
#include "FbneoUpdateCheck.h"

#include <fstream>
#include <nlohmann/json.hpp>

namespace FbneoChannel {
namespace {

nlohmann::json read_config() {
    nlohmann::json j = nlohmann::json::object();
    std::ifstream fi(AppContext::get_config_path());
    if (fi) { try { fi >> j; } catch (...) { j = nlohmann::json::object(); } }
    if (!j.is_object()) j = nlohmann::json::object();
    return j;
}

void write_config(const nlohmann::json& j) {
    std::ofstream fo(AppContext::get_config_path());
    if (fo) fo << j.dump(4);
}

std::string exe_slot(const std::string& channel) {
    return channel == kOfficial ? "fbneo_exe_official" : "fbneo_exe_bootcade";
}

} // namespace

std::string current() {
    const auto j = read_config();
    return j.value("fbneo_channel", std::string()) == kOfficial ? kOfficial : kBootcade;
}

bool official() { return current() == kOfficial; }

const char* sha_key() {
    return official() ? "fbneo_release_sha_official" : "fbneo_release_sha";
}

const char* upstream_key() {
    return official() ? "fbneo_upstream_official" : "fbneo_upstream";
}

std::string download_url() {
    return official()
        ? "https://github.com/finalburnneo/FBNeo/releases/download/latest/" + FbneoUpdateCheck::asset_name()
        // finalburnneo/FBNeo stopped maintaining the SDL/Linux build (missing DAT
        // export calls for several systems, upstream declined the fix) : this fork
        // tracks their master daily and carries just that fix. See website FAQ.
        : FbneoUpdateCheck::download_url();          // le zip de la plateforme
}

std::string releases_url() {
    return official()
        ? "https://api.github.com/repos/finalburnneo/FBNeo/releases/tags/latest"
        : "https://api.github.com/repos/battousai90/FBNeo/releases/tags/latest";
}

std::string install_subfolder() {
    return official() ? "Emulators/FBNeo-Official" : "Emulators/FBNeo";
}

std::string official_group_id() { return "fbneo-official"; }

Switched switch_to(const std::string& channel, const std::string& current_executable)
{
    Switched out;
    auto j = read_config();
    const std::string from = j.value("fbneo_channel", std::string()) == kOfficial ? kOfficial : kBootcade;
    const std::string to   = channel == kOfficial ? kOfficial : kBootcade;

    // L'executable qu'on quitte garde sa place.
    if (from != to) j[exe_slot(from)] = current_executable;
    j["fbneo_channel"] = to;
    out.executable = j.value(exe_slot(to), std::string());
    // Premiere fois sur la version Bootcade (config d'avant ce choix) : son
    // executable est celui qui etait configure.
    if (to == kBootcade && out.executable.empty() && from == to)
        out.executable = current_executable;
    j["fbneo_executable"] = out.executable;
    write_config(j);

    // Les DAT : un groupe pour l'officiel, et un seul groupe FBNeo actif a la fois.
    auto groups = DatSource::load_groups();
    size_t official_at = DatSource::kNoGroup;
    for (size_t i = 0; i < groups.size(); ++i)
        if (groups[i].id == official_group_id()
            || (groups[i].emulator == "fbneo" && groups[i].variant == "official")) { official_at = i; break; }
    if (official_at == DatSource::kNoGroup) {
        DatSource::Group g;
        g.id        = official_group_id();
        g.name      = "FBNeo official";
        g.folder    = EmulatorVariants::default_folder("fbneo", "official");
        g.source    = DatSource::Kind::Emulator;
        g.emulator  = "fbneo";
        g.variant   = "official";
        g.active    = false;
        groups.push_back(std::move(g));
        official_at = groups.size() - 1;
    }
    // Les autres groupes FinalBurn Neo sans version ecrite decrivent le fork.
    for (size_t i = 0; i < groups.size(); ++i)
        if (i != official_at && groups[i].emulator == "fbneo" && groups[i].variant.empty())
            groups[i].variant = "bootcade";

    j = read_config();
    size_t keep = DatSource::kNoGroup;
    if (to == kOfficial) {
        // On retient le groupe Bootcade actif pour le retrouver au retour.
        for (size_t i = 0; i < groups.size(); ++i)
            if (groups[i].emulator == "fbneo" && groups[i].active && i != official_at)
                j["fbneo_bootcade_group"] = groups[i].id;
        write_config(j);
        keep = official_at;
    } else {
        const std::string remembered = j.value("fbneo_bootcade_group", std::string());
        for (size_t i = 0; i < groups.size(); ++i)
            if (i != official_at && groups[i].emulator == "fbneo" && groups[i].id == remembered) keep = i;
        if (keep == DatSource::kNoGroup)
            for (size_t i = 0; i < groups.size(); ++i)
                if (i != official_at && groups[i].emulator == "fbneo") { keep = i; break; }
        // Aucun groupe Bootcade : l'officiel reste le seul, il ne faut pas le couper.
        if (keep == DatSource::kNoGroup) keep = official_at;
    }
    DatSource::one_active_per_emulator(groups, keep);
    DatSource::save_groups(groups);
    out.dat_folder = groups[keep].folder;
    return out;
}

}
