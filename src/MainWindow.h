// src/MainWindow.h
#pragma once

#include <gtkmm.h>
#include <string>
#include <thread>
#include "Game.h"
#include "SettingsPanel.h"
#include "HiscoreClient.h"
#include "ModelColumns.h"
#include "SettingsUi.h"
#include "ROMScanDialog.h"
#include "RomManagerWindow.h"
#include "ThumbnailDownloader.h"
#include "DatabaseManager.h"
#include "DATUpdateDialog.h"
#include "ConfirmationDialog.h"
#include "FilterCache.h"
#include "ControllerConfig.h"
#include "ControllerManager.h"
#include <atomic>
#include <functional>
#include <memory>
#include <map>
#include <deque>
#include <ctime>
#include <set>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <cstdint>

class ControllerDialog;

class MainWindow : public Gtk::Window {
public:
    /* Ouverture directe d'une fenetre, pour l'automatisation.
     *
     * Sans elle, photographier « Controller Configuration » demandait de
     * piloter le menu avance a la souris ou au clavier dans un ecran
     * virtuel, ce qui echouait et rendait toute verification visuelle
     * impossible. Une application de bureau doit pouvoir s'ouvrir sur un
     * ecran precis quand on le lui demande.
     *
     *   bootcade --open=controller
     *   bootcade --open=settings
     *   bootcade --open=roms
     */
    void open_named_window(const std::string& which);


    MainWindow(std::shared_ptr<DatabaseManager> database,
               std::function<void(double, const std::string&)> progress_callback = nullptr,
               // Par valeur : le catalogue est cede, pas recopie. A 30 000 jeux
               // la copie coutait deja cher, et le catalogue va doubler.
               std::vector<Game> preloaded_games = {});
    virtual ~MainWindow();

private:
    // === Private Methods ===
    Glib::RefPtr<Gdk::Pixbuf> get_status_icon(const std::string& status);
    void on_game_selected();
    void show_game_details(const Gtk::TreeModel::Row& row); // populate the detail dock
    void on_play_clicked();
    void on_download_art_clicked();
    /* Reglages et manettes : de VRAIES fenetres, pas des boites attachees.
     *
     * Elles etaient creees avec la fenetre principale comme parent et
     * ouvertes par run() : transient_for les collait a elle, et run() les
     * rendait modales. On ne pouvait donc ni les deplacer librement, ni les
     * envoyer sur un second ecran, ni toucher au launcher pendant qu'elles
     * etaient ouvertes.
     *
     * Elles vivent maintenant sur le tas, sans parent, non modales, et
     * repondent par signal plutot qu'en bloquant. Les pointeurs sont
     * conserves pour reveler une fenetre deja ouverte au lieu d'en empiler
     * une seconde.
     */
    Gtk::Dialog*      m_settings_win   = nullptr;
    Gtk::Window*      m_controller_win = nullptr;

    void on_settings_clicked();
    void on_hide();
    void on_quit();
    
    // Menu handlers
    void on_export_game_list();
    void on_fbneo_menu();
    /* Vrai pendant que le menu « Launch » est realigne sur l'ecran des
     * reglages. Sans lui, cocher l'entree du menu par programme rappelait son
     * gestionnaire, qui ECRIVAIT la configuration : « Restore Default
     * Settings » puis « Cancel » laissait donc le plein ecran deja enregistre
     * sur le disque, alors que Cancel promet le contraire. */
    bool m_syncing_launch_menu{false};
    void on_video_settings();
    void on_audio_settings();
    void on_input_settings();
    void on_fullscreen_mode();
    void on_integerscale_mode();
    void on_arcade_mode();
    void on_console_mode();
    void on_all_systems();
    // Le bouton « de » : tire un jeu selon Settings > Library > Random game,
    // le selectionne dans la liste, et le lance si le reglage le demande.
    void on_random_game_clicked();
    // Selectionne une ligne du modele et la montre dans la vue courante.
    void reveal_model_row(const Gtk::TreeModel::iterator& it);
    void on_rescan_roms();
    void on_verify_roms();
    void on_find_duplicate_roms();
    void on_rom_manager();
    void on_show_available_only();
    void on_show_missing_roms();
    void on_rom_info();
    void on_about_fbneo();
    void on_controls_help();
    void on_about_launcher();
    void on_download_latest_fbneo();
    void on_generate_dat_files();
    void update_status_bar_stats();
    void on_start_scan_clicked();
    // ROM Management › Library › Scan ROMs : the library of the DAT group's
    // emulator, whatever the main window is showing. Same confirmation as the
    // header button.
    void on_library_scan_requested(const std::string& emulator);
    // The FinalBurn Neo per-file scan, after its confirmation.
    void scan_fbneo_library();
    void on_scan_dialog_complete();
    void on_scan_go_background();
    void set_scan_status(const Glib::ustring& text, SettingsUi::State state);
    bool on_scan_bg_poll();        // called by Glib::signal_timeout
    
    // Artwork download methods
    void show_download_progress(const std::string& filename, int current, int total, double percentage);
    void hide_download_progress();
    void on_download_previews_clicked();
    void on_download_titles_clicked();
    void on_download_cancel_clicked();
    
    // Settings dialog management
    sigc::signal<void> m_close_settings_signal;
    
    // ROM scan methods
    void on_scan_progress();
    void on_scan_finished();
    // `emulator` : whose library `roms_paths` is (see ROMScanDialog). A scan
    // asked for while another runs is queued and started once it is over.
    void start_scan_thread(const std::vector<std::string>& roms_paths,
                           const std::string& emulator = "fbneo");
    std::vector<std::string> m_pending_scans;   // emulators waiting for their scan
    void on_update_dat_clicked();
    // `emulator` : the one whose games are reloaded (see DATUpdateDialog) ;
    // empty reloads every emulator.
    void confirm_update_dat(const std::string& emulator);
    // The actual reload, with no confirmation dialog of its own : for callers
    // (like the post-FBNeo-download chain) that already got the user's OK a
    // moment ago and would otherwise show a second, easy-to-dismiss prompt
    // that silently drops the database refresh if cancelled.
    void do_update_dat(const std::string& emulator = "");
    void run_update_dat_once(const std::string& emulator);
    bool m_dat_update_running = false, m_dat_update_again = false;
    // What the queued reload covers : two requests for different emulators
    // queue a reload of both.
    std::string m_dat_update_again_emulator;
    void update_fbneo_config(const std::vector<std::string>& roms_paths);
    void set_fbneo_system(const std::string& system);
    // In-game screenshot capture: FBNeo's own unmodified F6 hotkey already
    // writes timestamped PNGs to its screenshots folder : after the session
    // ends, offer to use any capture(s) as this game's Title/Preview artwork.
    static bool normalize_artwork_file(const std::string& path, int target_w, int target_h);
    static std::string get_fbneo_screenshots_dir();
    static std::vector<std::string> find_session_screenshots(const std::string& fbneo_rom_name, std::time_t launch_time);
    void on_screenshot_batch_found();
    // Filter TreeView handlers
    void on_filter_selection_changed();
    void populate_filter_tree();
    void apply_tree_filters();
    void update_filter_counts();
    void filter_games();
    void filter_games_async();
    void filter_games_simple();
    void apply_filters();
    void append_game_rows(const std::vector<const Game*>& games);
    const std::string& search_blob(size_t idx);
    std::vector<std::string> m_search_blobs;      // lower-cased haystack per cached game
    void load_filter_cache();
    void save_filter_cache();
    void configure_columns();
    std::string escape_markup(const std::string& text);
    Glib::RefPtr<Gdk::Pixbuf> get_filter_icon(const std::string& category);

    // === Widgets ===
    SettingsPanel m_settings_panel;
    
    // === Database ===
    std::shared_ptr<DatabaseManager> m_database;

    // === Header bar (modern titlebar) ===
    Gtk::HeaderBar    m_headerbar;
    Gtk::Button       m_btn_settings; // gear button in the header
    Gtk::MenuButton   m_menu_button;
    Gtk::Menu         m_app_menu;   // hamburger popup hosting the top-level menus
    Gtk::ToggleButton m_btn_favorites; // ★ header toggle: show favourites only

    // ── Compte, dans la barre du haut ────────────────────────────────────
    // L'etat de connexion doit se voir sans ouvrir les reglages : c'est lui
    // qui decide si les scores partent, et un joueur ne doit pas avoir a
    // chercher pour le savoir.
    Gtk::MenuButton m_btn_account;
    Gtk::Box        m_account_face{Gtk::ORIENTATION_HORIZONTAL, 6};
    Gtk::Image      m_account_avatar;
    Gtk::Label      m_account_label;
    Gtk::Label      m_account_state;   // « En ligne » / « Hors ligne »
    Gtk::Menu       m_account_menu;      // connecte
    Gtk::Menu       m_account_menu_out;  // deconnecte : une seule entree
    Gtk::MenuItem   m_mi_signin;
    Gtk::MenuItem   m_mi_profile, m_mi_leaderboard, m_mi_settings, m_mi_signout;
    // Emis depuis le fil de restauration : une interface ne se touche que
    // depuis le fil principal, et un Dispatcher est fait pour ce passage.
    Glib::Dispatcher m_account_restored;
    // Emis a la FIN de la restauration, qu'elle ait abouti ou non. Le succes
    // a deja son signal ; celui-ci dit seulement que la question est tranchee,
    // ce qui est le seul moment ou le bandeau « pas de compte » peut parler
    // sans risque de mentir.
    Glib::Dispatcher m_account_settled;
    /* Tant que la restauration court, l'etat du compte est INCONNU, pas
     * « absent ». Le bandeau attend : une session relue depuis le disque doit
     * encore etre revalidee par le serveur, et cela prend le temps d'un
     * aller-retour reseau pendant lequel signed_in() repond non.
     */
    bool             m_account_pending{false};
    void build_account_button();
    /* TROIS etats INDEPENDANTS, jamais fusionnes.
     *
     * Les fusionner en un seul « etat global » etait une faute : on ne
     * pouvait plus exprimer « machine connectee, joueur sans compte,
     * classements desactives », qui est un usage parfaitement normal et
     * durable de Bootcade, ni « reseau coupe mais donnees locales connues ».
     *
     * Regle d'architecture : Bootcade est un launcher LOCAL. Le reseau, le
     * compte et les classements sont trois couches optionnelles et
     * independantes. Aucune n'est un prerequis pour jouer.
     *
     * NET est deduit d'une sonde dediee, JAMAIS de la presence d'une session
     * ni du reglage des classements : un compte absent ne dit rien de la
     * connectivite, et un joueur qui desactive les classements n'est pas
     * hors ligne.
     */
    enum class NetState { Unknown, Online, Offline };
    std::atomic<NetState> m_net{NetState::Unknown};

    NetState net_state()   const { return m_net.load(); }
    bool     is_online()   const { return m_net.load() == NetState::Online; }
    // Definis dans le .cpp : l'en-tete ne connait pas BootcadeAuth.
    bool     has_account() const;
    bool     hiscores_on() const;

    // La sonde vit sur un fil : l'interface ne se touche que par ce relais.
    Glib::Dispatcher m_online_state_changed;
    // La session est morte sans que le joueur l'ait demande : voir
    // BootcadeAuth::set_session_lost_handler.
    Glib::Dispatcher m_session_lost;

    void apply_online_state();

    void refresh_account_button();
    void open_web(const std::string& path);
    bool m_show_favorites_only = false;
    // Set while populate_filter_tree() rebuilds the sidebar. Clearing the model
    // makes GTK walk the selection down the surviving rows, emitting a
    // selection-changed for each : which the handler would mistake for the user
    // clicking those filters.
    bool m_populating_filters = false;
    bool m_suppress_fav_toggle = false;
    void set_favorites_only(bool on);
    // Language is chosen in Settings only : it is not a day-to-day action.
    void on_language_selected(const std::string& code);
    
    // File Menu
    Gtk::MenuItem m_menu_file;
    Gtk::Menu m_submenu_file;
    Gtk::MenuItem m_menu_item_settings;
    Gtk::MenuItem m_menu_item_export_game_list;
    Gtk::MenuItem m_menu_item_refresh_hiscores;
    Gtk::MenuItem m_menu_item_quit;
    
    // Emulator Menu
    Gtk::MenuItem m_menu_emulator;
    Gtk::Menu m_submenu_emulator;
    Gtk::MenuItem m_menu_item_fbneo_menu;
    Gtk::MenuItem m_menu_item_video_settings;
    Gtk::MenuItem m_menu_item_audio_settings;
    Gtk::MenuItem m_menu_item_input_settings;
    Gtk::CheckMenuItem m_menu_item_fullscreen_mode;
    Gtk::CheckMenuItem m_menu_item_integerscale_mode;
    Gtk::MenuItem m_menu_item_download_latest_fbneo;
    Gtk::MenuItem m_menu_item_generate_dat_files;
    
    // Filter Menu
    Gtk::MenuItem m_menu_filter;
    Gtk::Menu m_submenu_filter;
    Gtk::MenuItem m_menu_item_arcade_mode;
    Gtk::MenuItem m_menu_item_console_mode;
    Gtk::MenuItem m_menu_item_all_systems;
    Gtk::MenuItem m_menu_item_show_available_only;
    Gtk::MenuItem m_menu_item_show_missing_roms;
    
    // ROMs Menu
    Gtk::MenuItem m_menu_roms;
    Gtk::Menu m_submenu_roms;
    Gtk::MenuItem m_menu_item_rescan_roms;
    Gtk::MenuItem m_menu_item_update_dat;
    Gtk::MenuItem m_menu_item_verify_roms;
    Gtk::MenuItem m_menu_item_rom_info;
    Gtk::MenuItem m_menu_item_find_duplicates;
    Gtk::MenuItem m_menu_item_rom_manager;

    // Help Menu
    Gtk::MenuItem m_menu_help;
    Gtk::Menu m_submenu_help;
    Gtk::MenuItem m_menu_item_about_fbneo;
    Gtk::MenuItem m_menu_item_controls_help;
    Gtk::MenuItem m_menu_item_about_launcher;

    // === Toolbar ===
    Gtk::Box m_main_box{Gtk::ORIENTATION_VERTICAL};
    Gtk::Box m_toolbar_container{Gtk::ORIENTATION_VERTICAL};
    Gtk::Box m_toolbar_row1{Gtk::ORIENTATION_HORIZONTAL};
    Gtk::Box m_toolbar_row2{Gtk::ORIENTATION_HORIZONTAL};
    Gtk::Button m_toolbar_play;   // libelle pose dans le constructeur, sinon non traduit
    Gtk::Button m_button_scan{"Scan ROMs"}; // Button to scan for ROMs
    Gtk::Button m_btn_random;               // le « de » : un jeu au hasard
    Gtk::Button m_button_update_dat{"Update DAT"}; // Button to update DAT database
    // --- Portee par emulateur ------------------------------------------
    //
    // Un encart au-dessus de l'arbre des filtres montre le catalogue courant ;
    // le clic ouvre une modale d'une carte par emulateur. L'encart vit hors de
    // l'arbre exprès : un selecteur qui serait une ligne de l'arbre et dont le
    // clic le reconstruit serait reentrant, ce que populate_filter_tree evite
    // deja a grand-peine.
    //
    // m_active_emulator vide veut dire « tous » : c'est une portee, pas un
    // filtre, et elle ne figure donc pas dans m_active_filters.
    Gtk::Button  m_btn_emu_picker;
    Gtk::Box     m_emu_picker_box{Gtk::ORIENTATION_HORIZONTAL, 10};
    Gtk::Image   m_emu_picker_logo;
    Gtk::Label   m_emu_picker_count;
    std::string  m_active_emulator;          // "" = tous les catalogues

    // Plusieurs catalogues peuplent la liste a la fois. Retenu plutot que
    // recalcule : les vues le demandent une fois par ligne.
    bool         m_multi_emulator = false;
    // La colonne « Emulator » de la table, gardee pour pouvoir la cacher :
    // la retrouver par son titre la perdrait a la premiere traduction.
    Gtk::TreeViewColumn* m_emulator_column = nullptr;
    // Vrai quand une ligne peut venir de l'un ou l'autre catalogue : c'est le
    // seul cas ou dire d'ou elle vient apprend quelque chose. Devant un seul
    // catalogue, la marque ne serait que du bruit repete a chaque ligne.
    bool show_emulator_marks() const {
        return m_active_emulator.empty() && m_multi_emulator;
    }

    // Demande son verdict a MAME sur la collection et le reporte dans le
    // catalogue. Long : plusieurs minutes sur un disque externe.
    // Les deux catalogues reunis, dans l'ordre ou l'interface les montre.
    // A utiliser partout ou m_cached_games est reconstruit : la base ne rend
    // que les jeux FBNeo, et recharger sans cette fusion faisait disparaitre
    // toutes les machines MAME de la liste.
    std::vector<Game> load_all_catalogs();
    void run_mame_audit();
    void build_emulator_picker();
    void refresh_emulator_picker();
    void on_emulator_picker_clicked();
    void set_active_emulator(const std::string& id);
    // Combien de jeux chaque emulateur apporte, catalogue courant en main.
    std::map<std::string, int> emulator_counts() const;
    bool emulator_in_scope(const Game& g) const {
        return m_active_emulator.empty() || g.emulator == m_active_emulator;
    }

    std::vector<Game> m_cached_games; // Cache for games (legacy, kept for compatibility)
    Gtk::Entry m_search_entry; // Search entry for filtering games
    // MAMEUI-style filter panel with TreeView
    Gtk::ScrolledWindow m_scrolled_filters;

    // Pied de la colonne de gauche : version du launcher et etat de
    // l'emulateur, visibles en permanence. Ces deux informations vivaient
    // dans les reglages et dans une boite « A propos », donc nulle part
    // pour qui ne les cherche pas : « FBNeo est-il pret ? » est pourtant la
    // premiere question quand un lancement echoue.
    Gtk::Box   m_sidebar_box{Gtk::ORIENTATION_VERTICAL};
    Gtk::Box   m_sidebar_foot{Gtk::ORIENTATION_VERTICAL, 2};
    Gtk::Label m_lbl_app_version;
    Gtk::Label m_lbl_emu_state;
    void refresh_emu_state();

    /* Largeur du volet de details, proportionnelle a la fenetre.
     *
     * Avec une largeur minimale fixe, un ecran de 3440 px donnait 460 px au
     * volet et 2700 a la liste : le volet paraissait etrangle alors que la
     * place ne manquait pas. La cible est d'environ un quart de la fenetre,
     * bornee pour rester lisible sur un petit ecran et ne pas devenir
     * absurde sur un tres grand.
     */
    /* Le volet a TROIS zones verticales, et une seule grandit.
     *
     *   contenu compact  (artwork, titre, cartes)   -> hauteur du contenu
     *   espace vide flexible                        -> absorbe le reste
     *   barre Play                                  -> ancree en bas
     *
     * Les cartes ne s'etirent JAMAIS : « Game information » devenait absurde
     * en hauteur des qu'on agrandissait la fenetre, parce qu'elle absorbait
     * l'espace au lieu de le laisser au vide. La barre Play vit hors de la
     * zone defilante : a 1440 px de haut elle doit etre visible sans avoir a
     * faire defiler quoi que ce soit.
     */
    Gtk::Box m_dock_root{Gtk::ORIENTATION_VERTICAL, 0};

    /* Selection automatique au demarrage.
     *
     * « Select a game to play » sur un ecran qui contient 29 000 jeux ne
     * dit rien d'utile et laisse le tiers droit de la fenetre vide. Le
     * launcher choisit donc un jeu, selon une strategie que le joueur
     * regle. Toute strategie qui ne rend rien retombe sur le premier jeu
     * disponible : il ne doit JAMAIS rester vide quand la bibliotheque ne
     * l'est pas.
     */
    bool m_startup_selection_done = false;
    void select_startup_game();
    std::string m_last_selected_rom;      // strategie « dernier consulte »
    std::string m_last_selected_system;   // le nom seul ne designe pas un jeu
    // Et l'emulateur avec : sans lui, le repli ci-dessus renvoyait une machine
    // MAME vers FinalBurn Neo, qui ne la connait pas.
    std::string m_last_selected_emulator;

    void update_dock_width();
    int  m_last_alloc_width = 0;
    Gtk::TreeView m_treeview_filters;
    Glib::RefPtr<Gtk::TreeStore> m_model_filters;
    
    // Filter TreeView columns
    class FilterColumns : public Gtk::TreeModel::ColumnRecord {
    public:
        FilterColumns() { add(m_col_icon); add(m_col_name); add(m_col_type); add(m_col_value); add(m_col_count); }
        Gtk::TreeModelColumn<Glib::RefPtr<Gdk::Pixbuf>> m_col_icon;
        Gtk::TreeModelColumn<Glib::ustring> m_col_name;
        Gtk::TreeModelColumn<Glib::ustring> m_col_type;  // "system", "manufacturer", etc.
        Gtk::TreeModelColumn<Glib::ustring> m_col_value; // actual filter value
        Gtk::TreeModelColumn<int> m_col_count; // number of games
    };
    FilterColumns m_filter_columns;
    
    // Current active filters
    std::map<std::string, std::string> m_active_filters;

    // === FBNeo update banner ===
    // Shown only if a startup check finds our fork's "latest" release has moved
    // past whatever build the user last downloaded through this launcher // never shown when we don't know the currently-installed build's provenance
    // (e.g. they pointed Settings at some FBNeo they already had), to avoid a
    // false-positive nag.
    Gtk::InfoBar  m_fbneo_update_infobar;
    Gtk::Label    m_fbneo_update_label;
    Glib::Dispatcher m_fbneo_update_dispatcher;
    std::string   m_fbneo_update_sha;   // written by the worker thread, read after dispatch
    std::string   m_fbneo_update_tag;
    std::string   m_fbneo_update_published_at;
    // ── Sorting ────────────────────────────────────────────────────────
    // Applied to the filtered vector before the model is filled, so all three
    // views (table, list, grid) inherit the same order. Sorting the TreeView's
    // own columns would only ever reorder the table.
    //
    // "Default" is not "unsorted": it is the order the DAT files give, grouped
    // by system, and it is what the catalogue has always shown. Replacing it
    // with alphabetical would silently change the shelf a user knows.
    enum class SortMode { Default, Name, Year, YearAsc, RecentlyPlayed, Highscore };
    SortMode m_sort_mode = SortMode::Default;
    Gtk::ComboBoxText m_combo_sort;
    void sort_games(std::vector<const Game*>& games);

    // ── Online scores ──────────────────────────────────────────────────
    // Chaque appel réseau tourne sur un fil détaché qui survit à ce qu'il ne
    // contrôle pas : quitter pendant une requête laissait un ouvrier réveiller
    // un Glib::Dispatcher dont le tube était déjà fermé, puis lire des membres
    // détruits.
    //
    // Un simple drapeau atomique ne suffisait pas : entre le test et l'envoi,
    // la fenêtre peut disparaître. Le verrou rend les deux indivisibles, et le
    // destructeur le prend aussi. Détenu par shared_ptr pour qu'un ouvrier
    // puisse encore le lire une fois MainWindow détruite.
    struct AliveToken {
        std::mutex mutex;
        bool       alive = true;
    };
    std::shared_ptr<AliveToken> m_alive_token{std::make_shared<AliveToken>()};
    // Which games the service can rank, fetched once at startup. Read from
    // the GTK thread while the worker may still be writing it, hence the
    // mutex : the list is small and consulted once per visible row.
    std::set<std::string> m_hiscore_supported;
    std::mutex            m_hiscore_supported_mutex;
    Glib::Dispatcher      m_hiscore_supported_dispatcher;
    // Un chargement unique au démarrage : jeux classables ET tous les
    // classements : plutôt qu'une requête par jeu sélectionné. `announce`
    // fait parler la barre d'état : le joueur qui demande un rafraîchissement
    // accepte l'attente, mais il doit voir qu'il se passe quelque chose,
    // sinon il relance.
    void refresh_hiscore_data_async(bool announce);
    // Le oui explicite, demande une seule fois au premier lancement.
    void ask_hiscore_optin();
    // Repose la question a ceux qui avaient repondu a sa version d'avant les
    // comptes, qui promettait qu'aucun compte n'etait necessaire.
    void ask_hiscore_account_again();
    // Affiche, ou retire, le rappel permanent « scores actives, pas de
    // compte » selon l'etat courant.
    void refresh_hiscore_nudge();
    // One-time offer to re-read the DAT files after a schema migration left a
    // new column empty (see DatabaseManager::needsDatResync).
    void ask_dat_resync();
    void ask_mame_dat_resync();
    void on_hiscore_supported_ready();
    void on_hiscore_refresh_done();
    Glib::Dispatcher m_hiscore_refresh_dispatcher;
    std::atomic<bool> m_hiscore_refreshing{false};
    std::mutex  m_hiscore_status_mutex;
    std::string m_hiscore_status;
    bool game_ranks_online(const std::string& system, const std::string& game);

    // Leaderboard of whatever game the detail dock is showing. The player
    // clicks through a list faster than the network answers, so each request
    // carries a sequence number and a late reply for a game they have already
    // left is dropped rather than painted over the current one.
    Glib::Dispatcher m_hiscore_top_dispatcher;
    std::mutex       m_hiscore_top_mutex;
    unsigned         m_hiscore_seq = 0;
    unsigned         m_hiscore_seq_done = 0;
    std::vector<HiscoreClient::Entry> m_hiscore_top;
    // Non-empty when what is on screen came from the cache: the date it was
    // fetched, shown to the player so old figures are never passed off as live.
    std::string m_hiscore_top_stale;
    void fetch_hiscore_top_async(const std::string& system, const std::string& game);
    void on_hiscore_top_ready();
    // Peint le classement depuis le cache local, sans réseau. C'est ce qui
    // rend la sélection d'un jeu instantanée.
    void show_cached_board(const std::string& system, const std::string& game);
    void render_board(const std::vector<HiscoreClient::Entry>& rows,
                      const std::string& stale);

    // Submission. The watcher thread that already waits on the emulator does
    // the sending too : it is the only place that knows a session just ended.
    // The outcome is queued here and shown by the infobar on the GTK thread.
    Gtk::InfoBar     m_hiscore_infobar;
    Gtk::Label       m_hiscore_infobar_label;
    // Reponse du bouton « Sign in » du bandeau. Distincte de GTK_RESPONSE_*
    // pour ne pas se confondre avec la croix de fermeture.
    static constexpr int kHiscoreSignIn = 1;
    // Le bouton lui-meme, garde pour pouvoir le cacher : un bandeau qui
    // annonce une publication reussie tout en proposant de se connecter fait
    // douter le joueur de son propre compte.
    Gtk::Button*     m_hiscore_signin_button{nullptr};
    // Le bandeau sert a deux choses : le resultat d'une partie, qui se ferme
    // et ne revient pas, et l'etat « pas de compte », qui doit revenir tant
    // qu'il est vrai. Ce drapeau dit lequel des deux est affiche.
    bool             m_hiscore_nudge_shown{false};
    std::mutex       m_hiscore_result_mutex;
    // Le texte, et le seul fait que le bandeau ne peut pas deduire : ce
    // message-ci a-t-il un geste de connexion a proposer. Une session expiree
    // repond oui alors que le jeton est toujours la, donc interroger
    // BootcadeAuth au moment de l'affichage donnerait la mauvaise reponse.
    struct HiscoreNotice {
        // Implicite a dessein : la grande majorite des messages n'ont rien a
        // proposer, et les ecrire tous avec un « , false » noierait les trois
        // qui, eux, portent un geste.
        HiscoreNotice(std::string m, bool offer = false)
            : message(std::move(m)), offer_signin(offer) {}
        std::string message;
        bool        offer_signin;
    };
    std::deque<HiscoreNotice> m_hiscore_results;
    // Le jeu dont le classement vient de changer, à recharger sur le fil
    // graphique. La publication a lieu sur un fil de travail, qui n'a pas le
    // droit de toucher aux widgets.
    std::pair<std::string, std::string> m_hiscore_refresh_target;
    // Le jeu dont le tableau est affiche. render_board() ne recoit que des
    // lignes : sans cela il ne peut pas savoir si la valeur est un score ou
    // un chrono, et afficherait 917504 au lieu de 14'00"00.
    std::pair<std::string, std::string> m_board_game;
    Glib::Dispatcher m_hiscore_result_dispatcher;
    // `player` is passed in rather than read from the settings panel: this
    // runs on the watcher thread, and touching a GTK widget from outside the
    // main thread is undefined behaviour. Both values are captured at launch,
    // which is also the moment the player's intent actually applies.
    void submit_session_score(const std::string& system,
                              const std::string& game,
                              const std::string& fbneo_rom_name,
                              const std::string& hi_before,
                              const std::string& player,
                              const std::string& country,
                              bool hiscore_enabled,
                              bool share_playtime);
    void on_hiscore_result_ready();

    // Mise a jour du lanceur lui-meme. Aucun de nos formats ne sait se mettre
    // a jour seul : le flatpak est livre en bundle et non par un depot,
    // l'AppImage et l'archive n'ont aucun mecanisme, et le .deb ne vient
    // d'aucun depot apt. Sans cet avertissement, un utilisateur reste sur sa
    // version indefiniment sans jamais apprendre qu'il en existe une autre.
    Gtk::InfoBar      m_app_update_infobar;
    Gtk::Label        m_app_update_label;
    Glib::Dispatcher  m_app_update_dispatcher;
    std::string       m_app_update_tag;
    void check_app_update_async();
    void on_app_update_result();

    void check_fbneo_update_async();
    void on_fbneo_update_check_result();
    void on_fbneo_update_infobar_response(int response_id);

    // Screenshot-as-artwork: the watcher thread queues a batch here and wakes
    // the UI thread; on_screenshot_batch_found() drains it, one dialog at a time.
    struct PendingScreenshotBatch {
        std::string fbneo_rom_name;
        std::vector<std::string> screenshot_paths;
        std::string previews_dir;
        std::string titles_dir;
    };
    std::mutex m_screenshot_queue_mutex;
    std::deque<PendingScreenshotBatch> m_screenshot_queue;
    Glib::Dispatcher m_screenshot_found_dispatcher;

    // === Status Bar ===
    Gtk::Box   m_status_box{Gtk::ORIENTATION_HORIZONTAL};
    Gtk::Label m_status_label;

    // Taille des cartes, dans la barre du BAS.
    //
    // Remplace les boutons « 3 4 5 » de la barre du haut : personne ne
    // devine ce que « 4 » designe avant d'avoir clique. Un curseur dit
    // « plus petit / plus grand », ce qui est la seule chose qu'on veut
    // exprimer. Il vit en bas parce que c'est un reglage d'affichage, pas
    // une action courante.
    Gtk::Box   m_zoom_box{Gtk::ORIENTATION_HORIZONTAL, 6};
    Gtk::Label m_zoom_label;
    Gtk::Scale m_zoom_scale{Gtk::ORIENTATION_HORIZONTAL};
    bool       m_suppress_zoom = false;
    Gtk::Box   m_stats_box{Gtk::ORIENTATION_HORIZONTAL};
    Gtk::Label m_summary_label; // "N available / Total" on the right

    // Scan progress widgets (shown only while a scan is running)
    Gtk::Box         m_scan_status_box{Gtk::ORIENTATION_HORIZONTAL, 4};
    Gtk::ProgressBar m_scan_progress_bar;
    Gtk::Label       m_scan_progress_label;
    Gtk::Button      m_scan_details_button{"Details"};
    Gtk::Box         m_scan_status_dot{Gtk::ORIENTATION_HORIZONTAL, 0};
    
    // Thumbnail download progress
    Gtk::Box m_download_progress_box{Gtk::ORIENTATION_HORIZONTAL, 5};
    Gtk::Label m_download_status_label;
    Gtk::ProgressBar m_download_progress_bar;
    Gtk::Button m_download_cancel_button{"Cancel"};

    // === Games List ===
    Gtk::ScrolledWindow m_scrolled_games;
    Gtk::TreeView m_treeview_games;
    Glib::RefPtr<Gtk::ListStore> m_model_games;
    ModelColumns m_columns;

    // === Games views: modern list <-> cover grid (Gtk::Stack) ===
    // The dense TreeView (m_scrolled_games) stays in the stack, hidden, as the data
    // + selection backbone; the visible views are the styled ListBox and FlowBox.
    Gtk::Stack          m_view_stack;
    Gtk::ScrolledWindow m_scrolled_grid;
    Gtk::FlowBox        m_flowbox;
    Gtk::ScrolledWindow m_scrolled_mlist;
    /* En-tetes de la liste.
     *
     * La liste est une ListBox de lignes construites a la main, pas une
     * TreeView : elle n'a donc pas d'en-tetes gratuits. Ils sont batis ici
     * avec EXACTEMENT les memes largeurs que les cellules d'une ligne, et
     * les constantes ci-dessous sont partagees par les deux. Les ecrire
     * deux fois aurait garanti un desalignement au premier changement.
     */
    /* Le badge « Ctrl+K », POSE SUR le champ de recherche.
     *
     * Une Gtk::Entry ne sait pas porter une pastille de texte : elle n'a que
     * des icones. L'infobulle que j'avais mise n'annonce rien, il faut deja
     * savoir que le raccourci existe pour aller le chercher. Un Overlay pose
     * donc un vrai libelle par-dessus le champ, comme le mockup.
     */
    Gtk::Overlay        m_search_overlay;
    Gtk::Label          m_search_hint;

    Gtk::Box            m_mlist_head{Gtk::ORIENTATION_HORIZONTAL, 10};
    Gtk::Box            m_mlist_wrap{Gtk::ORIENTATION_VERTICAL, 0};

    /* Compteur du bas de la liste, « 29 461 games (filtered) ».
     *
     * Il vit sous la zone centrale, avec le curseur de taille, et non dans
     * la barre d'etat generale : il decrit CE QUE MONTRE la liste, pas
     * l'etat de la collection. La barre du bas garde son bilan de scan,
     * qui repond a une autre question.
     */
    Gtk::Box            m_center_box{Gtk::ORIENTATION_VERTICAL, 0};
    Gtk::Box            m_center_foot{Gtk::ORIENTATION_HORIZONTAL, 8};
    Gtk::Label          m_center_count;
    // m_hdr_emu ne parait qu'en portee « tous » : ailleurs la colonne
    // repeterait la meme valeur sur chaque ligne.
    Gtk::Button         m_hdr_game, m_hdr_emu, m_hdr_system, m_hdr_year;
    Gtk::Label          m_hdr_status, m_hdr_hs;
    void build_mlist_header();
    void refresh_mlist_header();

    Gtk::ListBox        m_mlist;
    Gtk::ToggleButton   m_btn_view_grid;
    Gtk::ToggleButton   m_btn_view_list;
    // Cards-per-row selector (grid view only): 3 / 4 / 5. Forcing an exact column
    // count means a long game title wraps inside its card instead of widening it
    // and squeezing the row down to two cards.
    Gtk::Box            m_grid_cols_seg{Gtk::ORIENTATION_HORIZONTAL};
    Gtk::ToggleButton   m_btn_cols3, m_btn_cols4, m_btn_cols5;
    int                 m_grid_columns = 180;  // LARGEUR d'une carte, en px
    bool                m_suppress_cols_toggle = false;
    void set_grid_columns(int n);
    std::vector<Gtk::TreeRowReference> m_grid_refs;  // card index -> model row
    std::vector<Gtk::TreeRowReference> m_mlist_refs; // list-row index -> model row
    // Widgets are materialised lazily: a first batch on rebuild, then one more
    // each time the user scrolls near the bottom. Building all 25k rows at once
    // would freeze the UI, so only what is (nearly) on screen ever exists.
    static constexpr int kViewBatch = 120;           // rows built per batch : a screenful and a half
    int  m_grid_built  = 0;                          // cards already built
    int  m_mlist_built = 0;                          // list rows already built
    bool m_batch_lock  = false;                      // re-entrancy guard while filling
    bool m_suppress_view_toggle = false;
    void set_view_mode(bool grid);
    void refresh_active_view();                      // rebuild whichever custom view is shown
    void rebuild_grid();
    void rebuild_mlist();
    void append_grid_batch();
    void append_mlist_batch();
    void maybe_extend_grid();                        // called on grid scroll/resize
    void maybe_extend_mlist();                       // called on list scroll/resize
    Gtk::Widget* make_game_card(const Gtk::TreeModel::Row& row);
    Gtk::Widget* make_list_row(const Gtk::TreeModel::Row& row);
    // Cover art is resolved AND decoded on a background thread so the UI never
    // touches the disk on the scroll path. Cards/rows are built with a titled
    // placeholder "holder" box; the worker reads the file (possibly from a slow
    // network mount) and decodes the PNG, then hands the finished pixbuf back to
    // the main thread via a Glib::Dispatcher, which swaps it into the holder.
    // A generation counter, bumped on every rebuild, makes stale results (whose
    // holder widgets have been destroyed) safe to drop without dereferencing them.
    struct ArtRequest {
        std::uint64_t gen; Gtk::Box* holder;
        std::string name, system, previews_dir, titles_dir; int w, h;
    };
    struct ArtResult {
        std::uint64_t gen; Gtk::Box* holder; Glib::RefPtr<Gdk::Pixbuf> pix;
    };
    std::deque<ArtRequest>    m_art_requests;
    std::deque<ArtResult>     m_art_results;
    std::mutex                m_art_req_mutex;
    std::mutex                m_art_res_mutex;
    std::condition_variable   m_art_req_cv;
    std::thread               m_art_thread;
    Glib::Dispatcher          m_art_dispatcher;
    std::atomic<bool>         m_art_thread_stop{false};
    std::atomic<std::uint64_t> m_art_generation{0};
    void start_art_thread();
    void art_worker();                          // background: resolve + decode
    void on_art_ready();                         // main thread: swap pixbufs in
    void queue_art(Gtk::Box* holder, const std::string& name,
                   const std::string& system, const std::string& emulator,
                   int w, int h);
    void clear_art_queue();                      // bump generation, drop pending
    void on_grid_selection_changed();
    void on_grid_child_activated(Gtk::FlowBoxChild* child);
    void on_mlist_row_selected(Gtk::ListBoxRow* row);
    void on_mlist_row_activated(Gtk::ListBoxRow* row);

    // === 3-Panel Layout like MAMEUI ===
    Gtk::Paned m_paned_main{Gtk::ORIENTATION_HORIZONTAL}; // Filter panel | Rest
    Gtk::Box m_right_box{Gtk::ORIENTATION_VERTICAL};       // views on top, detail dock at bottom
    // Active-filter chips. Filters stack across dimensions (Arcade + Original),
    // and the sidebar can only ever highlight one row : so the chips are what
    // makes the full active set visible, and each chip's × removes its own.
    Gtk::Box m_chips_box{Gtk::ORIENTATION_HORIZONTAL, 6};
    void rebuild_filter_chips();
    // Detail dock. It can live either at the bottom (m_details_box horizontal:
    // image | info column) or docked to the right (vertical: image on top of the
    // info column). The user picks, and the choice is persisted in config.json.
    Gtk::Paned          m_content_paned{Gtk::ORIENTATION_VERTICAL}; // views + detail dock
    Gtk::ScrolledWindow m_details_scroll;   // makes the dock scrollable when info is long
    // Three reflowable groups: A = the two artworks, B = title + metadata,
    // C = status pills + action buttons. m_details_box lays them out vertically
    // when docked right (fill height) and horizontally when docked at the bottom
    // (fill width); m_detail_image_wrap flips the same way so the two artworks
    // stack on the right and sit side-by-side at the bottom.
    Gtk::Box m_details_box{Gtk::ORIENTATION_HORIZONTAL, 16};      // A | B | C
    Gtk::Box m_detail_image_wrap{Gtk::ORIENTATION_HORIZONTAL, 8}; // A: Title / Preview
    Gtk::Box m_detail_text_col{Gtk::ORIENTATION_VERTICAL, 6};     // B: title + info
    Gtk::Box m_detail_actions_col{Gtk::ORIENTATION_VERTICAL, 8};  // C: pills + buttons
    Gtk::Box m_detail_actions{Gtk::ORIENTATION_HORIZONTAL, 8};    // Play / ★ / ⋯

    /* Actions secondaires du panneau.
     *
     * « Launch », « Download Art » et « ★ » avaient le meme poids visuel
     * alors qu'une seule de ces actions est celle qu'on vient faire. Jouer
     * reste seul en avant ; telecharger une jaquette, ouvrir la page du jeu
     * ou inspecter la ROM passent derriere « ⋯ », ou on les trouve quand on
     * les cherche sans qu'elles disputent l'attention le reste du temps.
     */
    // Play scinde : le bouton, son chevron, et le menu des variantes.
    Gtk::Box         m_play_split{Gtk::ORIENTATION_HORIZONTAL, 0};
    Gtk::MenuButton  m_btn_play_more;
    Gtk::Menu        m_play_menu;
    Gtk::MenuItem    m_mi_play_fullscreen;
    Gtk::MenuItem    m_mi_play_integer;
    Gtk::MenuItem    m_mi_play_fbneo;

    Gtk::MenuButton  m_btn_detail_more;
    Gtk::Menu        m_detail_menu;
    Gtk::MenuItem    m_mi_download_art;
    Gtk::MenuItem    m_mi_game_page;
    Gtk::MenuItem    m_mi_game_controls;
    Gtk::MenuItem    m_mi_reset_settings;
    Gtk::Image m_preview_image;
    Gtk::Image m_title_image;
    Gtk::Label m_label_title;
    Gtk::Label m_label_meta;
    Gtk::Label m_label_info;
    // Online leaderboard for the selected game. Its own label rather than
    // extra lines in m_label_info: it arrives from the network well after the
    // rest of the panel is painted, and rebuilding the whole metadata block
    // on every reply would make the text flicker.
    // Le classement et son bouton de rafraichissement. Enterre dans le menu
    // Fichier, ce bouton etait introuvable : sa place est sous le tableau
    // qu'il met a jour.
    // Le classement, dessine comme un tableau de borne plutot qu'ecrit dans
    // une etiquette : une seule etiquette de balisage ne permet ni d'aligner
    // les colonnes ni de traiter differemment une place libre d'un vrai score.
    /* « Ta meilleure place », en carte distincte au-dessus du classement.
     *
     * Le rang personnel n'existait que sous la forme d'un petit marqueur
     * vert accole au titre « Highscore » : il se lisait apres coup, alors
     * que c'est la premiere chose qu'un joueur cherche en ouvrant un jeu
     * qu'il connait. En carte, il se voit avant la table.
     *
     * La carte disparait quand le joueur n'a pas de score sur ce jeu, et
     * laisse alors une invitation a en poser un : une carte vide affichant
     * un rang absent decouragerait au lieu d'appeler.
     */
    /* Encart d'alerte de la zone Highscore d'une fiche de jeu.
     *
     * Il occupe la place du classement quand celui-ci ne peut pas marcher :
     * highscores coupes, ou personne de connecte. Sans lui, la zone
     * disparaissait purement et simplement, et le joueur n'avait aucun moyen
     * d'apprendre, depuis la fiche d'un jeu classe, pourquoi il n'y etait
     * pas.
     */
    Gtk::Box    m_hs_warn{Gtk::ORIENTATION_HORIZONTAL, 12};
    Gtk::Label  m_hs_warn_title;
    Gtk::Label  m_hs_warn_label;
    Gtk::Button m_hs_warn_btn;
    // Ce que fait le bouton : activer les highscores, ou ouvrir la connexion.
    bool        m_hs_warn_signin{false};
    void refresh_hiscore_dock_warning(bool enabled, bool signed_in);
    Gtk::Box    m_best_box{Gtk::ORIENTATION_VERTICAL, 6};
    Gtk::Label  m_best_title;
    Gtk::Box    m_best_row{Gtk::ORIENTATION_HORIZONTAL, 12};
    Gtk::Label  m_best_rank;
    Gtk::Label  m_best_who;
    Gtk::Label  m_best_score;
    Gtk::Label  m_best_hint;
    Gtk::LinkButton m_best_link{"", ""};
    Gtk::LinkButton m_board_link{"", ""};

    Gtk::Box    m_hiscore_box{Gtk::ORIENTATION_VERTICAL, 0};
    Gtk::Box    m_hiscore_head{Gtk::ORIENTATION_HORIZONTAL, 8};
    Gtk::Label  m_hiscore_title;
    Gtk::Label  m_hiscore_note;
    Gtk::Button m_btn_hiscore_refresh;
    Gtk::Grid   m_hiscore_grid;
    Gtk::Label  m_label_hiscore;      // conserve pour les messages courts
    // Les caracteristiques du jeu, en deux colonnes alignees. Un seul bloc de
    // texte gris donnait un mur illisible ou rien ne ressortait.
    Gtk::Grid   m_specs_grid;

    /* Activite personnelle, separee de la fiche technique.
     *
     * « Derniere session », « Plus longue », « Temps total » et « Parties »
     * vivaient dans la meme grille que la resolution et le driver. Ce sont
     * pourtant deux natures differentes : l'une decrit le JEU et ne bouge
     * jamais, l'autre decrit CE JOUEUR et change a chaque partie. Les
     * melanger obligeait a lire dix lignes pour trouver la seule qui
     * concerne celui qui regarde.
     *
     * Le bloc n'apparait que si le joueur a deja lance ce jeu : un « 0 min,
     * 0 partie » n'apprend rien et occupe autant de place qu'une vraie
     * information.
     */
    Gtk::Box    m_specs_row{Gtk::ORIENTATION_HORIZONTAL, 12};  // fiche | capture
    /* Deux sections repliables.
     *
     * Sur une fenetre basse, la fiche technique et l'activite repoussaient le
     * classement et le bouton Play hors de vue : il fallait faire defiler
     * pour jouer. Repliees, elles se reduisent a leur en-tete, qui porte
     * alors un resume, « 1 partie, 15 min » ou « Arcade, 1996, Capcom », de
     * sorte que replier ne fasse pas disparaitre TOUTE l'information.
     *
     * Un Gtk::Expander se reduit exactement a la hauteur de son en-tete, ce
     * qui est la demande : aucune zone vide conservee.
     */
    Gtk::Expander m_activity_exp;
    Gtk::Expander m_specs_exp;
    Gtk::Label    m_activity_sum;
    Gtk::Label    m_specs_sum;
    bool          m_dock_prefs_known = false;   // l'utilisateur a-t-il deja choisi ?
    void          build_dock_section(Gtk::Expander& exp, Gtk::Label& sum,
                                     const std::string& title, Gtk::Widget& body);

    Gtk::Label  m_specs_title;   // « Game information », en regard de « Your activity »
    Gtk::Box    m_activity_box{Gtk::ORIENTATION_VERTICAL, 4};
    Gtk::Label  m_activity_title;
    Gtk::Grid   m_activity_grid;
    Gtk::Button m_button_play;    // idem : « ▶ Launch », pose a la construction
    Gtk::Button m_button_download_art{"Download Art"};
    Gtk::Box    m_dock_pills{Gtk::ORIENTATION_HORIZONTAL, 6}; // status / zip / CRC pills
    Gtk::Button m_button_favorite{"★"};
    void on_dock_favorite_clicked();
    void on_reset_game_settings();
    void on_game_controls();
    // Ouvre le dialogue des manettes, global ou par jeu : meme cycle de vie.
    void present_controller_dialog(ControllerDialog* dlg, const std::string& cfg_path);
    // Le profil qui vaut pour ce jeu : le sien s'il en a un, le defaut sinon.
    const ControllerConfig* controller_profile_for(const std::string& fbneo_rom_name) const;
    void refresh_reset_settings_item();

    // Detail dock position: "bottom" (default) or "right".
    Gtk::ToggleButton m_btn_dock_toggle;
    std::string       m_dock_position{"bottom"};
    void set_dock_position(const std::string& pos);
    
    // === Thumbnail Downloader ===
    ThumbnailDownloader m_thumbnail_downloader;
    
    // Threading pour progression thumbnails
    Glib::Dispatcher m_download_progress_dispatcher;
    Glib::Dispatcher m_download_finished_dispatcher;
    
    // Threading for ROM scan
    Glib::Dispatcher m_scan_progress_dispatcher;
    Glib::Dispatcher m_scan_finished_dispatcher;
    
    // Removed filter population threading
    
    // Variables partagées pour la progression (protégées par le dispatcher)
    std::mutex  m_download_progress_mutex;   // worker writes, dispatcher reads
    std::string m_current_download_file;
    int m_current_download_index;
    int m_total_download_count;
    double m_download_percentage;
    
    // Variables for ROM scan progress (protected by dispatcher)
    std::atomic<bool> m_scan_cancelled{false};
    std::atomic<int> m_scan_current{0};
    std::atomic<int> m_scan_total{0};
    std::string m_current_scan_game;
    bool m_scan_in_progress = false;
    std::thread m_scan_thread;

    // Non-modal scan dialog (heap-allocated, kept alive until user closes it)
    std::unique_ptr<ROMScanDialog> m_scan_dialog;
    sigc::connection m_scan_bg_poll_timer; // polls progress when running in background

    // Non-modal ROM management window (inbox/outbox rebuilder + DAT tools),
    // created on first use and kept alive so its state survives being closed.
    std::unique_ptr<RomManagerWindow> m_rom_manager;
    
    // Filter performance optimization
    sigc::connection m_search_timeout_connection;
    // Pointe dans m_cached_games : a vider partout ou ce vecteur est
    // reaffecte, sous peine de pointeurs pendants.
    std::vector<const Game*> m_filtered_games;
    std::mutex m_filter_mutex;
    
    // Filter cache data
    FilterCache::FilterData m_filter_cache;
    bool m_filter_cache_loaded = false;

    // Controller profiles (loaded on startup, used by ControllerDialog)
    std::map<std::string, ControllerConfig> m_controller_profiles;
    std::string m_active_controller_profile{"Default"};

    // Launch options (persisted in config.json, applied to every game launch)
    bool m_launch_fullscreen{false};
    bool m_launch_integerscale{false};

    // Helper methods
    std::string find_rom_zip_path(const std::string& rom_name);
    bool        verify_zip_integrity(const std::string& zip_path);
    void        load_launch_prefs();
    void        save_launch_prefs();

    // Theme management (dark / light / system)
    Glib::RefPtr<Gtk::CssProvider> m_css_common;
    Glib::RefPtr<Gtk::CssProvider> m_css_dark;
    Glib::RefPtr<Gtk::CssProvider> m_css_light;
    Glib::RefPtr<Gio::Settings>    m_desktop_settings;   // org.gnome.desktop.interface, for System
    std::string m_theme_mode{"dark"};
    bool        m_theme_dark = true;     // the palette actually in use
    void apply_theme(const std::string& mode);
};