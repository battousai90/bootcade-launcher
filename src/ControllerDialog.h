// src/ControllerDialog.h
#pragma once
#include <gtkmm.h>
#include <string>
#include <vector>
#include <map>
#include "ControllerConfig.h"
#include "SettingsUi.h"
#include "ControllerManager.h"

class ControllerDialog : public Gtk::Dialog {
public:
    // Takes a copy of all profiles + the active profile name.
    // On Save, writes everything to config_path.
    /* Pas de parent : c'est une fenetre a part entiere, deplacable sur un
     * autre ecran pendant que le launcher reste ou il est. La lier par
     * transient_for la collait a lui. */
    ControllerDialog(const std::map<std::string, ControllerConfig>& profiles,
                     const std::string& active_profile,
                     const std::string& config_path);
    ~ControllerDialog();

    /* Portee « ce jeu seulement ».
     *
     * Le meme ecran, mais Save n'ecrit que config/games/<rom>.ini et
     * l'assignation du profil a ce jeu : le profil par defaut, celui qui vaut
     * pour tous les autres jeux, ne bouge pas. Un jeu de combat et un shoot
     * n'ont pas le meme plan de boutons, et regler l'un ne doit pas deregler
     * l'autre. `game_profile` est le profil deja assigne, vide s'il n'y en a
     * pas : on ouvre alors sur le profil par defaut. A appeler avant show(). */
    /* `mame` : le jeu tourne sous MAME. `fbneo_rom_name` est alors sa cle
     * « mame:<machine> », et Save comme « Use default profile » n'ecrivent
     * que l'assignation : MAME recoit le profil au lancement, dans un
     * fichier controleur regenere a chaque fois. */
    void set_game_scope(const std::string& fbneo_rom_name,
                        const std::string& game_title,
                        const std::string& game_profile,
                        bool mame = false);

private:
    // ── Profile state ─────────────────────────────────────────────────────
    std::map<std::string, ControllerConfig> m_profiles;
    std::string                             m_active_profile_name;
    std::string                             m_config_path;
    // Le profil par defaut tel qu'il etait a l'ouverture : en portee jeu,
    // c'est lui qu'on reecrit comme actif, quel que soit le profil choisi
    // pour le jeu.
    std::string                             m_global_active_profile;
    std::string                             m_game_rom;
    std::string                             m_game_title;
    Gtk::Button                             m_btn_use_default;
    bool                                    m_game_mame{false};
    bool game_scoped() const { return !m_game_rom.empty(); }
    bool                                    m_profile_switching{false};

    // Working copy of the active profile
    ControllerConfig m_config;

    // ── Profile bar widgets ───────────────────────────────────────────────
    Gtk::Box          m_profile_bar{Gtk::ORIENTATION_HORIZONTAL, 6};
    Gtk::Label        m_profile_label;
    Gtk::ComboBoxText m_profile_combo;
    // Les trois gestes rares passent derriere un menu, voir build_profile_bar.
    Gtk::MenuButton   m_btn_profile_more;
    Gtk::Menu         m_profile_menu;
    Gtk::MenuItem     m_mi_new, m_mi_rename, m_mi_delete;
    Gtk::Button       m_btn_new;      // conserves : encore references ailleurs
    Gtk::Button       m_btn_rename;
    Gtk::Button       m_btn_delete;

    // ── Player tabs ───────────────────────────────────────────────────────
    Gtk::Notebook              m_notebook;

    /* Elements de composition ajoutes pour suivre la maquette.
     *
     * L'ancienne fenetre etait un formulaire GTK empile ; la maquette montre
     * une entete, une barre de configuration, des onglets joueurs mis en
     * avant, trois cartes de liaisons cote a cote, un panneau manette a
     * droite et un pied a deux groupes. Les widgets de saisie ne changent
     * pas, c'est leur agencement et leur habillage qui suivent la maquette.
     */
    /* Meme traitement de fenetre que le reste de Bootcade : une barre de
     * titre cote client, avec son unique bouton de fermeture. Un entete
     * complet empile SOUS la barre du gestionnaire de fenetres donnait deux
     * titres et deux croix pour la meme fenetre. */
    Gtk::HeaderBar m_headerbar;
    Gtk::Box    m_header{Gtk::ORIENTATION_HORIZONTAL, 11};
    SettingsUi::Icon m_header_icon{"bc-logo-pad.svg", 26};
    Gtk::Label  m_header_title;
    Gtk::Label  m_header_sub;
    Gtk::Box    m_topbar{Gtk::ORIENTATION_HORIZONTAL, 18};
    Gtk::Box    m_footer{Gtk::ORIENTATION_HORIZONTAL, 10};
    Gtk::Button m_btn_clear_all;
    Gtk::Button m_btn_restore;

    // Panneau manette, a droite de la zone de liaisons.
    /* Une carte par joueur, pas une seule.
     *
     * Construite pour le seul joueur 1, elle disparaissait en passant sur
     * l'onglet Player 2 et la colonne de gauche s'etalait sur toute la
     * largeur : la fenetre changeait de forme d'un onglet a l'autre. */
    Gtk::Box*   m_device_panel[2]{};
    Gtk::Label* m_device_title[2]{};
    Gtk::Label* m_device_sub[2]{};
    Gtk::Image* m_device_art[2]{};

    /* L'encart du bas dit toujours quelque chose : la recommandation quand
     * elle existe, l'etat neutre sinon. Un conteneur vert vide qui affirme
     * un succes sans rien annoncer serait pire que pas d'encart du tout. */
    Gtk::Grid*   m_detected_box[2]{};
    SettingsUi::Icon* m_detected_icon[2]{};
    Gtk::Label*  m_detected_title[2]{};
    Gtk::Label*  m_detected_sub[2]{};
    Gtk::Button* m_detected_btn[2]{};

    /* Le materiel reconnu et le prereglage choisi sont deux choses : un
     * prereglage pose a la main ne change pas l'identite de la manette. */
    Gtk::ComboBoxText* m_preset_combos[2]{};
    std::string        m_reco_preset[2];

    /* La ligne manette et la ligne prereglage appartiennent au joueur
     * affiche, mais la maquette les place hors de l'onglet : l'une dans la
     * barre du haut, l'autre au bout de la ligne des onglets. Une pile les
     * garde a cette place en montrant celles du joueur courant. */
    Gtk::Stack  m_device_stack;
    Gtk::Stack  m_preset_stack;

    /* En fermeture, plus personne n'ecrit dans les widgets.
     *
     * Un carnet d'onglets emet encore switch-page pendant sa destruction :
     * le gestionnaire allait alors poser la page des deux piles et redessiner
     * le panneau manette sur des widgets deja liberes, et l'application
     * tombait en fermant la fenetre. */
    bool m_closing{false};

    // Retour visuel de l'enregistrement, coupe a la fermeture.
    sigc::connection m_saved_timer;
    Gtk::Label* m_conn_dot[2]{};
    Gtk::Label* m_conn_lbl[2]{};
    void        update_device_panel(int p);

    Gtk::Widget* icon(const std::string& file, int px = 20);
    /* Deux conteneurs a taille fixe, dont le contenu est centre par la
     * geometrie et non par des marges au juge : c'est ce qui garantit que
     * les quatre fleches et les six pastilles sont rigoureusement
     * identiques et alignees sur les memes axes. */
    Gtk::Widget* glyph_cell(const std::string& file, bool tile);
    Gtk::Widget* number_badge(int n);
    Gtk::Widget* card(const std::string& title, const std::string& icon_file,
                      Gtk::Widget& body, const std::string& subtitle = {});
    std::vector<JoystickInfo>  m_devices;
    Gtk::ComboBoxText*         m_device_combos[2]{};

    /* Boutons 7 a 16 : une carte a part, et une ligne par bouton, montrees
     * selon le nombre de boutons de la manette choisie. */
    Gtk::Widget* m_extra_card[2]{};
    Gtk::Widget* m_extra_rows[2][MAX_GAME_BUTTONS]{};
    // Combien de boutons l'ecran propose pour ce joueur : ceux de sa manette
    // (six au moins), plus tout bouton deja lie au-dela.
    int  shown_buttons(int p) const;
    bool action_shown(int p, GameAction a) const;
    void update_button_rows(int p);

    // Binding labels / buttons: indexed [player * GAME_ACTION_COUNT + action]
    std::vector<Gtk::Label*>   m_binding_labels;
    std::vector<Gtk::Button*>  m_bind_buttons;

    // Active "press a button" state
    int              m_bind_fd  = -1;
    sigc::connection m_poll_conn;

    // ── UI builders ───────────────────────────────────────────────────────
    void build_profile_bar();
    void build_player_tab(int p);

    // ── Analog tab ────────────────────────────────────────────────────────
    // One page for the controls that are not buttons: wheels, paddles, dials,
    // pedals, pointers. Separate from the player tabs because these are
    // per-role rather than per-direction, and because the page is where a
    // mouse or a light gun will be added without disturbing anything else.
    // Un bouton qui dit a quoi la commande est liee, et une case pour
    // inverser. Le reste, numero d'axe et mode, se deduit de ce que le joueur
    // bouge ou tape : lui demander etait lui faire faire le travail du code.
    struct AnalogWidgets {
        Gtk::Button*      bind   = nullptr;
        Gtk::CheckButton* invert = nullptr;
        AnalogBinding     value;               // ce qui est lie aujourd'hui
    };
    AnalogWidgets m_analog_widgets[2][ANALOG_ROLE_COUNT];
    bool          m_analog_loading = false;    // suppresses signals while filling
    // Built inside each player tab, not as a page of its own: these controls
    // belong to a player exactly like their buttons do, and a separate tab
    // duplicated the Player 1 / Player 2 split that already existed.
    Gtk::Widget* build_analog_section(int p);
    void analog_ui_from_config();
    void capture_analog(int player, AnalogRole role);
    void analog_config_from_ui();

    // ── Profile management ────────────────────────────────────────────────
    void populate_profile_combo();
    void save_active_to_profiles();   // sync m_config → m_profiles[m_active]
    void load_profile(const std::string& name); // sync m_profiles[name] → m_config + refresh UI

    // "Press a button on the pad you want." Two identical-looking pads are
    // indistinguishable from their names alone : /dev/input/js0 and js1 say
    // nothing about which one is in your hands.
    void identify_device(int p);
    /* Essayer la manette sans rien enregistrer : chaque appui et chaque axe
     * s'affichent en direct. C'est le seul moyen de savoir si un bouton
     * repond avant de le lier. */
    void open_test_dialog(int p);

    void on_profile_changed();
    void on_new_profile_clicked();
    void on_rename_profile_clicked();
    void on_delete_profile_clicked();

    // ── Binding helpers ───────────────────────────────────────────────────
    void refresh_bindings(int p);
    void refresh_from_config();       // update device combos + all labels from m_config
    void on_device_changed(int p);
    /* Assistant sequentiel : les douze commandes, dans l'ordre.
     *
     * Il travaille sur une COPIE et ne l'applique qu'a la fin. Ecrire au fur
     * et a mesure dans le profil actif detruirait une configuration existante
     * des que le joueur abandonne en cours de route, ce qui est le geste le
     * plus probable quand on decouvre l'ecran.
     */
    void run_auto_configure(int p);

    void start_binding(int p, GameAction action);
    void stop_binding();
    void on_save_clicked();
};
