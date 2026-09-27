// src/SettingsPanel.h
#pragma once

#include <gtkmm.h>
#include "SettingsUi.h"
// Seulement la declaration du type : les options MAME se lisent et s'ecrivent
// en JSON, mais inclure tout nlohmann ici le ferait recompiler a chaque
// fichier qui n'a besoin que du panneau.
#include <nlohmann/json_fwd.hpp>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <set>

class SettingsPanel : public Gtk::Box {
public:
    SettingsPanel();
    virtual ~SettingsPanel();

    std::string get_roms_path() const;  // Deprecated - returns first path for compatibility
    void set_roms_path(const std::string& path);  // Deprecated - sets first path for compatibility

    std::vector<std::string> get_roms_paths() const;
    void set_roms_paths(const std::vector<std::string>& paths);
    void add_roms_path(const std::string& path);
    void remove_roms_path(int index);

    std::string get_dat_path() const;
    void set_dat_path(const std::string& path);

    std::string get_previews_path() const;
    void set_previews_path(const std::string& path);

    /* ── La bibliotheque, un jeu de reglages PAR EMULATEUR ────────────────
     *
     * Les dossiers de ROMs, les previsualisations, les titres et les options
     * de balayage ne veulent pas dire la meme chose pour FinalBurn Neo et
     * pour MAME : ce ne sont ni les memes collections, ni les memes images.
     * Les tenir dans un seul jeu de cles obligeait MAME a redeclarer ses
     * dossiers ailleurs, ce qui est exactement le doublon qu'on supprime.
     *
     * Les accesseurs SANS emulateur rendent ceux de FinalBurn Neo : leurs
     * appelants (fenetre principale, balayage, audit) ne connaissent pas
     * encore la notion d'emulateur, et leur rendre les reglages de l'ecran
     * affiche a cet instant ferait dependre un balayage de ce qu'une liste
     * deroulante montre.
     */
    struct LibrarySettings {
        std::vector<std::string> roms_paths;
        std::string previews_path;
        std::string titles_path;
        /* Ou telecharger les images, dans l'ordre d'essai.
         *
         * Une liste et non une adresse : aucun depot ne couvre un catalogue
         * entier, et celui de FinalBurn Neo ne connait rien de MAME. La
         * premiere qui rend l'image gagne, les suivantes comblent ses trous.
         * Vide veut dire « aucune » : c'est un choix, pas un oubli, et
         * ArtworkSources::load_for le respecte. */
        std::vector<std::string> artwork_sources;
        bool scan_recursive   = true;
        bool scan_loose_files = true;
    };
    LibrarySettings          library_for(const std::string& emulator) const;
    std::vector<std::string> get_roms_paths(const std::string& emulator) const;
    std::string              get_previews_path(const std::string& emulator) const;
    std::string              get_titles_path(const std::string& emulator) const;
    bool                     is_scan_recursive(const std::string& emulator) const;
    bool                     is_scan_loose_files(const std::string& emulator) const;

    // ROM Management's own folders : where Fix writes, and where rejects go.
    // Ils ne se reglent plus ici — chaque onglet du gestionnaire de ROMs
    // choisit le sien — mais la cle reste ecrite et relue par ce panneau,
    // qui est le seul a savoir ouvrir et sauver config.json.
    std::string get_outbox_path() const;
    std::string get_quarantine_path() const;
    void set_outbox_path(const std::string& path);
    void set_quarantine_path(const std::string& path);
    // "Manage DATs in ROM Management" : the owner opens that window.
    sigc::signal<void>& signal_open_rom_manager() { return m_sig_open_rom_manager; }

    std::string get_titles_path() const;
    void set_titles_path(const std::string& path);

    bool save_to_file(const std::string& filename = "config.json");
    bool load_from_file(const std::string& filename = "config.json");

    // Appearance / language
    std::string get_theme() const;               // "system" | "dark" | "light"
    void set_theme(const std::string& mode);
    std::string get_language() const;            // "" (system) | "en" | "fr" | ...
    void set_language(const std::string& code);
    sigc::signal<void, Glib::ustring>& signal_theme_changed()    { return m_sig_theme_changed; }
    sigc::signal<void, Glib::ustring>& signal_language_changed() { return m_sig_language_changed; }
    // Emis des que l'interrupteur bouge, pour que l'effet soit immediat.
    // Exiger un redemarrage pour un interrupteur serait une regression
    // deguisee en reglage.
    sigc::signal<void, bool>&          signal_hiscore_toggled()  { return m_sig_hiscore_toggled; }

    /* ── La coquille de l'ecran ────────────────────────────────────────
     *
     * La barre de titre appartient au DESSIN de l'ecran, pas a la fenetre qui
     * le porte : le pictogramme, le titre et le sous-titre sont sur la
     * maquette au meme titre que les cartes. La fenetre se contente de la
     * poser par set_titlebar, exactement comme Controller Configuration.
     *
     * Le pied, lui, agit sur la fenetre : seule elle sait se fermer, donc les
     * boutons ne font qu'emettre.
     */
    Gtk::HeaderBar&     header_bar()             { return m_headerbar; }
    sigc::signal<void>& signal_close_requested() { return m_sig_close; }
    sigc::signal<void>& signal_save_requested()  { return m_sig_save; }
    // A appeler quand la fenetre s'ouvre : relit ce qui a pu changer dehors
    // (compte, emulateur present ou non) et relance la sonde reseau.
    void on_window_shown();

    std::string get_fbneo_executable() const;
    void set_fbneo_executable(const std::string& path);

    // Public method for menu access
    void on_download_fbneo_clicked();

    // Public access to download previews button
    Gtk::Button& get_download_previews_button() { return m_button_download_previews; }

    // Public access to download titles button
    Gtk::Button& get_download_titles_button() { return m_button_download_titles; }

    // Public access to entry for menu
    Gtk::Entry m_entry_fbneo;
    // Scan options (celles de FinalBurn Neo ; voir library_for).
    bool is_scan_recursive() const;
    bool is_scan_loose_files() const;

    /* ── Comportement de l'application ────────────────────────────────
     *
     * Deux reglages, et deux seulement, parce que ce sont les deux seuls qui
     * commandent quelque chose que le lanceur fait deja. La maquette en
     * montre d'autres : les inventer donnerait des interrupteurs qui ne
     * branchent rien, ce qui est pire qu'une page plus courte.
     */
    bool restores_window_state() const { return m_switch_window_state.get_active(); }
    bool keeps_play_history()    const { return m_switch_play_history.get_active(); }
    bool checks_updates_auto()   const { return m_switch_auto_update.get_active(); }

    /* ── Jeu au hasard (le bouton « de » de la barre du haut) ─────────────
     *
     * `from_shown` : tirer parmi les jeux affiches (les filtres de la colonne
     * de gauche et la recherche font foi) ; sinon parmi `systems`. Les trois
     * restrictions s'appliquent dans les deux cas. `systems` vide = tous. */
    struct RandomPick {
        bool from_shown     = true;
        std::set<std::string> systems;
        bool hiscore_only   = false;
        bool originals_only = false;
        bool unplayed_only  = false;
        bool launch         = false;
    };
    RandomPick random_pick() const;
    // Les systemes connus ne le sont qu'une fois la base lue : la fenetre
    // principale les fournit, et la carte construit alors ses cases.
    void set_random_systems(const std::vector<std::string>& systems);

    /* ── Options propres a l'emulateur ────────────────────────────────
     *
     * Elles appartiennent a la fiche de FinalBurn Neo, pas aux reglages
     * generaux de Bootcade : le jour ou un second emulateur arrive, il aura
     * les siennes, et « plein ecran » ne voudra pas dire la meme chose pour
     * les deux.
     *
     * Le plein ecran et la mise a l'echelle entiere existaient deja, pilotes
     * par le menu « Launch » de la fenetre principale. Ce sont les MEMES
     * reglages, lus et ecrits sous les memes cles : en creer de nouveaux
     * aurait donne deux commandes pour un seul comportement, donc tot ou tard
     * deux etats contradictoires a l'ecran.
     */
    bool        launches_fullscreen()   const { return m_switch_fullscreen.get_active(); }
    bool        launches_integerscale() const { return m_switch_integerscale.get_active(); }
    // Flippers et machines a sous : un tiers du catalogue MAME, ecarte par
    // defaut du compte de jeux comme des filtres.
    /* Les dossiers de ROMs de MAME, separes par « ; ».
     *
     * Ce ne sont plus ceux d'un champ a part : ce sont les dossiers de
     * l'entree « mame » de la page Library. Un champ propre a la carte MAME
     * faisait dire deux fois la meme chose a deux endroits, avec la
     * certitude qu'ils divergeraient. La forme, elle, ne change pas : le
     * lancement attend toujours une liste separee par des points-virgules.
     */
    std::string mame_rompaths() const;
    void        set_mame_rompaths(const std::string& v);

    /* ── MAME : son binaire, ses options ──────────────────────────────
     *
     * MAME ne vient pas forcement de la distribution : on peut en poser un
     * binaire n'importe ou, exactement comme FinalBurn Neo. Le champ prime
     * donc toujours sur la detection, et la detection ne sert que de repli
     * pour l'immense majorite des installations ou MAME est un paquet.
     */
    std::string mame_executable() const;
    // Le pendant exact de fbneo_extra_args : ce que le joueur ajoute a la
    // main, apres tout ce que les interrupteurs ont construit.
    std::string mame_extra_args() const { return m_entry_mame_args.get_text(); }
    /* La ligne de commande que les options de l'ecran decrivent.
     *
     * Rendue toute faite pour que le lancement n'ait plus qu'a la concatener :
     * si le lanceur refabriquait ces arguments de son cote, l'ecran et le jeu
     * finiraient par ne plus dire la meme chose.
     */
    std::vector<std::string> mame_launch_args() const;
    bool        shows_mechanical() const { return m_switch_mechanical.get_active(); }
    void        set_shows_mechanical(bool on) { m_switch_mechanical.set_active(on); }
    void        set_launch_flags(bool fullscreen, bool integerscale);
    std::string get_emulator_extra_args() const { return m_entry_emu_args.get_text(); }
    // Emis quand une de ces trois options change, pour que la fenetre
    // principale realigne son menu « Launch » sur ce que l'ecran affiche.
    sigc::signal<void>& signal_launch_options_changed() { return m_sig_launch_options; }

    // ── Fonctions en ligne, au-dela de la publication des scores ──────────
    // Chacune commande quelque chose de reel ; aucune n'est decorative.
    bool syncs_automatically()   const { return m_switch_autosync.get_active(); }
    bool shares_play_statistics() const { return m_switch_playstats.get_active(); }
    bool community_enabled()      const { return m_switch_community.get_active(); }

    // La carte de profil compte des favoris et des jeux joues : ils vivent
    // dans la base, que la fenetre principale detient deja. Le panneau ne la
    // possede pas, il l'emprunte.
    void set_database(std::shared_ptr<class DatabaseManager> db) { m_database = std::move(db); }

    // ── Online scores ────────────────────────────────────────────────────
    // Nothing is ever sent without both of these: a name to sign with, and an
    // explicit yes. Sending a score also publishes how long someone played,
    // which is a habit, not a game statistic : it needs asking, not assuming.
    // Interrupteur unique : il gouverne tout, affichage comme envoi. Deux
    // réglages distincts, l'un pour lire et l'autre pour publier, étaient
    // redondants : personne n'active un classement en ligne pour le regarder
    // sans y figurer. Allumé, la pastille s'affiche et les scores partent ;
    // éteint, aucune pastille et aucune requête.
    bool        is_hiscore_enabled() const;
    // Emis apres une connexion ou une deconnexion, pour que la fenetre
    // principale rafraichisse ce qui depend du compte.
    sigc::signal<void>& signal_account_changed() { return m_sig_account_changed; }
    // A appeler quand l'etat de connexion change SANS passer par ce panneau,
    // typiquement apres la restauration de session au demarrage, qui aboutit
    // bien apres la construction de l'interface.
    void refresh_account() { refresh_account_row(); }
    std::string get_hiscore_player() const;
    std::string get_startup_selection() const {
        const std::string id = m_combo_startup.get_active_id();
        return id.empty() ? std::string("last_played") : id;
    }
    std::string get_hiscore_country() const;

    // Gives the player a name to sign with and a country when the config has
    // neither. Demanding that they invent one first is what made the whole
    // feature inert: an empty name silently dropped every score AND every
    // minute of play, with nothing on screen ever saying so. Both stay
    // editable in Settings. Safe to call after load_from_file whether or not
    // it found a file.
    void ensure_hiscore_identity();

    // False until the player has been asked, once, whether to publish. The
    // question is put at first launch rather than assumed either way: leaving
    // it on by default publishes someone's play habits before they have said
    // anything, leaving it off by default hides the feature from everyone who
    // never opens Settings : which is almost everyone.
    bool was_hiscore_asked() const { return m_hiscore_asked; }
    /* La VERSION qui a pose la question du compte, pas un simple oui/non.
     *
     * Un drapeau booleen n'aurait prevenu qu'une fois, pour toujours : le
     * joueur arrive d'une version anterieure, ou qui a repondu « plus tard »
     * il y a trois versions, n'aurait plus jamais rien vu. En gardant la
     * version, chaque mise a jour repose la question a ceux pour qui le
     * probleme dure encore : voir MainWindow::ask_hiscore_account_again, qui
     * ne la repose PAS a un joueur connecte, pour qui il n'y a rien a
     * corriger.
     */
    bool was_account_asked_this_version() const;
    void record_account_answer();
    void record_hiscore_answer(bool publish);

private:
    void on_folder_clicked(Gtk::Entry* entry);
    void on_add_roms_path_clicked();
    void on_remove_roms_path_clicked();
    void refresh_roms_list();
    void on_download_previews_clicked();
    void on_download_titles_clicked();

    // ── Coquille ─────────────────────────────────────────────────────────
    void         build_shell();
    void         add_tab(const std::string& id, const std::string& icon_file,
                         const std::string& label);
    Gtk::Widget* build_page_general();
    Gtk::Widget* build_page_library();
    Gtk::Widget* build_page_random();
    Gtk::Widget* build_page_emulator();
    Gtk::Widget* build_page_online();
    void         on_restore_defaults_clicked();
    void         on_clear_cache_clicked();
    void         on_reset_settings_clicked();
    void         apply_defaults();

    Gtk::HeaderBar m_headerbar;
    Gtk::Box    m_header{Gtk::ORIENTATION_HORIZONTAL, 12};
    Gtk::Label  m_header_title;
    Gtk::Label  m_header_sub;
    Gtk::Button m_btn_close;
    Gtk::Box    m_tabbar{Gtk::ORIENTATION_HORIZONTAL, 8};
    Gtk::Stack  m_pages;
    Gtk::Box    m_footer{Gtk::ORIENTATION_HORIZONTAL, 10};
    Gtk::Button m_btn_restore_defaults;
    Gtk::Button m_btn_cancel;
    Gtk::Button m_btn_save;
    std::vector<Gtk::ToggleButton*> m_tabs_buttons;
    bool m_tab_switching{false};
    sigc::signal<void> m_sig_close;
    sigc::signal<void> m_sig_save;

    /* Quel jeu montrer au demarrage.
     *
     * Cinq strategies, parce qu'elles ne repondent pas a la meme habitude :
     * reprendre sa derniere partie, retrouver son jeu de chevet, se remettre
     * devant son meilleur classement, ou simplement reprendre ou l'on etait.
     */
    Gtk::ComboBoxText m_combo_startup;

    // ROMs paths management
    std::vector<std::string> m_roms_paths;
    Gtk::ScrolledWindow m_scrolled_roms;
    Gtk::TreeView m_treeview_roms;
    Glib::RefPtr<Gtk::ListStore> m_model_roms;
    struct RomCols : public Gtk::TreeModel::ColumnRecord {
        Gtk::TreeModelColumn<Glib::RefPtr<Gdk::Pixbuf>> icon;
        Gtk::TreeModelColumn<Glib::ustring>             path;
        // Le statut est un MARKUP : la pastille et son mot partagent une
        // cellule, faute de quoi une colonne de plus decalerait la colonne
        // des chemins d'une ligne a l'autre.
        Gtk::TreeModelColumn<Glib::ustring>             status;
        RomCols() { add(icon); add(path); add(status); }
    };
    RomCols m_cols_roms;
    Gtk::Button m_button_add_roms;
    Gtk::Button m_button_remove_roms;

    /* La poignee qui regle la hauteur de la liste des dossiers.
     *
     * Le nombre de dossiers de ROMs varie enormement d'une installation a
     * l'autre : cinq lignes suffisent a l'un et en cachent quinze a l'autre.
     * La liste est donc la SEULE chose qui defile dans cet ecran, et sa
     * hauteur se regle a la main. Tirer la poignee agrandit aussi la fenetre,
     * faute de quoi la carte grandirait dans une fenetre figee et ramenerait
     * la barre de defilement qu'on a justement supprimee.
     */
    Gtk::EventBox m_roms_grip;
    int  m_roms_list_height{200};
    int  m_grip_start_height{0};
    int  m_grip_max_height{900};
    Glib::RefPtr<Gtk::GestureDrag> m_grip_drag;
    sigc::connection m_fit_conn;
    void set_roms_list_height(int height);
    void apply_roms_list_height();
    // Ramene la fenetre a la hauteur de la page affichee.
    void fit_to_page();

    // Other entries
    Gtk::Entry m_entry_dat;
    Gtk::Entry m_entry_previews;
    Gtk::Entry m_entry_titles;
    /* Outbox et quarantaine : deux valeurs, plus aucun widget.
     *
     * Elles decrivent ou l'outil de reparation depose ses resultats, donc
     * elles se choisissent dans les onglets qui s'en servent, pas dans un
     * ecran de reglages qui les nommait sans jamais les montrer a l'oeuvre.
     * Le panneau n'en garde que la lecture et l'ecriture de config.json.
     */
    std::string m_outbox_path;
    std::string m_quarantine_path;
    sigc::signal<void> m_sig_open_rom_manager;

    // Boutons
    Gtk::Button m_button_browse_previews;
    Gtk::Button m_button_download_previews;
    Gtk::Button m_button_browse_titles;
    Gtk::Button m_button_download_titles;
    Gtk::Button m_button_browse_fbneo;
    Gtk::Button m_button_download_fbneo;
    // Scan options widgets
    Gtk::CheckButton m_check_recursive;
    Gtk::CheckButton m_check_loose_files;

    /* ── La page Library, vue depuis UN emulateur ─────────────────────────
     *
     * Une barre d'onglets en tete de page, du meme dessin que celle de la
     * fenetre : le choix se voit sans rien ouvrir, chaque marque se
     * reconnait a son logo, et changer d'emulateur coute un clic la ou une
     * liste deroulante en demandait deux. Elle se construit en parcourant
     * emulator_registry(), donc un emulateur ajoute au registre y prend sa
     * place sans qu'une ligne de cet ecran bouge.
     *
     * Les widgets ci-dessus (liste des dossiers, previsualisations, titres,
     * cases de balayage) EDITENT l'emulateur choisi : m_library garde ce que
     * les autres contiennent, et changer d'entree range puis recharge.
     */
    std::vector<std::pair<std::string, Gtk::ToggleButton*>> m_library_tabs;
    /* Les trois sous-titres nomment l'emulateur choisi.
     *
     * Sans cela, « Add the folders that contain your ROMs » reste vrai pour
     * les deux et ne dit jamais lequel on est en train de regler : le
     * selecteur seul, en haut de page, se perd des qu'on a fait defiler. */
    Gtk::Label* m_lbl_lib_roms_sub = nullptr;
    Gtk::Label* m_lbl_lib_art_sub  = nullptr;
    Gtk::Label* m_lbl_lib_scan_sub = nullptr;

    /* ── Les sources d'images, editables ──────────────────────────────────
     *
     * La liste est RECONSTRUITE a chaque ajout, retrait ou deplacement, et
     * jamais pendant la frappe : rebatir les lignes a chaque caractere
     * enleverait le curseur du champ qu'on est en train de remplir. Les
     * champs ecrivent donc directement dans m_art_sources, qui fait foi.
     */
    Gtk::ListBox m_art_sources_list;
    Gtk::Button  m_btn_add_source;
    std::vector<std::string> m_art_sources;
    void art_sources_rebuild();

    /* La carte des genres MAME ne parait que sur l'onglet MAME : catver.ini
     * ne dit rien de FinalBurn Neo, dont les DAT portent deja leur genre. */
    Gtk::Widget* m_lib_catver_card = nullptr;
    std::map<std::string, LibrarySettings> m_library;
    std::string m_library_emu;          // l'entree que les widgets editent
    bool        m_library_switching{false};
    void        library_store_current();
    void        library_show(const std::string& emulator);

    // ── General : comportement, mises a jour, donnees ────────────────────
    Gtk::Switch m_switch_window_state;
    Gtk::Switch m_switch_play_history;
    // Jeu au hasard
    Gtk::ComboBoxText m_combo_random_from;
    Gtk::Switch m_switch_random_hiscore, m_switch_random_originals,
                m_switch_random_unplayed, m_switch_random_launch;
    Gtk::FlowBox m_random_systems_box;
    std::vector<Gtk::CheckButton*> m_random_system_checks;
    std::set<std::string> m_random_systems_saved;   // lu du fichier avant que la liste existe
    Gtk::Switch m_switch_auto_update;
    Gtk::Button m_btn_check_updates;
    Gtk::Label  m_lbl_version;
    Gtk::Box    m_update_state{Gtk::ORIENTATION_HORIZONTAL, 7};
    SettingsUi::Icon m_update_state_icon{"bc-info.svg", 16};
    Gtk::Label  m_update_state_text;
    Gtk::Button m_btn_clear_cache;
    Gtk::Button m_btn_reset_settings;
    void set_update_state(const std::string& text, const std::string& tone);
    void check_launcher_update_async();
    Glib::Dispatcher m_update_done;
    std::mutex       m_update_mutex;
    std::string      m_update_tag;      // vide = pas de nouvelle version
    bool             m_update_failed{false};

    // ── Emulator ─────────────────────────────────────────────────────────
    Gtk::ListBox m_emu_list;
    Gtk::Label   m_lbl_emu_version;
    Gtk::Label   m_lbl_emu_systems;
    Gtk::Label   m_lbl_emu_checked;
    Gtk::Box     m_emu_status_pill{Gtk::ORIENTATION_HORIZONTAL, 8};
    Gtk::Label   m_emu_status_text;
    Gtk::Box     m_exe_state{Gtk::ORIENTATION_HORIZONTAL, 7};
    SettingsUi::Icon m_exe_state_icon{"bc-info.svg", 16};
    Gtk::Label   m_exe_state_text;
    Gtk::Button  m_btn_test_emu;
    Gtk::Button  m_btn_emu_updates;
    Gtk::Label   m_lbl_emu_note;
    // La pastille « Active » de l'en-tete du panneau. Distincte de celle de la
    // liste : un widget ne peut pas etre a deux endroits, et les deux disent
    // la meme chose parce que refresh_emulator_state les ecrit ensemble.
    Gtk::Box     m_emu_head_pill{Gtk::ORIENTATION_HORIZONTAL, 8};
    Gtk::Box     m_emu_head_dot{Gtk::ORIENTATION_HORIZONTAL, 0};
    Gtk::Label   m_emu_head_text;
    Gtk::Switch  m_switch_fullscreen;
    Gtk::Switch  m_switch_integerscale;
    Gtk::Switch  m_switch_mechanical;

    /* ── Les sections repliables de la page Emulator ──────────────────────
     *
     * Meme langage visuel que le volet de details de la fenetre principale :
     * un en-tete qui porte le titre ET un resume, pour que replier une
     * section ne fasse pas disparaitre ce qu'elle disait. La
     * fiche de MAME pese a elle seule une quinzaine d'options : pouvoir la
     * refermer est ce qui rend la page lisible sur une dalle ordinaire.
     *
     * L'etat est garde dans config.json : refaire le meme pliage a chaque
     * ouverture serait un reglage qui ne se souvient de rien.
     */
    struct Section {
        Gtk::Revealer*    body     = nullptr;
        Gtk::Label*       summary  = nullptr;
        SettingsUi::Icon* chevron  = nullptr;
        // Ce que la section dit d'elle-meme une fois fermee. Recalcule a
        // chaque repli : un resume fige mentirait des le premier reglage.
        std::function<std::string()> describe;
    };
    std::map<std::string, Section> m_sections;
    Gtk::Widget* collapsible(const SettingsUi::Card& card, const std::string& key,
                             bool open_by_default,
                             std::function<std::string()> describe);
    void         refresh_sections();

    // ── MAME : la carte de l'executable, jumelle de celle de FBNeo ───────
    Gtk::Entry   m_entry_mame_exe;
    Gtk::Button  m_button_browse_mame;
    Gtk::Box     m_mame_exe_state{Gtk::ORIENTATION_HORIZONTAL, 7};
    SettingsUi::Icon m_mame_exe_state_icon{"bc-info.svg", 16};
    Gtk::Label   m_mame_exe_state_text;
    Gtk::Button  m_btn_test_mame;
    // Les tuiles du bandeau d'identite, au meme endroit et dans le meme
    // dessin que la bande Build / Installed / Last checked de FinalBurn Neo :
    // deux emulateurs qui repondent aux memes questions doivent y repondre a
    // la meme place, sinon l'ecran se lit deux fois.
    Gtk::Label   m_lbl_mame_build;
    Gtk::Label   m_lbl_mame_path;
    Gtk::Label   m_lbl_mame_date;
    Gtk::Label   m_lbl_mame_checked;
    /* Les deux lignes « aucun DAT » / « les mises a jour viennent de la
     * distribution » ne sont vraies que d'un MAME installe en paquet : devant
     * un binaire pose a la main par le joueur, elles mentiraient. */
    Gtk::Widget* m_mame_distro_rows = nullptr;

    /* ── catver.ini : le genre des machines MAME ─────────────────────────
     *
     * MAME n'expose aucun genre. Tant que ce fichier n'est pas charge, les
     * machines MAME n'en ont pas, et la carte doit le DIRE : sans cela le
     * joueur filtre sur « Racing », ne voit que du FinalBurn Neo, et conclut
     * que le filtre est casse.
     */
    Gtk::Entry   m_entry_catver;
    Gtk::Button  m_btn_browse_catver;
    Gtk::Button  m_btn_download_catver;
    Gtk::Label   m_lbl_catver_state;
    /* L'adresse de telechargement, modifiable.
     *
     * Vide veut dire « celle que Bootcade construit pour la version de MAME
     * installee ici » : y figer le resultat du calcul obligerait le joueur a
     * la corriger a la main a chaque mise a jour de MAME.
     */
    Gtk::Entry   m_entry_catver_url;
    Gtk::Button  m_btn_check_catver;
    Gtk::Label   m_lbl_catver_check;
    // L'adresse que le calcul donne aujourd'hui, ou vide si la version de
    // MAME ne se lit pas.
    std::string  catver_url_in_use() const;
    std::string  catver_url_default() const;
    /* La verification part sur un fil : elle enchaine jusqu'a douze requetes
     * HTTP, et les faire sur le fil de l'interface figerait la fenetre le
     * temps que le serveur reponde. */
    void         check_catver_update_async();
    Glib::Dispatcher m_catver_done;
    std::mutex       m_catver_mutex;
    std::string      m_catver_msg;
    std::string      m_catver_found_url;   // vide = rien de plus recent
    void         refresh_catver_state();
    // Relit le fichier retenu et pose les genres. Silencieux quand il n'y a
    // rien a lire : c'est l'etat normal d'une installation neuve.
    void         apply_catver_now(bool announce);
    void         download_catver_clicked();

    // ── Les options, un groupe par emulateur ─────────────────────────────
    // « Start FinalBurn Neo in fullscreen » n'a rien a faire sous MAME : les
    // deux groupes vivent dans la meme carte mais ne s'affichent jamais
    // ensemble.
    Gtk::Widget* m_emu_opt_fbneo = nullptr;
    Gtk::Widget* m_emu_opt_mame  = nullptr;

    /* Les options MAME.
     *
     * Chacune est un interrupteur ; celles qui ont plusieurs valeurs portent
     * en plus un selecteur, grise tant que l'interrupteur est eteint. Le
     * selecteur ne choisit rien tant que l'option n'est pas active : l'un ne
     * va pas sans l'autre, et deux controles independants auraient laisse
     * croire qu'on peut regler une option qu'on n'a pas allumee.
     */
    Gtk::Switch       m_sw_mame_fullscreen;
    Gtk::Switch       m_sw_mame_keepaspect;
    Gtk::Switch       m_sw_mame_intscale;
    Gtk::Switch       m_sw_mame_video;
    Gtk::ComboBoxText m_cb_mame_video;
    Gtk::Switch       m_sw_mame_vsync;
    Gtk::Switch       m_sw_mame_sound;
    Gtk::ComboBoxText m_cb_mame_sound;
    Gtk::Switch       m_sw_mame_volume;
    Gtk::ComboBoxText m_cb_mame_volume;
    Gtk::Switch       m_sw_mame_skipinfo;
    Gtk::Switch       m_sw_mame_hiscore;
    Gtk::Switch       m_sw_mame_autofire;
    Gtk::Switch       m_sw_mame_autosave;
    Gtk::Switch       m_sw_mame_snapdir;
    Gtk::Entry        m_entry_mame_snapdir;
    Gtk::Button       m_button_browse_snapdir;
    Gtk::Switch       m_sw_mame_snapname;
    Gtk::ComboBoxText m_cb_mame_snapname;
    Gtk::Entry        m_entry_mame_args;
    // Batit la carte « Options » de MAME. Sortie de build_page_emulator
    // parce qu'elle pese a elle seule autant que le reste de la page.
    Gtk::Widget* build_mame_options();
    // Grise (ou degrise) chaque selecteur selon son interrupteur.
    void         sync_mame_option_sensitivity();
    // Ce que l'ecran affiche, ecrit dans / relu de l'objet « mame_options ».
    void         load_mame_options(const nlohmann::json& j);
    void         save_mame_options(nlohmann::json& j) const;
    // La page Emulateur change de contenu selon la ligne choisie a gauche.
    // Ces morceaux sont donc gardes pour etre remplis ou caches ensuite.
    Gtk::Box*    m_emu_head_txt   = nullptr;
    Gtk::Box*    m_emu_head_logo  = nullptr;
    Gtk::Widget* m_emu_exe_frame  = nullptr;
    Gtk::Widget* m_emu_stats_row  = nullptr;
    // La bande de MAME vit dans le meme bandeau que celle de FinalBurn Neo :
    // seuls les intitules different, d'ou deux rangees et non une seule
    // qu'on reecrirait a chaque changement d'emulateur.
    Gtk::Widget* m_emu_mame_stats_row = nullptr;
    Gtk::Widget* m_emu_upd_row    = nullptr;
    // MAME ne se choisit pas et ne se telecharge pas : sa carte remplace
    // celle de l'executable plutot que de s'y ajouter.
    Gtk::Widget* m_emu_mame_frame = nullptr;
    Gtk::Label   m_lbl_mame_exe;
    // Interroger le binaire coute un processus : une fois par ouverture de
    // la fenetre suffit, personne n'installe MAME pendant qu'il la regarde.
    // `mutable` : mame_executable() est const et doit pouvoir repondre avant
    // que la fenetre des reglages n'ait jamais ete ouverte, donc avant que
    // quoi que ce soit n'ait sonde le systeme.
    mutable bool         m_mame_probed   = false;
    mutable std::string  m_mame_exe;      // vide = MAME absent du systeme
    // Le bandeau ne porte qu'une pastille alors que la page decrit deux
    // emulateurs : elle doit savoir duquel elle parle.
    bool         m_emu_shown_mame = false;
    void show_emulator_page(size_t index);
    void refresh_mame_state();
    void refresh_emulator_pill();
    Gtk::Entry   m_entry_emu_args;
    sigc::signal<void> m_sig_launch_options;
    void refresh_emulator_state();
    void check_emulator_update_async();
    /* MAMEdev ne publie que des sources pour Linux : cette verification ne
     * peut que constater un ecart et dire ou regarder. Elle partage le
     * dispatcher et le libelle de FinalBurn Neo, puisque le bandeau n'en
     * montre qu'un a la fois. */
    void check_mame_update_async();
    Glib::Dispatcher m_emu_update_done;
    std::mutex       m_emu_mutex;
    std::string      m_emu_update_msg;
    std::string      m_emu_update_tone;

    // ── Online : reseau ──────────────────────────────────────────────────
    Gtk::Box    m_net_row{Gtk::ORIENTATION_HORIZONTAL, 11};
    Gtk::Box    m_net_dot{Gtk::ORIENTATION_HORIZONTAL, 0};
    Gtk::Label  m_net_title;
    Gtk::Label  m_net_sub;
    Gtk::Button m_btn_test_net;
    /* Le volet « Your Profile ».
     *
     * Il ne montre QUE ce que le lanceur sait reellement : l'identite que
     * porte le jeton (nom, avatar, pays) et ce que la couche de scores compte
     * localement. La maquette y ajoute des succes et des compteurs de parties ;
     * aucun service ne les expose aujourd'hui, et les dessiner en dur ferait
     * d'une page de reglages une vitrine mensongere.
     */
    Gtk::Box    m_profile_signed{Gtk::ORIENTATION_VERTICAL, 14};
    Gtk::Image  m_profile_avatar;
    Gtk::Label  m_profile_name;
    Gtk::Label  m_profile_country;
    Gtk::Box    m_profile_state{Gtk::ORIENTATION_HORIZONTAL, 8};
    Gtk::Label  m_profile_state_text;
    Gtk::Box    m_profile_dot{Gtk::ORIENTATION_HORIZONTAL, 0};
    Gtk::Label  m_profile_queued;
    Gtk::Box    m_profile_empty{Gtk::ORIENTATION_VERTICAL, 10};
    Gtk::Button m_btn_view_profile;
    // Trois compteurs, tous LOCAUX et tous verifiables : les classements
    // personnels connus, les jeux effectivement lances, les favoris. Le
    // service n'expose ni succes ni total de parties, donc rien de tel n'est
    // affiche.
    Gtk::Label  m_stat_hiscores;
    Gtk::Label  m_stat_played;
    Gtk::Label  m_stat_favorites;
    std::shared_ptr<class DatabaseManager> m_database;
    void refresh_profile_stats();
    void probe_network_async();
    void set_network_state(int state);   // 0 inconnu, 1 en ligne, 2 hors ligne
    Glib::Dispatcher  m_net_done;
    std::atomic<int>  m_net_state{0};
    // Un fil detache survit a ce qu'il ne controle pas : le verrou rend
    // indivisibles le test du drapeau et l'emission, exactement comme dans
    // la fenetre principale.
    struct AliveToken {
        std::mutex mutex;
        bool       alive = true;
    };
    std::shared_ptr<AliveToken> m_alive{std::make_shared<AliveToken>()};

    // Online scores
    Gtk::Entry       m_entry_hiscore_player;
    // Un vrai interrupteur, pas une case a cocher : c'est un etat marche /
    // arret pour toute une fonctionnalite, pas une option parmi d'autres.
    Gtk::Switch m_switch_hiscore;
    Gtk::Switch m_switch_community;
    Gtk::Switch m_switch_autosync;
    Gtk::Switch m_switch_playstats;

    // Le compte remplace l'ancien champ de nom libre : il n'y a plus qu'un
    // seul nom, celui du compte Bootcade, et il ne se saisit pas ici.
    Gtk::Image  m_account_avatar;
    Gtk::Label  m_account_name;      // nom et drapeau
    Gtk::Label  m_account_sub;       // « Connecte » ou l'invitation a le faire
    Gtk::Button m_button_account;
    Gtk::Button m_button_manage_account;
    Gtk::Box    m_account_row{Gtk::ORIENTATION_HORIZONTAL, 14};
    Gtk::Box    m_account_text{Gtk::ORIENTATION_VERTICAL, 3};
    // La publication n'a de sens qu'avec un compte : l'interrupteur suit donc
    // l'etat de connexion, et un texte dit pourquoi il est grise.
    Gtk::Label  m_hiscore_hint;
    void refresh_account_row();
    // Persisted as "hiscore_asked": the switch alone cannot tell "off because
    // they said no" from "off because nobody has asked yet", and asking again
    // every launch would be its own bug.
    bool        m_hiscore_asked{false};
    // Persiste sous "hiscore_account_asked_version". Vide = jamais posee,
    // donc ecrite avant que la question ne parle du compte.
    std::string m_account_asked_version;
    // A typed field with completion rather than a 249-row dropdown: scrolling
    // to your own country in an alphabetical list of every country on earth is
    // slower than typing three letters of it.
    Gtk::Entry m_entry_hiscore_country;
    struct CountryCols : public Gtk::TreeModel::ColumnRecord {
        Gtk::TreeModelColumn<Glib::ustring> name;
        Gtk::TreeModelColumn<Glib::ustring> code;
        CountryCols() { add(name); add(code); }
    };
    CountryCols                       m_country_cols;
    Glib::RefPtr<Gtk::ListStore>      m_country_model;
    Glib::RefPtr<Gtk::EntryCompletion> m_country_completion;
    void build_country_completion();
    void set_hiscore_country(const std::string& code);

    // Appearance / language
    Gtk::ComboBoxText m_combo_theme;
    Gtk::ComboBoxText m_combo_language;
    sigc::signal<void, Glib::ustring> m_sig_theme_changed;
    sigc::signal<void, Glib::ustring> m_sig_language_changed;
    sigc::signal<void, bool>          m_sig_hiscore_toggled;
    sigc::signal<void>                m_sig_account_changed;
    bool m_suppress_appearance_signals{false};
};
