// src/FbneoPaths.cpp
#include "FbneoPaths.h"

#include "AppContext.h"
#include "DefaultFolders.h"
#include "Platform.h"

#include <filesystem>
#include <fstream>
#include <vector>
#include <cctype>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace FbneoPaths {

#ifdef _WIN32

namespace {

std::string configured_executable() {
    try {
        std::ifstream in(AppContext::get_config_path());
        if (!in) return {};
        nlohmann::json j;
        in >> j;
        return j.value("fbneo_executable", std::string());
    } catch (...) {
        return {};
    }
}

}  // namespace

std::string data_dir() {
    const std::string exe = configured_executable();
    if (!exe.empty()) return fs::path(exe).parent_path().string();
    // Pas encore installe : le dossier ou Bootcade l'installera.
    return DefaultFolders::sub("Emulators/FBNeo");
}

// Le port SDL2 sous Windows travaille dans le dossier de son exe : fbneo.ini a
// sa racine, config/, support/ et screenshots/ a cote. Le FBNeo officiel range
// le sien dans config/, sous le nom de son exe (config/fbneo64.ini).
std::string ini_path() {
    const std::string exe = configured_executable();
    if (is_win32_build(exe)) return config_dir() + "/" + fs::path(exe).stem().string() + ".ini";
    return data_dir() + "/fbneo.ini";
}

std::string dat_lists_dir() { return data_dir() + "/support/lists/dat"; }

#else

std::string data_dir()      { return Platform::home_dir() + "/.local/share/fbneo"; }
std::string ini_path()      { return config_dir() + "/fbneo.ini"; }
std::string dat_lists_dir() { return data_dir() + "/support/lists/dat"; }

#endif

std::string config_dir()      { return data_dir() + "/config"; }
std::string games_dir()       { return config_dir() + "/games"; }
std::string hiscores_dir()    { return data_dir() + "/support/hiscores"; }
std::string screenshots_dir() { return data_dir() + "/screenshots"; }

bool is_win32_build(const std::string& exe) {
#ifdef _WIN32
    std::string stem = fs::path(exe).stem().string();
    for (auto& c : stem) c = (char)std::tolower((unsigned char)c);
    return stem.rfind("fbneo64", 0) == 0 || stem.rfind("fbneo32", 0) == 0;
#else
    (void)exe;
    return false;
#endif
}

bool set_ini_value(const std::string& key, const std::string& value) {
    const std::string path = ini_path();
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);

    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        std::string line;
        while (in && std::getline(in, line)) lines.push_back(line);
    }
    const std::string wanted = key + " " + value;
    bool found = false;
    for (auto& line : lines) {
        if (line.compare(0, key.size() + 1, key + " ") == 0) {
            if (line == wanted) return true;   // deja a la bonne valeur : rien a ecrire
            line = wanted;
            found = true;
            break;
        }
    }
    if (!found) lines.push_back(wanted);
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    for (const auto& l : lines) out << l << "\n";
    return true;
}

std::string with_trailing_separator(std::string path) {
    if (!path.empty() && path.back() != '/' && path.back() != '\\') path += '/';
    return path;
}

}  // namespace FbneoPaths
