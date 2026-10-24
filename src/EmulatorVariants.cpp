// src/EmulatorVariants.cpp
#include "EmulatorVariants.h"

#include "AppContext.h"
#include "DefaultFolders.h"
#include "FbneoChannel.h"
#include "MameCatalog.h"
#include "MameSooner.h"
#include "Platform.h"
#include "MameOfficial.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <unistd.h>

namespace fs = std::filesystem;

namespace EmulatorVariants {
namespace {

nlohmann::json read_config() {
    nlohmann::json j = nlohmann::json::object();
    std::ifstream fi(AppContext::get_config_path());
    if (fi) { try { fi >> j; } catch (...) { j = nlohmann::json::object(); } }
    return j.is_object() ? j : nlohmann::json::object();
}

bool runnable(const std::string& exe) {
    std::error_code ec;
    return !exe.empty() && fs::is_regular_file(exe, ec) && ::access(exe.c_str(), X_OK) == 0;
}

// « 0.289 » -> « 0.290 » : le Sooner prepare la version suivante.
std::string next_version(const std::string& v) {
    const size_t dot = v.find('.');
    if (dot == std::string::npos) return v;
    try {
        const std::string frac = v.substr(dot + 1);
        const int n = std::stoi(frac) + 1;
        std::string out = std::to_string(n);
        while (out.size() < frac.size()) out = "0" + out;
        return v.substr(0, dot + 1) + out;
    } catch (...) { return v; }
}

// Le numero du MAME officiel installe, « 0.289 ». Demander a MAME coute un
// processus : une fois par executable et par session.
std::string mame_release_number() {
    const std::string exe = executable("mame", "release");
    if (exe.empty()) return {};
    static std::mutex m;
    static std::map<std::string, std::string> cache;
    std::lock_guard<std::mutex> lock(m);
    auto it = cache.find(exe);
    if (it != cache.end()) return it->second;
    std::string v = MameCatalog::version_number(MameCatalog::installed_build(exe));
    cache[exe] = v;
    return v;
}

// « 0.289 (mame0289-1306-g8aeb28649e1) » -> numero « 0.289 » et commit « 8aeb286 ».
void sooner_build(std::string& number, std::string& commit) {
    const auto here = MameSooner::installed();
    number = MameCatalog::version_number(here.build);
    commit = here.commit.substr(0, 7);
    if (commit.empty()) {
        const size_t g = here.build.rfind("-g");
        if (g != std::string::npos) commit = here.build.substr(g + 2, 7);
    }
}

} // namespace

const std::vector<Variant>& all() {
    static const std::vector<Variant> list = {
        {"fbneo", "bootcade", "FinalBurn Neo — Nightly (Bootcade, rebuilt every day)", "FBNeo nightly",  "FinalBurn Neo Nightly"},
        {"fbneo", "official", "FinalBurn Neo — Official (finalburnneo)",               "FBNeo official", "FinalBurn Neo Official"},
        {"mame",  "release",  "MAME — Official release",                               "MAME official",  "MAME Official"},
        {"mame",  "sooner",   "MAME — Sooner (in development)",                        "MAME sooner",    "MAME Sooner"},
    };
    return list;
}

std::string short_name(const std::string& emulator, const std::string& variant_in) {
    const std::string variant = resolve(emulator, variant_in);
    for (const auto& v : all()) if (v.emulator == emulator && v.variant == variant) return v.short_name;
    return emulator;
}

std::string key(const std::string& emulator, const std::string& variant) {
    return emulator + ":" + resolve(emulator, variant);
}

bool split(const std::string& k, std::string& emulator, std::string& variant) {
    const size_t c = k.find(':');
    if (c == std::string::npos) return false;
    emulator = k.substr(0, c);
    variant  = k.substr(c + 1);
    return true;
}

std::string resolve(const std::string& emulator, const std::string& variant) {
    if (!variant.empty()) return variant;
    if (emulator == "mame") return MameSooner::channel() == MameSooner::kChannelSooner ? "sooner" : "release";
    return "bootcade";
}

std::string executable(const std::string& emulator, const std::string& variant_in) {
    const std::string variant = resolve(emulator, variant_in);
    const auto j = read_config();
    if (emulator == "fbneo") {
        // La version en service est dans fbneo_executable ; l'autre garde sa
        // place a part (FbneoChannel::switch_to).
        const bool current = FbneoChannel::current() == (variant == "official" ? FbneoChannel::kOfficial
                                                                              : FbneoChannel::kBootcade);
        const std::string exe = current
            ? j.value("fbneo_executable", std::string())
            : j.value(variant == "official" ? "fbneo_exe_official" : "fbneo_exe_bootcade", std::string());
        return runnable(exe) ? exe : std::string();
    }
    if (emulator == "mame") {
        if (variant == "sooner") return MameSooner::executable();
        std::string exe = MameSooner::resolve_executable(MameSooner::kChannelRelease,
                                                         j.value("mame_executable", std::string()));
        if (exe.empty()) exe = MameOfficial::executable();
        return MameCatalog::is_runnable(exe) ? exe : std::string();
    }
    return {};
}

std::string version(const std::string& emulator, const std::string& variant_in) {
    const std::string variant = resolve(emulator, variant_in);
    if (emulator == "fbneo") {
        const auto j = read_config();
        // Le commit de finalburnneo que ce build contient : celui du fork
        // Bootcade n'en dit rien au joueur.
        const std::string up = j.value(variant == "official" ? "fbneo_upstream_official" : "fbneo_upstream",
                                       std::string());
        if (!up.empty()) return up.substr(0, 7);
        // L'officiel est construit sur le commit upstream lui-meme.
        if (variant == "official") return j.value("fbneo_release_sha_official", std::string()).substr(0, 7);
        return {};
    }
    if (emulator == "mame") {
        if (variant == "sooner") {
            std::string number, commit;
            sooner_build(number, commit);
            if (number.empty()) return {};
            return next_version(number) + (commit.empty() ? "" : "-" + commit);
        }
        return mame_release_number();
    }
    return {};
}

std::string default_folder(const std::string& emulator, const std::string& variant_in) {
    const std::string variant = resolve(emulator, variant_in);
    if (emulator == "fbneo")
        return DefaultFolders::sub(variant == "official" ? "Support/DAT/fbneo_official" : "Support/DAT/fbneo_nightly");
    if (variant == "sooner") {
        std::string number, commit;
        sooner_build(number, commit);
        return DefaultFolders::sub("Support/DAT/Mame_" + (number.empty() ? std::string("sooner")
                                                                         : next_version(number) + "_sooner"));
    }
    const std::string number = mame_release_number();
    return DefaultFolders::sub("Support/DAT/Mame_" + (number.empty() ? std::string("official")
                                                                     : number + "_official"));
}

}
