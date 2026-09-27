// src/ControllerConfig.h
#pragma once
#include "i18n.h"
#include <cstdio>
#include <string>
#include <map>
#include <vector>

// ── Generic arcade actions ─────────────────────────────────────────────────
// Seize boutons et non plus six : une manette actuelle en a onze ou douze, et
// MAME numerote ses boutons jusqu'a P1_BUTTON16. Les six premiers gardent leur
// cle d'enregistrement, un ancien profil se relit donc tel quel ; les boutons
// que le profil ne lie pas n'existent simplement pas dans le fichier.
enum class GameAction {
    UP = 0, DOWN, LEFT, RIGHT,
    BUTTON1, BUTTON2, BUTTON3, BUTTON4, BUTTON5, BUTTON6,
    BUTTON7, BUTTON8, BUTTON9, BUTTON10, BUTTON11, BUTTON12,
    BUTTON13, BUTTON14, BUTTON15, BUTTON16,
    START, COIN,
    COUNT
};

constexpr int GAME_ACTION_COUNT = static_cast<int>(GameAction::COUNT);
constexpr int MAX_GAME_BUTTONS  = 16;
// Ce que l'ecran montre quand on ne sait rien de la manette : la borne
// classique a six boutons.
constexpr int BASE_GAME_BUTTONS = 6;

inline bool is_button_action(GameAction a) {
    return a >= GameAction::BUTTON1 && a <= GameAction::BUTTON16;
}
// Numero du bouton, 1 a 16, ou 0 si l'action n'est pas un bouton.
inline int button_number(GameAction a) {
    return is_button_action(a) ? static_cast<int>(a) - static_cast<int>(GameAction::BUTTON1) + 1 : 0;
}
inline GameAction button_action(int number) {
    return static_cast<GameAction>(static_cast<int>(GameAction::BUTTON1) + number - 1);
}

inline const char* game_action_name(GameAction a) {
    static const char* const buttons[MAX_GAME_BUTTONS] = {
        "Button 1", "Button 2", "Button 3", "Button 4", "Button 5", "Button 6",
        "Button 7", "Button 8", "Button 9", "Button 10", "Button 11", "Button 12",
        "Button 13", "Button 14", "Button 15", "Button 16",
    };
    if (is_button_action(a)) return buttons[button_number(a) - 1];
    switch (a) {
        case GameAction::UP:      return "Up";
        case GameAction::DOWN:    return "Down";
        case GameAction::LEFT:    return "Left";
        case GameAction::RIGHT:   return "Right";
        case GameAction::START:   return "Start";
        case GameAction::COIN:    return "Coin / Select";
        default:                  return "Unknown";
    }
}

inline const char* game_action_key(GameAction a) {
    static const char* const buttons[MAX_GAME_BUTTONS] = {
        "button1", "button2", "button3", "button4", "button5", "button6",
        "button7", "button8", "button9", "button10", "button11", "button12",
        "button13", "button14", "button15", "button16",
    };
    if (is_button_action(a)) return buttons[button_number(a) - 1];
    switch (a) {
        case GameAction::UP:      return "up";
        case GameAction::DOWN:    return "down";
        case GameAction::LEFT:    return "left";
        case GameAction::RIGHT:   return "right";
        case GameAction::START:   return "start";
        case GameAction::COIN:    return "coin";
        default:                  return "unknown";
    }
}

// ── Single input binding ───────────────────────────────────────────────────
// Nom de secours d'un code touche, quand on n'a pas releve le nom sur le
// clavier du joueur : ces codes sont des positions, pas des lettres, et la
// meme position porte A sur un clavier QWERTY et Q sur un AZERTY. Seules les
// touches dont le nom ne depend pas de la disposition sont nommees ici ; les
// autres s'affichent en hexadecimal plutot que d'annoncer la mauvaise lettre.
inline std::string key_label(int code) {
    switch (code) {
        case 0x01: return "Esc";
        case 0x0E: return "Backspace";
        case 0x0F: return "Tab";
        case 0x1C: return "Enter";
        case 0x1D: return "Ctrl gauche";
        case 0x2A: return "Maj gauche";
        case 0x36: return "Maj droite";
        case 0x38: return "Alt gauche";
        case 0x39: return "Espace";
        case 0x9C: return "Entree (pave)";
        case 0x9D: return "Ctrl droite";
        case 0xB8: return "Alt droite";
        case 0xC7: return "Debut";
        case 0xC8: return "Haut";
        case 0xC9: return "Page haut";
        case 0xCB: return "Gauche";
        case 0xCD: return "Droite";
        case 0xCF: return "Fin";
        case 0xD0: return "Bas";
        case 0xD1: return "Page bas";
        case 0xD2: return "Inser";
        case 0xD3: return "Suppr";
        default: break;
    }
    if (code >= 0x02 && code <= 0x0B) {                 // 1 a 9 puis 0
        const char* digits = "1234567890";
        return std::string(1, digits[code - 0x02]);
    }
    if (code >= 0x3B && code <= 0x44)                   // F1 a F10
        return "F" + std::to_string(code - 0x3B + 1);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "Touche 0x%02X", code);
    return buf;
}

// D'ou vient une commande. Le clavier n'etait pas propose : un joueur sans
// manette ne pouvait rien configurer, et un joueur qui en a une ne pouvait pas
// garder deux ou trois touches au clavier a cote.
enum class InputSource { PAD = 0, KEY };

struct InputBinding {
    bool valid    = false;
    InputSource source = InputSource::PAD;
    bool is_axis  = false;
    int  button   = -1;   // button index (source PAD, !is_axis)
    int  axis     = -1;   // axis index   (source PAD, is_axis)
    int  axis_dir = 0;    // +1 or -1     (source PAD, is_axis)
    int  key      = -1;   // code touche de l'emulateur (source KEY)
    // Nom releve sur le clavier du joueur au moment de la liaison. Le code
    // seul ne suffit pas a l'ecrire : il designe une position, et la meme
    // position ne porte pas la meme lettre selon la disposition.
    std::string key_name;

    std::string label() const {
        if (!valid) return "";
        if (source == InputSource::KEY)
            return key_name.empty() ? key_label(key) : key_name;
        if (is_axis)
            return _("Axis ") + std::to_string(axis)
                   + (axis_dir > 0 ? " +" : " -");
        return _("Button ") + std::to_string(button);
    }
};

// ── Analog inputs ──────────────────────────────────────────────────────────
// A whole family of games has no D-pad at all: Out Run steers, Arkanoid uses a
// paddle, Tempest a dial, light-gun games a pair of pointer axes. FBNeo names
// these per game : "Steering", "Paddle", "Dial", "Gun X" : so the launcher
// cannot bind them by name the way it binds "P1 Left".
//
// Roles are the answer: what an input DOES, which is stable across the dozens
// of names games give it. A role is matched to a game's input name by pattern
// (see analog_role_for_input), and carries the binding the player chose.
enum class AnalogRole { STEERING = 0, THROTTLE, BRAKE, AIM_X, AIM_Y, COUNT };

constexpr int ANALOG_ROLE_COUNT = static_cast<int>(AnalogRole::COUNT);

inline const char* analog_role_name(AnalogRole r) {
    switch (r) {
        case AnalogRole::STEERING: return "Steering / paddle / dial";
        case AnalogRole::THROTTLE: return "Accelerate / throttle";
        case AnalogRole::BRAKE:    return "Brake";
        case AnalogRole::AIM_X:    return "Aim X (gun, trackball)";
        case AnalogRole::AIM_Y:    return "Aim Y (gun, trackball)";
        default:                   return "Unknown";
    }
}

inline const char* analog_role_key(AnalogRole r) {
    switch (r) {
        case AnalogRole::STEERING: return "steering";
        case AnalogRole::THROTTLE: return "throttle";
        case AnalogRole::BRAKE:    return "brake";
        case AnalogRole::AIM_X:    return "aim_x";
        case AnalogRole::AIM_Y:    return "aim_y";
        default:                   return "unknown";
    }
}

inline AnalogRole analog_role_from_key(const std::string& k) {
    for (int i = 0; i < ANALOG_ROLE_COUNT; ++i) {
        auto r = static_cast<AnalogRole>(i);
        if (k == analog_role_key(r)) return r;
    }
    return AnalogRole::COUNT;
}

// Where an analog input reads from. Only joystick axes are wired today; the
// enum exists so a mouse or a light gun can be added later without changing
// the saved file format or the dialog's shape.
// MOUSE_AXIS est conserve pour ne pas invalider les configurations deja
// enregistrees, mais le pilote SDL de l'emulateur ne lit pas la souris : la
// valeur est ecrite et jamais appliquee. Le dialogue ne la propose plus.
//
// KEY_PAIR est la facon dont l'emulateur fait tourner un volant au clavier :
// deux touches, une par sens, avec une vitesse et un retour au centre. C'est
// exactement ce qu'After Burner utilise par defaut.
enum class AnalogSource { NONE = 0, JOY_AXIS, MOUSE_AXIS, KEY_PAIR };

struct AnalogBinding {
    AnalogSource source = AnalogSource::NONE;
    int  index    = -1;      // axis number on the device
    int  key_neg  = -1;      // KEY_PAIR : touche du sens negatif
    int  key_pos  = -1;      // KEY_PAIR : touche du sens positif
    std::string key_neg_name;   // releves sur le clavier du joueur, voir
    std::string key_pos_name;   // InputBinding::key_name
    bool invert   = false;
    // FBNeo drives an analog input in one of two ways, and the right one
    // depends on the control being emulated:
    //   absolute : the stick's position IS the wheel's position (a real wheel)
    //   relative : the stick turns a wheel that drifts back to centre, which
    //              is how a keyboard or a digital pad has to fake one
    bool relative = false;
    int  speed    = 0x800;   // relative only: how fast the axis moves it
    int  center   = 10;      // relative only: how fast it recentres
    bool is_set() const {
        if (source == AnalogSource::KEY_PAIR) return key_neg >= 0 && key_pos >= 0;
        return source != AnalogSource::NONE && index >= 0;
    }
};

// Sensible starting point for a standard twin-stick pad: steering and aiming
// on the left stick, pedals on the triggers. A player who disagrees changes it
// in the dialog; a player who never opens it still gets a playable Out Run.
inline AnalogBinding default_analog_binding(AnalogRole r) {
    AnalogBinding b;
    b.source = AnalogSource::JOY_AXIS;
    switch (r) {
        case AnalogRole::STEERING: b.index = 0; break;   // left stick X
        case AnalogRole::AIM_X:    b.index = 0; break;
        case AnalogRole::AIM_Y:    b.index = 1; break;   // left stick Y
        case AnalogRole::THROTTLE: b.index = 5; break;   // right trigger
        case AnalogRole::BRAKE:    b.index = 2; break;   // left trigger
        default:                   b.source = AnalogSource::NONE; break;
    }
    return b;
}

// ── Per-player configuration ───────────────────────────────────────────────
struct PlayerConfig {
    std::string device_path;   // "" = unset, "/dev/input/js0" etc.
    std::string device_name;
    /* Le prereglage choisi par le joueur.
     *
     * Ce n'est pas qu'un point de depart pour les liaisons : c'est aussi lui
     * qui dit a quoi ressemble la manette quand le pilote ne le revele pas,
     * donc l'illustration montree et le nom des boutons dans l'ecran de test.
     * Non enregistre, il repassait a << Keyboard >> a chaque ouverture et le
     * dessin redevenait generique alors que les liaisons, elles, tenaient. */
    std::string preset;
    std::map<GameAction, InputBinding> bindings;
    std::map<AnalogRole, AnalogBinding> analog;
};

// ── Préréglages ────────────────────────────────────────────────────────────
//
// Un point de départ par type de manette, pour ne pas faire lier douze
// commandes une par une à quelqu'un qui vient de brancher une manette
// standard.
//
// LES INDEX VIENNENT DU PILOTE, PAS DE LA MANETTE. Linux numerote les boutons
// dans l'ordre ou son pilote les declare, et cet ordre differe d'une famille a
// l'autre : la croix de la DualShock 4 est le bouton 1, celle de la DualSense
// le bouton 0. D'ou un prereglage par famille plutot qu'un seul « manette ».
//
// LA DISPOSITION EST CELLE DES BORNES : les trois boutons du bas de la borne
// sur la rangee du bas de la manette, les trois du haut sur la rangee du haut,
// avec les gachettes en troisieme colonne. C'est la disposition attendue par
// les jeux de combat, qui sont les seuls a utiliser six boutons.
//
// La croix directionnelle est un chapeau, donc deux axes et non des boutons,
// sur toutes ces manettes.
struct ControllerPreset {
    const char* name;
    bool        keyboard;          // sinon manette
    int         face[6];           // boutons 1 a 6
    int         start;
    int         coin;
    /* Les axes du chapeau directionnel.
     *
     * La plupart des manettes le posent sur 6 et 7, mais ce n'est pas une
     * regle : les copies de DualShock 3 n'ont que six axes et rangent le
     * chapeau sur 4 et 5. Fige a 6 et 7 pour tout le monde, le prereglage
     * laissait ces manettes sans directions. */
    int         axis_x = 6;
    int         axis_y = 7;
};

inline const std::vector<ControllerPreset>& controller_presets() {
    static const std::vector<ControllerPreset> presets = {
        // Clavier : les reglages classiques de l'emulateur, fleches pour se
        // deplacer, 1 pour demarrer, 5 pour la piece.
        {"Keyboard",       true,  {0x1E, 0x1F, 0x20, 0x2C, 0x2D, 0x2E}, 0x02, 0x06},
        // Xbox 360 et Xbox One : A0 B1 X2 Y3, LB4 RB5, Back6 Start7.
        {"Xbox",           false, {2, 3, 4, 0, 1, 5}, 7, 6},
        // DualShock 4 : Carre0 Croix1 Rond2 Triangle3, L1 4, R1 5,
        // Share 8, Options 9.
        {"PlayStation 4",  false, {0, 3, 4, 1, 2, 5}, 9, 8},
        // DualSense : le pilote a renumerote, Croix0 Rond1 Triangle2 Carre3.
        {"PlayStation 5",  false, {3, 2, 4, 0, 1, 5}, 9, 8},
        /* DualShock 3 et ses copies, RELEVE sur la manette et non deduit
         * d'une documentation : Triangle 0, Rond 1, Croix 2, Carre 3, L1 4,
         * R1 5, Select 8, Start 9, et le chapeau sur les axes 4 et 5. La
         * numerotation Sony d'origine (12 a 15) ne s'applique pas ici : ces
         * manettes n'annoncent que douze boutons, et les numeros au-dela
         * n'existent tout simplement pas. */
        {"PlayStation 3",  false, {3, 0, 4, 2, 1, 5}, 9, 8, 4, 5},
        /* Stick d'arcade a encodeur XInput : les boutons portent les noms
         * Xbox, A0 B1 X2 Y3 LB4 RB5, Select 6, Start 7. On range la rangee du
         * haut en poings (X, Y, RB) et celle du bas en pieds (A, B, LB) :
         * c'est la disposition Capcom a six boutons. On ne se sert que des
         * six touches dont la numerotation est certaine ; LT et RT sont des
         * AXES sur beaucoup d'encodeurs, et les lier ici ne marcherait pas. */
        {"Arcade",         false, {2, 3, 5, 0, 1, 4}, 7, 6},
        // Switch Pro et Joy-Con apparies : B0 A1 Y2 X3, L4 R5, Moins8 Plus9.
        {"Switch Pro",     false, {2, 3, 4, 0, 1, 5}, 9, 8},
    };
    return presets;
}

// Le chapeau directionnel, identique sur les quatre manettes : deux axes.
inline InputBinding preset_direction(GameAction action, int axis_x = 6, int axis_y = 7) {
    InputBinding b;
    b.valid  = true;
    b.source = InputSource::PAD;
    b.is_axis = true;
    switch (action) {
        case GameAction::LEFT:  b.axis = axis_x; b.axis_dir = -1; break;
        case GameAction::RIGHT: b.axis = axis_x; b.axis_dir =  1; break;
        case GameAction::UP:    b.axis = axis_y; b.axis_dir = -1; break;
        case GameAction::DOWN:  b.axis = axis_y; b.axis_dir =  1; break;
        default: b.valid = false; break;
    }
    return b;
}

// Les fleches du clavier, pour le prereglage clavier.
inline InputBinding preset_direction_key(GameAction action) {
    InputBinding b;
    b.valid  = true;
    b.source = InputSource::KEY;
    switch (action) {
        case GameAction::LEFT:  b.key = 0xCB; b.key_name = "Left";  break;
        case GameAction::RIGHT: b.key = 0xCD; b.key_name = "Right"; break;
        case GameAction::UP:    b.key = 0xC8; b.key_name = "Up";    break;
        case GameAction::DOWN:  b.key = 0xD0; b.key_name = "Down";  break;
        default: b.valid = false; break;
    }
    return b;
}

inline void apply_preset(PlayerConfig& player, const ControllerPreset& preset) {
    player.bindings.clear();
    for (int a = 0; a < 4; ++a) {
        auto action = static_cast<GameAction>(a);
        InputBinding b = preset.keyboard
                       ? preset_direction_key(action)
                       : preset_direction(action, preset.axis_x, preset.axis_y);
        if (b.valid) player.bindings[action] = b;
    }
    auto set = [&](GameAction action, int code) {
        InputBinding b;
        b.valid = true;
        if (preset.keyboard) { b.source = InputSource::KEY; b.key = code; }
        else                 { b.source = InputSource::PAD; b.button = code; }
        player.bindings[action] = b;
    };
    for (int i = 0; i < 6; ++i)
        set(button_action(i + 1), preset.face[i]);
    set(GameAction::START, preset.start);
    set(GameAction::COIN,  preset.coin);

    // Les commandes analogiques aussi : un profil qui ne remplit que les
    // boutons laisse sans volant les jeux de course et sans manche les jeux
    // de vol, c'est-a-dire precisement ceux ou la manette change tout.
    player.analog.clear();
    if (preset.keyboard) {
        // Au clavier un axe se tient avec deux touches, et il doit revenir au
        // centre quand on les relache : c'est le mode relatif.
        auto pair = [](int neg, const char* neg_name, int pos, const char* pos_name) {
            AnalogBinding b;
            b.source = AnalogSource::KEY_PAIR;
            b.relative = true;
            b.key_neg = neg; b.key_neg_name = neg_name;
            b.key_pos = pos; b.key_pos_name = pos_name;
            return b;
        };
        player.analog[AnalogRole::STEERING] = pair(0xCB, "Left", 0xCD, "Right");
        player.analog[AnalogRole::AIM_X]    = pair(0xCB, "Left", 0xCD, "Right");
        player.analog[AnalogRole::AIM_Y]    = pair(0xC8, "Up",   0xD0, "Down");
        // Gaz et frein sur deux touches voisines, faute d'analogique.
        player.analog[AnalogRole::THROTTLE] = pair(0x1F, "S", 0x11, "Z");
        player.analog[AnalogRole::BRAKE]    = pair(0x2D, "X", 0x2C, "W");
    } else {
        // Les valeurs d'origine conviennent a toutes ces manettes : stick
        // gauche pour la direction, gachettes pour les pedales. Elles sont
        // absolues, la position du stick etant celle du volant.
        for (int r = 0; r < ANALOG_ROLE_COUNT; ++r) {
            auto role = static_cast<AnalogRole>(r);
            AnalogBinding b = default_analog_binding(role);
            if (b.is_set()) player.analog[role] = b;
        }
    }
}

// ── Full controller configuration (2 players) ─────────────────────────────
struct ControllerConfig {
    std::vector<PlayerConfig> players;
    ControllerConfig() { players.resize(2); }
};
