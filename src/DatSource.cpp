// src/DatSource.cpp
#include "DatSource.h"
#include "DatParser.h"

#include "AppContext.h"
#include "i18n.h"

#include <archive.h>
#include <archive_entry.h>
#include <curl/curl.h>
#include <glibmm/checksum.h>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace DatSource {
namespace {

size_t write_to_string(char* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t write_to_file(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::ofstream*>(userdata);
    out->write(ptr, (std::streamsize)(size * nmemb));
    return out->good() ? size * nmemb : 0;
}

struct DownloadCtx {
    const Progress* progress;
    const std::function<bool()>* cancelled;
    std::string label;
};

int progress_cb(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<DownloadCtx*>(clientp);
    if (ctx->cancelled && *ctx->cancelled && (*ctx->cancelled)()) return 1;   // aborts the transfer
    if (ctx->progress && *ctx->progress && dltotal > 0)
        (*ctx->progress)(100.0 * (double)dlnow / (double)dltotal, ctx->label);
    return 0;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// "…/dat/dat-manifest.json" + "FBNeo_-_Arcade.dat" → "…/dat/FBNeo_-_Arcade.dat".
// The files sit next to the manifest : that is the contract.
std::string sibling_url(const std::string& manifest_url, const std::string& name) {
    std::string base = manifest_url;
    size_t slash = base.find_last_of('/');
    if (slash != std::string::npos) base = base.substr(0, slash + 1);
    // Percent-encode what a DAT name can carry that a URL cannot.
    std::string enc;
    for (unsigned char c : name) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') enc += (char)c;
        else { char buf[4]; snprintf(buf, sizeof(buf), "%%%02X", c); enc += buf; }
    }
    return base + enc;
}

} // namespace

// ── Model ───────────────────────────────────────────────────────────────────

const char* kind_key(Kind k) {
    switch (k) {
        case Kind::Emulator: return "emulator";
        case Kind::Folder:   return "folder";
        default:             return "http";
    }
}

Kind kind_from_key(const std::string& s) {
    std::string v = lower(s);
    if (v == "emulator") return Kind::Emulator;
    if (v == "folder")   return Kind::Folder;
    return Kind::Http;
}

std::string effective_merge(const DatTraits& traits, const DatRule* rule) {
    if (!traits.linked) return "";
    auto known = [](const std::string& m) {
        return m == kMergeSplit || m == kMergeNonMerged || m == kMergeMerged;
    };
    if (known(traits.declared) && !(rule && rule->override_dat && known(rule->merge)))
        return traits.declared;
    if (rule && known(rule->merge)) return rule->merge;
    return kMergeSplit;
}

const DatRule* rule_of(const std::vector<Group>& groups, const std::string& file) {
    for (const auto& g : groups) {
        // A group taking every file of its folder selects any name : the
        // file must also be in that folder.
        std::error_code ec;
        if (!g.active || !g.selects(file) || !fs::is_regular_file(fs::path(g.folder) / file, ec)) continue;
        auto it = g.rules.find(file);
        return it == g.rules.end() ? nullptr : &it->second;
    }
    return nullptr;
}

bool Group::selects(const std::string& file) const {
    if (all_files) return true;
    return std::find(files.begin(), files.end(), file) != files.end();
}

void Group::set_selected(const std::string& file, bool on, const std::vector<std::string>& provided) {
    if (all_files) {
        if (on) return;
        // Leaving "everything" : the list starts as everything the source
        // provides right now, minus this one.
        all_files = false;
        files = provided;
    }
    files.erase(std::remove(files.begin(), files.end(), file), files.end());
    if (on) files.push_back(file);
    std::sort(files.begin(), files.end());
    // Back to "everything" when the list covers the whole source again : a
    // file appearing later is then taken in, as the user meant.
    if (!provided.empty()) {
        bool all = true;
        for (const auto& p : provided) if (std::find(files.begin(), files.end(), p) == files.end()) { all = false; break; }
        if (all) { all_files = true; files.clear(); }
    }
}

std::string now_iso() {
    std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::vector<std::string> list_folder(const std::string& folder) {
    std::vector<std::string> out;
    std::error_code ec;
    if (folder.empty() || !fs::is_directory(folder, ec)) return out;
    for (auto it = fs::directory_iterator(folder, ec); it != fs::directory_iterator(); ++it) {
        if (!it->is_regular_file(ec)) continue;
        // .dat, or a .xml whose root is a DAT (<datafile>) or a raw MAME
        // -listxml (<mame>) : what DatParser knows how to import.
        if (DatParser::datKind(it->path().string()) == DatParser::DatKind::None) continue;
        out.push_back(it->path().filename().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string make_id(const std::string& name, const std::vector<Group>& taken) {
    std::string base;
    for (unsigned char c : lower(name)) {
        if (isalnum(c)) base += (char)c;
        else if (!base.empty() && base.back() != '-') base += '-';
    }
    while (!base.empty() && base.back() == '-') base.pop_back();
    if (base.empty()) base = "group";
    auto used = [&](const std::string& id) { for (const auto& g : taken) if (g.id == id) return true; return false; };
    std::string id = base;
    for (int n = 2; used(id); ++n) id = base + "-" + std::to_string(n);
    return id;
}

std::vector<Group> load_groups() {
    nlohmann::json j;
    std::ifstream fi(AppContext::get_config_path());
    if (fi) { try { fi >> j; } catch (...) { j = nlohmann::json{}; } }

    std::vector<Group> groups;
    auto str = [](const nlohmann::json& o, const char* k, const std::string& d = "") {
        return (o.contains(k) && o[k].is_string()) ? o[k].get<std::string>() : d;
    };
    auto flag = [](const nlohmann::json& o, const char* k, bool d) {
        return (o.contains(k) && o[k].is_boolean()) ? o[k].get<bool>() : d;
    };
    auto list = [](const nlohmann::json& o, const char* k) {
        std::vector<std::string> v;
        if (o.contains(k) && o[k].is_array())
            for (const auto& f : o[k]) if (f.is_string()) v.push_back(f.get<std::string>());
        return v;
    };
    const std::string legacy_style = (j.contains("rom_manager") && j["rom_manager"].is_object())
                                   ? str(j["rom_manager"], "set_style", "non-merged") : "non-merged";
    if (j.contains("rom_manager") && j["rom_manager"].is_object()
        && j["rom_manager"].contains("dat_groups") && j["rom_manager"]["dat_groups"].is_array()) {
        for (const auto& g : j["rom_manager"]["dat_groups"]) {
            if (!g.is_object()) continue;
            Group grp;
            grp.id          = str(g, "id");
            grp.name        = str(g, "name");
            grp.folder      = str(g, "folder");
            grp.source      = kind_from_key(str(g, "source", "http"));
            // Absent des fichiers ecrits avant MAME : ils decrivent tous
            // FinalBurn Neo, et le defaut les laisse intacts.
            grp.emulator    = str(g, "emulator", "fbneo");
            grp.url         = str(g, "url", kDefaultManifestUrl);
            grp.set_style   = str(g, "set_style", legacy_style);
            grp.active      = flag(g, "active", true);
            grp.last_check  = str(g, "last_check");
            grp.last_update = str(g, "last_update");
            if (g.contains("all_files") || g.contains("files")) {
                grp.all_files = flag(g, "all_files", true);
                grp.files     = list(g, "files");
            } else {
                // Previous shape : every file of the folder, minus inactive
                // ones. Same meaning, expressed as a selection.
                std::vector<std::string> inactive = list(g, "inactive_files");
                if (inactive.empty()) {
                    grp.all_files = true;
                } else {
                    grp.all_files = false;
                    for (const auto& f : list_folder(grp.folder))
                        if (std::find(inactive.begin(), inactive.end(), f) == inactive.end()) grp.files.push_back(f);
                }
            }
            if (g.contains("dat_rules") && g["dat_rules"].is_object()) {
                for (auto it = g["dat_rules"].begin(); it != g["dat_rules"].end(); ++it) {
                    if (!it.value().is_object()) continue;
                    DatRule r;
                    r.merge        = str(it.value(), "merge");
                    r.override_dat = flag(it.value(), "override", false);
                    r.folder       = str(it.value(), "folder");
                    grp.rules[it.key()] = std::move(r);
                }
            } else {
                // Written before merge modes were per DAT : each DAT the group
                // selects takes the mode the group had, once. Saving writes
                // dat_rules, and this never runs again for that group.
                for (const auto& f : list_folder(grp.folder))
                    if (grp.selects(f)) grp.rules[f].merge = grp.set_style;
            }
            if (!grp.id.empty()) groups.push_back(std::move(grp));
        }
    }
    if (groups.empty()) {
        // First run of this model : the one group every install has, built
        // from the folder the launcher already used. Its source is the
        // Bootcade file server : the way DATs reach a user who never runs
        // fbneo -dat by hand. Not saved here : nothing changes on disk until
        // the user acts on the screen.
        Group fb;
        fb.id        = "fbneo";
        fb.name      = "FinalBurn Neo";
        fb.folder    = str(j, "dat_path");
        fb.source    = Kind::Http;
        fb.url       = kDefaultManifestUrl;
        fb.set_style = legacy_style;
        for (const auto& f : list_folder(fb.folder)) fb.rules[f].merge = legacy_style;
        groups.push_back(std::move(fb));
    }
    return groups;
}

bool save_groups(const std::vector<Group>& groups) {
    nlohmann::json j;
    const std::string path = AppContext::get_config_path();
    { std::ifstream fi(path); if (fi) { try { fi >> j; } catch (...) { j = nlohmann::json{}; } } }
    auto arr = nlohmann::json::array();
    for (const auto& g : groups) {
        nlohmann::json o;
        o["id"] = g.id;
        o["name"] = g.name;
        o["folder"] = g.folder;
        o["source"] = kind_key(g.source);
        o["emulator"] = g.emulator.empty() ? std::string("fbneo") : g.emulator;
        o["url"] = g.url;
        o["set_style"] = g.set_style;
        o["active"] = g.active;
        o["all_files"] = g.all_files;
        o["files"] = g.files;
        nlohmann::json rules = nlohmann::json::object();
        for (const auto& [file, r] : g.rules) {
            nlohmann::json one;
            if (!r.merge.empty())  one["merge"] = r.merge;
            if (r.override_dat)    one["override"] = true;
            if (!r.folder.empty()) one["folder"] = r.folder;
            if (!one.empty()) rules[file] = std::move(one);
        }
        o["dat_rules"] = std::move(rules);
        o["last_check"] = g.last_check;
        o["last_update"] = g.last_update;
        arr.push_back(std::move(o));
    }
    j["rom_manager"]["dat_groups"] = std::move(arr);
    // The first group's folder is also the launcher's dat_path : the two keys
    // name the same thing, and the rest of the application reads the old one.
    if (!groups.empty()) j["dat_path"] = groups.front().folder;
    std::ofstream fo(path);
    if (!fo) return false;
    fo << j.dump(4);
    return true;
}

std::set<std::string> selected_in_folder(const Group& g) {
    std::set<std::string> out;
    for (const auto& f : list_folder(g.folder))
        if (g.selects(f)) out.insert(f);
    return out;
}

Group library_group() {
    nlohmann::json j;
    std::ifstream fi(AppContext::get_config_path());
    if (fi) { try { fi >> j; } catch (...) { j = nlohmann::json{}; } }
    std::string wanted;
    if (j.contains("rom_manager") && j["rom_manager"].is_object()
        && j["rom_manager"].contains("library_group") && j["rom_manager"]["library_group"].is_string())
        wanted = j["rom_manager"]["library_group"].get<std::string>();
    // Same choice as the Library tab's combo : the remembered group if it is
    // still active, else the first active one.
    const std::vector<Group> groups = load_groups();
    const Group* first = nullptr;
    for (const auto& g : groups) {
        if (!g.active) continue;
        if (!first) first = &g;
        if (!wanted.empty() && g.id == wanted) return g;
    }
    return first ? *first : Group{};
}

const Group* group_for(const std::vector<Group>& groups, const std::string& emulator) {
    const Group lib = library_group();
    const Group* first_active = nullptr;
    const Group* first_any = nullptr;
    for (const auto& g : groups) {
        if (g.emulator != emulator) continue;
        if (g.id == lib.id && g.active) return &g;
        if (!first_any) first_any = &g;
        if (!first_active && g.active) first_active = &g;
    }
    return first_active ? first_active : first_any;
}

std::vector<std::string> roms_paths_for(const std::string& emulator) {
    nlohmann::json j;
    std::ifstream fi(AppContext::get_config_path());
    if (fi) { try { fi >> j; } catch (...) { j = nlohmann::json{}; } }
    std::vector<std::string> out;
    auto take = [&out](const nlohmann::json& arr) {
        for (const auto& p : arr) if (p.is_string() && !p.get<std::string>().empty()) out.push_back(p.get<std::string>());
    };
    if (j.contains("emulators") && j["emulators"].is_object() && j["emulators"].contains(emulator)
        && j["emulators"][emulator].is_object()) {
        const auto& e = j["emulators"][emulator];
        if (e.contains("roms_paths") && e["roms_paths"].is_array()) take(e["roms_paths"]);
        return out;
    }
    if (emulator == "mame") {
        // "a;b;c", the form MAME's own -rompath takes.
        const std::string joined = (j.contains("mame_rompaths") && j["mame_rompaths"].is_string())
                                 ? j["mame_rompaths"].get<std::string>() : std::string();
        std::string one;
        for (char c : joined) {
            if (c == ';') { if (!one.empty()) out.push_back(one); one.clear(); }
            else one += c;
        }
        if (!one.empty()) out.push_back(one);
        return out;
    }
    if (emulator == "fbneo") {
        if (j.contains("roms_paths") && j["roms_paths"].is_array()) take(j["roms_paths"]);
        else if (j.contains("roms_path") && j["roms_path"].is_string() && !j["roms_path"].get<std::string>().empty())
            out.push_back(j["roms_path"].get<std::string>());
    }
    return out;
}

std::vector<std::string> files_to_load(const std::vector<Group>& groups, std::vector<std::string>* conflicts) {
    std::vector<std::string> out;
    std::map<std::string, std::string> seen;   // file name → folder it was taken from
    for (const auto& g : groups) {
        if (!g.active) continue;
        for (const auto& f : selected_in_folder(g)) {
            auto it = seen.find(f);
            if (it == seen.end()) {
                seen[f] = g.folder;
                out.push_back((fs::path(g.folder) / f).string());
            } else if (it->second != g.folder && conflicts) {
                conflicts->push_back(f + " (" + g.name + " : " + g.folder + " ignored, already loaded from " + it->second + ")");
            }
        }
    }
    return out;
}

// ── HTTP ────────────────────────────────────────────────────────────────────

// ── Download from a site ────────────────────────────────────────────────────

const std::vector<Site>& sites() {
    static const std::vector<Site> list = {
        // The emulators' own teams first : the reference, straight from them.
        {"fbneo", N_("FinalBurn Neo team — every system (official DATs)"), "FinalBurn Neo team",
         "https://github.com/libretro/FBNeo/tree/master/dats",
         "https://api.github.com/repos/libretro/FBNeo/contents/dats"},
        {"fbneo", N_("FinalBurn Neo team — Arcade only"), "FinalBurn Neo team",
         "https://github.com/libretro/FBNeo/tree/master/dats",
         "https://raw.githubusercontent.com/libretro/FBNeo/master/dats/FinalBurn%20Neo%20(ClrMame%20Pro%20XML%2C%20Arcade%20only).dat"},
        {"fbneo", N_("FinalBurn Neo team, libretro GitLab — every system (official DATs)"), "FinalBurn Neo team",
         "https://git.libretro.com/libretro/FBNeo/-/tree/master/dats",
         "https://git.libretro.com/api/v4/projects/libretro%2FFBNeo/repository/tree?path=dats&per_page=100"},
        {"fbneo", N_("Pleasuredome — FBNeo ROMs (split), every system"), "Pleasuredome",
         "https://pleasuredome.github.io/pleasuredome/nonmame/fbneo/",
         "https://github.com/pleasuredome/pleasuredome/raw/gh-pages/nonmame/fbneo/FBNeo%201.0.0.2%20ROMs%20(split).zip"},
        {"mame", N_("MAME team — the official MAME XML of the latest release"), "MAME team",
         "https://www.mamedev.org/release.html",
         "https://github.com/mamedev/mame/releases/download/mame0289/mame0289lx.zip"},
        {"mame", N_("progettosnaps — MAME DAT pack (every DAT + the MAME XML)"), "progettosnaps",
         "https://www.progettosnaps.net/dats/MAME/",
         "https://www.progettosnaps.net/download/?tipo=dat_mame&file=/dats/MAME/packs/MAME_Dats_289.7z"},
        {"mame", N_("Pleasuredome — MAME ROMs (split)"), "Pleasuredome", "https://pleasuredome.github.io/pleasuredome/mame/",
         "https://github.com/pleasuredome/pleasuredome/raw/gh-pages/mame/MAME%200.289%20ROMs%20(split).zip"},
        {"mame", N_("Pleasuredome — MAME ROMs (merged)"), "Pleasuredome", "https://pleasuredome.github.io/pleasuredome/mame/",
         "https://github.com/pleasuredome/pleasuredome/raw/gh-pages/mame/MAME%200.289%20ROMs%20(merged).zip"},
        {"mame", N_("Pleasuredome — MAME ROMs (non-merged)"), "Pleasuredome", "https://pleasuredome.github.io/pleasuredome/mame/",
         "https://github.com/pleasuredome/pleasuredome/raw/gh-pages/mame/MAME%200.289%20ROMs%20(non-merged).zip"},
        {"mame", N_("Pleasuredome — MAME ROMs (bios-devices)"), "Pleasuredome", "https://pleasuredome.github.io/pleasuredome/mame/",
         "https://github.com/pleasuredome/pleasuredome/raw/gh-pages/mame/MAME%200.289%20ROMs%20(bios-devices).zip"},
        {"mame", N_("Pleasuredome — MAME CHDs (merged)"), "Pleasuredome", "https://pleasuredome.github.io/pleasuredome/mame/",
         "https://github.com/pleasuredome/pleasuredome/raw/gh-pages/mame/MAME%200.289%20CHDs%20(merged).zip"},
        {"mame", N_("AntoPISA — MAME ROMs (machines, BIOS, devices)"), "AntoPISA", "https://github.com/AntoPISA/MAME_Dats",
         "https://raw.githubusercontent.com/AntoPISA/MAME_Dats/main/MAME_dat/MAME_ROMs.dat"},
        {"mame", N_("AntoPISA — MAME CHDs"), "AntoPISA", "https://github.com/AntoPISA/MAME_Dats",
         "https://raw.githubusercontent.com/AntoPISA/MAME_Dats/main/MAME_dat/MAME_CHD.dat"},
        {"mame", N_("AntoPISA — MAME arcade XML"), "AntoPISA", "https://github.com/AntoPISA/MAME_Dats",
         "https://raw.githubusercontent.com/AntoPISA/MAME_Dats/main/ARCADE_xml/arcade.xml"},
    };
    return list;
}

namespace {

std::string url_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
            out.push_back((char)std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else out.push_back(s[i]);
    }
    return out;
}

// Links copied from the sites' pages carry raw spaces ; curl wants them encoded.
std::string url_encode_spaces(const std::string& s) {
    std::string out;
    for (char c : s) { if (c == ' ') out += "%20"; else out.push_back(c); }
    return out;
}

// The file name an address carries : the file= parameter of a download
// script (progettosnaps), else the last path segment.
std::string file_name_of(const std::string& url) {
    const auto q = url.find('?');
    if (q != std::string::npos) {
        std::istringstream in(url.substr(q + 1));
        std::string kv;
        while (std::getline(in, kv, '&'))
            if (kv.rfind("file=", 0) == 0) return fs::path(url_decode(kv.substr(5))).filename().string();
    }
    return fs::path(url_decode(url.substr(0, q))).filename().string();
}

bool http_get(const std::string& url, std::string& body, std::string& error) {
    CURL* curl = curl_easy_init();
    if (!curl) { error = "curl_easy_init failed"; return false; }
    const std::string u = url_encode_spaces(url);
    curl_easy_setopt(curl, CURLOPT_URL, u.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Bootcade");
    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) { error = curl_easy_strerror(res); return false; }
    if (status != 200) { error = "HTTP " + std::to_string(status); return false; }
    return true;
}

}  // namespace

std::string latest_url(const std::string& url, std::string& error) {
    // The MAME team : its latest GitHub release carries the XML as
    // mame0XXXlx.zip.
    if (url.find("github.com/mamedev/mame/releases/download/") != std::string::npos) {
        std::string body;
        if (!http_get("https://api.github.com/repos/mamedev/mame/releases/latest", body, error)) return url;
        try {
            const auto j = nlohmann::json::parse(body);
            for (const auto& a : j.at("assets")) {
                const std::string n = a.value("name", "");
                if (n.size() > 6 && n.compare(n.size() - 6, 6, "lx.zip") == 0)
                    return a.value("browser_download_url", url);
            }
        } catch (...) { error = "unexpected answer from GitHub"; }
        return url;
    }
    // The page the site lists its files on : that of the menu entry whose
    // address sits in the same directory.
    auto dir_of = [](const std::string& u) { return url_decode(u.substr(0, u.rfind('/') + 1)); };
    std::string index;
    for (const auto& s : sites())
        if (dir_of(s.url) == dir_of(url)) { index = s.homepage; break; }
    if (index.empty() || index.find("github.com/") != std::string::npos
        || index.find("git.libretro.com/") != std::string::npos) return url;   // fixed addresses
    const std::string name = file_name_of(url);
    // "MAME_Dats_289.7z", "MAME 0.289 ROMs (split).zip",
    // "FBNeo 1.0.0.3 260723 GIT7a28a7d debug ROMs (split).zip" : a prefix,
    // a version, maybe a build tag, then the kind of DAT. The newest file of
    // the same prefix and kind wins ; update, rollback and software-list
    // files share a kind but are other DATs.
    static const std::regex parts(R"(^(.*?)(\d+(?:\.\d+)*)(.*?)((?:ROMs|CHDs) \([^)]*\)\.zip|\.7z|\.zip)$)");
    std::smatch m;
    if (!std::regex_match(name, m, parts)) return url;
    const std::string prefix = m[1].str(), kind = m[4].str();
    auto version_of = [](const std::string& v) {
        std::vector<long> out;
        std::istringstream in(v);
        std::string part;
        while (std::getline(in, part, '.')) { try { out.push_back(std::stol(part)); } catch (...) { out.push_back(0); } }
        return out;
    };
    std::string page;
    if (!http_get(index, page, error)) return url;
    // Links as HTML writes them : "&amp;" between the parameters.
    for (size_t at = page.find("&amp;"); at != std::string::npos; at = page.find("&amp;", at + 1))
        page.replace(at, 5, "&");
    page = url_decode(page);
    std::vector<long> best = version_of(m[2].str());
    std::string best_name;
    static const std::regex link(R"re(href="([^"]+)")re");
    for (auto it = std::sregex_iterator(page.begin(), page.end(), link); it != std::sregex_iterator(); ++it) {
        const std::string candidate = file_name_of((*it)[1].str());
        std::smatch c;
        if (!std::regex_match(candidate, c, parts)) continue;
        if (c[1].str() != prefix || c[4].str() != kind) continue;
        const std::string middle = c[3].str();
        if (middle.find("Software") != std::string::npos || middle.find("Update") != std::string::npos ||
            middle.find("Rollback") != std::string::npos) continue;
        const auto v = version_of(c[2].str());
        if (v > best) { best = v; best_name = candidate; }
    }
    if (best_name.empty()) return url;
    std::string out = url_decode(url);
    const auto pos = out.rfind(name);
    if (pos == std::string::npos) return url;
    out.replace(pos, name.size(), best_name);
    return url_encode_spaces(out);
}

bool fetch_direct(const std::string& url, const std::string& folder,
                  std::vector<std::string>& written, std::string& error,
                  const std::function<void(double, const std::string&)>& progress,
                  const std::function<bool()>& cancelled) {
    // A folder of a GitHub repository (the FinalBurn Neo team publishes one
    // DAT per system) : every .dat / .xml it lists, each fetched on its own.
    if (url.rfind("https://api.github.com/repos/", 0) == 0 && url.find("/contents/") != std::string::npos) {
        std::string body;
        if (!http_get(url, body, error)) return false;
        std::vector<std::string> files;
        try {
            for (const auto& e : nlohmann::json::parse(body)) {
                const std::string n = lower(e.value("name", ""));
                const std::string ext = lower(fs::path(n).extension().string());
                if (e.value("type", "") == "file" && (ext == ".dat" || ext == ".xml") && e.contains("download_url"))
                    files.push_back(e.value("download_url", ""));
            }
        } catch (...) { error = "unexpected answer from GitHub"; return false; }
        if (files.empty()) { error = "no DAT file in " + url; return false; }
        for (size_t i = 0; i < files.size(); ++i) {
            if (cancelled && cancelled()) { error = "cancelled"; return false; }
            auto sub = [&](double p, const std::string& m) {
                if (progress) progress((100.0 * (double)i + p) / (double)files.size(), m);
            };
            if (!fetch_direct(files[i], folder, written, error, sub, cancelled)) return false;
        }
        return true;
    }
    // A folder of a GitLab repository (libretro's own server) : its tree,
    // then each .dat / .xml through the raw address.
    if (url.find("/api/v4/projects/") != std::string::npos && url.find("/repository/tree") != std::string::npos) {
        std::string body;
        if (!http_get(url, body, error)) return false;
        const auto p0 = url.find("/api/v4/projects/") + 17;
        const std::string project = url_decode(url.substr(p0, url.find('/', p0) - p0));   // "libretro/FBNeo"
        const std::string host = url.substr(0, url.find("/api/v4/"));
        std::vector<std::string> files;
        try {
            for (const auto& e : nlohmann::json::parse(body)) {
                const std::string ext = lower(fs::path(e.value("name", "")).extension().string());
                if (e.value("type", "") == "blob" && (ext == ".dat" || ext == ".xml"))
                    files.push_back(host + "/" + project + "/-/raw/master/" + url_encode_spaces(e.value("path", "")));
            }
        } catch (...) { error = "unexpected answer from " + host; return false; }
        if (files.empty()) { error = "no DAT file in " + url; return false; }
        for (size_t i = 0; i < files.size(); ++i) {
            if (cancelled && cancelled()) { error = "cancelled"; return false; }
            auto sub = [&](double p, const std::string& m) {
                if (progress) progress((100.0 * (double)i + p) / (double)files.size(), m);
            };
            if (!fetch_direct(files[i], folder, written, error, sub, cancelled)) return false;
        }
        return true;
    }
    std::error_code ec;
    fs::create_directories(folder, ec);
    const std::string name = file_name_of(url);
    if (name.empty()) { error = "no file name in " + url; return false; }
    const fs::path tmp = fs::path(folder) / ("." + name + ".download");
    fs::remove(tmp, ec);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { error = "cannot write in " + folder; return false; }
        CURL* curl = curl_easy_init();
        if (!curl) { error = "curl_easy_init failed"; return false; }
        Progress p = progress;
        std::function<bool()> c = cancelled;
        DownloadCtx ctx{&p, &c, name};
        const std::string u = url_encode_spaces(url);
        curl_easy_setopt(curl, CURLOPT_URL, u.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_file);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 1800L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Bootcade");
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
        CURLcode res = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(curl);
        out.close();
        if (res == CURLE_ABORTED_BY_CALLBACK) { error = "cancelled"; fs::remove(tmp, ec); return false; }
        if (res != CURLE_OK) { error = curl_easy_strerror(res); fs::remove(tmp, ec); return false; }
        if (status != 200) { error = "HTTP " + std::to_string(status); fs::remove(tmp, ec); return false; }
    }

    // An archive whatever its name says : the first bytes decide.
    unsigned char magic[4] = {0};
    { std::ifstream in(tmp, std::ios::binary); in.read(reinterpret_cast<char*>(magic), sizeof(magic)); }
    const bool is_zip = magic[0] == 'P' && magic[1] == 'K';
    const bool is_7z  = magic[0] == '7' && magic[1] == 'z' && magic[2] == 0xBC && magic[3] == 0xAF;
    if (!is_zip && !is_7z) {
        fs::rename(tmp, fs::path(folder) / name, ec);
        if (ec) { error = "cannot replace " + name + ": " + ec.message(); fs::remove(tmp, ec); return false; }
        written.push_back(name);
        return true;
    }

    // Every .dat / .xml of the archive, under its own name : the archive's
    // sub-folders mean nothing to a DAT folder.
    if (progress) progress(100.0, "unpacking " + name);
    struct archive* a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    bool ok = archive_read_open_filename(a, tmp.c_str(), 1 << 16) == ARCHIVE_OK;
    if (!ok) error = archive_error_string(a) ? archive_error_string(a) : "cannot open the archive";
    struct archive_entry* e = nullptr;
    while (ok && archive_read_next_header(a, &e) == ARCHIVE_OK) {
        if (cancelled && cancelled()) { error = "cancelled"; ok = false; break; }
        if (archive_entry_filetype(e) != AE_IFREG) continue;
        const std::string entry = fs::path(archive_entry_pathname(e)).filename().string();
        const std::string ext = lower(fs::path(entry).extension().string());
        if (ext != ".dat" && ext != ".xml") continue;
        const fs::path part = fs::path(folder) / ("." + entry + ".download");
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out) { error = "cannot write " + part.string(); ok = false; break; }
        const void* buf; size_t size; la_int64_t offset;
        int r;
        while ((r = archive_read_data_block(a, &buf, &size, &offset)) == ARCHIVE_OK)
            out.write(static_cast<const char*>(buf), (std::streamsize)size);
        out.close();
        if (r != ARCHIVE_EOF || !out) { error = "cannot unpack " + entry; fs::remove(part, ec); ok = false; break; }
        fs::rename(part, fs::path(folder) / entry, ec);
        if (ec) { error = "cannot replace " + entry + ": " + ec.message(); fs::remove(part, ec); ok = false; break; }
        written.push_back(entry);
    }
    archive_read_free(a);
    fs::remove(tmp, ec);
    if (ok && written.empty()) { error = name + " holds no .dat or .xml file"; ok = false; }
    return ok;
}

static const char* kSourcesFile = ".bootcade-sources.json";

void record_source(const std::string& folder, const std::vector<std::string>& files,
                   const Site& site, const std::string& url) {
    const fs::path path = fs::path(folder) / kSourcesFile;
    nlohmann::json j = nlohmann::json::object();
    { std::ifstream in(path); if (in) { try { in >> j; } catch (...) { j = nlohmann::json::object(); } } }
    if (!j.is_object()) j = nlohmann::json::object();
    for (const auto& f : files)
        j[f] = {{"source", site.source}, {"homepage", site.homepage}, {"url", url_decode(url)}, {"date", now_iso()}};
    std::ofstream out(path, std::ios::trunc);
    out << j.dump(2) << "\n";
}

std::string source_of(const std::string& folder, const std::string& file) {
    std::ifstream in(fs::path(folder) / kSourcesFile);
    if (!in) return {};
    nlohmann::json j;
    try { in >> j; } catch (...) { return {}; }
    if (!j.is_object() || !j.contains(file) || !j[file].is_object()) return {};
    const auto& s = j[file];
    return s.value("source", "") + " — " + s.value("homepage", "");
}

bool fetch_manifest(const std::string& url, Manifest& out, std::string& error) {
    CURL* curl = curl_easy_init();
    if (!curl) { error = "curl_easy_init failed"; return false; }
    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Bootcade");
    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) { error = curl_easy_strerror(res); return false; }
    if (status != 200) { error = "HTTP " + std::to_string(status); return false; }

    nlohmann::json j;
    try { j = nlohmann::json::parse(body); }
    catch (const std::exception& e) { error = std::string("not a JSON manifest: ") + e.what(); return false; }
    if (!j.is_object() || !j.contains("schema_version") || !j["schema_version"].is_number_integer()) {
        error = "not a dat-manifest.json (no schema_version)";
        return false;
    }
    out = Manifest{};
    out.schema_version = j["schema_version"].get<int>();
    if (out.schema_version != kManifestSchema) {
        error = "manifest schema " + std::to_string(out.schema_version) + " is not supported by this version of Bootcade (expected "
              + std::to_string(kManifestSchema) + ")";
        return false;
    }
    out.generated = j.value("generated", "");
    out.group     = j.value("group", "");
    if (j.contains("files") && j["files"].is_array()) {
        for (const auto& f : j["files"]) {
            if (!f.is_object()) continue;
            RemoteFile rf;
            rf.name    = f.value("name", "");
            rf.size    = f.contains("size") && f["size"].is_number() ? f["size"].get<uint64_t>() : 0;
            rf.sha256  = lower(f.value("sha256", ""));
            rf.version = f.value("version", "");
            rf.date    = f.value("date", "");
            // A name that could leave the folder is not a file we want.
            if (rf.name.empty() || rf.name.find('/') != std::string::npos || rf.name.find("..") != std::string::npos) continue;
            out.files.push_back(std::move(rf));
        }
    }
    return true;
}

std::string sha256_of(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    Glib::Checksum sum(Glib::Checksum::CHECKSUM_SHA256);
    std::vector<char> buf(1 << 16);
    while (in) {
        in.read(buf.data(), (std::streamsize)buf.size());
        std::streamsize n = in.gcount();
        if (n > 0) sum.update(reinterpret_cast<const guchar*>(buf.data()), (gsize)n);
    }
    return lower(sum.get_string());
}

std::vector<FileStatus> compare(const std::string& folder, const Manifest& manifest) {
    std::vector<FileStatus> out;
    std::error_code ec;
    std::map<std::string, const RemoteFile*> remote;
    for (const auto& f : manifest.files) remote[f.name] = &f;

    for (const auto& [name, rf] : remote) {
        FileStatus st;
        st.name = name;
        st.remote_size = rf->size;
        st.remote_sha256 = rf->sha256;
        st.remote_version = rf->version;
        st.remote_date = rf->date;
        fs::path local = fs::path(folder) / name;
        if (!fs::exists(local, ec)) {
            st.state = State::Missing;
        } else {
            st.local_size = fs::file_size(local, ec);
            st.local_sha256 = sha256_of(local.string());
            st.state = (st.local_sha256 == rf->sha256) ? State::UpToDate : State::Outdated;
        }
        out.push_back(std::move(st));
    }
    // Local files the manifest does not know : shown, never touched.
    for (auto it = fs::directory_iterator(folder, ec); it != fs::directory_iterator(); ++it) {
        if (!it->is_regular_file(ec) || DatParser::datKind(it->path().string()) == DatParser::DatKind::None) continue;
        std::string name = it->path().filename().string();
        if (remote.count(name)) continue;
        FileStatus st;
        st.name = name;
        st.state = State::LocalOnly;
        st.local_size = fs::file_size(it->path(), ec);
        out.push_back(std::move(st));
    }
    return out;
}

bool download(const std::string& manifest_url, const RemoteFile& file,
              const std::string& folder, std::string& error,
              const Progress& progress, const std::function<bool()>& cancelled) {
    std::error_code ec;
    fs::create_directories(folder, ec);
    const fs::path final_path = fs::path(folder) / file.name;
    // Hidden and suffixed : nothing that reads *.dat can mistake it for a
    // DAT, whatever happens to this process mid-transfer.
    const fs::path tmp_path = fs::path(folder) / ("." + file.name + ".download");
    fs::remove(tmp_path, ec);

    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) { error = "cannot write in " + folder; return false; }
        CURL* curl = curl_easy_init();
        if (!curl) { error = "curl_easy_init failed"; return false; }
        DownloadCtx ctx{&progress, &cancelled, file.name};
        std::string url = sibling_url(manifest_url, file.name);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_file);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Bootcade");
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
        CURLcode res = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(curl);
        out.close();
        if (res == CURLE_ABORTED_BY_CALLBACK) { error = "cancelled"; fs::remove(tmp_path, ec); return false; }
        if (res != CURLE_OK) { error = curl_easy_strerror(res); fs::remove(tmp_path, ec); return false; }
        if (status != 200) { error = "HTTP " + std::to_string(status); fs::remove(tmp_path, ec); return false; }
    }

    // What the manifest promised, or nothing : the local DAT is not touched
    // by a download that does not match.
    uint64_t got = fs::file_size(tmp_path, ec);
    if (ec || got != file.size) {
        error = "size mismatch: got " + std::to_string(got) + " bytes, manifest says " + std::to_string(file.size);
        fs::remove(tmp_path, ec);
        return false;
    }
    std::string sum = sha256_of(tmp_path.string());
    if (sum != file.sha256) {
        error = "SHA256 mismatch (the file changed on the server, try again)";
        fs::remove(tmp_path, ec);
        return false;
    }
    fs::rename(tmp_path, final_path, ec);
    if (ec) {
        error = "cannot replace " + final_path.string() + ": " + ec.message();
        fs::remove(tmp_path, ec);
        return false;
    }
    return true;
}

// ── Header ──────────────────────────────────────────────────────────────────

Header read_header(const std::string& path, int preview_lines) {
    Header h;
    std::ifstream in(path);
    std::string line;
    std::string head_text;
    int n = 0;
    // The <header> of a logiqx DAT sits in the first lines; reading them is
    // enough for both the preview and the fields, without loading megabytes
    // of <game> entries.
    while (std::getline(in, line) && n < 400) {
        if (n < preview_lines) h.preview.push_back(line);
        head_text += line + "\n";
        ++n;
        if (line.find("</header>") != std::string::npos) break;
    }
    size_t a = head_text.find("<header>"), b = head_text.find("</header>");
    if (a == std::string::npos || b == std::string::npos) {
        // A raw MAME -listxml has no Logiqx header : its root carries the
        // build. Named so the DAT list says what it is.
        const size_t root = head_text.find("<mame build=\"");
        if (root != std::string::npos) {
            const size_t e = head_text.find('"', root + 13);
            const std::string build = head_text.substr(root + 13, e == std::string::npos ? 0 : e - root - 13);
            h.name        = "MAME listxml";
            h.description = "MAME -listxml " + build;
            h.version     = build.substr(0, build.find(' '));
        }
        return h;
    }
    std::string xml = head_text.substr(a, b - a + 9);
    pugi::xml_document doc;
    if (!doc.load_string(xml.c_str())) return h;
    auto hd = doc.child("header");
    h.name        = hd.child("name").text().get();
    h.description = hd.child("description").text().get();
    h.version     = hd.child("version").text().get();
    h.date        = hd.child("date").text().get();
    h.author      = hd.child("author").text().get();
    return h;
}

} // namespace DatSource
