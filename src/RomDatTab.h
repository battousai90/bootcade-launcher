// src/RomDatTab.h
//
// The DAT tab of ROM Management : the DAT groups, and where their files
// come from.
//
// A DAT group is a named selection of DAT files the user organises as they
// please ("FinalBurn Neo", "FBNeo - GBA", "Special Arcade"); several groups
// may draw on the same source and folder. The left column lists the groups;
// everything else on the screen is the group selected there : its folder,
// its source, the files the source provides with a tick for the ones in the
// group, and the panels describing the selected file and the source.
//
// Only the actions the chosen source can honour are shown : Generate from
// the emulator when the emulator produces the DATs, Check for updates /
// Download when a file server publishes them, Rescan when the user fills the
// folder by hand. The emulator a group describes is a property of the group,
// chosen in its card next to the folder and the set style : it says which
// executable the generation calls and which catalogue the audit judges (the
// catalogues share set names, mslug, so they are never the same rows). Add
// DAT files… works with any source. Reloading the database from the DAT
// files stays an operation with a name, in the menu, and runs on its own
// after anything that changed the folder or a selection.
#pragma once

#include "DatSource.h"
#include "DatabaseManager.h"
#include "SettingsUi.h"

#include <gtkmm.h>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class RomDatTab : public Gtk::Box {
public:
    struct Env {
        std::string fbneo_executable;   // owned by Settings › Emulator
    };
    using EnvProvider = std::function<Env()>;

    RomDatTab(std::shared_ptr<DatabaseManager> db, EnvProvider env);
    ~RomDatTab() override;

    // Re-read the groups from config.json and the folder from disk.
    void refresh();
    bool busy() const { return m_busy.load(); }

    // The database must be rebuilt from the (selected) DAT files. `confirm`
    // is false when the tab just changed the folder or a selection and the
    // reload is the natural continuation; true for the explicit menu entry.
    // The string is the emulator whose games are to be reloaded (that of the
    // group the change was made in) ; empty for every emulator.
    sigc::signal<void, bool, std::string>& signal_reload_database() { return m_sig_reload; }
    // The first group's folder changed : Settings keeps the same key.
    sigc::signal<void, std::string>& signal_folder_changed() { return m_sig_folder; }
    // FBNeo should write its DATs into the folder : the owner runs GenerateDAT
    // (it owns the window and the dialogs), then the tab is refreshed.
    sigc::signal<void, std::string>& signal_generate_requested() { return m_sig_generate; }
    // Groups were added, renamed, removed, enabled or their selection
    // changed : the Library combo follows.
    sigc::signal<void>& signal_groups_changed() { return m_sig_groups; }

private:
    void build_groups_column();
    void build_group_card();
    void build_source_card();
    void build_table();
    void build_footer();

    const DatSource::Group& group() const { return m_groups[m_current]; }
    DatSource::Group&       group()       { return m_groups[m_current]; }
    void save_groups();
    void select_group(size_t index);
    void rebuild_group_list();
    // Le nouveau groupe reprend la source, le dossier et l'emulateur du
    // groupe courant : tout cela s'edite ensuite dans sa carte.
    void on_add_group();
    // MAME ne se lance pas : la conversion vit dans GenerateDAT, et le
    // resultat est un dossier de DAT comme un autre.
    void on_generate_mame();

    // Produire les DAT depend de l'emulateur que le groupe decrit, jamais du
    // type de source. Cette table est le seul endroit de l'ecran ou un
    // emulateur est nomme : le bouton, son infobulle et la fiche de source ne
    // lisent que ce qu'elle rend. Un troisieme emulateur capable d'ecrire ses
    // DAT tient en une entree de plus.
    struct Backend {
        std::function<std::string(RomDatTab&)> locate;     // le chemin, vide si absent
        std::function<void(RomDatTab&)>        generate;   // ecrit les DAT dans le dossier du groupe
        const char* ready;     // N_(), %1 = le nom de l'emulateur
        const char* missing;   // N_(), %1 = le nom de l'emulateur
    };
    static const std::map<std::string, Backend>& backends();
    // L'entree de l'emulateur du groupe courant, ou nullptr : un groupe peut
    // decrire un emulateur qui ne sait pas produire ses propres DAT.
    const Backend* backend() const;
    // Le chemin d'un emulateur, cherche une fois par session : un `which` est
    // trop cher pour un ecran qui se rafraichit a chaque clic.
    std::string executable_of(const std::string& emulator);
    void on_rename_group(size_t index);
    void on_delete_group(size_t index);
    void on_toggle_group_active(size_t index);
    void groups_changed();
    // Selections and enabled groups change what the database holds : one
    // reload, shortly after the last change, however many ticks in a row.
    void schedule_reload(const std::string& emulator);
    // The emulator of the group with that id ; empty when there is none.
    std::string emulator_of_group(const std::string& id) const;
    // The database holds the union of what active groups select. A change
    // that leaves that union as it was (a file another group already loads)
    // needs no reload : compare with the union taken before the change.
    std::vector<std::string> union_files() const { return DatSource::files_to_load(m_groups); }
    bool reload_if_union_changed(const std::vector<std::string>& before,
                                 const std::string& emulator);

    void apply_source_ui();
    void on_source_changed();
    void on_browse_folder();
    void on_add_files();
    void on_primary_action();
    void on_check_updates();
    void on_download();
    void on_rescan();
    void on_in_group_toggled(const Glib::ustring& path);
    void on_selection_changed();
    void on_open_folder();
    // Every file name the source provides : the folder, plus what the last
    // Check said the server has.
    std::vector<std::string> provided_files() const;

    void populate();
    void update_summary();
    void show_file_info(int index);
    void show_source_info();

    // ── Worker (HTTP) ──────────────────────────────────────────────────────
    enum class Job { None, Check, Download, Site };
    void worker_check();
    void worker_site();
    void on_download_site(size_t site);
    void worker_download();
    void push_progress(double pct, const std::string& msg);
    void push_log(const std::string& msg);
    void on_progress_update();
    void on_worker_finished();
    void set_busy(bool busy);
    void flash(const Glib::ustring& text);

    std::shared_ptr<DatabaseManager> m_db;
    EnvProvider m_env;
    std::vector<DatSource::Group> m_groups;
    size_t m_current = 0;
    std::map<std::string, DatabaseManager::DatFileStats> m_stats;
    std::map<std::string, std::string> m_exe_cache;

    // ── Widgets ────────────────────────────────────────────────────────────
    Gtk::Box            m_columns{Gtk::ORIENTATION_HORIZONTAL, SettingsUi::kCardSpacing};
    Gtk::Box            m_main{Gtk::ORIENTATION_VERTICAL, SettingsUi::kCardSpacing};
    Gtk::ListBox        m_group_list;
    Gtk::Button*        m_btn_add_group = nullptr;
    Gtk::Box            m_top{Gtk::ORIENTATION_HORIZONTAL, SettingsUi::kCardSpacing};
    SettingsUi::Card    m_group_card;
    Gtk::Label          m_group_sub;
    Gtk::Entry          m_entry_folder;
    Gtk::Button*        m_btn_browse = nullptr;
    // How the selected DAT is read (its DatSource::DatRule) : the merge mode
    // when its sets are linked, whether it overrides the DAT's own, and the
    // folder its sets go to.
    Gtk::Grid           m_rule_grid;
    std::map<std::string, Gtk::Label*> m_rule_values;
    Gtk::Box            m_rule_merge_line{Gtk::ORIENTATION_HORIZONTAL, 8};
    Gtk::ComboBoxText   m_combo_merge;
    Gtk::CheckButton    m_check_override;
    Gtk::Entry          m_entry_dat_folder;
    int                 m_rule_item = -1;      // index into m_items the controls describe
    bool                m_rule_filling = false;
    void show_dat_rule(int index);
    void store_dat_rule();
    Gtk::ComboBoxText   m_combo_emulator;   // rempli depuis EmulatorRegistry
    Gtk::Button*        m_btn_primary = nullptr;
    Gtk::Button*        m_btn_check = nullptr;
    Gtk::Button*        m_btn_add = nullptr;
    Gtk::MenuButton*    m_btn_more = nullptr;
    Gtk::Menu           m_more_menu;
    Gtk::MenuButton*    m_btn_site = nullptr;   // Local folder : download from a DAT site
    Gtk::Menu           m_site_menu;
    std::vector<std::pair<Gtk::MenuItem*, std::string>> m_site_items;   // item, emulator
    Gtk::RadioButton    m_radio_emulator, m_radio_http, m_radio_folder;
    Gtk::Box            m_url_line;   // libelle + adresse du manifeste
    Gtk::Entry          m_entry_url;
    // Le groupe dont le champ montre l'adresse : il est partage par tous les
    // groupes, et le quitter apres avoir change de groupe ecrivait l'adresse
    // de l'ancien dans le nouveau.
    std::string         m_url_group;
    Gtk::Label          m_source_hint;
    Gtk::Label          m_status;

    SettingsUi::Table*  m_table = nullptr;
    Gtk::Box            m_info_column{Gtk::ORIENTATION_VERTICAL, SettingsUi::kCardSpacing};
    // Les quatre cartes de la colonne se replient comme celles des Settings
    // (SettingsUi::fold) ; leur etat se retrouve d'une ouverture a l'autre
    // (cle « rom_dat_sections » de config.json).
    std::map<std::string, SettingsUi::Fold> m_folds;
    Gtk::Widget* fold_card(const SettingsUi::Card& card, const std::string& key,
                           std::function<std::string()> describe);
    void         refresh_folds();
    Gtk::Grid           m_info_grid;
    std::map<std::string, Gtk::Label*> m_info_values;
    Gtk::Button*        m_btn_open_in_folder = nullptr;
    Gtk::Grid           m_source_grid;
    std::map<std::string, Gtk::Label*> m_source_values;
    Gtk::TextView       m_preview;
    Glib::RefPtr<Gtk::TextBuffer> m_preview_buffer;

    Gtk::Box            m_footer{Gtk::ORIENTATION_HORIZONTAL, 10};
    Gtk::Box            m_actions{Gtk::ORIENTATION_HORIZONTAL, 10};   // the group's actions, bottom right
    Gtk::Label          m_footer_label;
    Gtk::ProgressBar    m_progress;
    Gtk::Label          m_progress_label;
    Gtk::Button*        m_btn_cancel = nullptr;
    sigc::connection    m_flash_timer;
    sigc::connection    m_reload_timer;

    // ── Model ──────────────────────────────────────────────────────────────
    struct Item {
        std::string name, path, system, header_name, version, date;
        uint64_t    size = 0;
        int         games = 0, roms = 0;
        bool        linked = false;        // its sets refer to one another : a merge mode applies
        std::string declared;              // the merge mode its header declares
        bool        in_group = true;
        bool        on_disk = true;
        DatSource::State remote_state = DatSource::State::LocalOnly;
        bool        remote_known = false;
        std::string remote_version, remote_date, sha256;
    };
    std::vector<Item> m_items;
    std::vector<DatSource::FileStatus> m_last_compare;   // from the last Check, for the current group
    std::string m_last_compare_group;                     // which group it was made for
    std::string m_last_manifest_generated;

    struct Columns : public Gtk::TreeModel::ColumnRecord {
        Gtk::TreeModelColumn<bool>          in_group;
        Gtk::TreeModelColumn<bool>          on_disk;
        Gtk::TreeModelColumn<Glib::ustring> file, system, sets, roms, version, size, update;
        Gtk::TreeModelColumn<unsigned int>  index;
        Columns() { add(in_group); add(on_disk); add(file); add(system); add(sets); add(roms); add(version); add(size); add(update); add(index); }
    };
    Columns m_cols;
    Glib::RefPtr<Gtk::ListStore> m_store;
    struct StatusColours { Gdk::RGBA ok, warn, err, muted; bool ready = false; } m_colours;
    void ensure_colours();

    Job              m_job = Job::None;
    std::thread      m_worker;
    Glib::Dispatcher m_progress_dispatcher;
    Glib::Dispatcher m_finished_dispatcher;
    mutable std::mutex       m_shared_mutex;
    std::atomic<double>      m_progress_value{0.0};
    std::atomic<bool>        m_cancelled{false};
    std::atomic<bool>        m_busy{false};
    std::string              m_current_message;
    std::vector<std::string> m_log_messages;
    DatSource::Manifest      m_job_manifest;
    std::string              m_job_error;
    int                      m_job_downloaded = 0, m_job_failed = 0;
    std::string              m_job_url, m_job_folder, m_job_group_id;
    size_t                   m_job_site = 0;
    std::vector<std::string> m_job_written, m_job_before;
    DatSource::Group         m_job_group;

    sigc::signal<void, bool, std::string> m_sig_reload;
    // What the reload the timer holds covers : changes in groups of two
    // emulators reload both.
    std::string         m_reload_emulator;
    sigc::signal<void, std::string> m_sig_folder;
    sigc::signal<void, std::string> m_sig_generate;
    sigc::signal<void>              m_sig_groups;
};
