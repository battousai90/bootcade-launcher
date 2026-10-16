// src/FbneoPaths.cpp
#include "FbneoPaths.h"

#include "AppContext.h"
#include "DefaultFolders.h"
#include "Platform.h"

#include <filesystem>
#include <fstream>
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
// sa racine, config/, support/ et screenshots/ a cote.
std::string ini_path() { return data_dir() + "/fbneo.ini"; }

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

std::string with_trailing_separator(std::string path) {
    if (!path.empty() && path.back() != '/' && path.back() != '\\') path += '/';
    return path;
}

}  // namespace FbneoPaths
