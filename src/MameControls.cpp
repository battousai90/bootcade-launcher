// src/MameControls.cpp
#include "MameControls.h"
#include "ControllerManager.h"
#include "AppContext.h"
#include <linux/joystick.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace MameControls {

namespace {

/* Ce que MAME met par defaut sur chaque commande, SANS ses codes manette.
 *
 * Releve sur MAME 0.289 par script Lua (ioport:type_seq), pas recopie d'une
 * documentation. On garde ces touches a cote de la liaison du joueur : le
 * clavier doit continuer de marcher. Les codes JOYCODE sont retires, sinon
 * le bouton 1 de la manette declencherait encore P1_BUTTON1 apres avoir ete
 * deplace ailleurs. La souris et le pistolet du joueur 1 restent ; ceux du
 * joueur 2 n'ont pas de nom stable sans le peripherique, on les laisse. */
struct Defaults { const char* type; const char* standard; const char* increment; const char* decrement; };

const Defaults kDefaults[] = {
    {"COIN1",  "KEYCODE_5", "", ""},
    {"COIN2",  "KEYCODE_6", "", ""},
    {"START1", "KEYCODE_1", "", ""},
    {"START2", "KEYCODE_2", "", ""},
    {"P1_JOYSTICK_UP",    "KEYCODE_UP",    "", ""},
    {"P1_JOYSTICK_DOWN",  "KEYCODE_DOWN",  "", ""},
    {"P1_JOYSTICK_LEFT",  "KEYCODE_LEFT",  "", ""},
    {"P1_JOYSTICK_RIGHT", "KEYCODE_RIGHT", "", ""},
    {"P1_BUTTON1",  "KEYCODE_LCONTROL OR MOUSECODE_1_BUTTON1 OR GUNCODE_1_BUTTON1", "", ""},
    {"P1_BUTTON2",  "KEYCODE_LALT OR MOUSECODE_1_BUTTON3 OR GUNCODE_1_BUTTON2", "", ""},
    {"P1_BUTTON3",  "KEYCODE_SPACE OR MOUSECODE_1_BUTTON2", "", ""},
    {"P1_BUTTON4",  "KEYCODE_LSHIFT", "", ""},
    {"P1_BUTTON5",  "KEYCODE_Z", "", ""},
    {"P1_BUTTON6",  "KEYCODE_X", "", ""},
    {"P1_BUTTON7",  "KEYCODE_C", "", ""},
    {"P1_BUTTON8",  "KEYCODE_V", "", ""},
    {"P1_BUTTON9",  "KEYCODE_B", "", ""},
    {"P1_BUTTON10", "KEYCODE_N", "", ""},
    {"P1_BUTTON11", "KEYCODE_M", "", ""},
    {"P1_BUTTON12", "KEYCODE_COMMA", "", ""},
    {"P1_BUTTON13", "KEYCODE_STOP", "", ""},
    {"P1_BUTTON14", "KEYCODE_SLASH", "", ""},
    {"P1_BUTTON15", "KEYCODE_RSHIFT", "", ""},
    {"P1_BUTTON16", "", "", ""},
    {"P2_JOYSTICK_UP",    "KEYCODE_R", "", ""},
    {"P2_JOYSTICK_DOWN",  "KEYCODE_F", "", ""},
    {"P2_JOYSTICK_LEFT",  "KEYCODE_D", "", ""},
    {"P2_JOYSTICK_RIGHT", "KEYCODE_G", "", ""},
    {"P2_BUTTON1", "KEYCODE_A", "", ""},
    {"P2_BUTTON2", "KEYCODE_S", "", ""},
    {"P2_BUTTON3", "KEYCODE_Q", "", ""},
    {"P2_BUTTON4", "KEYCODE_W", "", ""},
    {"P2_BUTTON5", "KEYCODE_E", "", ""},
    {"P1_PADDLE",      "MOUSECODE_1_XAXIS", "KEYCODE_RIGHT", "KEYCODE_LEFT"},
    {"P1_DIAL",        "MOUSECODE_1_XAXIS", "KEYCODE_RIGHT", "KEYCODE_LEFT"},
    {"P1_POSITIONAL",  "MOUSECODE_1_XAXIS", "KEYCODE_RIGHT", "KEYCODE_LEFT"},
    {"P1_AD_STICK_X",  "MOUSECODE_1_XAXIS", "KEYCODE_RIGHT", "KEYCODE_LEFT"},
    {"P1_TRACKBALL_X", "MOUSECODE_1_XAXIS", "KEYCODE_RIGHT", "KEYCODE_LEFT"},
    {"P1_LIGHTGUN_X",  "GUNCODE_1_XAXIS OR MOUSECODE_1_XAXIS", "KEYCODE_RIGHT", "KEYCODE_LEFT"},
    {"P1_AD_STICK_Y",  "MOUSECODE_1_YAXIS", "KEYCODE_DOWN", "KEYCODE_UP"},
    {"P1_TRACKBALL_Y", "MOUSECODE_1_YAXIS", "KEYCODE_DOWN", "KEYCODE_UP"},
    {"P1_PADDLE_V",    "MOUSECODE_1_YAXIS", "KEYCODE_DOWN", "KEYCODE_UP"},
    {"P1_DIAL_V",      "MOUSECODE_1_YAXIS", "KEYCODE_DOWN", "KEYCODE_UP"},
    {"P1_LIGHTGUN_Y",  "GUNCODE_1_YAXIS OR MOUSECODE_1_YAXIS", "KEYCODE_DOWN", "KEYCODE_UP"},
    {"P1_PEDAL",       "", "KEYCODE_LCONTROL", ""},
    {"P1_PEDAL2",      "", "KEYCODE_LALT", ""},
    {"P2_PADDLE",      "", "KEYCODE_G", "KEYCODE_D"},
    {"P2_DIAL",        "", "KEYCODE_G", "KEYCODE_D"},
    {"P2_POSITIONAL",  "", "KEYCODE_G", "KEYCODE_D"},
    {"P2_AD_STICK_X",  "", "KEYCODE_G", "KEYCODE_D"},
    {"P2_TRACKBALL_X", "", "KEYCODE_G", "KEYCODE_D"},
    {"P2_LIGHTGUN_X",  "", "KEYCODE_G", "KEYCODE_D"},
    {"P2_AD_STICK_Y",  "", "KEYCODE_F", "KEYCODE_R"},
    {"P2_TRACKBALL_Y", "", "KEYCODE_F", "KEYCODE_R"},
    {"P2_PADDLE_V",    "", "KEYCODE_F", "KEYCODE_R"},
    {"P2_DIAL_V",      "", "KEYCODE_F", "KEYCODE_R"},
    {"P2_LIGHTGUN_Y",  "", "KEYCODE_F", "KEYCODE_R"},
    {"P2_PEDAL",       "", "KEYCODE_A", ""},
    {"P2_PEDAL2",      "", "KEYCODE_S", ""},
};

const Defaults& defaults_for(const std::string& type) {
    static const Defaults none{"", "", "", ""};
    for (const auto& d : kDefaults) if (type == d.type) return d;
    return none;
}

// Les codes touche de Bootcade sont ceux de DirectInput, des positions comme
// les KEYCODE de MAME : la table ne traduit que des noms.
const char* mame_key(int code) {
    static const struct { int code; const char* name; } keys[] = {
        {0x01, "ESC"}, {0x02, "1"}, {0x03, "2"}, {0x04, "3"}, {0x05, "4"}, {0x06, "5"},
        {0x07, "6"}, {0x08, "7"}, {0x09, "8"}, {0x0A, "9"}, {0x0B, "0"},
        {0x0C, "MINUS"}, {0x0D, "EQUALS"}, {0x0E, "BACKSPACE"}, {0x0F, "TAB"},
        {0x10, "Q"}, {0x11, "W"}, {0x12, "E"}, {0x13, "R"}, {0x14, "T"}, {0x15, "Y"},
        {0x16, "U"}, {0x17, "I"}, {0x18, "O"}, {0x19, "P"},
        {0x1A, "OPENBRACE"}, {0x1B, "CLOSEBRACE"}, {0x1C, "ENTER"}, {0x1D, "LCONTROL"},
        {0x1E, "A"}, {0x1F, "S"}, {0x20, "D"}, {0x21, "F"}, {0x22, "G"}, {0x23, "H"},
        {0x24, "J"}, {0x25, "K"}, {0x26, "L"}, {0x27, "COLON"}, {0x28, "QUOTE"},
        {0x29, "TILDE"}, {0x2A, "LSHIFT"}, {0x2B, "BACKSLASH"},
        {0x2C, "Z"}, {0x2D, "X"}, {0x2E, "C"}, {0x2F, "V"}, {0x30, "B"}, {0x31, "N"},
        {0x32, "M"}, {0x33, "COMMA"}, {0x34, "STOP"}, {0x35, "SLASH"}, {0x36, "RSHIFT"},
        {0x37, "ASTERISK"}, {0x38, "LALT"}, {0x39, "SPACE"}, {0x3A, "CAPSLOCK"},
        {0x3B, "F1"}, {0x3C, "F2"}, {0x3D, "F3"}, {0x3E, "F4"}, {0x3F, "F5"},
        {0x40, "F6"}, {0x41, "F7"}, {0x42, "F8"}, {0x43, "F9"}, {0x44, "F10"},
        {0x45, "NUMLOCK"}, {0x46, "SCRLOCK"},
        {0x47, "7_PAD"}, {0x48, "8_PAD"}, {0x49, "9_PAD"}, {0x4A, "MINUS_PAD"},
        {0x4B, "4_PAD"}, {0x4C, "5_PAD"}, {0x4D, "6_PAD"}, {0x4E, "PLUS_PAD"},
        {0x4F, "1_PAD"}, {0x50, "2_PAD"}, {0x51, "3_PAD"}, {0x52, "0_PAD"},
        {0x53, "DEL_PAD"}, {0x56, "BACKSLASH2"}, {0x57, "F11"}, {0x58, "F12"},
        {0x9C, "ENTER_PAD"}, {0x9D, "RCONTROL"}, {0xB5, "SLASH_PAD"}, {0xB8, "RALT"},
        {0xC5, "PAUSE"}, {0xC7, "HOME"}, {0xC8, "UP"}, {0xC9, "PGUP"}, {0xCB, "LEFT"},
        {0xCD, "RIGHT"}, {0xCF, "END"}, {0xD0, "DOWN"}, {0xD1, "PGDN"},
        {0xD2, "INSERT"}, {0xD3, "DEL"}, {0xDB, "LWIN"}, {0xDC, "RWIN"}, {0xDD, "MENU"},
    };
    for (const auto& k : keys) if (k.code == code) return k.name;
    return nullptr;
}

std::string key_code(int code) {
    const char* n = mame_key(code);
    return n ? std::string("KEYCODE_") + n : std::string();
}

/* Une manette telle que MAME la verra.
 *
 * Sous -joystickprovider sdljoy, SDL numerote les boutons exactement comme
 * /dev/input/js* : meme parcours des codes BTN_*. Les axes, non : SDL sort
 * les chapeaux (ABS_HAT0X a ABS_HAT3Y) de la liste des axes pour en faire des
 * « hats ». La table des axes de joydev (JSIOCGAXMAP) dit, pour chaque numero
 * d'axe de Bootcade, de quel code ABS il s'agit, donc ou il tombe chez SDL. */
struct Pad {
    int joy = 0;                  // JOYCODE_<joy>
    std::vector<int> axmap;       // numero joydev -> code ABS
    std::string id;               // fragment de l'identifiant SDL, ou vide
};

constexpr int kAbsHat0X = 0x10, kAbsHat3Y = 0x17;

// Le chemin de la manette du joueur. Les numeros jsN changent quand on
// branche les manettes dans un autre ordre ; le nom, lui, suit la manette.
std::string resolve_path(const PlayerConfig& player) {
    if (player.device_name.empty()) return player.device_path;
    const auto devices = ControllerManager::list_devices();
    for (const auto& d : devices)
        if (d.path == player.device_path && d.name == player.device_name) return d.path;
    for (const auto& d : devices)
        if (d.name == player.device_name) return d.path;
    return player.device_path;
}

std::string read_first_line(const std::string& path) {
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    return s;
}

/* Le morceau de l'identifiant SDL qui ne depend que du materiel.
 *
 * MAME nomme une manette par son GUID SDL (« 0300605b1008000001000000100100
 * 00 ») et <mapdevice> y cherche une SOUS-CHAINE. Le GUID porte, en petit-
 * boutiste, le bus, une empreinte du nom, puis vendeur, produit et version
 * separes de zeros : ces trois-la se lisent dans /sys, on n'a pas besoin de
 * refaire l'empreinte. */
std::string sdl_id_fragment(const std::string& js_path) {
    const auto slash = js_path.rfind('/');
    const std::string node = js_path.substr(slash == std::string::npos ? 0 : slash + 1);
    const std::string base = "/sys/class/input/" + node + "/device/id/";
    auto hex = [&](const char* f) -> long {
        const std::string s = read_first_line(base + f);
        if (s.empty()) return -1;
        try { return std::stol(s, nullptr, 16); } catch (...) { return -1; }
    };
    const long vendor = hex("vendor"), product = hex("product"), version = hex("version");
    // Sans vendeur ni produit, SDL met le NOM dans le GUID : rien de fiable.
    if (vendor <= 0 || product <= 0 || version < 0) return "";
    auto le16 = [](long v) {
        char b[8];
        std::snprintf(b, sizeof(b), "%02x%02x", (unsigned)(v & 0xFF), (unsigned)((v >> 8) & 0xFF));
        return std::string(b);
    };
    return le16(vendor) + "0000" + le16(product) + "0000" + le16(version);
}

Pad open_pad(const PlayerConfig& player, int joy) {
    Pad pad;
    pad.joy = joy;
    const std::string path = resolve_path(player);
    if (path.empty()) return pad;
    pad.id = sdl_id_fragment(path);
    const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) return pad;
    uint8_t count = 0;
    uint8_t map[ABS_CNT] = {};
    if (ioctl(fd, JSIOCGAXES, &count) >= 0 && ioctl(fd, JSIOCGAXMAP, map) >= 0)
        pad.axmap.assign(map, map + count);
    close(fd);
    return pad;
}

// Numero d'axe chez SDL, ou -1 pour un chapeau ou un axe inconnu.
int sdl_axis(const Pad& pad, int axis) {
    if (axis < 0 || axis >= (int)pad.axmap.size()) return -1;
    const int code = pad.axmap[axis];
    if (code >= kAbsHat0X && code <= kAbsHat3Y) return -1;
    int n = 0;
    for (int i = 0; i < axis; ++i)
        if (pad.axmap[i] < kAbsHat0X || pad.axmap[i] > kAbsHat3Y) ++n;
    return n;
}

const char* axis_item(int sdl_index) {
    static const char* const names[] = {"XAXIS", "YAXIS", "ZAXIS", "RXAXIS",
                                        "RYAXIS", "RZAXIS", "SLIDER1", "SLIDER2"};
    return sdl_index >= 0 && sdl_index < 8 ? names[sdl_index] : nullptr;
}

std::string joy(const Pad& pad) { return "JOYCODE_" + std::to_string(pad.joy) + "_"; }

// Code MAME d'une liaison numerique, vide si elle n'a pas d'equivalent.
std::string digital_code(const InputBinding& b, const Pad* pad) {
    if (!b.valid) return "";
    if (b.source == InputSource::KEY) return key_code(b.key);
    if (!pad) return "";
    if (!b.is_axis) {
        if (b.button < 0 || b.button >= 32) return "";
        return joy(*pad) + "BUTTON" + std::to_string(b.button + 1);
    }
    if (b.axis < 0 || b.axis >= (int)pad->axmap.size()) return "";
    const int code = pad->axmap[b.axis];
    if (code >= kAbsHat0X && code <= kAbsHat3Y) {
        const int hat = (code - kAbsHat0X) / 2 + 1;
        const bool x = ((code - kAbsHat0X) % 2) == 0;
        const char* dir = x ? (b.axis_dir < 0 ? "LEFT" : "RIGHT")
                            : (b.axis_dir < 0 ? "UP" : "DOWN");
        return joy(*pad) + "HAT" + std::to_string(hat) + dir;
    }
    const int idx = sdl_axis(*pad, b.axis);
    const char* item = axis_item(idx);
    if (!item) return "";
    const char* half = idx == 0 ? (b.axis_dir < 0 ? "LEFT" : "RIGHT")
                     : idx == 1 ? (b.axis_dir < 0 ? "UP" : "DOWN")
                                : (b.axis_dir < 0 ? "NEG" : "POS");
    return joy(*pad) + item + "_" + half + "_SWITCH";
}

// « a OR b OR c », sans doublon et sans element vide.
std::string join_or(const std::vector<std::string>& parts) {
    std::vector<std::string> seen;
    std::string out;
    for (const auto& p : parts) {
        if (p.empty()) continue;
        bool dup = false;
        for (const auto& s : seen) dup = dup || s == p;
        if (dup) continue;
        seen.push_back(p);
        if (!out.empty()) out += " OR ";
        out += p;
    }
    return out;
}

std::vector<std::string> split_or(const std::string& seq) {
    std::vector<std::string> out;
    std::string::size_type start = 0;
    while (start < seq.size()) {
        auto end = seq.find(" OR ", start);
        out.push_back(seq.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 4;
    }
    return out;
}

std::string xml_seq(const char* type, const std::string& seq) {
    // Une sequence vide ne s'ecrit pas « » : MAME l'ignorerait et garderait
    // son defaut, codes manette compris. NONE la vide pour de bon.
    return std::string("                <newseq type=\"") + type + "\">"
         + (seq.empty() ? std::string("NONE") : seq) + "</newseq>\n";
}

void write_port(std::ostringstream& out, const std::string& type,
                const std::string& standard, const std::string* increment = nullptr,
                const std::string* decrement = nullptr) {
    out << "            <port type=\"" << type << "\">\n";
    out << xml_seq("standard", standard);
    if (increment) out << xml_seq("increment", *increment);
    if (decrement) out << xml_seq("decrement", *decrement);
    out << "            </port>\n";
}

// Les commandes de MAME que couvre chaque role analogique de Bootcade.
std::vector<const char*> analog_ports(AnalogRole role) {
    switch (role) {
        case AnalogRole::STEERING: return {"PADDLE", "DIAL", "POSITIONAL"};
        case AnalogRole::THROTTLE: return {"PEDAL"};
        case AnalogRole::BRAKE:    return {"PEDAL2"};
        case AnalogRole::AIM_X:    return {"AD_STICK_X", "LIGHTGUN_X", "TRACKBALL_X"};
        case AnalogRole::AIM_Y:    return {"AD_STICK_Y", "LIGHTGUN_Y", "TRACKBALL_Y",
                                           "PADDLE_V", "DIAL_V"};
        default:                   return {};
    }
}

bool player_configured(const PlayerConfig& p) {
    return !p.device_path.empty() || !p.bindings.empty();
}

} // namespace

std::string ctrlr_dir() {
    return AppContext::get_user_config_dir() + "/mame/ctrlr";
}

bool uses_pad(const ControllerConfig& cfg) {
    for (const auto& p : cfg.players)
        if (!p.device_path.empty()) return true;
    return false;
}

std::string ctrlr_xml(const ControllerConfig& cfg) {
    // Une manette par joueur, JOYCODE_1 pour le premier. Deux joueurs sur la
    // meme manette partagent son numero.
    Pad pads[2];
    bool has_pad[2] = {false, false};
    for (int p = 0; p < 2 && p < (int)cfg.players.size(); ++p) {
        const auto& player = cfg.players[p];
        if (player.device_path.empty()) continue;
        const bool shared = p == 1 && has_pad[0]
                         && player.device_path == cfg.players[0].device_path;
        pads[p] = open_pad(player, shared ? 1 : p + 1);
        has_pad[p] = true;
    }

    std::ostringstream out;
    out << "<?xml version=\"1.0\"?>\n"
        << "<!-- Written by Bootcade before each MAME launch; edits are overwritten. -->\n"
        << "<mameconfig version=\"10\">\n"
        << "    <system name=\"default\">\n"
        << "        <input>\n";

    /* Epingler chaque manette sur son numero.
     *
     * Sans cela MAME numerote dans l'ordre ou SDL trouve les peripheriques,
     * qui n'est pas celui des js*. Deux manettes identiques donnent le meme
     * fragment : MAME rangerait les deux sur la premiere trouvee, et l'on
     * s'en remet alors a l'ordre de SDL. */
    const bool same_model = has_pad[0] && has_pad[1] && pads[0].joy != pads[1].joy
                         && pads[0].id == pads[1].id;
    for (int p = 0; p < 2; ++p) {
        if (!has_pad[p] || pads[p].id.empty() || same_model) continue;
        if (p == 1 && pads[1].joy == 1) continue;           // manette partagee
        out << "            <mapdevice device=\"" << pads[p].id
            << "\" controller=\"JOYCODE_" << pads[p].joy << "\" />\n";
    }

    for (int p = 0; p < 2 && p < (int)cfg.players.size(); ++p) {
        const auto& player = cfg.players[p];
        if (!player_configured(player)) continue;             // MAME garde ses defauts
        const Pad* pad = has_pad[p] ? &pads[p] : nullptr;
        const std::string pn = "P" + std::to_string(p + 1) + "_";

        auto digital = [&](GameAction action, const std::string& type) {
            std::vector<std::string> parts;
            auto it = player.bindings.find(action);
            if (it != player.bindings.end()) parts.push_back(digital_code(it->second, pad));
            for (const auto& d : split_or(defaults_for(type).standard)) parts.push_back(d);
            write_port(out, type, join_or(parts));
        };
        digital(GameAction::UP,    pn + "JOYSTICK_UP");
        digital(GameAction::DOWN,  pn + "JOYSTICK_DOWN");
        digital(GameAction::LEFT,  pn + "JOYSTICK_LEFT");
        digital(GameAction::RIGHT, pn + "JOYSTICK_RIGHT");
        for (int b = 1; b <= MAX_GAME_BUTTONS; ++b)
            digital(button_action(b), pn + "BUTTON" + std::to_string(b));
        digital(GameAction::START, "START" + std::to_string(p + 1));
        digital(GameAction::COIN,  "COIN" + std::to_string(p + 1));

        /* Analogique : l'axe de la manette, ou deux touches.
         *
         * Le fichier controleur ne regle que les sequences ; MAME ignore
         * volontairement sensibilite, vitesse et recentrage a ce niveau (code
         * desactive dans ioport.cpp). « Inverser » passe donc par le
         * modificateur REVERSE, et vitesse et retour au centre du mode
         * relatif restent ceux de chaque jeu. */
        for (int r = 0; r < ANALOG_ROLE_COUNT; ++r) {
            const auto role = static_cast<AnalogRole>(r);
            auto it = player.analog.find(role);
            AnalogBinding b = it != player.analog.end() ? it->second
                            : (pad ? default_analog_binding(role) : AnalogBinding{});
            // Avec une manette on ecrit la commande meme sans axe utilisable :
            // les defauts de MAME y mettent des codes manette (la pedale sur
            // le bouton 1) qui entreraient en conflit avec les liaisons du
            // joueur. Sans manette, MAME garde les siens.
            if (!b.is_set() && !pad) continue;
            std::string axis, inc_key, dec_key;
            if (b.source == AnalogSource::KEY_PAIR) {
                inc_key = key_code(b.invert ? b.key_neg : b.key_pos);
                dec_key = key_code(b.invert ? b.key_pos : b.key_neg);
            } else if (b.source == AnalogSource::JOY_AXIS && pad) {
                // Un axe entier, pas une moitie : une gachette va de -32767
                // relachee a +32767 enfoncee, et MAME ramene lui-meme le bas
                // de course d'une pedale a zero.
                if (const char* item = axis_item(sdl_axis(*pad, b.index)))
                    axis = joy(*pad) + item + (b.invert ? "_REVERSE" : "");
            }
            if (!pad && axis.empty() && inc_key.empty() && dec_key.empty()) continue;
            for (const char* port : analog_ports(role)) {
                const std::string type = pn + port;
                const Defaults& d = defaults_for(type);
                std::vector<std::string> std_parts{axis}, inc{inc_key}, dec{dec_key};
                for (const auto& x : split_or(d.standard))  std_parts.push_back(x);
                for (const auto& x : split_or(d.increment)) inc.push_back(x);
                for (const auto& x : split_or(d.decrement)) dec.push_back(x);
                const std::string si = join_or(inc), sd = join_or(dec);
                write_port(out, type, join_or(std_parts), &si, &sd);
            }
        }
    }

    out << "        </input>\n"
        << "    </system>\n"
        << "</mameconfig>\n";
    return out.str();
}

bool write_ctrlr(const ControllerConfig& cfg, const std::string& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string path = dir + "/" + CTRLR_NAME + ".cfg";
    const std::string tmp  = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) {
            std::cerr << "[MameControls] Cannot write " << tmp << "\n";
            return false;
        }
        f << ctrlr_xml(cfg);
        if (!f.flush()) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::cerr << "[MameControls] Cannot write " << path << ": " << ec.message() << "\n";
        std::filesystem::remove(tmp, ec);
        return false;
    }
    std::cout << "[MameControls] Wrote " << path << "\n";
    return true;
}

namespace {

bool read_all(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// Debut et fin (exclue) du bloc <input> d'un .cfg, ligne entiere comprise.
bool find_input(const std::string& xml, size_t& begin, size_t& end) {
    const size_t open = xml.find("<input");
    if (open == std::string::npos) return false;
    const size_t tag_end = xml.find('>', open);
    if (tag_end == std::string::npos) return false;
    if (xml[tag_end - 1] == '/') {
        end = tag_end + 1;
    } else {
        const size_t close = xml.find("</input>", tag_end);
        if (close == std::string::npos) return false;
        end = close + 8;
    }
    begin = open;
    // Emporter l'indentation et le saut de ligne, pour ne pas laisser une
    // ligne blanche a la place du bloc.
    while (begin > 0 && (xml[begin - 1] == ' ' || xml[begin - 1] == '\t')) --begin;
    if (end < xml.size() && xml[end] == '\n') ++end;
    return true;
}

} // namespace

int input_overrides(const std::string& cfg_file) {
    std::string xml;
    if (!read_all(cfg_file, xml)) return -1;
    size_t b = 0, e = 0;
    if (!find_input(xml, b, e)) return 0;
    // Seules les commandes redefinies comptent : un <port> sans <newseq>
    // porte la valeur d'un DIP, pas une touche.
    int n = 0;
    for (size_t at = xml.find("<newseq", b); at != std::string::npos && at < e;
         at = xml.find("<newseq", at + 1))
        ++n;
    return n;
}

bool reset_input(const std::string& cfg_file) {
    std::string xml;
    if (!read_all(cfg_file, xml)) return false;
    size_t b = 0, e = 0;
    if (!find_input(xml, b, e)) return true;                 // rien a retirer
    std::error_code ec;
    std::filesystem::copy_file(cfg_file, cfg_file + ".bak",
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "[MameControls] Cannot back up " << cfg_file << ": " << ec.message() << "\n";
        return false;
    }
    const std::string tmp = cfg_file + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << xml.substr(0, b) << xml.substr(e);
        if (!f.flush()) return false;
    }
    std::filesystem::rename(tmp, cfg_file, ec);
    if (ec) { std::filesystem::remove(tmp, ec); return false; }
    return true;
}

} // namespace MameControls
