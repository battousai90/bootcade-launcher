// src/RetroAchievements.cpp
#include "RetroAchievements.h"

#include "AppContext.h"
#include "i18n.h"

#include <curl/curl.h>
#include <glibmm.h>
#include <nlohmann/json.hpp>
#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <chrono>
#include <iterator>
#include <set>
#include <unordered_map>

namespace RetroAchievements {
namespace {

const char* const kServer = "https://retroachievements.org/dorequest.php";

std::mutex g_mutex;
bool g_loaded = false;
std::string g_user, g_token;
bool g_hardcore = true;

std::string path() {
    return AppContext::get_user_config_dir() + "/retroachievements.json";
}

void load_locked() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream f(path());
    if (!f) return;
    try {
        nlohmann::json j;
        f >> j;
        g_user     = j.value("user", std::string());
        g_token    = j.value("token", std::string());
        g_hardcore = j.value("hardcore", true);
    } catch (...) {}
}

// Le jeton vaut le compte : son propre fichier, lisible du seul joueur.
void save_locked() {
    nlohmann::json j;
    j["user"] = g_user;
    j["token"] = g_token;
    j["hardcore"] = g_hardcore;
    std::ofstream f(path(), std::ios::trunc);
    if (!f) return;
    f << j.dump(2);
    f.close();
    ::chmod(path().c_str(), S_IRUSR | S_IWUSR);
}

size_t to_string(char* p, size_t size, size_t n, void* out) {
    static_cast<std::string*>(out)->append(p, size * n);
    return size * n;
}

std::string escape(const std::string& s) {
    char* e = curl_easy_escape(nullptr, s.c_str(), static_cast<int>(s.size()));
    if (!e) return s;
    std::string out(e);
    curl_free(e);
    return out;
}

// Un appel a dorequest.php. Rend le corps JSON, ou une erreur dans `error`.
nlohmann::json request(const std::string& form, std::string& error) {
    std::string body;
    long status = 0;
    CURL* curl = curl_easy_init();
    if (!curl) { error = "curl"; return {}; }
    const std::string agent = std::string("Bootcade/") + BOOTCADE_VERSION;
    curl_easy_setopt(curl, CURLOPT_URL, kServer);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, form.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, agent.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) { error = curl_easy_strerror(rc); return {}; }
    try {
        return nlohmann::json::parse(body);
    } catch (...) {
        error = status ? "HTTP " + std::to_string(status) : std::string("no answer");
        return {};
    }
}

}  // namespace

bool signed_in() {
    std::lock_guard<std::mutex> lock(g_mutex);
    load_locked();
    return !g_user.empty() && !g_token.empty();
}

std::string username() {
    std::lock_guard<std::mutex> lock(g_mutex);
    load_locked();
    return g_user;
}

bool hardcore() {
    std::lock_guard<std::mutex> lock(g_mutex);
    load_locked();
    return g_hardcore;
}

void set_hardcore(bool on) {
    std::lock_guard<std::mutex> lock(g_mutex);
    load_locked();
    g_hardcore = on;
    save_locked();
}

std::string login(const std::string& user, const std::string& password) {
    if (user.empty() || password.empty()) return _("Enter your RetroAchievements user name and password.");
    std::string error;
    const auto j = request("r=login2&u=" + escape(user) + "&p=" + escape(password), error);
    if (!error.empty()) return Glib::ustring::compose(_("RetroAchievements could not be reached: %1"), error).raw();
    if (!j.is_object() || !j.value("Success", false))
        return j.is_object() && j.contains("Error") && j["Error"].is_string()
                   ? j["Error"].get<std::string>()
                   : std::string(_("RetroAchievements refused the sign-in."));
    std::lock_guard<std::mutex> lock(g_mutex);
    load_locked();
    g_user  = j.value("User", user);       // l'orthographe exacte du compte
    g_token = j.value("Token", std::string());
    save_locked();
    return g_token.empty() ? std::string(_("RetroAchievements did not return a session.")) : std::string();
}

void sign_out() {
    std::lock_guard<std::mutex> lock(g_mutex);
    load_locked();
    g_user.clear();
    g_token.clear();
    save_locked();
}

std::vector<std::string> launch_env() {
    std::lock_guard<std::mutex> lock(g_mutex);
    load_locked();
    if (g_user.empty() || g_token.empty()) return {};
    return {"BOOTCADE_RA_USER=" + g_user, "BOOTCADE_RA_TOKEN=" + g_token,
            std::string("BOOTCADE_RA_HARDCORE=") + (g_hardcore ? "1" : "0")};
}

bool supports(const std::string& emulator, const std::string& system) {
    return emulator == "fbneo" && (system == "Arcade" || system == "Neo Geo");
}

namespace {
std::mutex g_cat_mutex;
bool g_cat_loaded = false;
std::unordered_map<std::string, unsigned> g_cat;     // md5 du nom de set -> nombre de succes
std::unordered_map<std::string, unsigned> g_by_set;  // nom de set -> nombre (cache du precedent)

std::string catalog_path() {
    return AppContext::get_user_config_dir() + "/retroachievements-arcade.json";
}

// Le JSON de « systemgames » : [{NumAchievements, SupportedHashes[]}...].
void read_catalog_locked(const std::string& text) {
    g_cat.clear();
    g_by_set.clear();
    try {
        const auto j = nlohmann::json::parse(text);
        if (!j.is_object() || !j.contains("Response") || !j["Response"].is_array()) return;
        for (const auto& g : j["Response"]) {
            const unsigned n = g.value("NumAchievements", 0u);
            if (!n || !g.contains("SupportedHashes") || !g["SupportedHashes"].is_array()) continue;
            for (const auto& h : g["SupportedHashes"])
                if (h.is_string()) g_cat[h.get<std::string>()] = n;
        }
    } catch (...) {}
}

void load_catalog_locked() {
    if (g_cat_loaded) return;
    g_cat_loaded = true;
    std::ifstream f(catalog_path());
    if (!f) return;
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    read_catalog_locked(text);
}
}  // namespace

unsigned achievement_count(const std::string& emulator, const std::string& system,
                           const std::string& set_name) {
    if (!supports(emulator, system)) return 0;
    std::lock_guard<std::mutex> lock(g_cat_mutex);
    load_catalog_locked();
    // Le filtre et la liste le demandent pour des dizaines de milliers de
    // jeux, a chaque frappe dans la recherche : l'empreinte n'est calculee
    // qu'une fois par set.
    auto known = g_by_set.find(set_name);
    if (known != g_by_set.end()) return known->second;
    const std::string hash = Glib::Checksum::compute_checksum(Glib::Checksum::CHECKSUM_MD5, set_name);
    auto it = g_cat.find(hash);
    const unsigned n = it == g_cat.end() ? 0 : it->second;
    g_by_set.emplace(set_name, n);
    return n;
}

bool refresh_catalog() {
    std::error_code ec;
    const auto when = std::filesystem::last_write_time(catalog_path(), ec);
    if (!ec && std::filesystem::file_time_type::clock::now() - when < std::chrono::hours(24))
        return false;
    // 27 : la console « Arcade » de RetroAchievements (rc_consoles.h).
    std::string error;
    std::string body;
    {
        CURL* curl = curl_easy_init();
        if (!curl) return false;
        const std::string agent = std::string("Bootcade/") + BOOTCADE_VERSION;
        curl_easy_setopt(curl, CURLOPT_URL, kServer);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "r=systemgames&s=27");
        curl_easy_setopt(curl, CURLOPT_USERAGENT, agent.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, to_string);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
        long status = 0;
        const CURLcode rc = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(curl);
        if (rc != CURLE_OK || status != 200) return false;
    }
    std::lock_guard<std::mutex> lock(g_cat_mutex);
    const size_t before = g_cat.size();
    read_catalog_locked(body);
    g_cat_loaded = true;
    if (g_cat.empty()) return false;      // reponse inattendue : on garde l'ancien fichier
    std::ofstream f(catalog_path(), std::ios::trunc);
    if (f) f << body;
    return g_cat.size() != before;
}

GameAchievements fetch(const std::string& set_name) {
    GameAchievements out;
    std::string user, token;
    bool hc;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        load_locked();
        user = g_user; token = g_token; hc = g_hardcore;
    }
    if (user.empty() || token.empty()) { out.error = _("Not signed in"); return out; }

    // Un jeu d'arcade se reconnait a l'empreinte MD5 du nom de son set.
    const std::string hash = Glib::Checksum::compute_checksum(Glib::Checksum::CHECKSUM_MD5, set_name);
    const std::string auth = "&u=" + escape(user) + "&t=" + escape(token);

    std::string error;
    const auto sets = request("r=achievementsets" + auth + "&m=" + hash, error);
    if (!error.empty()) { out.error = error; return out; }
    out.answered = true;
    if (!sets.is_object() || !sets.value("Success", false)) return out;   // jeu inconnu
    out.game_id = sets.value("GameId", 0u);
    if (sets.contains("Sets") && sets["Sets"].is_array())
        for (const auto& set : sets["Sets"]) {
            if (set.value("Type", std::string("core")) != "core") continue;
            if (!set.contains("Achievements") || !set["Achievements"].is_array()) continue;
            for (const auto& a : set["Achievements"]) {
                if (a.value("Flags", 3) != 3) continue;          // 5 : non officiel
                // Ce n'est pas un succes : un message du serveur. Meme seuil
                // que rcheevos (RC_CLIENT_ACHIEVEMENT_WARNING_ID).
                if (a.value("ID", 0u) >= 101000001u) {
                    out.warning = a.value("Title", std::string());
                    const std::string why = a.value("Description", std::string());
                    if (!why.empty()) out.warning += " : " + why;
                    continue;
                }
                Achievement x;
                x.id          = a.value("ID", 0u);
                x.title       = a.value("Title", std::string());
                x.description = a.value("Description", std::string());
                x.points      = a.value("Points", 0u);
                out.list.push_back(std::move(x));
            }
        }
    if (!out.game_id || out.list.empty()) return out;

    const auto unlocks = request("r=unlocks" + auth + "&g=" + std::to_string(out.game_id)
                                 + "&h=" + (hc ? "1" : "0"), error);
    std::set<unsigned> done;
    if (error.empty() && unlocks.is_object() && unlocks.contains("UserUnlocks")
        && unlocks["UserUnlocks"].is_array())
        for (const auto& id : unlocks["UserUnlocks"])
            if (id.is_number_unsigned()) done.insert(id.get<unsigned>());
    for (auto& a : out.list) {
        a.unlocked = done.count(a.id) > 0;
        out.points += a.points;
        if (a.unlocked) { ++out.unlocked; out.points_unlocked += a.points; }
    }
    return out;
}

}  // namespace RetroAchievements
