// src/ControllerManager.cpp
#include "ControllerManager.h"
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#include <cmath>
#include <deque>
#endif
#include "FbneoPaths.h"
#include "Platform.h"
#ifndef _WIN32
#include <linux/joystick.h>
#include <sys/ioctl.h>
#include <glob.h>
#endif
#include <fcntl.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <cstdlib>
#include <vector>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>
#include <array>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

#ifdef _WIN32
// ── Manettes Windows (winmm) ───────────────────────────────────────────────
//
// L'API joystick de Windows lit toutes les manettes HID, y compris celles que
// DirectInput et XInput exposent : un nom, des boutons, six axes et un chapeau
// (POV). On la presente comme joydev le fait sous Linux : un identifiant
// « winmm:js<N> » (le chiffre final est lu comme un numero de manette, exactement
// comme /dev/input/js<N>), des axes de -32767 a 32767, et un chapeau rendu par
// deux axes supplementaires, 6 (X) et 7 (Y), comme joydev.
namespace {

std::string narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

struct WinPad {
    UINT id = 0;
    JOYCAPSW caps{};
    int buttons = 0;
    int axes = 0;                // axes « vrais » : X, Y, Z, R, U, V selon la manette
    bool has_pov = false;
    int axis_slot[6] = {-1, -1, -1, -1, -1, -1};   // axe winmm -> numero d'axe presente
    int value[8] = {};           // dernier etat des axes presentes (6 vrais + 2 chapeau)
    DWORD held = 0;              // boutons enfonces
    bool primed = false;
    std::deque<ControllerManager::RawInput> pending;
};

std::map<int, WinPad>& pads() { static std::map<int, WinPad> m; return m; }

int axis_value(DWORD pos, DWORD lo, DWORD hi) {
    if (hi <= lo) return 0;
    const double mid = (double(lo) + double(hi)) / 2.0;
    const double half = (double(hi) - double(lo)) / 2.0;
    double v = (double(pos) - mid) / half * 32767.0;
    if (v > 32767) v = 32767;
    if (v < -32767) v = -32767;
    return (int)v;
}

bool read_pad(WinPad& p, JOYINFOEX& ji) {
    ji = {};
    ji.dwSize = sizeof ji;
    ji.dwFlags = JOY_RETURNALL | JOY_RETURNPOVCTS;
    return joyGetPosEx(p.id, &ji) == JOYERR_NOERROR;
}

// Etat courant -> valeurs d'axes presentes.
void axes_of(const WinPad& p, const JOYINFOEX& ji, int out[8]) {
    for (int i = 0; i < 8; ++i) out[i] = 0;
    const DWORD pos[6] = {ji.dwXpos, ji.dwYpos, ji.dwZpos, ji.dwRpos, ji.dwUpos, ji.dwVpos};
    const UINT lo[6] = {p.caps.wXmin, p.caps.wYmin, p.caps.wZmin, p.caps.wRmin, p.caps.wUmin, p.caps.wVmin};
    const UINT hi[6] = {p.caps.wXmax, p.caps.wYmax, p.caps.wZmax, p.caps.wRmax, p.caps.wUmax, p.caps.wVmax};
    for (int i = 0; i < 6; ++i)
        if (p.axis_slot[i] >= 0) out[p.axis_slot[i]] = axis_value(pos[i], lo[i], hi[i]);
    if (p.has_pov && ji.dwPOV != JOY_POVCENTERED && ji.dwPOV <= 36000) {
        const double a = ji.dwPOV / 100.0 * 3.14159265358979 / 180.0;   // 0 = haut, sens horaire
        const double x = std::sin(a), y = -std::cos(a);
        out[6] = x > 0.5 ? 32767 : (x < -0.5 ? -32767 : 0);
        out[7] = y > 0.5 ? 32767 : (y < -0.5 ? -32767 : 0);
    }
}

int pad_number(const std::string& path) {
    size_t i = path.size();
    while (i > 0 && std::isdigit((unsigned char)path[i - 1])) --i;
    return i < path.size() ? std::stoi(path.substr(i)) : -1;
}

}  // namespace

static std::vector<JoystickInfo> win_list_devices() {
    std::vector<JoystickInfo> result;
    const UINT n = joyGetNumDevs();
    for (UINT id = 0; id < n; ++id) {
        JOYCAPSW caps{};
        if (joyGetDevCapsW(id, &caps, sizeof caps) != JOYERR_NOERROR) continue;
        JOYINFOEX ji{};
        ji.dwSize = sizeof ji;
        ji.dwFlags = JOY_RETURNALL;
        if (joyGetPosEx(id, &ji) != JOYERR_NOERROR) continue;   // rien de branche a cet emplacement
        JoystickInfo info;
        info.path = "winmm:js" + std::to_string(id);
        info.name = narrow(caps.szPname);
        if (info.name.empty()) info.name = "Joystick";
        info.num_buttons = (int)caps.wNumButtons;
        int axes = (int)caps.wNumAxes;
        if (caps.wCaps & JOYCAPS_HASPOV) axes += 2;
        info.num_axes = axes;
        result.push_back(std::move(info));
    }
    return result;
}

static int win_open(const std::string& path) {
    const int id = pad_number(path);
    if (id < 0) return -1;
    WinPad p;
    p.id = (UINT)id;
    if (joyGetDevCapsW(p.id, &p.caps, sizeof p.caps) != JOYERR_NOERROR) return -1;
    JOYINFOEX ji;
    if (!read_pad(p, ji)) return -1;
    p.buttons = (int)p.caps.wNumButtons;
    p.has_pov = (p.caps.wCaps & JOYCAPS_HASPOV) != 0;
    // Les axes que la manette declare, dans l'ordre X Y Z R U V, numerotes de 0.
    const bool has[6] = {true, true, (p.caps.wCaps & JOYCAPS_HASZ) != 0, (p.caps.wCaps & JOYCAPS_HASR) != 0,
                         (p.caps.wCaps & JOYCAPS_HASU) != 0, (p.caps.wCaps & JOYCAPS_HASV) != 0};
    int slot = 0;
    for (int i = 0; i < 6 && slot < 6; ++i)
        if (has[i] && slot < (int)p.caps.wNumAxes) p.axis_slot[i] = slot++;
    p.axes = slot;
    // Le chapeau suit les vrais axes : joydev les place a la fin.
    // On le rapporte toujours en 6 et 7 pour garder des numeros stables.
    axes_of(p, ji, p.value);
    p.held = ji.dwButtons;
    p.primed = true;
    const int fd = id + 1;
    pads()[fd] = std::move(p);
    return fd;
}

static void win_close(int fd) { pads().erase(fd); }

// Compare l'etat courant au dernier connu, empile les differences, rend la suivante.
static bool win_next_event(int fd, ControllerManager::RawInput& out) {
    auto it = pads().find(fd);
    if (it == pads().end()) return false;
    WinPad& p = it->second;
    if (p.pending.empty()) {
        JOYINFOEX ji;
        if (read_pad(p, ji)) {
            const DWORD changed = ji.dwButtons ^ p.held;
            for (int b = 0; b < p.buttons && b < 32; ++b)
                if (changed & (1u << b))
                    p.pending.push_back({false, b, (ji.dwButtons & (1u << b)) ? 1 : 0});
            p.held = ji.dwButtons;
            int now[8];
            axes_of(p, ji, now);
            for (int a = 0; a < 8; ++a)
                if (now[a] != p.value[a]) { p.pending.push_back({true, a, now[a]}); p.value[a] = now[a]; }
        }
    }
    if (p.pending.empty()) return false;
    out = p.pending.front();
    p.pending.pop_front();
    return true;
}
#endif

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

#ifdef _WIN32
    return win_list_devices();
#else
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
#endif
}

// ── Ce que sont les axes ─────────────────────────────────────────────────

std::vector<int> ControllerManager::axis_codes(const std::string& device_path) {
    std::vector<int> codes;
    if (device_path.empty()) return codes;
#ifdef _WIN32
    const int id = pad_number(device_path);
    if (id < 0) return codes;
    JOYCAPSW caps{};
    if (joyGetDevCapsW((UINT)id, &caps, sizeof caps) != JOYERR_NOERROR) return codes;
    // Meme numerotation que win_open : les axes declares dans l'ordre
    // X Y Z R U V, puis le chapeau toujours en 6 et 7.
    const bool has[6] = {true, true, (caps.wCaps & JOYCAPS_HASZ) != 0, (caps.wCaps & JOYCAPS_HASR) != 0,
                         (caps.wCaps & JOYCAPS_HASU) != 0, (caps.wCaps & JOYCAPS_HASV) != 0};
    // R, U et V n'ont pas de correspondance fixe avec les axes de DirectInput
    // (ce que lit MAME) : on ne les nomme pas plutot que de deviner.
    const int known[6] = {0x00 /*ABS_X*/, 0x01 /*ABS_Y*/, 0x02 /*ABS_Z*/, -1, -1, -1};
    codes.assign(8, -1);
    int slot = 0;
    for (int i = 0; i < 6 && slot < 6; ++i)
        if (has[i] && slot < (int)caps.wNumAxes) codes[slot++] = known[i];
    if (caps.wCaps & JOYCAPS_HASPOV) { codes[6] = 0x10; codes[7] = 0x11; }   // ABS_HAT0X/Y
    return codes;
#else
    const int fd = open(device_path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) return codes;
    uint8_t count = 0;
    uint8_t map[ABS_CNT] = {};
    if (ioctl(fd, JSIOCGAXES, &count) >= 0 && ioctl(fd, JSIOCGAXMAP, map) >= 0)
        codes.assign(map, map + count);
    close(fd);
    return codes;
#endif
}

void ControllerManager::fill_right_stick(PlayerConfig& player) {
    if (player.device_path.empty()) return;
    bool missing = false;
    for (int a = (int)GameAction::RSTICK_UP; a <= (int)GameAction::RSTICK_RIGHT; ++a) {
        auto it = player.bindings.find(static_cast<GameAction>(a));
        missing = missing || it == player.bindings.end() || !it->second.valid;
    }
    if (!missing) return;
    const std::vector<int> codes = axis_codes(player.device_path);
    constexpr int kAbsRX = 0x03, kAbsRY = 0x04;
    int rx = -1, ry = -1;
    for (int i = 0; i < (int)codes.size(); ++i) {
        if (codes[i] == kAbsRX) rx = i;
        if (codes[i] == kAbsRY) ry = i;
    }
    /* Sans RX/RY, la convention HID des manettes : le stick droit est sur
     * Z et RZ. C'est le cas des copies de manette PS3 et de beaucoup de pads
     * generiques ; une manette Xbox, elle, a RX/RY et ses gachettes sur Z/RZ,
     * elle ne passe donc jamais ici. */
    if (rx < 0 || ry < 0) {
        constexpr int kAbsZ = 0x02, kAbsRZ = 0x05;
        rx = ry = -1;
        for (int i = 0; i < (int)codes.size(); ++i) {
            if (codes[i] == kAbsZ)  rx = i;
            if (codes[i] == kAbsRZ) ry = i;
        }
    }
    if (rx < 0 || ry < 0) return;
    auto put = [&player](GameAction a, int axis, int dir) {
        auto it = player.bindings.find(a);
        if (it != player.bindings.end() && it->second.valid) return;   // le choix du joueur
        InputBinding b;
        b.valid = true;
        b.source = InputSource::PAD;
        b.is_axis = true;
        b.axis = axis;
        b.axis_dir = dir;
        player.bindings[a] = b;
    };
    put(GameAction::RSTICK_UP,    ry, -1);
    put(GameAction::RSTICK_DOWN,  ry,  1);
    put(GameAction::RSTICK_LEFT,  rx, -1);
    put(GameAction::RSTICK_RIGHT, rx,  1);
}

AnalogBinding ControllerManager::default_analog(const PlayerConfig& player, AnalogRole role) {
    // Une manette connue par son seul nom (chemin absent) : la premiere
    // manette, avec les numeros habituels, voir apply_analog_bindings.
    if (!player.has_device() && player.device_name.empty()) return {};
    const std::vector<int> codes = axis_codes(player.device_path);
    if (codes.empty()) return default_analog_binding(role);   // manette illisible

    constexpr int kAbsX = 0x00, kAbsY = 0x01, kAbsZ = 0x02, kAbsRX = 0x03,
                  kAbsRY = 0x04, kAbsRZ = 0x05, kAbsWheel = 0x08,
                  kAbsGas = 0x09, kAbsBrake = 0x0a;
    auto find = [&codes](int code) {
        for (int i = 0; i < (int)codes.size(); ++i) if (codes[i] == code) return i;
        return -1;
    };
    bool unknown = false;
    for (int c : codes) unknown = unknown || c < 0;
    // Z et RZ ne sont des gachettes que si le stick droit est ailleurs.
    const bool trigger_axes = find(kAbsRX) >= 0 && find(kAbsRY) >= 0;

    int axis = -1;
    switch (role) {
        case AnalogRole::STEERING:
            axis = find(kAbsWheel) >= 0 ? find(kAbsWheel) : find(kAbsX); break;
        case AnalogRole::AIM_X: axis = find(kAbsX); break;
        case AnalogRole::AIM_Y: axis = find(kAbsY); break;
        case AnalogRole::THROTTLE:
            axis = find(kAbsGas) >= 0 ? find(kAbsGas) : (trigger_axes ? find(kAbsRZ) : -1);
            break;
        case AnalogRole::BRAKE:
            axis = find(kAbsBrake) >= 0 ? find(kAbsBrake) : (trigger_axes ? find(kAbsZ) : -1);
            break;
        default: return {};
    }
    if (axis < 0) return unknown ? default_analog_binding(role) : AnalogBinding{};
    AnalogBinding b;
    b.source = AnalogSource::JOY_AXIS;
    b.index  = axis;
    return b;
}

namespace {

// Le prereglage d'un joueur, s'il en a un de connu.
const ControllerPreset* preset_named(const std::string& name) {
    for (const auto& p : controller_presets())
        if (name == p.name) return &p;
    return nullptr;
}

// Le joueur d'un profil fourni, au depart : celui du profil de base, ou le
// clavier pour un joueur 1 qui n'a encore rien de regle.
PlayerConfig template_player(const ControllerConfig& base, int p) {
    PlayerConfig out = p < (int)base.players.size() ? base.players[p] : PlayerConfig{};
    if (!out.bindings.empty()) return out;
    if (const ControllerPreset* pr = preset_named(out.preset)) {
        if (!pr->keyboard && out.has_device()) apply_preset(out, *pr);
        else if (pr->keyboard)                 apply_preset(out, *pr);
    } else if (p == 0 && !out.has_device() && !controller_presets().empty()
               && controller_presets().front().keyboard) {
        apply_preset(out, controller_presets().front());
        out.preset = controller_presets().front().name;
    }
    return out;
}

// La disposition a six boutons des bornes, celle du prereglage du joueur.
void six_button_layout(PlayerConfig& pl) {
    const ControllerPreset* pr = preset_named(pl.preset);
    if (!pr || (!pr->keyboard && !pl.has_device())) return;   // on garde ses boutons
    for (int i = 0; i < 6; ++i) {
        InputBinding b;
        b.valid = true;
        if (pr->keyboard) { b.source = InputSource::KEY; b.key = pr->face[i]; }
        else              { b.source = InputSource::PAD; b.button = pr->face[i]; }
        pl.bindings[button_action(i + 1)] = b;
    }
}

} // namespace

std::vector<std::pair<std::string, ControllerConfig>>
ControllerManager::builtin_profiles(const ControllerConfig& base)
{
    std::vector<std::pair<std::string, ControllerConfig>> out;
    auto each_player = [&base](auto&& shape) {
        ControllerConfig cfg;
        cfg.analog_percent = base.analog_percent;
        for (int p = 0; p < MAX_PLAYERS; ++p) {
            cfg.players[p] = template_player(base, p);
            shape(cfg.players[p], p);
        }
        return cfg;
    };

    // Combat : la rangee du haut en poings, celle du bas en pieds.
    out.emplace_back(_("Fighting (6 buttons)"),
                     each_player([](PlayerConfig& pl, int) { six_button_layout(pl); }));

    /* Neo Geo et shoot'em up : les quatre boutons principaux sur les quatre
     * boutons du pouce, A en bas, B a droite, C a gauche, D en haut, comme
     * les manettes Neo Geo. Le tir tombe sous le pouce, la bombe a cote. */
    out.emplace_back(_("Neo Geo / Shoot'em up (4 buttons)"),
                     each_player([](PlayerConfig& pl, int) {
        six_button_layout(pl);
        auto get = [&pl](int n) {
            auto it = pl.bindings.find(button_action(n));
            return it != pl.bindings.end() ? it->second : InputBinding{};
        };
        const InputBinding b[6] = {get(4), get(5), get(1), get(2), get(3), get(6)};
        for (int i = 0; i < 6; ++i) {
            if (b[i].valid) pl.bindings[button_action(i + 1)] = b[i];
            else            pl.bindings.erase(button_action(i + 1));
        }
    }));

    // Deux sticks : on se deplace au stick gauche, on tire au stick droit.
    out.emplace_back(_("Twin-stick"), each_player([](PlayerConfig& pl, int) {
        if (!pl.has_device()) return;
        const std::vector<int> codes = axis_codes(pl.device_path);
        int ax = -1, ay = -1;
        for (int i = 0; i < (int)codes.size(); ++i) {
            if (codes[i] == 0x00) ax = i;
            if (codes[i] == 0x01) ay = i;
        }
        if (ax < 0 || ay < 0) return;
        auto put = [&pl](GameAction a, int axis, int dir) {
            InputBinding b;
            b.valid = true; b.source = InputSource::PAD;
            b.is_axis = true; b.axis = axis; b.axis_dir = dir;
            pl.bindings[a] = b;
        };
        put(GameAction::UP, ay, -1);   put(GameAction::DOWN, ay, 1);
        put(GameAction::LEFT, ax, -1); put(GameAction::RIGHT, ax, 1);
        for (int a = (int)GameAction::RSTICK_UP; a <= (int)GameAction::RSTICK_RIGHT; ++a)
            pl.bindings.erase(static_cast<GameAction>(a));
        fill_right_stick(pl);
    }));

    // Course : volant au stick gauche, pedales sur les gachettes analogiques.
    out.emplace_back(_("Racing"), each_player([](PlayerConfig& pl, int) {
        if (!pl.has_device()) return;
        // Recalcule d'apres la manette, sans reprendre le profil de base :
        // l'ancien prereglage PS3 y mettait l'accelerateur sur la croix.
        for (AnalogRole r : {AnalogRole::STEERING, AnalogRole::THROTTLE, AnalogRole::BRAKE}) {
            const AnalogBinding b = default_analog(pl, r);
            if (b.is_set()) pl.analog[r] = b;
            else            pl.analog.erase(r);
        }
    }));

    /* Clavier et souris, pour le joueur 1 : la souris vise (pistolets),
     * roule (trackballs) et tourne (spinners, paddles) ; ses trois boutons
     * sont les trois premiers boutons du jeu. Les autres joueurs gardent
     * leur manette. */
    out.emplace_back(_("Keyboard and mouse"), each_player([](PlayerConfig& pl, int p) {
        if (p != 0) return;
        const ControllerPreset& kb = controller_presets().front();
        pl = PlayerConfig{};
        apply_preset(pl, kb);
        pl.preset = kb.name;
        for (int i = 0; i < 3; ++i) {
            InputBinding b;
            b.valid = true; b.source = InputSource::MOUSE; b.button = i;
            pl.bindings[button_action(i + 1)] = b;
        }
        AnalogBinding mx, my;
        mx.source = my.source = AnalogSource::MOUSE_AXIS;
        mx.index = 0; my.index = 1;
        pl.analog[AnalogRole::AIM_X]    = mx;
        pl.analog[AnalogRole::AIM_Y]    = my;
        pl.analog[AnalogRole::STEERING] = mx;
    }));

    // Clavier seul : le joueur 1 au clavier, les autres avec les touches de
    // l'emulateur.
    out.emplace_back(_("Keyboard"), [] {
        ControllerConfig cfg;
        const ControllerPreset& kb = controller_presets().front();
        apply_preset(cfg.players[0], kb);
        cfg.players[0].preset = kb.name;
        return cfg;
    }());
    return out;
}

// ── Device I/O ────────────────────────────────────────────────────────────

#ifdef _WIN32
int ControllerManager::open_device(const std::string& path) { return win_open(path); }
void ControllerManager::close_device(int fd) { win_close(fd); }

bool ControllerManager::poll_event(int fd, InputBinding& result) {
    RawInput ev;
    while (win_next_event(fd, ev)) {
        if (!ev.is_axis && ev.value == 1) {
            result = { true, InputSource::PAD, false, ev.index, -1, 0, -1 };
            return true;
        }
        if (ev.is_axis && std::abs(ev.value) > 16000) {
            result = { true, InputSource::PAD, true, -1, ev.index, ev.value > 0 ? 1 : -1, -1 };
            return true;
        }
    }
    return false;
}

bool ControllerManager::poll_raw(int fd, RawInput& out) { return win_next_event(fd, out); }
#else
namespace {
// Position de repos de chaque axe, relevee sur les evenements d'etat initial
// que le pilote envoie a l'ouverture. Une gachette repose a -32767 : sans
// cette valeur, le premier evenement d'un appui (-30000, -20000...) etait pris
// pour un mouvement vers le NEGATIF, et la gachette se liait « Axis N - »,
// c'est-a-dire toujours enfoncee dans le jeu.
std::map<int, std::array<int, 64>>& axis_rest_table() {
    static std::map<int, std::array<int, 64>> table;
    return table;
}
}  // namespace

int ControllerManager::open_device(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd >= 0) axis_rest_table().erase(fd);
    return fd;
}

void ControllerManager::close_device(int fd) {
    if (fd >= 0) { axis_rest_table().erase(fd); close(fd); }
}

bool ControllerManager::poll_event(int fd, InputBinding& result) {
    if (fd < 0) return false;

    struct js_event ev;
    while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        const uint8_t type = ev.type & ~JS_EVENT_INIT;
        if (type == JS_EVENT_AXIS && ev.number < 64) {
            auto& rest = axis_rest_table()[fd];
            if (ev.type & JS_EVENT_INIT) { rest[ev.number] = ev.value; continue; }
        }
        if (type == JS_EVENT_BUTTON && ev.value == 1) {
            result = { true, InputSource::PAD, false, (int)ev.number, -1, 0, -1 };
            return true;
        }
        if (type == JS_EVENT_AXIS) {
            const int rest = ev.number < 64 ? axis_rest_table()[fd][ev.number] : 0;
            int dir = 0;
            if (rest <= -16000)      { if (ev.value - rest > 24000) dir = 1;  }   // gachette
            else if (rest >= 16000)  { if (rest - ev.value > 24000) dir = -1; }
            else if (std::abs(ev.value) > 16000) dir = ev.value > 0 ? 1 : -1;
            if (dir != 0) {
                result = { true, InputSource::PAD, true, -1, (int)ev.number, dir, -1 };
                return true;
            }
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
#endif

// ── JSON helpers ──────────────────────────────────────────────────────────

static GameAction action_from_key(const std::string& k) {
    for (int a = 0; a < GAME_ACTION_COUNT; ++a) {
        const auto action = static_cast<GameAction>(a);
        if (k == game_action_key(action)) return action;
    }
    return GameAction::COUNT;
}

// ── JSON serialization helpers ────────────────────────────────────────────

// Une entree de manette (bouton ou moitie d'axe) d'une paire analogique.
static json pad_input_to_json(const InputBinding& b) {
    json j;
    j["is_axis"] = b.is_axis;
    if (b.is_axis) { j["axis"] = b.axis; j["dir"] = b.axis_dir; }
    else           { j["button"] = b.button; }
    return j;
}

static InputBinding pad_input_from_json(const json& j) {
    InputBinding b;
    if (!j.is_object()) return b;
    b.source   = InputSource::PAD;
    b.is_axis  = j.value("is_axis", false);
    b.button   = j.value("button",  -1);
    b.axis     = j.value("axis",    -1);
    b.axis_dir = j.value("dir",      0);
    b.valid    = b.is_axis ? (b.axis >= 0 && b.axis_dir != 0) : b.button >= 0;
    return b;
}

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
            } else if (binding.source == InputSource::MOUSE) {
                jv["source"] = "mouse";
                jv["button"] = binding.button;
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
            if (b.pad_neg.valid) jv["pad_neg"] = pad_input_to_json(b.pad_neg);
            if (b.pad_pos.valid) jv["pad_pos"] = pad_input_to_json(b.pad_pos);
            jv["index"]    = b.index;
            jv["invert"]   = b.invert;
            jv["relative"] = b.relative;
            jv["speed"]    = b.speed;
            jv["center"]   = b.center;
            ja[analog_role_key(role)] = jv;
        }
        jp["analog"] = ja;
        // Une seule valeur par jeu, portee par le premier joueur.
        if (jctrl.empty() && cfg.analog_percent > 0) jp["analog_percent"] = cfg.analog_percent;
        jctrl.push_back(jp);
    }
    return jctrl;
}

static ControllerConfig json_to_config(const json& jctrl) {
    ControllerConfig cfg;
    for (int p = 0; p < MAX_PLAYERS && p < (int)jctrl.size(); ++p) {
        const auto& jp = jctrl[p];
        cfg.players[p].device_path = jp.value("device",      "");
        cfg.players[p].device_name = jp.value("device_name", "");
        cfg.players[p].preset      = jp.value("preset",      "");
        if (p == 0)
            cfg.analog_percent = std::clamp(jp.value("analog_percent", 0), 0, ANALOG_PERCENT_MAX);
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
                if (val.value("source", std::string("pad")) == "mouse") {
                    b.source = InputSource::MOUSE;
                    b.button = val.value("button", -1);
                    if (b.button < 0 || b.button > 2) continue;
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
            if (val.contains("pad_neg")) b.pad_neg = pad_input_from_json(val["pad_neg"]);
            if (val.contains("pad_pos")) b.pad_pos = pad_input_from_json(val["pad_pos"]);
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

// Monte d'un cran quand de nouveaux profils fournis s'ajoutent : seuls les
// noms encore absents sont alors crees.
static constexpr int kBuiltinProfilesVersion = 1;

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

    /* Les profils fournis, une seule fois.
     *
     * Construits sur le profil actif, donc sur la manette que le joueur a
     * deja reglee. Un nom deja pris n'est pas touche, et la marque
     * « builtin_profiles » fait qu'un profil fourni supprime ne revient pas.
     * Enregistres tout de suite : un jeu peut se voir assigner l'un d'eux
     * avant que l'ecran des manettes ait jamais ete ouvert. */
    if (j.value("builtin_profiles", 0) < kBuiltinProfilesVersion) {
        for (auto& [name, cfg] : builtin_profiles(profiles[active_name]))
            if (!profiles.count(name)) profiles[name] = std::move(cfg);
        save_profiles(profiles, active_name, config_path);
    }
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
    j["builtin_profiles"]          = kBuiltinProfilesVersion;

    // Also write "controllers" key with the active profile (backward compat / FBNeo write)
    if (profiles.count(active_name))
        j["controllers"] = config_to_json(profiles.at(active_name));

    std::ofstream fo(config_path);
    if (fo) fo << j.dump(2) << std::endl;
}

// ── FBNeo config writer ───────────────────────────────────────────────────

std::string ControllerManager::get_fbneo_config_dir() {
    return FbneoPaths::config_dir();
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

/* Les noms affiches du second stick dans les pilotes de FBNeo, sans le
 * prefixe du joueur ni la direction. Releves dans les sources du fork : il n'y
 * a pas de nom commun, Smash TV dit « Fire Up », Total Carnage « Shoot Up »,
 * d'autres « Right Stick Up » (ou « Rightstick », ou « Rght Stick »). */
static const char* const kRightStickNames[] = {"Right Stick", "Rightstick", "Rght Stick",
                                               "Shoot", "Fire"};
// Et ceux du stick qui deplace, quand le jeu en a deux.
static const char* const kLeftStickNames[] = {"Left Stick", "Leftstick", "Stick"};

/* Les noms sous lesquels une action s'ecrit dans pNdefaults.ini.
 *
 * FBNeo cherche chaque ligne d'abord par le nom affiche de la commande, puis
 * par son nom interne, sans tenir compte des majuscules. Pour le second
 * stick, le nom interne le plus repandu est « p1 up 2 » ; on ecrit aussi
 * chaque nom affiche connu. Un jeu qui n'en a aucun ignore ces lignes. */
static std::vector<std::string> fbneo_default_names(GameAction a)
{
    if (!is_right_stick_action(a)) {
        const std::string n = fbneo_action_name(a);
        if (n.empty()) return {};
        return {n};
    }
    static const char* const dir[4] = {"up", "down", "left", "right"};
    const std::string d = dir[(int)a - (int)GameAction::RSTICK_UP];
    std::vector<std::string> names{d + " 2"};
    for (const char* n : kRightStickNames) {
        std::string lower;
        for (const char* c = n; *c; ++c) lower += (char)std::tolower((unsigned char)*c);
        names.push_back(lower + " " + d);
    }
    return names;
}

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

// Code « switch » d'une liaison telle que FBNeo l'ecrit, ou false si elle n'a
// pas d'equivalent. Partage entre les defauts par joueur et le .ini par jeu :
// les deux fichiers parlent la meme langue.
//
// Les codes d'axe du pilote SDL vont par paire : 2 x axe pour le sens negatif,
// 2 x axe + 1 pour le positif, jusqu'a l'axe 7. Les quatre premiers sont aussi
// ceux de la croix directionnelle, que SDL ne montre pas comme des axes : elle
// se lit par les codes 0 a 3, selon son orientation et non selon l'axe de
// l'enregistrement, qui change d'un pilote a l'autre.
//
// L'ancien code ecrivait toujours 0 a 3 pour un axe, quel qu'il soit : une
// gachette liee a Gauche devenait le stick gauche dans le .ini du jeu.
// Un joueur dont on ecrit les commandes : il a une manette, ou au moins une
// touche ou un bouton de souris (profil « Keyboard and mouse »).
static bool writes_defaults(const PlayerConfig& player)
{
    if (player.bindings.empty()) return false;
    if (player.has_device()) return true;
    for (const auto& [a, b] : player.bindings)
        if (b.valid && !binding_needs_pad(b)) return true;
    return false;
}

static bool switch_code_for(const InputBinding& b, uint32_t base,
                            const std::string& preset, uint32_t& sw,
                            const std::array<int, 64>* rest = nullptr)
{
    if (!b.valid) return false;
    if (b.source == InputSource::KEY) {
        // Le clavier occupe le bas de l'espace de codes, les manettes
        // le haut : un code de touche s'ecrit tel quel.
        if (b.key < 0) return false;
        sw = static_cast<uint32_t>(b.key);
        return true;
    }
    if (b.source == InputSource::MOUSE) {
        // La souris du systeme : 0x8080 + bouton (gauche, droit, milieu).
        if (b.button < 0 || b.button > 2) return false;
        sw = 0x8080u + static_cast<uint32_t>(b.button);
        return true;
    }
    if (b.is_axis) {
        if (b.axis < 0 || b.axis_dir == 0) return false;
        int hat_x = 6, hat_y = 7;
        preset_hat_axes(preset, hat_x, hat_y);
        int dir = b.axis_dir;
        // Une gachette repose a -32767 : « Axis N - » serait vrai en
        // permanence. Les profils enregistres avant la correction de la
        // capture portent ce sens-la ; on le remet dans le sens de l'appui.
        if (rest && b.axis < 64) {
            const int r = (*rest)[b.axis];
            if (r <= -16000 && dir < 0) dir = 1;
            else if (r >= 16000 && dir > 0) dir = -1;
        }
        const uint32_t positive = dir > 0 ? 1u : 0u;
        if (b.axis == hat_x)      { sw = base | (0u + positive); return true; }
        if (b.axis == hat_y)      { sw = base | (2u + positive); return true; }
        if (b.axis > 7)           return false;
        sw = base | (static_cast<uint32_t>(b.axis) * 2u + positive);
        return true;
    }
    if (b.button < 0) return false;
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

// Positions de repos des axes de la manette, lues sur l'etat initial que
// joydev envoie a l'ouverture. Vide si la manette n'est pas lisible (absente,
// ou Windows) : on n'a alors aucune raison de corriger quoi que ce soit.
static bool read_axis_rest(const std::string& device_path, std::array<int, 64>& rest)
{
    rest.fill(0);
#ifdef _WIN32
    (void)device_path;
    return false;
#else
    if (device_path.empty()) return false;
    const int fd = open(device_path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) return false;
    bool any = false;
    struct js_event ev;
    while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if ((ev.type & JS_EVENT_INIT) && (ev.type & ~JS_EVENT_INIT) == JS_EVENT_AXIS
            && ev.number < 64) { rest[ev.number] = ev.value; any = true; }
    }
    close(fd);
    return any;
#endif
}

void ControllerManager::write_fbneo_config(const ControllerConfig& cfg,
                                            const std::string& fbneo_config_dir)
{
    // Sous Windows aussi : le port SDL2 est le meme. A VERIFIER avec une vraie
    // manette : SDL numerote les manettes a sa facon, pas toujours comme winmm
    // sur laquelle repose la detection.
    // Read the installed FBNeo version so we can stamp it in the defaults files.
    const uint32_t burn_ver = read_fbneo_burn_ver(FbneoPaths::ini_path());

    for (int p = 0; p < MAX_PLAYERS && p < (int)cfg.players.size(); ++p) {
        PlayerConfig player = cfg.players[p];
        // Sans manette, seules les touches et la souris ont un sens.
        if (!writes_defaults(player)) continue;
        fill_right_stick(player);

        const uint32_t base = joystick_base_for(player.device_path);
        std::array<int, 64> rest_values{};
        const bool has_rest = read_axis_rest(player.device_path, rest_values);

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
            if (binding_needs_pad(it->second) && !player.has_device()) continue;

            uint32_t sw = 0;
            if (!switch_code_for(it->second, base, player.preset, sw,
                                 has_rest ? &rest_values : nullptr)) continue;

            for (const std::string& name : fbneo_default_names(action)) {
                std::ostringstream oss;
                oss << "input \"" << prefix << name
                    << "\" switch 0x"
                    << std::uppercase << std::hex << sw;
                f << oss.str() << "\n";
            }
        }

        std::cout << "[ControllerManager] Wrote " << ini_path << "\n";
    }

    // ── Update fbneo.ini szPlayerDefaultIni entries ───────────────────────
    std::string fbneo_ini = FbneoPaths::ini_path();

    std::vector<std::string> lines;
    {
        std::ifstream fi(fbneo_ini);
        if (fi) {
            std::string line;
            while (std::getline(fi, line)) lines.push_back(line);
        }
    }

    for (int p = 0; p < MAX_PLAYERS && p < (int)cfg.players.size(); ++p) {
        const auto& player = cfg.players[p];
        std::string key = "szPlayerDefaultIni[" + std::to_string(p) + "]";

        std::string new_line;
        if (writes_defaults(player)) {
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
        || has("target l/r") || has("l/r"))                          return AnalogRole::AIM_X;

    // Axe vertical.
    if (has("gun y") || has("target y") || has("aim y") || has("crosshair y")
        || has("trackball y") || has("mouse y") || has("track y")
        || has("fire y") || has("spinner y") || has("stick y")
        || has("y axis") || has("y-axis")
        || has("target u/d") || has("u/d") || has("pitch"))          return AnalogRole::AIM_Y;

    // Un pistolet nomme sans son axe. APRES l'axe vertical : place avec
    // l'horizontal, « gun » attrapait aussi « Gun Y », et le viseur de tous
    // les jeux au pistolet montait et descendait avec la souris gauche-droite.
    if (has("gun"))                                                  return AnalogRole::AIM_X;

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

// Code d'un des deux sens d'une paire : la touche, ou l'entree de manette si
// le joueur en a choisi une. false si ce sens n'a rien de valide.
bool pair_side_code(int key, const InputBinding& pad, int joy_index,
                    const std::string& preset,
                    const std::array<int, 64>* rest, uint32_t& code)
{
    if (pad.valid) {
        const uint32_t base = 0x4000u | (static_cast<uint32_t>(joy_index) << 8);
        return switch_code_for(pad, base, preset, code, rest);
    }
    if (key < 0) return false;
    code = static_cast<uint32_t>(key);
    return true;
}

// Rebuilds the value part of an `input "..." <value>` line for one binding.
//
// L'inversion : un `joyaxis` n'a aucun moyen de se retourner dans le .ini, il
// n'y a que la position du stick. Un `joyslider` porte une vitesse, et une
// vitesse negative va dans l'autre sens ; une paire s'inverse en echangeant
// ses deux entrees.
std::string analog_value_for(const AnalogBinding& b, int joy_index,
                             const std::string& preset,
                             const std::array<int, 64>* rest)
{
    char buf[160];
    if (b.source == AnalogSource::KEY_PAIR) {
        // Deux entrees font tourner l'axe, qui revient au centre quand on
        // relache : c'est la seule facon de conduire au clavier, et c'est le
        // reglage d'origine des jeux de vol et de course. Les memes deux
        // entrees peuvent etre des boutons ou des gachettes : le pilote lit
        // une touche et un bouton de la meme facon.
        uint32_t neg = 0, pos = 0;
        if (!pair_side_code(b.key_neg, b.pad_neg, joy_index, preset, rest, neg)
         || !pair_side_code(b.key_pos, b.pad_pos, joy_index, preset, rest, pos))
            return {};
        if (b.invert) std::swap(neg, pos);
        std::snprintf(buf, sizeof(buf), "slider 0x%x 0x%x speed 0x%x center %d",
                      neg, pos, b.speed, b.center);
        return buf;
    }
    if (b.source == AnalogSource::MOUSE_AXIS) {
        std::snprintf(buf, sizeof(buf), "mouseaxis %d", b.index);
        return buf;
    }
    if (b.relative) {
        std::snprintf(buf, sizeof(buf), "joyslider %d %d speed %s0x%x center %d",
                      joy_index, b.index, b.invert ? "-" : "", b.speed, b.center);
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
    /* Les jeux a deux manches : le stick de gauche deplace, celui de droite
     * tire. Sans ces noms, « P1 Shoot Up » passait pour un bouton et prenait
     * la place du bouton 1, ce qui decalait tous les suivants. */
    static const char* const kDirs[4] = {"Up", "Down", "Left", "Right"};
    for (int d = 0; d < 4; ++d) {
        for (const char* n : kLeftStickNames)
            if (rest == std::string(n) + " " + kDirs[d])
                return static_cast<int>(GameAction::UP) + d;
        for (const char* n : kRightStickNames)
            if (rest == std::string(n) + " " + kDirs[d])
                return static_cast<int>(GameAction::RSTICK_UP) + d;
    }
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
    for (int p = 0; p < MAX_PLAYERS && p < (int)cfg.players.size(); ++p) {
        PlayerConfig player = cfg.players[p];
        if (!writes_defaults(player)) continue;
        fill_right_stick(player);
        const uint32_t base = joystick_base_for(player.device_path);
        std::array<int, 64> rest_values{};
        const bool has_rest = read_axis_rest(player.device_path, rest_values);

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
            if (binding_needs_pad(it->second) && !player.has_device()) continue;
            uint32_t sw = 0;
            if (!switch_code_for(it->second, base, player.preset, sw,
                                 has_rest ? &rest_values : nullptr)) continue;

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
        if (chosen.device_path.empty() && chosen.device_name.empty()
            && !chosen.has_pad_free_inputs())
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
    // Un joueur 1 au clavier et a la souris a lui aussi ses commandes.
    if (p1.device_path.empty() && p1.device_name.empty() && !p1.has_pad_free_inputs()) return;

    // Les positions de repos des deux manettes, une fois pour tout le fichier.
    std::array<int, 64> rest_values[MAX_PLAYERS]{};
    bool has_rest[MAX_PLAYERS] = {};
    for (size_t i = 0; i < (size_t)MAX_PLAYERS && i < cfg.players.size(); ++i)
        has_rest[i] = read_axis_rest(cfg.players[i].device_path, rest_values[i]);

    bool changed = false;
    for (auto& line : lines) {
        // La sensibilite : une seule valeur pour tout le jeu, en tete du .ini.
        // Elle multiplie la vitesse de tout ce qui est relatif (molette,
        // palette, trackball, viseur). Ligne absente du fichier : on ne
        // l'invente pas, l'emulateur l'ecrit lui-meme a la premiere sortie.
        if (cfg.analog_percent > 0 && line.rfind("analog", 0) == 0
            && (line.size() == 6 || line[6] == ' ' || line[6] == '\t')) {
            char head[32];
            std::snprintf(head, sizeof(head), "analog  0x%04X",
                          (unsigned)(cfg.analog_percent * 0x100 / 100));
            if (line != head) { line = head; changed = true; }
            continue;
        }
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
        if (!who) continue;
        const bool who_pad = !who->device_path.empty() || !who->device_name.empty();
        auto it = who->analog.find(role);
        AnalogBinding b = it != who->analog.end() ? it->second : default_analog(*who, role);
        if (!b.is_set()) continue;
        if (analog_needs_pad(b) && !who_pad) continue;
        const int pad = fbneo_joy_index(who->device_path);
        const size_t who_index = who - cfg.players.data();
        const bool who_has_rest = who_index < (size_t)MAX_PLAYERS && has_rest[who_index];

        const std::string value_text = analog_value_for(
            b, pad, who->preset, who_has_rest ? &rest_values[who_index] : nullptr);
        if (value_text.empty()) continue;
        std::string rebuilt = "input  \"" + name + "\"" +
                              std::string(name.size() < 18 ? 18 - name.size() : 1, ' ') +
                              value_text;
        if (rebuilt != line) { line = rebuilt; changed = true; }
    }
    if (!changed) return;

    std::ofstream fo(path, std::ios::trunc);
    if (!fo) return;
    for (const auto& line : lines) fo << line << "\n";
}

// ── Manettes branchees aujourd'hui ───────────────────────────────────────

namespace {

std::string trim_spaces(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

bool read_all(const std::string& path, std::string& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::ostringstream ss; ss << f.rdbuf(); out = ss.str();
    return true;
}

// Les touches d'origine de FBNeo pour le joueur 1 (le joueur 2 n'en a pas :
// ses defauts sont des boutons de manette). Codes DirectInput de FBNeo.
bool keyboard_code_for(int player, int action, uint32_t& code)
{
    if (player != 0) return false;
    static const uint32_t fire[9] = {0x2C, 0x2D, 0x2E, 0x1E, 0x1F, 0x20, 0x10, 0x11, 0x12};  // Z X C A S D Q W E
    switch (static_cast<GameAction>(action)) {
        case GameAction::UP:    code = 0xC8; return true;
        case GameAction::DOWN:  code = 0xD0; return true;
        case GameAction::LEFT:  code = 0xCB; return true;
        case GameAction::RIGHT: code = 0xCD; return true;
        case GameAction::START: code = 0x02; return true;   // 1
        case GameAction::COIN:  code = 0x06; return true;   // 5
        default: break;
    }
    const int n = button_number(static_cast<GameAction>(action));
    if (n >= 1 && n <= 9) { code = fire[n - 1]; return true; }
    return false;
}

// Le nom d'une ligne « input "<nom>" switch|undefined ... », ou "" sinon.
std::string switch_line_name(const std::string& l)
{
    const size_t p0 = l.find_first_not_of(" \t");
    if (p0 == std::string::npos || l.compare(p0, 5, "input") != 0) return {};
    const size_t q1 = l.find('"');
    const size_t q2 = q1 == std::string::npos ? q1 : l.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    const size_t kind = l.find_first_not_of(" \t", q2 + 1);
    if (kind == std::string::npos
        || (l.compare(kind, 6, "switch") != 0 && l.compare(kind, 9, "undefined") != 0)) return {};
    return l.substr(q1 + 1, q2 - q1 - 1);
}

// « switch 0x4186 » : la valeur de la ligne, ou false si ce n'est pas un code.
bool switch_value(const std::string& l, size_t& value_at, size_t& value_end, uint32_t& value)
{
    const size_t q2 = l.find('"', l.find('"') + 1);
    const size_t kind = l.find_first_not_of(" \t", q2 + 1);
    if (kind == std::string::npos || l.compare(kind, 6, "switch") != 0) return false;
    value_at = l.find_first_not_of(" \t", kind + 6);
    if (value_at == std::string::npos) return false;
    value_end = l.find_first_of(" \t\r\n", value_at);
    if (value_end == std::string::npos) value_end = l.size();
    try { value = (uint32_t)std::stoul(l.substr(value_at, value_end - value_at), nullptr, 0); }
    catch (...) { return false; }
    return true;
}

// Les commandes de chaque joueur, d'apres la manette qu'il a MAINTENANT.
// target[p] : numero de manette actuel (>= 0), -1 (absente), -2 (pas de manette
// reglee pour ce joueur : on n'y touche pas). Une manette presente recoit les
// boutons de SON profil : deux manettes n'ont pas les memes numeros de boutons.
std::string retarget_players(const std::string& text, const int target[MAX_PLAYERS],
                             const ControllerConfig& cfg)
{
    std::istringstream in(text);
    std::string out, line;
    int fires[MAX_PLAYERS] = {};
    while (std::getline(in, line)) {
        const std::string name = switch_line_name(line);
        if (!name.empty()) {
            for (int p = 0; p < MAX_PLAYERS && p < (int)cfg.players.size(); ++p) {
                const int action = game_action_of(name, p, fires[p]);
                if (action < 0 || target[p] == -2) continue;
                size_t at = 0, end = 0; uint32_t val = 0;
                const bool has_code = switch_value(line, at, end, val);
                const bool is_pad = has_code && (val & 0xF000u) == 0x4000u;
                char buf[32];
                if (target[p] >= 0) {
                    const uint32_t base = 0x4000u | ((uint32_t)target[p] << 8);
                    uint32_t sw = 0;
                    const auto& binds = cfg.players[p].bindings;
                    auto it = binds.find(static_cast<GameAction>(action));
                    if (it != binds.end() && it->second.valid
                        && switch_code_for(it->second, base, cfg.players[p].preset, sw)) {
                        // Le bouton que le profil donne a cette action, sur cette manette.
                        const size_t q2 = line.find('"', line.find('"') + 1);
                        const size_t kind = line.find_first_not_of(" \t", q2 + 1);
                        std::snprintf(buf, sizeof(buf), "switch 0x%X", sw);
                        line = line.substr(0, kind) + buf;
                    } else if (is_pad) {
                        // Rien dans le profil : le meme bouton, sur la bonne manette.
                        std::snprintf(buf, sizeof(buf), "0x%X", (val & 0xF0FFu) | ((unsigned)target[p] << 8));
                        line = line.substr(0, at) + buf + line.substr(end);
                    }
                } else {
                    uint32_t code = 0;
                    if (keyboard_code_for(p, action, code)) {          // joueur 1 : le clavier
                        const size_t q2 = line.find('"', line.find('"') + 1);
                        const size_t kind = line.find_first_not_of(" \t", q2 + 1);
                        std::snprintf(buf, sizeof(buf), "switch 0x%02X", code);
                        line = line.substr(0, kind) + buf;
                    } else if (is_pad) {                               // joueurs 2 a 4 : rien
                        std::snprintf(buf, sizeof(buf), "0x%X", (val & 0xF0FFu) | (0xFu << 8));
                        line = line.substr(0, at) + buf + line.substr(end);
                    }
                }
                break;                                       // la ligne appartient a ce joueur
            }
        }
        out += line + "\n";
    }
    return out;
}

// Les lignes des deux joueurs reprises du fichier d'origine, par leur nom.
std::string players_from_original(const std::string& current, const std::string& original)
{
    std::map<std::string, std::string> kept;
    {
        std::istringstream in(original);
        std::string line;
        int fires[MAX_PLAYERS] = {};
        while (std::getline(in, line)) {
            const std::string name = switch_line_name(line);
            if (name.empty()) continue;
            for (int p = 0; p < MAX_PLAYERS; ++p)
                if (game_action_of(name, p, fires[p]) >= 0) { kept[name] = line; break; }
        }
    }
    std::istringstream in(current);
    std::string out, line;
    int fires[MAX_PLAYERS] = {};
    while (std::getline(in, line)) {
        const std::string name = switch_line_name(line);
        if (!name.empty())
            for (int p = 0; p < MAX_PLAYERS; ++p)
                if (game_action_of(name, p, fires[p]) >= 0) {
                    auto it = kept.find(name);
                    if (it != kept.end()) line = it->second;
                    break;
                }
        out += line + "\n";
    }
    return out;
}

// Les reglages par joueur (p1defaults.ini, p2defaults.ini) : tout le fichier
// est a ce joueur, et c'est le numero de manette seul qui change.
std::string retarget_defaults(const std::string& text, int target)
{
    std::istringstream in(text);
    std::string out, line;
    while (std::getline(in, line)) {
        size_t at = 0, end = 0; uint32_t val = 0;
        if (!switch_line_name(line).empty() && switch_value(line, at, end, val)
            && (val & 0xF000u) == 0x4000u) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "0x%X", (val & 0xF0FFu) | ((unsigned)(target < 0 ? 0xF : target) << 8));
            line = line.substr(0, at) + buf + line.substr(end);
        }
        out += line + "\n";
    }
    return out;
}

const char* const kMark = "// bootcade-device-map";

// Remet un fichier tel qu'il etait avant le lancement. Les reglages que le jeu
// a ecrits entre-temps (DIP...) sont gardes : seules les commandes des deux
// joueurs reprennent leur forme d'origine.
void put_back_file(const std::string& path)
{
    const std::string side = path + ".bcslots";
    std::string kept;
    if (!read_all(side, kept)) return;
    const size_t nl = kept.find('\n');
    const std::string head = kept.substr(0, nl);
    const std::string original = nl == std::string::npos ? std::string() : kept.substr(nl + 1);

    std::error_code ec;
    if (head.find("defaults") != std::string::npos) {
        std::ofstream fo(path);                       // ces fichiers ne sont lus qu'au debut
        if (fo) fo << original;
    } else {
        std::string now;
        if (read_all(path, now)) {
            std::ofstream fo(path);
            if (fo) fo << players_from_original(now, original);
        }
    }
    std::filesystem::remove(side, ec);
}

void keep_original(const std::string& path, const char* kind, const std::string& text)
{
    std::ofstream side(path + ".bcslots");
    if (side) side << kMark << ' ' << kind << '\n' << text;
}

} // namespace

void ControllerManager::adopt_plugged_pads(ControllerConfig& cfg, const std::vector<JoystickInfo>& present)
{
    std::vector<bool> taken(present.size(), false);
    for (auto& player : cfg.players) {
        const std::string name = trim_spaces(player.device_name);
        if (name.empty() && player.device_path.empty()) continue;    // clavier

        int found = -1;
        // Sans nom (profil ancien) : seul le chemin dit de quelle manette il s'agit.
        if (name.empty()) {
            for (size_t i = 0; i < present.size(); ++i)
                if (!taken[i] && present[i].path == player.device_path) { found = (int)i; break; }
        } else {
            // Deux manettes du meme nom : celle qui n'a pas bouge est preferee.
            for (size_t i = 0; found < 0 && i < present.size(); ++i)
                if (!taken[i] && present[i].path == player.device_path
                    && trim_spaces(present[i].name) == name) found = (int)i;
            for (size_t i = 0; found < 0 && i < present.size(); ++i)
                if (!taken[i] && trim_spaces(present[i].name) == name) found = (int)i;
        }
        if (found >= 0) {
            taken[found] = true;
            player.device_path = present[found].path;
            player.device_name = present[found].name;
        } else {
            player.device_path.clear();                              // absente : le nom reste, pour son retour
        }
    }
}

void ControllerManager::put_back_saved_devices(const std::string& fbneo_rom_name)
{
    if (fbneo_rom_name.empty()) return;
    const std::string dir = get_fbneo_config_dir();
    put_back_file(dir + "/games/" + fbneo_rom_name + ".ini");
    for (int p = 1; p <= MAX_PLAYERS; ++p)
        put_back_file(dir + "/p" + std::to_string(p) + "defaults.ini");
}

void ControllerManager::use_current_devices(const std::string& fbneo_rom_name,
                                            const ControllerConfig& cfg)
{
    if (fbneo_rom_name.empty()) return;
    // Une session precedente finie sans remise en place (arret brutal).
    put_back_saved_devices(fbneo_rom_name);

    // `cfg` est deja rapproche des manettes branchees (adopt_plugged_pads) :
    // un joueur avec un chemin a sa manette, un joueur avec un nom seul l'a
    // debranchee, un joueur sans rien joue au clavier ou n'a rien regle.
    int target[MAX_PLAYERS];
    bool any = false;
    // Le second stick par defaut fait partie du profil de chaque manette.
    ControllerConfig filled = cfg;
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        target[p] = -2;
        if (p >= (int)cfg.players.size()) continue;
        const auto& pl = cfg.players[p];
        if (!pl.device_path.empty())            target[p] = (int)((joystick_base_for(pl.device_path) >> 8) & 0xF);
        else if (!pl.device_name.empty())       target[p] = -1;
        any = any || target[p] != -2;
        fill_right_stick(filled.players[p]);
    }
    if (!any) return;

    const std::string dir = get_fbneo_config_dir();
    const std::string ini = dir + "/games/" + fbneo_rom_name + ".ini";
    std::string text;
    bool changed = false;
    if (read_all(ini, text)) {
        const std::string out = retarget_players(text, target, filled);
        if (out != text) {
            keep_original(ini, "game", text);
            std::ofstream fo(ini);
            if (fo) fo << out;
            changed = true;
        }
    } else {
        // Premier lancement : FBNeo cree le .ini depuis les reglages par joueur.
        for (int p = 0; p < MAX_PLAYERS; ++p) {
            if (target[p] == -2) continue;
            const std::string path = dir + "/p" + std::to_string(p + 1) + "defaults.ini";
            if (!read_all(path, text)) continue;
            if (target[p] == -1 && p == 0) {
                // Sans manette, FBNeo reprend ses touches du clavier.
                keep_original(path, "defaults", text);
                std::error_code ec;
                std::filesystem::remove(path, ec);
                changed = true;
                continue;
            }
            const std::string out = retarget_defaults(text, target[p]);
            if (out != text) {
                keep_original(path, "defaults", text);
                std::ofstream fo(path);
                if (fo) fo << out;
                changed = true;
            }
        }
    }
    if (changed) {
        std::cout << "[ControllerManager] " << fbneo_rom_name << ": pads set for this game";
        for (int p = 0; p < MAX_PLAYERS; ++p)
            std::cout << " P" << p + 1 << "="
                      << (target[p] >= 0 ? "js" + std::to_string(target[p])
                          : target[p] == -1 ? (p == 0 ? "keyboard" : "none") : "untouched");
        std::cout << std::endl;
    }
}
