// src/ControllerManager.h
#pragma once
#include "ControllerConfig.h"
#include <string>
#include <vector>
#include <map>

struct JoystickInfo {
    std::string path;        // /dev/input/js0
    std::string name;
    int num_buttons = 0;
    int num_axes    = 0;
};

class ControllerManager {
public:
    // Scan /dev/input/js* and return available joysticks
    static std::vector<JoystickInfo> list_devices();

    // Open a joystick device in non-blocking mode. Returns fd >= 0 or -1.
    static int  open_device(const std::string& path);
    static void close_device(int fd);

    // Non-blocking poll. Returns true and fills 'result' if a button press
    // or significant axis movement was detected.
    static bool poll_event(int fd, InputBinding& result);

    /* Lecture BRUTE d'un evenement, pour l'ecran de test.
     *
     * poll_event ne rend que ce qui sert a poser une liaison : il jette les
     * relachements et tout ce qui bouge moins que la zone morte. Un panneau
     * de test doit au contraire montrer chaque appui, chaque relachement et
     * la position exacte de chaque axe. */
    struct RawInput {
        bool is_axis = false;
        int  index   = -1;
        int  value   = 0;   // bouton : 0 ou 1 ; axe : -32767 a 32767
    };
    static bool poll_raw(int fd, RawInput& out);

    // ── Single-config persistence (legacy / backward-compat) ─────────────
    // Reads/writes only the "controllers" key in config_path.
    static void load_config(ControllerConfig& out,  const std::string& config_path);
    static void save_config(const ControllerConfig& cfg, const std::string& config_path);

    // ── Profile persistence ───────────────────────────────────────────────
    // "controller_profiles" + "active_controller_profile" keys in config_path.
    // On first call, migrates old "controllers" key → "Default" profile.
    static void load_profiles(std::map<std::string, ControllerConfig>& profiles,
                               std::string& active_name,
                               const std::string& config_path);

    static void save_profiles(const std::map<std::string, ControllerConfig>& profiles,
                               const std::string& active_name,
                               const std::string& config_path);

    // Write FBNeo per-player input defaults (p1defaults.ini / p2defaults.ini)
    // and update szPlayerDefaultIni entries in fbneo.ini.
    // fbneo_config_dir is typically ~/.local/share/fbneo/config
    static void write_fbneo_config(const ControllerConfig& cfg,
                                   const std::string& fbneo_config_dir);

    // Returns ~/.local/share/fbneo/config (does NOT create the directory)
    static std::string get_fbneo_config_dir();

    // Unbind player-2 Coin/Start in config/games/<rom>.ini when FBNeo has
    // mapped them to the SAME physical button as player 1.
    //
    // FBNeo's own defaults do exactly that on a single-pad setup, and the
    // result is unplayable: one press of Coin inserts two credits, one press
    // of Start begins a two-player game.
    //
    // Repairing the file afterwards is the only workable approach. Writing a
    // correct .ini up front does not work: FBNeo does not merge a partial
    // file with its defaults, it blanks every input the file omits : verified
    // by trying it, which wiped Coin 1, Start 1 and the whole D-pad.
    //
    // Only rewrites a binding that genuinely duplicates player 1's. A real
    // second pad maps to a different device id and is left untouched.
    static void fix_player2_input_conflicts(const std::string& fbneo_rom_name);

    /* Les manettes d'un jeu, aux numeros d'AUJOURD'HUI.
     *
     * Un .ini note chaque manette par sa POSITION dans la liste de l'ordinateur
     * (joystick 0, 1...). Quand une manette est debranchee, ou que l'ordre
     * change, ces numeros ne designent plus la bonne : le joueur 1 pointe sur
     * un pad qui n'existe plus et plus rien ne repond.
     *
     * Avant le lancement, les numeros du profil sont traduits d'apres le NOM
     * des manettes branchees ; une manette absente est envoyee sur un numero
     * qui n'existe pas, jamais sur celle d'un autre joueur. Quand tout est
     * deja en place, aucun fichier n'est touche. Le fichier d'origine est
     * garde a cote (<rom>.ini.bcslots) et remis apres la sortie du jeu, ce
     * qui rend la traduction sans memoire : un autre ordre au prochain
     * lancement donne la bonne traduction a ce moment-la. */
    static void use_current_devices(const std::string& fbneo_rom_name,
                                    const ControllerConfig& cfg);
    static void put_back_saved_devices(const std::string& fbneo_rom_name);

    // Bind the analog inputs of config/games/<rom>.ini to the pad.
    //
    // Needed because a whole family of games has no digital directions at all.
    // FBNeo leaves their analog inputs on the keyboard arrows : Out Run ships
    // as `input "Steering" slider 0xcb 0xcd`, which is why the pad steers
    // nothing. Nothing the launcher writes to p1defaults.ini can help: those
    // defaults are named "P1 Left" and this game has no such input.
    //
    // Rewritten in place, on the complete file, for the same reason as
    // fix_player2_input_conflicts: FBNeo does not merge a partial .ini, it
    // blanks every input the file omits.
    static void apply_analog_bindings(const std::string& fbneo_rom_name,
                                      const ControllerConfig& cfg);

    // Ecrit les liaisons NUMERIQUES d'un profil dans config/games/<rom>.ini,
    // et seulement la : un profil choisi POUR UN JEU ne doit pas toucher a
    // p1defaults.ini, qui vaut pour tous les autres.
    //
    // FBNeo applique ses defauts par identite canonique ("p1 fire 3"), qui
    // n'est PAS ecrite dans le .ini : on n'y trouve que le nom d'affichage
    // ("P1 Button C", "P1 Strong Punch"). Mais le fichier liste les entrees
    // dans l'ordre du pilote, et c'est cet ordre qui numerote les boutons :
    // le N-ieme bouton d'action du joueur est "fire N". Coin, Start et les
    // directions se reconnaissent a leur nom ; tout le reste (DIP, Service,
    // Reset, analogique, macros) est laisse tel quel.
    //
    // Fichier reecrit en entier, comme les deux fonctions ci-dessus. Rend le
    // nombre de lignes modifiees, ou -1 si le jeu n'a pas encore de .ini :
    // il n'existe qu'apres un premier lancement.
    static int write_game_config(const ControllerConfig& cfg,
                                 const std::string& fbneo_rom_name);

    // Profil de manette assigne a un jeu, dans config.json sous
    // "game_controller_profiles" : { "<rom>": "<profil>" }. Un profil vide
    // retire l'assignation. Le lancement lit cette table pour savoir quel
    // profil appliquer, et la reapplique a la sortie du jeu : FBNeo reecrit
    // le .ini en quittant, et un « Reset game settings » l'efface.
    static std::map<std::string, std::string> load_game_profiles(const std::string& config_path);
    static void set_game_profile(const std::string& config_path,
                                 const std::string& fbneo_rom_name,
                                 const std::string& profile_name);

    // Which role a game's analog input name plays, or COUNT if unrecognised.
    // Public so the dialog can show the player what a role actually covers.
    static AnalogRole analog_role_for_input(const std::string& fbneo_input_name);

    // Code touche de l'emulateur depuis le code materiel d'un evenement GTK,
    // ou -1 si la touche n'a pas d'equivalent. Voir l'implementation pour
    // pourquoi la traduction n'est pas une simple soustraction.
    static int fbneo_key_from_gtk(unsigned hardware_keycode);

    // La reciproque, pour retrouver quelle touche du clavier du joueur porte
    // un code donne. Rend 0 si le code n'a pas d'equivalent.
    static unsigned gtk_keycode_from_fbneo(int fbneo_key);
};
