// src/MameControls.cpp
#include "MameControls.h"
#include "ControllerManager.h"
#include "AppContext.h"
#include "Platform.h"
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <vector>

namespace MameControls {

namespace {

/* Ce que MAME met par defaut sur chaque commande, SANS ses codes manette.
 *
 * Tire du source de MAME 0.289 (src/emu/inpttype.ipp) par un script, pas
 * recopie a la main : joueurs 1 a 4, pieces et departs 1 a 4, les deux
 * sticks des jeux a deux manches et toutes les commandes analogiques. Les
 * lignes du joueur 1 ont ete comparees a un releve fait dans MAME par script
 * Lua (ioport:type_seq) : identiques.
 *
 * On garde ces touches a cote de la liaison du joueur : le clavier doit
 * continuer de marcher. La souris et le pistolet de CHAQUE joueur restent
 * aussi (MOUSECODE_2, GUNCODE_2... pour le joueur 2) : les retirer laissait
 * sans viseur le deuxieme pistolet. Les codes JOYCODE sont retires, sinon le
 * bouton 1 de la manette declencherait encore P1_BUTTON1 apres avoir ete
 * deplace ailleurs ; MAME 0.289 n'en met d'ailleurs que sur Start et Select.
 *
 * Colonnes : type, sequence standard, increment, decrement. */
struct Defaults { const char* type; const char* standard; const char* increment; const char* decrement; };

const Defaults kDefaults[] = {
    {"COIN1", "KEYCODE_5", "", ""},
    {"COIN2", "KEYCODE_6", "", ""},
    {"COIN3", "KEYCODE_7", "", ""},
    {"COIN4", "KEYCODE_8", "", ""},
    {"START1", "KEYCODE_1", "", ""},
    {"START2", "KEYCODE_2", "", ""},
    {"START3", "KEYCODE_3", "", ""},
    {"START4", "KEYCODE_4", "", ""},
    {"P1_JOYSTICK_UP", "KEYCODE_UP", "", ""},
    {"P1_JOYSTICK_DOWN", "KEYCODE_DOWN", "", ""},
    {"P1_JOYSTICK_LEFT", "KEYCODE_LEFT", "", ""},
    {"P1_JOYSTICK_RIGHT", "KEYCODE_RIGHT", "", ""},
    {"P1_JOYSTICKLEFT_UP", "KEYCODE_E", "", ""},
    {"P1_JOYSTICKLEFT_DOWN", "KEYCODE_D", "", ""},
    {"P1_JOYSTICKLEFT_LEFT", "KEYCODE_S", "", ""},
    {"P1_JOYSTICKLEFT_RIGHT", "KEYCODE_F", "", ""},
    {"P1_JOYSTICKRIGHT_UP", "KEYCODE_I", "", ""},
    {"P1_JOYSTICKRIGHT_DOWN", "KEYCODE_K", "", ""},
    {"P1_JOYSTICKRIGHT_LEFT", "KEYCODE_J", "", ""},
    {"P1_JOYSTICKRIGHT_RIGHT", "KEYCODE_L", "", ""},
    {"P1_BUTTON1", "KEYCODE_LCONTROL OR MOUSECODE_1_BUTTON1 OR GUNCODE_1_BUTTON1", "", ""},
    {"P1_BUTTON2", "KEYCODE_LALT OR MOUSECODE_1_BUTTON3 OR GUNCODE_1_BUTTON2", "", ""},
    {"P1_BUTTON3", "KEYCODE_SPACE OR MOUSECODE_1_BUTTON2", "", ""},
    {"P1_BUTTON4", "KEYCODE_LSHIFT", "", ""},
    {"P1_BUTTON5", "KEYCODE_Z", "", ""},
    {"P1_BUTTON6", "KEYCODE_X", "", ""},
    {"P1_BUTTON7", "KEYCODE_C", "", ""},
    {"P1_BUTTON8", "KEYCODE_V", "", ""},
    {"P1_BUTTON9", "KEYCODE_B", "", ""},
    {"P1_BUTTON10", "KEYCODE_N", "", ""},
    {"P1_BUTTON11", "KEYCODE_M", "", ""},
    {"P1_BUTTON12", "KEYCODE_COMMA", "", ""},
    {"P1_BUTTON13", "KEYCODE_STOP", "", ""},
    {"P1_BUTTON14", "KEYCODE_SLASH", "", ""},
    {"P1_BUTTON15", "KEYCODE_RSHIFT", "", ""},
    {"P1_BUTTON16", "", "", ""},
    {"P1_PADDLE", "MOUSECODE_1_XAXIS", "KEYCODE_RIGHT", "KEYCODE_LEFT"},
    {"P1_PADDLE_V", "MOUSECODE_1_YAXIS", "KEYCODE_DOWN", "KEYCODE_UP"},
    {"P1_DIAL", "MOUSECODE_1_XAXIS", "KEYCODE_RIGHT", "KEYCODE_LEFT"},
    {"P1_DIAL_V", "MOUSECODE_1_YAXIS", "KEYCODE_DOWN", "KEYCODE_UP"},
    {"P1_POSITIONAL", "MOUSECODE_1_XAXIS", "KEYCODE_RIGHT", "KEYCODE_LEFT"},
    {"P1_POSITIONAL_V", "MOUSECODE_1_YAXIS", "KEYCODE_DOWN", "KEYCODE_UP"},
    {"P1_AD_STICK_X", "MOUSECODE_1_XAXIS", "KEYCODE_RIGHT", "KEYCODE_LEFT"},
    {"P1_AD_STICK_Y", "MOUSECODE_1_YAXIS", "KEYCODE_DOWN", "KEYCODE_UP"},
    {"P1_AD_STICK_Z", "", "KEYCODE_Z", "KEYCODE_A"},
    {"P1_TRACKBALL_X", "MOUSECODE_1_XAXIS", "KEYCODE_RIGHT", "KEYCODE_LEFT"},
    {"P1_TRACKBALL_Y", "MOUSECODE_1_YAXIS", "KEYCODE_DOWN", "KEYCODE_UP"},
    {"P1_LIGHTGUN_X", "GUNCODE_1_XAXIS OR MOUSECODE_1_XAXIS", "KEYCODE_RIGHT", "KEYCODE_LEFT"},
    {"P1_LIGHTGUN_Y", "GUNCODE_1_YAXIS OR MOUSECODE_1_YAXIS", "KEYCODE_DOWN", "KEYCODE_UP"},
    {"P1_PEDAL", "", "KEYCODE_LCONTROL", ""},
    {"P1_PEDAL2", "", "KEYCODE_LALT", ""},
    {"P1_PEDAL3", "", "KEYCODE_SPACE", ""},
    {"P2_JOYSTICK_UP", "KEYCODE_R", "", ""},
    {"P2_JOYSTICK_DOWN", "KEYCODE_F", "", ""},
    {"P2_JOYSTICK_LEFT", "KEYCODE_D", "", ""},
    {"P2_JOYSTICK_RIGHT", "KEYCODE_G", "", ""},
    {"P2_JOYSTICKLEFT_UP", "", "", ""},
    {"P2_JOYSTICKLEFT_DOWN", "", "", ""},
    {"P2_JOYSTICKLEFT_LEFT", "", "", ""},
    {"P2_JOYSTICKLEFT_RIGHT", "", "", ""},
    {"P2_JOYSTICKRIGHT_UP", "", "", ""},
    {"P2_JOYSTICKRIGHT_DOWN", "", "", ""},
    {"P2_JOYSTICKRIGHT_LEFT", "", "", ""},
    {"P2_JOYSTICKRIGHT_RIGHT", "", "", ""},
    {"P2_BUTTON1", "KEYCODE_A OR MOUSECODE_2_BUTTON1 OR GUNCODE_2_BUTTON1", "", ""},
    {"P2_BUTTON2", "KEYCODE_S OR MOUSECODE_2_BUTTON3 OR GUNCODE_2_BUTTON2", "", ""},
    {"P2_BUTTON3", "KEYCODE_Q OR MOUSECODE_2_BUTTON2", "", ""},
    {"P2_BUTTON4", "KEYCODE_W", "", ""},
    {"P2_BUTTON5", "KEYCODE_E", "", ""},
    {"P2_BUTTON6", "", "", ""},
    {"P2_BUTTON7", "", "", ""},
    {"P2_BUTTON8", "", "", ""},
    {"P2_BUTTON9", "", "", ""},
    {"P2_BUTTON10", "", "", ""},
    {"P2_BUTTON11", "", "", ""},
    {"P2_BUTTON12", "", "", ""},
    {"P2_BUTTON13", "", "", ""},
    {"P2_BUTTON14", "", "", ""},
    {"P2_BUTTON15", "", "", ""},
    {"P2_BUTTON16", "", "", ""},
    {"P2_PADDLE", "MOUSECODE_2_XAXIS", "KEYCODE_G", "KEYCODE_D"},
    {"P2_PADDLE_V", "MOUSECODE_2_YAXIS", "KEYCODE_F", "KEYCODE_R"},
    {"P2_DIAL", "MOUSECODE_2_XAXIS", "KEYCODE_G", "KEYCODE_D"},
    {"P2_DIAL_V", "MOUSECODE_2_YAXIS", "KEYCODE_F", "KEYCODE_R"},
    {"P2_POSITIONAL", "MOUSECODE_2_XAXIS", "KEYCODE_G", "KEYCODE_D"},
    {"P2_POSITIONAL_V", "MOUSECODE_2_YAXIS", "KEYCODE_F", "KEYCODE_R"},
    {"P2_AD_STICK_X", "MOUSECODE_2_XAXIS", "KEYCODE_G", "KEYCODE_D"},
    {"P2_AD_STICK_Y", "MOUSECODE_2_YAXIS", "KEYCODE_F", "KEYCODE_R"},
    {"P2_AD_STICK_Z", "", "", ""},
    {"P2_TRACKBALL_X", "MOUSECODE_2_XAXIS", "KEYCODE_G", "KEYCODE_D"},
    {"P2_TRACKBALL_Y", "MOUSECODE_2_YAXIS", "KEYCODE_F", "KEYCODE_R"},
    {"P2_LIGHTGUN_X", "GUNCODE_2_XAXIS OR MOUSECODE_2_XAXIS", "KEYCODE_G", "KEYCODE_D"},
    {"P2_LIGHTGUN_Y", "GUNCODE_2_YAXIS OR MOUSECODE_2_YAXIS", "KEYCODE_F", "KEYCODE_R"},
    {"P2_PEDAL", "", "KEYCODE_A", ""},
    {"P2_PEDAL2", "", "KEYCODE_S", ""},
    {"P2_PEDAL3", "", "KEYCODE_Q", ""},
    {"P3_JOYSTICK_UP", "KEYCODE_I", "", ""},
    {"P3_JOYSTICK_DOWN", "KEYCODE_K", "", ""},
    {"P3_JOYSTICK_LEFT", "KEYCODE_J", "", ""},
    {"P3_JOYSTICK_RIGHT", "KEYCODE_L", "", ""},
    {"P3_JOYSTICKLEFT_UP", "", "", ""},
    {"P3_JOYSTICKLEFT_DOWN", "", "", ""},
    {"P3_JOYSTICKLEFT_LEFT", "", "", ""},
    {"P3_JOYSTICKLEFT_RIGHT", "", "", ""},
    {"P3_JOYSTICKRIGHT_UP", "", "", ""},
    {"P3_JOYSTICKRIGHT_DOWN", "", "", ""},
    {"P3_JOYSTICKRIGHT_LEFT", "", "", ""},
    {"P3_JOYSTICKRIGHT_RIGHT", "", "", ""},
    {"P3_BUTTON1", "KEYCODE_RCONTROL OR GUNCODE_3_BUTTON1", "", ""},
    {"P3_BUTTON2", "KEYCODE_RSHIFT OR GUNCODE_3_BUTTON2", "", ""},
    {"P3_BUTTON3", "KEYCODE_ENTER", "", ""},
    {"P3_BUTTON4", "", "", ""},
    {"P3_BUTTON5", "", "", ""},
    {"P3_BUTTON6", "", "", ""},
    {"P3_BUTTON7", "", "", ""},
    {"P3_BUTTON8", "", "", ""},
    {"P3_BUTTON9", "", "", ""},
    {"P3_BUTTON10", "", "", ""},
    {"P3_BUTTON11", "", "", ""},
    {"P3_BUTTON12", "", "", ""},
    {"P3_BUTTON13", "", "", ""},
    {"P3_BUTTON14", "", "", ""},
    {"P3_BUTTON15", "", "", ""},
    {"P3_BUTTON16", "", "", ""},
    {"P3_PADDLE", "MOUSECODE_3_XAXIS", "KEYCODE_L", "KEYCODE_J"},
    {"P3_PADDLE_V", "MOUSECODE_3_YAXIS", "KEYCODE_K", "KEYCODE_I"},
    {"P3_DIAL", "MOUSECODE_3_XAXIS", "KEYCODE_L", "KEYCODE_J"},
    {"P3_DIAL_V", "MOUSECODE_3_YAXIS", "KEYCODE_K", "KEYCODE_I"},
    {"P3_POSITIONAL", "MOUSECODE_3_XAXIS", "KEYCODE_L", "KEYCODE_J"},
    {"P3_POSITIONAL_V", "MOUSECODE_3_YAXIS", "KEYCODE_K", "KEYCODE_I"},
    {"P3_AD_STICK_X", "MOUSECODE_3_XAXIS", "KEYCODE_L", "KEYCODE_J"},
    {"P3_AD_STICK_Y", "MOUSECODE_3_YAXIS", "KEYCODE_K", "KEYCODE_I"},
    {"P3_AD_STICK_Z", "", "", ""},
    {"P3_TRACKBALL_X", "MOUSECODE_3_XAXIS", "KEYCODE_L", "KEYCODE_J"},
    {"P3_TRACKBALL_Y", "MOUSECODE_3_YAXIS", "KEYCODE_K", "KEYCODE_I"},
    {"P3_LIGHTGUN_X", "GUNCODE_3_XAXIS OR MOUSECODE_3_XAXIS", "KEYCODE_L", "KEYCODE_J"},
    {"P3_LIGHTGUN_Y", "GUNCODE_3_YAXIS OR MOUSECODE_3_YAXIS", "KEYCODE_K", "KEYCODE_I"},
    {"P3_PEDAL", "", "KEYCODE_RCONTROL", ""},
    {"P3_PEDAL2", "", "KEYCODE_RSHIFT", ""},
    {"P3_PEDAL3", "", "KEYCODE_ENTER", ""},
    {"P4_JOYSTICK_UP", "KEYCODE_8_PAD", "", ""},
    {"P4_JOYSTICK_DOWN", "KEYCODE_2_PAD", "", ""},
    {"P4_JOYSTICK_LEFT", "KEYCODE_4_PAD", "", ""},
    {"P4_JOYSTICK_RIGHT", "KEYCODE_6_PAD", "", ""},
    {"P4_JOYSTICKLEFT_UP", "", "", ""},
    {"P4_JOYSTICKLEFT_DOWN", "", "", ""},
    {"P4_JOYSTICKLEFT_LEFT", "", "", ""},
    {"P4_JOYSTICKLEFT_RIGHT", "", "", ""},
    {"P4_JOYSTICKRIGHT_UP", "", "", ""},
    {"P4_JOYSTICKRIGHT_DOWN", "", "", ""},
    {"P4_JOYSTICKRIGHT_LEFT", "", "", ""},
    {"P4_JOYSTICKRIGHT_RIGHT", "", "", ""},
    {"P4_BUTTON1", "KEYCODE_0_PAD", "", ""},
    {"P4_BUTTON2", "KEYCODE_DEL_PAD", "", ""},
    {"P4_BUTTON3", "KEYCODE_ENTER_PAD", "", ""},
    {"P4_BUTTON4", "", "", ""},
    {"P4_BUTTON5", "", "", ""},
    {"P4_BUTTON6", "", "", ""},
    {"P4_BUTTON7", "", "", ""},
    {"P4_BUTTON8", "", "", ""},
    {"P4_BUTTON9", "", "", ""},
    {"P4_BUTTON10", "", "", ""},
    {"P4_BUTTON11", "", "", ""},
    {"P4_BUTTON12", "", "", ""},
    {"P4_BUTTON13", "", "", ""},
    {"P4_BUTTON14", "", "", ""},
    {"P4_BUTTON15", "", "", ""},
    {"P4_BUTTON16", "", "", ""},
    {"P4_PADDLE", "MOUSECODE_4_XAXIS", "KEYCODE_6_PAD", "KEYCODE_4_PAD"},
    {"P4_PADDLE_V", "MOUSECODE_4_YAXIS", "KEYCODE_2_PAD", "KEYCODE_8_PAD"},
    {"P4_DIAL", "MOUSECODE_4_XAXIS", "KEYCODE_6_PAD", "KEYCODE_4_PAD"},
    {"P4_DIAL_V", "MOUSECODE_4_YAXIS", "KEYCODE_2_PAD", "KEYCODE_8_PAD"},
    {"P4_POSITIONAL", "MOUSECODE_4_XAXIS", "KEYCODE_6_PAD", "KEYCODE_4_PAD"},
    {"P4_POSITIONAL_V", "MOUSECODE_4_YAXIS", "KEYCODE_2_PAD", "KEYCODE_8_PAD"},
    {"P4_AD_STICK_X", "MOUSECODE_4_XAXIS", "KEYCODE_6_PAD", "KEYCODE_4_PAD"},
    {"P4_AD_STICK_Y", "MOUSECODE_4_YAXIS", "KEYCODE_2_PAD", "KEYCODE_8_PAD"},
    {"P4_AD_STICK_Z", "", "", ""},
    {"P4_TRACKBALL_X", "MOUSECODE_4_XAXIS", "KEYCODE_6_PAD", "KEYCODE_4_PAD"},
    {"P4_TRACKBALL_Y", "MOUSECODE_4_YAXIS", "KEYCODE_2_PAD", "KEYCODE_8_PAD"},
    {"P4_LIGHTGUN_X", "GUNCODE_4_XAXIS OR MOUSECODE_4_XAXIS", "KEYCODE_6_PAD", "KEYCODE_4_PAD"},
    {"P4_LIGHTGUN_Y", "GUNCODE_4_YAXIS OR MOUSECODE_4_YAXIS", "KEYCODE_2_PAD", "KEYCODE_8_PAD"},
    {"P4_PEDAL", "", "KEYCODE_0_PAD", ""},
    {"P4_PEDAL2", "", "KEYCODE_DEL_PAD", ""},
    {"P4_PEDAL3", "", "KEYCODE_ENTER_PAD", ""},
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
 * Sous Linux, MAME est lance avec -joystickprovider sdljoy : SDL numerote les
 * boutons exactement comme /dev/input/js*. Les axes, non : SDL sort les
 * chapeaux (ABS_HAT0X a ABS_HAT3Y) de la liste des axes pour en faire des
 * « hats », et nomme les axes restants dans l'ordre (XAXIS, YAXIS, ZAXIS...).
 *
 * Sous Windows, la version officielle de MAME n'a pas les pilotes SDL : on
 * passe par DirectInput (-joystickprovider dinput), qui nomme chaque axe par
 * ce qu'il est (X, Y, Z, Rx, Ry, Rz) et le premier chapeau HAT1. Les boutons
 * gardent l'ordre de winmm, que Bootcade lit.
 *
 * Dans les deux cas, ControllerManager::axis_codes dit pour chaque numero
 * d'axe de Bootcade de quel code ABS il s'agit. */
struct Pad {
    int joy = 0;                  // JOYCODE_<joy>
    std::vector<int> codes;       // numero d'axe Bootcade -> code ABS, -1 inconnu
    std::string id;               // fragment de l'identifiant SDL, ou vide
};

constexpr int kAbsX = 0x00, kAbsY = 0x01;              // ABS_X, ABS_Y
constexpr int kAbsHat0X = 0x10, kAbsHat3Y = 0x17;

bool is_hat(int code) { return code >= kAbsHat0X && code <= kAbsHat3Y; }

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
 * refaire l'empreinte. Vide sous Windows : pas de /sys, et DirectInput ne
 * nomme pas les manettes ainsi. */
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
#ifndef _WIN32
    pad.id = sdl_id_fragment(path);
#endif
    pad.codes = ControllerManager::axis_codes(path);
    return pad;
}

/* Le nom MAME d'un axe de Bootcade, ou nullptr pour un chapeau ou un axe
 * inconnu. */
const char* axis_name(const Pad& pad, int axis) {
    if (axis < 0 || axis >= (int)pad.codes.size()) return nullptr;
    const int code = pad.codes[axis];
    if (code < 0 || is_hat(code)) return nullptr;
#ifdef _WIN32
    // DirectInput : le nom suit l'axe lui-meme.
    static const char* const di[6] = {"XAXIS", "YAXIS", "ZAXIS", "RXAXIS", "RYAXIS", "RZAXIS"};
    return code < 6 ? di[code] : nullptr;
#else
    // SDL : les axes restants, dans l'ordre, chapeaux exclus.
    static const char* const names[] = {"XAXIS", "YAXIS", "ZAXIS", "RXAXIS",
                                        "RYAXIS", "RZAXIS", "SLIDER1", "SLIDER2"};
    int n = 0;
    for (int i = 0; i < axis; ++i)
        if (pad.codes[i] < 0 || !is_hat(pad.codes[i])) ++n;
    return n < 8 ? names[n] : nullptr;
#endif
}

std::string joy(const Pad& pad) { return "JOYCODE_" + std::to_string(pad.joy) + "_"; }

// « XAXIS_LEFT_SWITCH » : la moitie d'un axe vue comme un bouton. MAME nomme
// les moities d'apres l'axe : gauche/droite pour X, haut/bas pour Y, sinon
// negatif/positif.
std::string axis_switch(const Pad& pad, const char* item, int dir) {
    if (!item) return "";
    const std::string it = item;
    const char* half = it == "XAXIS" ? (dir < 0 ? "LEFT" : "RIGHT")
                     : it == "YAXIS" ? (dir < 0 ? "UP" : "DOWN")
                                     : (dir < 0 ? "NEG" : "POS");
    return joy(pad) + it + "_" + half + "_SWITCH";
}

/* Le stick gauche de la manette, pour une direction.
 *
 * MAME 0.289 ne met AUCUN code manette sur les directions : seul le pilote
 * sdlgame en ajoute, et Bootcade lance MAME avec un autre pilote. Le stick
 * gauche, c'est ABS_X / ABS_Y sur toutes les manettes : on le cherche par son
 * code, pas par sa place. Ajoute a cote de la liaison du joueur, la croix et
 * le stick marchent tous les deux. */
std::string stick_switch(const Pad* pad, GameAction action) {
    if (!pad || !is_direction_action(action)) return "";
    const bool x = action == GameAction::LEFT || action == GameAction::RIGHT;
    const int dir = (action == GameAction::LEFT || action == GameAction::UP) ? -1 : 1;
    const int code = x ? kAbsX : kAbsY;
    for (int i = 0; i < (int)pad->codes.size(); ++i)
        if (pad->codes[i] == code) return axis_switch(*pad, axis_name(*pad, i), dir);
    return "";
}

// Code MAME d'une liaison numerique, vide si elle n'a pas d'equivalent.
/* Un bouton de souris pour MAME. Une seule souris (multimouse 0 les reunit
 * toutes en MOUSECODE_1). Les numeros suivent le pilote : sous Linux, SDL
 * range gauche, milieu, droit (le clic droit est BUTTON3, d'ou les defauts de
 * MAME « P1_BUTTON2 = MOUSECODE_1_BUTTON3 ») ; sous Windows, rawinput et
 * DirectInput rangent gauche, droit, milieu. */
std::string mouse_button_code(int button) {
#ifdef _WIN32
    static const char* const kItem[3] = {"BUTTON1", "BUTTON2", "BUTTON3"};
#else
    static const char* const kItem[3] = {"BUTTON1", "BUTTON3", "BUTTON2"};
#endif
    if (button < 0 || button > 2) return "";
    return std::string("MOUSECODE_1_") + kItem[button];
}

std::string digital_code(const InputBinding& b, const Pad* pad) {
    if (!b.valid) return "";
    if (b.source == InputSource::KEY) return key_code(b.key);
    if (b.source == InputSource::MOUSE) return mouse_button_code(b.button);
    if (!pad) return "";
    if (!b.is_axis) {
        if (b.button < 0 || b.button >= 32) return "";
        return joy(*pad) + "BUTTON" + std::to_string(b.button + 1);
    }
    if (b.axis < 0 || b.axis >= (int)pad->codes.size()) return "";
    const int code = pad->codes[b.axis];
    if (is_hat(code)) {
        const int hat = (code - kAbsHat0X) / 2 + 1;
        const bool x = ((code - kAbsHat0X) % 2) == 0;
        const char* dir = x ? (b.axis_dir < 0 ? "LEFT" : "RIGHT")
                            : (b.axis_dir < 0 ? "UP" : "DOWN");
        return joy(*pad) + "HAT" + std::to_string(hat) + dir;
    }
    return axis_switch(*pad, axis_name(*pad, b.axis), b.axis_dir);
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
                                           "PADDLE_V", "DIAL_V", "POSITIONAL_V"};
        case AnalogRole::LEVER:    return {"AD_STICK_Z"};
        case AnalogRole::PEDAL3:   return {"PEDAL3"};
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

bool uses_mouse(const ControllerConfig& cfg) {
    for (const auto& p : cfg.players)
        if (p.uses_mouse()) return true;
    return false;
}

const char* joystick_provider() {
#ifdef _WIN32
    return "dinput";
#else
    return "sdljoy";
#endif
}

std::string ctrlr_xml(const ControllerConfig& cfg) {
    /* Un numero MAME par manette, dans l'ordre des joueurs : JOYCODE_1 pour la
     * manette du joueur 1, JOYCODE_2 pour la suivante qui n'a pas encore de
     * numero, etc. Deux joueurs sur la meme manette partagent son numero. */
    const int players = std::min<int>(MAX_PLAYERS, (int)cfg.players.size());
    Pad pads[MAX_PLAYERS];
    bool has_pad[MAX_PLAYERS] = {};
    int next_joy = 1;
    for (int p = 0; p < players; ++p) {
        const auto& player = cfg.players[p];
        if (player.device_path.empty()) continue;
        int shared = -1;
        for (int q = 0; q < p && shared < 0; ++q)
            if (has_pad[q] && cfg.players[q].device_path == player.device_path) shared = q;
        pads[p] = open_pad(player, shared >= 0 ? pads[shared].joy : next_joy++);
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
     * Sans cela MAME numerote dans l'ordre ou il trouve les peripheriques, qui
     * n'est pas celui des js*. Deux manettes identiques donnent le meme
     * fragment : MAME rangerait les deux sur la premiere trouvee, on n'epingle
     * donc pas celles-la et l'on s'en remet a l'ordre de MAME. */
    for (int p = 0; p < players; ++p) {
        if (!has_pad[p] || pads[p].id.empty()) continue;
        bool first_use = true, twin = false;
        for (int q = 0; q < players; ++q) {
            if (q == p || !has_pad[q]) continue;
            if (pads[q].joy == pads[p].joy && q < p) first_use = false;
            if (pads[q].joy != pads[p].joy && pads[q].id == pads[p].id) twin = true;
        }
        if (!first_use || twin) continue;
        out << "            <mapdevice device=\"" << pads[p].id
            << "\" controller=\"JOYCODE_" << pads[p].joy << "\" />\n";
    }

    for (int p = 0; p < players; ++p) {
        if (!player_configured(cfg.players[p])) continue;     // MAME garde ses defauts
        // Le second stick par defaut fait partie de ce qu'on ecrit, pas du
        // profil enregistre.
        PlayerConfig player = cfg.players[p];
        ControllerManager::fill_right_stick(player);
        const Pad* pad = has_pad[p] ? &pads[p] : nullptr;
        const std::string pn = "P" + std::to_string(p + 1) + "_";

        auto digital = [&](GameAction action, const std::string& type) {
            std::vector<std::string> parts;
            auto it = player.bindings.find(action);
            if (it != player.bindings.end()) parts.push_back(digital_code(it->second, pad));
            parts.push_back(stick_switch(pad, action));     // vide hors directions
            for (const auto& d : split_or(defaults_for(type).standard)) parts.push_back(d);
            write_port(out, type, join_or(parts));
        };
        static const char* const kDir[4] = {"UP", "DOWN", "LEFT", "RIGHT"};
        for (int d = 0; d < 4; ++d) {
            const auto dir = static_cast<GameAction>((int)GameAction::UP + d);
            // Le stick principal, et le stick gauche des jeux a deux manches.
            digital(dir, pn + "JOYSTICK_" + kDir[d]);
            digital(dir, pn + "JOYSTICKLEFT_" + kDir[d]);
        }
        for (int d = 0; d < 4; ++d)
            digital(static_cast<GameAction>((int)GameAction::RSTICK_UP + d),
                    pn + "JOYSTICKRIGHT_" + kDir[d]);
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
                            : (pad ? ControllerManager::default_analog(player, role) : AnalogBinding{});
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
                if (const char* item = axis_name(*pad, b.index))
                    axis = joy(*pad) + item + (b.invert ? "_REVERSE" : "");
            } else if (b.source == AnalogSource::MOUSE_AXIS && (b.index == 0 || b.index == 1)) {
                axis = std::string("MOUSECODE_1_") + (b.index == 0 ? "XAXIS" : "YAXIS")
                     + (b.invert ? "_REVERSE" : "");
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
