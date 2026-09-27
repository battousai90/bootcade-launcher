// src/ControllerManager.cpp
#include "ControllerManager.h"
#include <linux/joystick.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <glob.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <cstdlib>
#include <vector>
#include <cctype>
#include <cstdio>
#include <map>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ── Device discovery ───────────────────────────────────────────────────────

std::vector<JoystickInfo> ControllerManager::list_devices() {
    std::vector<JoystickInfo> result;

    /* Manettes simulees, pour le banc graphique uniquement.
     *
     * L'ecran de configuration change d'apparence selon la manette reconnue :
     * illustration, disposition annoncee, encart vert. Sans peripherique
     * branche on ne peut donc en verifier aucun etat, et un ecran Xvfb n'a
     * jamais de /dev/input/js0. BOOTCADE_FAKE_PADS liste des noms separes par
     * des points-virgules et rien d'autre ne lit cette variable.
     */
    if (const char* fake = std::getenv("BOOTCADE_FAKE_PADS")) {
        std::string all = fake, one;
        std::istringstream in(all);
        int n = 0;
        while (std::getline(in, one, ';')) {
            if (one.empty()) continue;
            JoystickInfo info;
            info.path = "/dev/input/js" + std::to_string(n++);
            info.name = one;
            info.num_buttons = 12;
            info.num_axes    = 8;
            result.push_back(std::move(info));
        }
        return result;
    }

    glob_t g{};
    if (glob("/dev/input/js*", 0, nullptr, &g) != 0) return result;

    for (size_t i = 0; i < g.gl_pathc; ++i) {
        std::string path = g.gl_pathv[i];
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        JoystickInfo info;
        info.path = path;

        char name[256] = {};
        if (ioctl(fd, JSIOCGNAME(sizeof(name)), name) >= 0)
            info.name = std::string(name);
        else
            info.name = "Joystick";

        uint8_t nb = 0, na = 0;
        ioctl(fd, JSIOCGBUTTONS, &nb);
        ioctl(fd, JSIOCGAXES,    &na);
        info.num_buttons = nb;
        info.num_axes    = na;

        close(fd);
        result.push_back(std::move(info));
    }
    globfree(&g);
    return result;
}

// ── Device I/O ────────────────────────────────────────────────────────────

int ControllerManager::open_device(const std::string& path) {
    return open(path.c_str(), O_RDONLY | O_NONBLOCK);
}

void ControllerManager::close_device(int fd) {
    if (fd >= 0) close(fd);
}

bool ControllerManager::poll_event(int fd, InputBinding& result) {
    if (fd < 0) return false;

    struct js_event ev;
    while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        const uint8_t type = ev.type & ~JS_EVENT_INIT;
        if (type == JS_EVENT_BUTTON && ev.value == 1) {
            result = { true, InputSource::PAD, false, (int)ev.number, -1, 0, -1 };
            return true;
        }
        if (type == JS_EVENT_AXIS && std::abs(ev.value) > 16000) {
            result = { true, InputSource::PAD, true, -1, (int)ev.number,
                       ev.value > 0 ? 1 : -1, -1 };
            return true;
        }
    }
    return false;
}

bool ControllerManager::poll_raw(int fd, RawInput& out) {
    if (fd < 0) return false;

    struct js_event ev;
    while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        // Le bit JS_EVENT_INIT marque l'etat initial que le pilote envoie a
        // l'ouverture : on le garde, c'est lui qui donne la position de repos.
        const uint8_t type = ev.type & ~JS_EVENT_INIT;
        if (type == JS_EVENT_BUTTON) {
            out = { false, (int)ev.number, (int)ev.value };
            return true;
        }
        if (type == JS_EVENT_AXIS) {
            out = { true, (int)ev.number, (int)ev.value };
            return true;
        }
        // Type inconnu : on continue a vider la file plutot que de s'arreter.
    }
    return false;
}

// ── JSON helpers ──────────────────────────────────────────────────────────

static GameAction action_from_key(const std::string& k) {
    for (int a = 0; a < GAME_ACTION_COUNT; ++a) {
        const auto action = static_cast<GameAction>(a);
        if (k == game_action_key(action)) return action;
    }
    return GameAction::COUNT;
}

// ── JSON serialization helpers ────────────────────────────────────────────

static json config_to_json(const ControllerConfig& cfg) {
    json jctrl = json::array();
    for (const auto& player : cfg.players) {
        json jp;
        jp["device"]      = player.device_path;
        jp["device_name"] = player.device_name;
        jp["preset"]      = player.preset;
        json jb = json::object();
        for (const auto& [action, binding] : player.bindings) {
            if (!binding.valid) continue;
            json jv;
            // "source" est absent des configurations d'avant le clavier : son
            // absence vaut donc manette, et rien n'est a migrer.
            if (binding.source == InputSource::KEY) {
                jv["source"]   = "key";
                jv["key"]      = binding.key;
                jv["key_name"] = binding.key_name;
            } else {
                jv["is_axis"] = binding.is_axis;
                if (binding.is_axis) { jv["axis"] = binding.axis; jv["dir"] = binding.axis_dir; }
                else                 { jv["button"] = binding.button; }
            }
            jb[game_action_key(action)] = jv;
        }
        jp["bindings"] = jb;
        json ja = json::object();
        for (const auto& [role, b] : player.analog) {
            if (!b.is_set()) continue;
            json jv;
            switch (b.source) {
                case AnalogSource::KEY_PAIR:   jv["source"] = "keys";  break;
                case AnalogSource::MOUSE_AXIS: jv["source"] = "mouse"; break;
                default:                       jv["source"] = "joy";   break;
            }
            jv["key_neg"]      = b.key_neg;
            jv["key_pos"]      = b.key_pos;
            jv["key_neg_name"] = b.key_neg_name;
            jv["key_pos_name"] = b.key_pos_name;
            jv["index"]    = b.index;
            jv["invert"]   = b.invert;
            jv["relative"] = b.relative;
            jv["speed"]    = b.speed;
            jv["center"]   = b.center;
            ja[analog_role_key(role)] = jv;
        }
        jp["analog"] = ja;
        jctrl.push_back(jp);
    }
    return jctrl;
}

static ControllerConfig json_to_config(const json& jctrl) {
    ControllerConfig cfg;
    for (int p = 0; p < 2 && p < (int)jctrl.size(); ++p) {
        const auto& jp = jctrl[p];
        cfg.players[p].device_path = jp.value("device",      "");
        cfg.players[p].device_name = jp.value("device_name", "");
        cfg.players[p].preset      = jp.value("preset",      "");
        if (jp.contains("bindings")) {
            for (auto& [key, val] : jp["bindings"].items()) {
                GameAction action = action_from_key(key);
                if (action == GameAction::COUNT) continue;
                InputBinding b;
                b.valid    = true;
                if (val.value("source", std::string("pad")) == "key") {
                    b.source = InputSource::KEY;
                    b.key      = val.value("key", -1);
                    b.key_name = val.value("key_name", std::string());
                    if (b.key < 0) continue;
                    cfg.players[p].bindings[action] = b;
                    continue;
                }
                b.is_axis  = val.value("is_axis", false);
                b.button   = val.value("button",  -1);
                b.axis     = val.value("axis",    -1);
                b.axis_dir = val.value("dir",      0);
                cfg.players[p].bindings[action] = b;
            }
        }
        if (!jp.contains("analog")) continue;
        for (auto& [key, val] : jp["analog"].items()) {
            AnalogRole role = analog_role_from_key(key);
            if (role == AnalogRole::COUNT) continue;
            AnalogBinding b;
            const std::string kind = val.value("source", std::string("joy"));
            b.source   = kind == "mouse" ? AnalogSource::MOUSE_AXIS
                       : kind == "keys"  ? AnalogSource::KEY_PAIR
                                         : AnalogSource::JOY_AXIS;
            b.key_neg  = val.value("key_neg",  -1);
            b.key_pos  = val.value("key_pos",  -1);
            b.key_neg_name = val.value("key_neg_name", std::string());
            b.key_pos_name = val.value("key_pos_name", std::string());
            b.index    = val.value("index",    -1);
            b.invert   = val.value("invert",   false);
            b.relative = val.value("relative", false);
            b.speed    = val.value("speed",    0x800);
            b.center   = val.value("center",   10);
            cfg.players[p].analog[role] = b;
        }
    }
    return cfg;
}

// ── Single-config persistence (legacy) ───────────────────────────────────

void ControllerManager::load_config(ControllerConfig& out, const std::string& config_path) {
    out = ControllerConfig();
    json j;
    {
        std::ifstream f(config_path);
        if (!f) return;
        try { f >> j; } catch (...) { return; }
    }
    if (j.contains("controller_profiles") && j.contains("active_controller_profile")) {
        std::string active = j["active_controller_profile"].get<std::string>();
        if (j["controller_profiles"].contains(active))
            out = json_to_config(j["controller_profiles"][active]);
        return;
    }
    if (j.contains("controllers"))
        out = json_to_config(j["controllers"]);
}

void ControllerManager::save_config(const ControllerConfig& cfg, const std::string& config_path) {
    json j;
    {
        std::ifstream fi(config_path);
        if (fi) { try { fi >> j; } catch (...) {} }
    }
    j["controllers"] = config_to_json(cfg);
    std::ofstream fo(config_path);
    if (fo) fo << j.dump(2) << std::endl;
}

// ── Profile persistence ───────────────────────────────────────────────────

void ControllerManager::load_profiles(std::map<std::string, ControllerConfig>& profiles,
                                       std::string& active_name,
                                       const std::string& config_path)
{
    profiles.clear();
    active_name = "Default";

    json j;
    {
        std::ifstream f(config_path);
        if (f) { try { f >> j; } catch (...) {} }
    }

    // Migration: old format had "controllers" array, no profiles
    if (!j.contains("controller_profiles") && j.contains("controllers")) {
        profiles["Default"] = json_to_config(j["controllers"]);
        active_name = "Default";
        return;
    }

    if (j.contains("controller_profiles")) {
        for (auto& [name, val] : j["controller_profiles"].items()) {
            try { profiles[name] = json_to_config(val); }
            catch (...) {}
        }
    }
    if (j.contains("active_controller_profile"))
        active_name = j["active_controller_profile"].get<std::string>();

    // Ensure at least one profile exists
    if (profiles.empty()) {
        profiles["Default"] = ControllerConfig{};
        active_name = "Default";
    }
    // Ensure active_name is valid
    if (!profiles.count(active_name))
        active_name = profiles.begin()->first;
}

void ControllerManager::save_profiles(const std::map<std::string, ControllerConfig>& profiles,
                                       const std::string& active_name,
                                       const std::string& config_path)
{
    json j;
    {
        std::ifstream fi(config_path);
        if (fi) { try { fi >> j; } catch (...) {} }
    }

    json jprofiles = json::object();
    for (const auto& [name, cfg] : profiles)
        jprofiles[name] = config_to_json(cfg);

    j["controller_profiles"]      = jprofiles;
    j["active_controller_profile"] = active_name;

    // Also write "controllers" key with the active profile (backward compat / FBNeo write)
    if (profiles.count(active_name))
        j["controllers"] = config_to_json(profiles.at(active_name));

    std::ofstream fo(config_path);
    if (fo) fo << j.dump(2) << std::endl;
}

// ── FBNeo config writer ───────────────────────────────────────────────────

std::string ControllerManager::get_fbneo_config_dir() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "") + "/.local/share/fbneo/config";
}

// Nom FBNeo d'une action, sans le prefixe du joueur ("up", "fire 3").
// Les boutons au-dela du sixieme existent dans FBNeo ("p1 fire 7" chez
// Mitchell, Data East) ; un jeu qui ne les a pas ignore simplement la ligne.
static std::string fbneo_action_name(GameAction a)
{
    if (is_button_action(a)) return "fire " + std::to_string(button_number(a));
    switch (a) {
        case GameAction::UP:    return "up";
        case GameAction::DOWN:  return "down";
        case GameAction::LEFT:  return "left";
        case GameAction::RIGHT: return "right";
        case GameAction::START: return "start";
        case GameAction::COIN:  return "coin";
        default:                return "";
    }
}

// FBNeo axis direction codes for UP/DOWN/LEFT/RIGHT (in GameAction order)
// FBNeo hardcodes: 0x00=left, 0x01=right, 0x02=up, 0x03=down
static const uint32_t FBNEO_AXIS_CODES[4] = {
    0x02, // UP
    0x03, // DOWN
    0x00, // LEFT
    0x01, // RIGHT
};

// Update or add a key=value line in fbneo.ini (key is the token before the first space)
static void upsert_ini_line(std::vector<std::string>& lines,
                            const std::string& key,
                            const std::string& new_line)
{
    for (auto& line : lines) {
        // Match lines that begin with the key followed by a space or end-of-string
        if (line.size() >= key.size()
            && line.compare(0, key.size(), key) == 0
            && (line.size() == key.size() || line[key.size()] == ' '))
        {
            line = new_line;
            return;
        }
    }
    lines.push_back(new_line);
}

// Read nIniVersion from fbneo.ini so we can stamp it into p?defaults.ini.
// FBNeo requires: nConfigMinVersion (0x020921) <= version <= nBurnVer
// Without a valid version line FBNeo skips all input entries.
static uint32_t read_fbneo_burn_ver(const std::string& fbneo_ini_path)
{
    std::ifstream f(fbneo_ini_path);
    if (!f) return 0x100000u; // safe fallback well above 0x020921

    std::string line;
    while (std::getline(f, line)) {
        if (line.compare(0, 12, "nIniVersion ") == 0) {
            try { return static_cast<uint32_t>(std::stoul(line.substr(12), nullptr, 0)); }
            catch (...) {}
        }
    }
    return 0x100000u;
}

// Code "switch" d'une liaison telle que FBNeo l'ecrit, ou false si elle n'a
// pas d'equivalent. Partage entre les defauts par joueur et le .ini par jeu :
// les deux fichiers parlent la meme langue.
static bool switch_code_for(const InputBinding& b, int action, uint32_t base, uint32_t& sw)
{
    if (!b.valid) return false;
    if (b.source == InputSource::KEY) {
        // Le clavier occupe le bas de l'espace de codes, les manettes
        // le haut : un code de touche s'ecrit tel quel.
        if (b.key < 0) return false;
        sw = static_cast<uint32_t>(b.key);
        return true;
    }
    if (b.is_axis) {
        // UP/DOWN/LEFT/RIGHT → FBNeo hardcoded axis direction codes.
        // Non-directional axis binding : FBNeo doesn't support it this way.
        if (action >= 4) return false;
        sw = base | FBNEO_AXIS_CODES[action];
        return true;
    }
    sw = base | 0x80u | static_cast<uint32_t>(b.button);
    return true;
}

// Index de manette FBNeo d'un chemin /dev/input/jsN, et la base du code.
static uint32_t joystick_base_for(const std::string& device_path)
{
    int joy_index = 0;
    size_t i = device_path.size();
    while (i > 0 && std::isdigit((unsigned char)device_path[i - 1])) --i;
    if (i < device_path.size()) joy_index = std::stoi(device_path.substr(i));
    return 0x4000u | (static_cast<uint32_t>(joy_index) << 8);
}

void ControllerManager::write_fbneo_config(const ControllerConfig& cfg,
                                            const std::string& fbneo_config_dir)
{
    // Read the installed FBNeo version so we can stamp it in the defaults files.
    const uint32_t burn_ver = read_fbneo_burn_ver(fbneo_config_dir + "/fbneo.ini");

    for (int p = 0; p < 2; ++p) {
        const auto& player = cfg.players[p];
        if (player.device_path.empty() || player.bindings.empty()) continue;

        const uint32_t base = joystick_base_for(player.device_path);

        // Write p(p+1)defaults.ini
        std::string ini_name = "p" + std::to_string(p + 1) + "defaults.ini";
        std::string ini_path = fbneo_config_dir + "/" + ini_name;

        std::ofstream f(ini_path);
        if (!f) {
            std::cerr << "[ControllerManager] Cannot write " << ini_path << std::endl;
            continue;
        }

        // Version header : required by FBNeo: nConfigMinVersion (0x020921) <= ver <= nBurnVer
        f << "version 0x" << std::uppercase << std::hex << burn_ver << "\n\n";

        const std::string prefix = "p" + std::to_string(p + 1) + " ";

        for (int a = 0; a < GAME_ACTION_COUNT; ++a) {
            GameAction action = static_cast<GameAction>(a);
            auto it = player.bindings.find(action);
            if (it == player.bindings.end() || !it->second.valid) continue;

            uint32_t sw = 0;
            if (!switch_code_for(it->second, a, base, sw)) continue;

            std::ostringstream oss;
            oss << "input \"" << prefix << fbneo_action_name(action)
                << "\" switch 0x"
                << std::uppercase << std::hex << sw;
            f << oss.str() << "\n";
        }

        std::cout << "[ControllerManager] Wrote " << ini_path << "\n";
    }

    // ── Update fbneo.ini szPlayerDefaultIni entries ───────────────────────
    std::string fbneo_ini = fbneo_config_dir + "/fbneo.ini";

    std::vector<std::string> lines;
    {
        std::ifstream fi(fbneo_ini);
        if (fi) {
            std::string line;
            while (std::getline(fi, line)) lines.push_back(line);
        }
    }

    for (int p = 0; p < 2; ++p) {
        const auto& player = cfg.players[p];
        std::string key = "szPlayerDefaultIni[" + std::to_string(p) + "]";

        std::string new_line;
        if (!player.device_path.empty() && !player.bindings.empty()) {
            new_line = key + " "
                       + fbneo_config_dir + "/p" + std::to_string(p + 1) + "defaults.ini";
        } else {
            new_line = key + " ";  // empty value → disabled
        }

        upsert_ini_line(lines, key, new_line);
    }

    std::ofstream fo(fbneo_ini);
    if (!fo) {
        std::cerr << "[ControllerManager] Cannot write " << fbneo_ini << "\n";
        return;
    }
    for (const auto& line : lines) fo << line << "\n";
    std::cout << "[ControllerManager] Updated " << fbneo_ini << "\n";
}

// ── Player-2 input conflict repair ──────────────────────────────────────
// See the header for why this runs after FBNeo rather than before it.
namespace {

// Matches: input  "<name>"   switch 0x4186
// Captures the name and the binding so we can compare players.
bool parse_input_line(const std::string& line,
                      std::string& name, std::string& binding)
{
    const std::string prefix = "input";
    size_t p = line.find_first_not_of(" \t");
    if (p == std::string::npos || line.compare(p, prefix.size(), prefix) != 0)
        return false;
    size_t q1 = line.find('"');
    size_t q2 = (q1 == std::string::npos) ? std::string::npos : line.find('"', q1 + 1);
    if (q2 == std::string::npos) return false;
    name = line.substr(q1 + 1, q2 - q1 - 1);

    size_t sw = line.find("switch", q2);
    if (sw == std::string::npos) return false;          // already undefined
    size_t v = line.find_first_not_of(" \t", sw + 6);
    if (v == std::string::npos) return false;
    size_t e = line.find_first_of(" \t\r\n", v);
    binding = line.substr(v, (e == std::string::npos ? line.size() : e) - v);
    return true;
}

} // namespace

void ControllerManager::fix_player2_input_conflicts(const std::string& fbneo_rom_name)
{
    if (fbneo_rom_name.empty()) return;
    const std::string path = get_fbneo_config_dir() + "/games/" + fbneo_rom_name + ".ini";

    std::ifstream fi(path);
    if (!fi) return;                                     // not played yet : nothing to repair
    std::vector<std::string> lines;
    for (std::string l; std::getline(fi, l); ) lines.push_back(l);
    fi.close();

    // Games do not agree on how they name these inputs: "Coin 1"/"Coin 2",
    // "P1 Coin"/"P2 Coin", and some carry both a P1 and a P2 prefix on the
    // same concept. Pair them explicitly rather than guessing from a pattern.
    const std::vector<std::pair<std::string, std::string>> pairs = {
        {"Coin 1",  "Coin 2"},
        {"Start 1", "Start 2"},
        {"P1 Coin",  "P2 Coin"},
        {"P1 Start", "P2 Start"},
    };

    std::map<std::string, std::string> bound;            // input name → binding
    for (const auto& l : lines) {
        std::string n, b;
        if (parse_input_line(l, n, b)) bound[n] = b;
    }

    int fixed = 0;
    for (const auto& pr : pairs) {
        auto p1 = bound.find(pr.first);
        auto p2 = bound.find(pr.second);
        if (p1 == bound.end() || p2 == bound.end()) continue;
        if (p1->second != p2->second) continue;          // distinct pad: legitimate

        for (auto& l : lines) {
            std::string n, b;
            if (!parse_input_line(l, n, b) || n != pr.second) continue;
            size_t sw = l.find("switch");
            l = l.substr(0, sw) + "undefined";
            ++fixed;
            break;
        }
    }
    if (!fixed) return;

    std::ofstream fo(path);
    if (!fo) {
        std::cerr << "[ControllerManager] Cannot write " << path << "\n";
        return;
    }
    for (const auto& l : lines) fo << l << "\n";
    std::cout << "[ControllerManager] " << fbneo_rom_name
              << ": unbound " << fixed
              << " player-2 input(s) that duplicated player 1\n";
}


// ── Analog inputs ─────────────────────────────────────────────────────────

// L'emulateur code les touches comme DirectInput, et X livre un code materiel
// qui vaut le code evdev plus huit. Les deux jeux coincident jusqu'a 0x58,
// tout le bloc principal du clavier : A vaut 0x1E des deux cotes, Espace 0x39.
//
// Ils divergent au-dela, la ou DirectInput prefixait autrefois ces touches
// d'un octet : la fleche gauche est 105 pour evdev et 0xCB ici. D'ou la table,
// qui ne couvre que ces touches-la.
int ControllerManager::fbneo_key_from_gtk(unsigned hardware_keycode) {
    const int evdev = (int)hardware_keycode - 8;
    if (evdev < 1) return -1;
    if (evdev <= 0x58) return evdev;

    static const struct { int evdev; int fbk; } extended[] = {
        { 96, 0x9C},  // Entree du pave numerique
        { 97, 0x9D},  // Ctrl droite
        { 98, 0xB5},  // Division du pave
        {100, 0xB8},  // Alt droite
        {102, 0xC7},  // Debut
        {103, 0xC8},  // Haut
        {104, 0xC9},  // Page haut
        {105, 0xCB},  // Gauche
        {106, 0xCD},  // Droite
        {107, 0xCF},  // Fin
        {108, 0xD0},  // Bas
        {109, 0xD1},  // Page bas
        {110, 0xD2},  // Inser
        {111, 0xD3},  // Suppr
        {119, 0xC5},  // Pause
        {125, 0xDB},  // Meta gauche
        {126, 0xDC},  // Meta droite
        {127, 0xDD},  // Menu
    };
    for (const auto& e : extended) if (e.evdev == evdev) return e.fbk;
    return -1;
}

unsigned ControllerManager::gtk_keycode_from_fbneo(int fbneo_key) {
    if (fbneo_key <= 0) return 0;
    // Le bloc principal se traduit directement ; au-dela on redemande a la
    // meme table, dans l'autre sens, plutot que d'en tenir une seconde qui
    // pourrait diverger.
    if (fbneo_key <= 0x58) return (unsigned)(fbneo_key + 8);
    for (unsigned code = 9; code < 256; ++code)
        if (fbneo_key_from_gtk(code) == fbneo_key) return code;
    return 0;
}

AnalogRole ControllerManager::analog_role_for_input(const std::string& name) {
    std::string n;
    for (char c : name) n += (char)std::tolower((unsigned char)c);
    auto has = [&n](const char* needle) { return n.find(needle) != std::string::npos; };

    // Les 111 noms de commandes analogiques declares par les pilotes de
    // l'emulateur, ramenes a cinq roles. La liste vient de la source et non
    // d'une intuition : chaque nom non reconnu laisse un jeu injouable a la
    // manette, ce qui est arrive a After Burner et a ses "Left/Right".
    //
    // L'ORDRE FAIT LA REGLE. Le plus precis d'abord : "Spinner X" est un axe
    // horizontal, "Spinner" seul est une molette ; "Steering Left/Right" reste
    // une direction alors que "Left/Right" seul est un manche. Le frein passe
    // avant l'accelerateur, quelques jeux nommant une pedale combinee.

    // Axe horizontal : visee, manche, molette d'un jeu a deux axes.
    if (has("gun x") || has("target x") || has("aim x") || has("crosshair x")
        || has("trackball x") || has("mouse x") || has("track x")
        || has("fire x") || has("spinner x") || has("stick x")
        || has("x axis") || has("x-axis")
        || has("target l/r") || has("l/r")
        || has("gun"))                                               return AnalogRole::AIM_X;

    // Axe vertical.
    if (has("gun y") || has("target y") || has("aim y") || has("crosshair y")
        || has("trackball y") || has("mouse y") || has("track y")
        || has("fire y") || has("spinner y") || has("stick y")
        || has("y axis") || has("y-axis")
        || has("target u/d") || has("u/d") || has("pitch"))          return AnalogRole::AIM_Y;

    if (has("brake"))                                                return AnalogRole::BRAKE;

    // Poussee : "accel" couvre Accelerate et Accelerator, "thrust" les jeux
    // spatiaux, "plunger" le lanceur d'un flipper, qui se tire de la meme
    // facon qu'on enfonce une gachette.
    if (has("accel") || has("throttle") || has("gas") || has("pedal")
        || has("thrust") || has("plunger"))                          return AnalogRole::THROTTLE;

    // Commandes rotatives a un seul axe : volant, palette, molette, bouton
    // rotatif. Apres les axes X et Y, pour que "Spinner X" reste un axe.
    if (has("steering") || has("wheel") || has("paddle") || has("dial")
        || has("handle") || has("steer") || has("spinner") || has("knob"))
        return AnalogRole::STEERING;

    // Manche nomme par sa direction plutot que par sa fonction, avec ou sans
    // espaces autour de la barre, et dans les deux ordres selon les pilotes.
    if (has("up/down") || has("up-down") || has("up / down"))        return AnalogRole::AIM_Y;
    if (has("left/right") || has("left-right") || has("left / right")
        || has("right / left") || has("right/left") || has("roll"))  return AnalogRole::AIM_X;

    // Laisse au clavier ce qui reste : une poignee de commandes sans
    // equivalent sur une manette, comme la force d'un coup de batte ou de
    // poing, ou un troisieme axe de manche. Les lier au hasard donnerait deux
    // commandes differentes sur le meme axe, ce qui est pire que rien.
    return AnalogRole::COUNT;
}

namespace {

// Rebuilds the value part of an `input "..." <value>` line for one binding.
std::string analog_value_for(const AnalogBinding& b, int joy_index) {
    char buf[128];
    if (b.source == AnalogSource::KEY_PAIR) {
        // Deux touches font tourner l'axe, qui revient au centre quand on
        // relache : c'est la seule facon de conduire au clavier, et c'est le
        // reglage d'origine des jeux de vol et de course.
        std::snprintf(buf, sizeof(buf), "slider 0x%x 0x%x speed 0x%x center %d",
                      b.key_neg, b.key_pos, b.speed, b.center);
        return buf;
    }
    if (b.source == AnalogSource::MOUSE_AXIS) {
        std::snprintf(buf, sizeof(buf), "mouseaxis %d", b.index);
        return buf;
    }
    if (b.relative) {
        std::snprintf(buf, sizeof(buf), "joyslider %d %d speed 0x%x center %d",
                      joy_index, b.index, b.speed, b.center);
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "joyaxis %d %d", joy_index, b.index);
    return buf;
}

// The joystick number FBNeo uses is the device's position in /dev/input/js*,
// which is also the order ControllerManager::list_devices reports.
int fbneo_joy_index(const std::string& device_path) {
    auto pos = device_path.rfind("js");
    if (pos == std::string::npos) return 0;
    try { return std::stoi(device_path.substr(pos + 2)); }
    catch (...) { return 0; }
}

} // namespace

namespace {

// Ce qu'une ligne d'entree du .ini represente pour un joueur : -1 si elle ne
// le concerne pas, sinon l'index GameAction. `fires` compte les boutons deja
// rencontres pour ce joueur, dans l'ordre du fichier.
int game_action_of(const std::string& name, int player, int& fires)
{
    const std::string pn = "P" + std::to_string(player + 1);
    const std::string n  = std::to_string(player + 1);
    if (name == pn + " Coin"  || name == "Coin " + n)  return static_cast<int>(GameAction::COIN);
    if (name == pn + " Start" || name == "Start " + n) return static_cast<int>(GameAction::START);
    if (name.compare(0, pn.size() + 1, pn + " ") != 0) return -1;
    const std::string rest = name.substr(pn.size() + 1);
    if (rest == "Up")    return static_cast<int>(GameAction::UP);
    if (rest == "Down")  return static_cast<int>(GameAction::DOWN);
    if (rest == "Left")  return static_cast<int>(GameAction::LEFT);
    if (rest == "Right") return static_cast<int>(GameAction::RIGHT);
    // Select n'est pas un bouton d'action : les defauts de FBNeo ne le
    // touchent pas, on fait pareil.
    if (rest == "Select") return -1;
    if (fires >= MAX_GAME_BUTTONS) return -1;
    return static_cast<int>(GameAction::BUTTON1) + fires++;
}

} // namespace

int ControllerManager::write_game_config(const ControllerConfig& cfg,
                                         const std::string& fbneo_rom_name)
{
    if (fbneo_rom_name.empty()) return -1;
    const std::string path = get_fbneo_config_dir() + "/games/" + fbneo_rom_name + ".ini";
    std::ifstream fi(path);
    if (!fi) return -1;                                  // jamais lance : rien a reecrire
    std::vector<std::string> lines;
    for (std::string l; std::getline(fi, l); ) lines.push_back(l);
    fi.close();

    int changed = 0;
    for (int p = 0; p < 2; ++p) {
        const auto& player = cfg.players[p];
        if (player.device_path.empty() || player.bindings.empty()) continue;
        const uint32_t base = joystick_base_for(player.device_path);

        int fires = 0;
        for (auto& l : lines) {
            // Seules les entrees a interrupteur : un slider (analogique) ou une
            // constante (DIP) n'a pas de "switch", et ce n'est pas notre affaire.
            size_t p0 = l.find_first_not_of(" \t");
            if (p0 == std::string::npos || l.compare(p0, 5, "input") != 0) continue;
            size_t q1 = l.find('"'), q2 = (q1 == std::string::npos) ? q1 : l.find('"', q1 + 1);
            if (q2 == std::string::npos) continue;
            const std::string name = l.substr(q1 + 1, q2 - q1 - 1);
            size_t kind = l.find_first_not_of(" \t", q2 + 1);
            if (kind == std::string::npos) continue;
            const bool is_switch = l.compare(kind, 6, "switch") == 0
                                || l.compare(kind, 9, "undefined") == 0;
            if (!is_switch) continue;

            const int action = game_action_of(name, p, fires);
            if (action < 0) continue;
            auto it = player.bindings.find(static_cast<GameAction>(action));
            if (it == player.bindings.end()) continue;
            uint32_t sw = 0;
            if (!switch_code_for(it->second, action, base, sw)) continue;

            std::ostringstream oss;
            oss << "switch 0x" << std::uppercase << std::hex << sw;
            const std::string updated = l.substr(0, kind) + oss.str();
            if (updated != l) { l = updated; ++changed; }
        }
    }
    if (!changed) return 0;

    std::ofstream fo(path);
    if (!fo) {
        std::cerr << "[ControllerManager] Cannot write " << path << "\n";
        return -1;
    }
    for (const auto& l : lines) fo << l << "\n";
    std::cout << "[ControllerManager] " << fbneo_rom_name << ": " << changed
              << " input(s) bound from the game's own profile\n";
    return changed;
}

std::map<std::string, std::string> ControllerManager::load_game_profiles(const std::string& config_path)
{
    std::map<std::string, std::string> out;
    json j;
    std::ifstream fi(config_path);
    if (fi) { try { fi >> j; } catch (...) { return out; } }
    if (!j.contains("game_controller_profiles") || !j["game_controller_profiles"].is_object())
        return out;
    for (auto& [rom, name] : j["game_controller_profiles"].items())
        if (name.is_string()) out[rom] = name.get<std::string>();
    return out;
}

void ControllerManager::set_game_profile(const std::string& config_path,
                                         const std::string& fbneo_rom_name,
                                         const std::string& profile_name)
{
    json j;
    {
        std::ifstream fi(config_path);
        if (fi) { try { fi >> j; } catch (...) {} }
    }
    if (!j.contains("game_controller_profiles") || !j["game_controller_profiles"].is_object())
        j["game_controller_profiles"] = json::object();
    if (profile_name.empty()) j["game_controller_profiles"].erase(fbneo_rom_name);
    else                      j["game_controller_profiles"][fbneo_rom_name] = profile_name;
    std::ofstream fo(config_path);
    if (fo) fo << j.dump(2) << std::endl;
}

void ControllerManager::apply_analog_bindings(const std::string& fbneo_rom_name,
                                              const ControllerConfig& cfg) {
    const std::string path = get_fbneo_config_dir() + "/games/" + fbneo_rom_name + ".ini";

    std::ifstream fi(path);
    if (!fi) return;                       // not played yet : nothing to rewrite
    std::vector<std::string> lines;
    for (std::string line; std::getline(fi, line); ) lines.push_back(line);
    fi.close();
    if (lines.empty()) return;

    // Le nom porte le joueur : "P2 Steering" appartient a la deuxieme manette.
    // Tout renvoyer sur la premiere donnait aux deux joueurs le meme volant.
    auto owner = [&cfg](const std::string& input) -> const PlayerConfig* {
        size_t index = 0;
        if (input.size() > 2 && (input[0] == 'P' || input[0] == 'p')
            && input[1] >= '1' && input[1] <= '4')
            index = (size_t)(input[1] - '1');
        // Au-dela de deux manettes configurees, on retombe sur la premiere :
        // mieux vaut une commande partagee qu'une commande morte.
        if (index >= cfg.players.size()) index = 0;
        const PlayerConfig& chosen = cfg.players[index];
        if (chosen.device_path.empty() && chosen.device_name.empty())
            return cfg.players.empty() ? nullptr : &cfg.players[0];
        return &chosen;
    };

    const PlayerConfig& p1 = cfg.players.empty() ? PlayerConfig{} : cfg.players[0];
    // Un chemin absent n'est pas une manette absente. Le profil enregistre ici
    // portait bien "Microsoft X-Box 360 pad" mais pas son chemin, et abandonner
    // la-dessus laissait TOUTES les commandes analogiques sur le clavier, y
    // compris celles que le reste du code savait deja reconnaitre.
    //
    // Sans chemin, fbneo_joy_index rend zero, soit la premiere manette : c'est
    // le cas de tout le monde sauf a en avoir branche plusieurs, et c'est en
    // tout cas meilleur que de ne rien lier du tout.
    if (p1.device_path.empty() && p1.device_name.empty()) return;

    bool changed = false;
    for (auto& line : lines) {
        // input "<name>"  <value>
        auto q1 = line.find('"');
        if (line.find("input") == std::string::npos || q1 == std::string::npos) continue;
        auto q2 = line.find('"', q1 + 1);
        if (q2 == std::string::npos) continue;
        std::string name = line.substr(q1 + 1, q2 - q1 - 1);

        std::string value = line.substr(q2 + 1);
        auto first = value.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        value = value.substr(first);

        // Only analog inputs. A `switch` is digital and already handled by the
        // per-player defaults; touching it here would undo the player's own
        // button mapping.
        bool analog = value.rfind("slider", 0) == 0 || value.rfind("joyslider", 0) == 0
                   || value.rfind("joyaxis", 0) == 0 || value.rfind("mouseaxis", 0) == 0;
        if (!analog) continue;

        AnalogRole role = analog_role_for_input(name);
        if (role == AnalogRole::COUNT) continue;      // unrecognised: leave alone

        const PlayerConfig* who = owner(name);
        if (!who || (who->device_path.empty() && who->device_name.empty())) continue;
        auto it = who->analog.find(role);
        AnalogBinding b = it != who->analog.end() ? it->second : default_analog_binding(role);
        if (!b.is_set()) continue;
        const int pad = fbneo_joy_index(who->device_path);

        std::string rebuilt = "input  \"" + name + "\"" +
                              std::string(name.size() < 18 ? 18 - name.size() : 1, ' ') +
                              analog_value_for(b, pad);
        if (rebuilt != line) { line = rebuilt; changed = true; }
    }
    if (!changed) return;

    std::ofstream fo(path, std::ios::trunc);
    if (!fo) return;
    for (const auto& line : lines) fo << line << "\n";
}
