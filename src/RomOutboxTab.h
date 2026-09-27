// src/RomOutboxTab.h
//
// The Outbox tab of ROM Management : the airlock between what Fix produced
// and the library itself.
//
// Everything here already satisfies the DAT : Import writes nothing else. So
// there is no status to show and nothing to analyse; the tab shows what each
// archive is, what was done to make it (from the manifest Import wrote), and
// where it will go. The one action is Move selected to Library : each archive
// is verified once more against the DAT, with the library's parents and BIOS
// at hand for a split collection, then moved into the ROM directory mapped
// to its system, replacing what was there or not, as chosen. A replaced file
// goes to quarantine rather than to nothing.
#pragma once

#include "DatabaseManager.h"
#include "RomInbox.h"
#include "RomManifest.h"
#include "SettingsUi.h"

#include <gtkmm.h>
#include <atomic>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class RomOutboxTab : public Gtk::Box {
public:
    struct Paths {
        std::string outbox;
        std::string quarantine;
        std::vector<std::string> roms_paths;
    };
    using PathsProvider = std::function<Paths()>;

    RomOutboxTab(std::shared_ptr<DatabaseManager> db, PathsProvider paths);
    ~RomOutboxTab() override;

    void refresh();
    void reload_settings();
    bool busy() const { return m_busy.load(); }
    void log(const std::string& line, SettingsUi::LogPanel::Level level = SettingsUi::LogPanel::Level::Info);

    // Files landed in the library : the owner rescans, silently.
    // Carries the emulator whose library received sets : that one is rescanned.
    sigc::signal<void, std::string>& signal_scan_requested() { return m_sig_scan; }
    // Files were moved to quarantine (replaced copies).
    sigc::signal<void>& signal_quarantine_changed() { return m_sig_quarantine; }
    // The user picked another outbox folder here : the same key lives in
    // config.json, so whoever else displays it is told.
    sigc::signal<void, std::string>& signal_outbox_path_changed() { return m_sig_outbox_path; }

private:
    enum class Collision { Replace, SkipIdentical, Skip };

    void build_header();
    void build_table();
    void build_footer();
    void save_settings() const;
    void on_browse_folder();
    // Take a folder the user chose : store it, tell the others, show it.
    void apply_outbox_path(const std::string& folder);
    void save_outbox_path(const std::string& folder) const;

    // Where a system folder of the outbox goes : an explicit mapping, else
    // the configured ROM directory whose name matches. Empty when neither.
    std::string destination_for(const std::string& system_folder, const Paths& p) const;
    std::string emulator_of_folder(const std::string& system_folder) const;
    // The library folder a system folder of the outbox goes to when the user
    // mapped none : a ROM directory of that name, or one under them.
    std::string auto_destination(const std::string& system_folder) const;
    void on_edit_destinations();

    void populate();
    void update_summary();
    bool row_visible(const Gtk::TreeModel::const_iterator& it) const;
    void refilter();
    void on_selection_changed();
    void on_row_toggled(const Glib::ustring& path);
    void set_all_checked(bool on);
    void on_context_menu(const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn* column, GdkEventButton* event);
    void update_action_buttons();
    void on_open_folder();
    void on_move_clicked(bool all);
    void worker_move();
    Gtk::TreeModel::Row source_row(const Gtk::TreeModel::Path& sorted_path) const;

    RomInbox::Callbacks make_callbacks();
    void push_progress(double pct, const std::string& msg);
    void push_log(const std::string& msg);
    void on_progress_update();
    void on_worker_finished();
    void set_busy(bool busy);
    void flash(const Glib::ustring& text);

    std::shared_ptr<DatabaseManager> m_db;
    PathsProvider m_paths;

    // ── Widgets ─────────────────────────────────────────────────────────────
    Gtk::Box            m_top{Gtk::ORIENTATION_HORIZONTAL, SettingsUi::kCardSpacing};
    Gtk::Entry          m_entry_folder;
    Gtk::Button*        m_btn_browse = nullptr;
    Gtk::Button*        m_btn_open = nullptr;
    Gtk::Button*        m_btn_refresh = nullptr;
    Gtk::CheckButton    m_check_keep_replaced;
    Gtk::ComboBoxText   m_combo_collision;
    Gtk::Button*        m_btn_destinations = nullptr;
    Gtk::Label          m_status;
    Gtk::Box            m_pills{Gtk::ORIENTATION_HORIZONTAL, 8};
    SettingsUi::Pill   *m_pill_total = nullptr, *m_pill_ready = nullptr, *m_pill_unmapped = nullptr;
    Gtk::Button        *m_btn_select_all = nullptr, *m_btn_select_none = nullptr;
    SettingsUi::FilterBar*   m_filter = nullptr;
    Gtk::ComboBoxText*       m_system_combo = nullptr;
    SettingsUi::Table*       m_table = nullptr;
    SettingsUi::DetailPanel* m_detail = nullptr;
    SettingsUi::LogPanel*    m_log = nullptr;
    Gtk::Menu                m_context_menu;
    Gtk::Box            m_footer{Gtk::ORIENTATION_HORIZONTAL, 10};
    Gtk::ProgressBar    m_progress;
    Gtk::Label          m_progress_label;
    Gtk::Button*        m_btn_cancel = nullptr;
    Gtk::Button*        m_btn_move = nullptr;
    Gtk::MenuButton*    m_btn_move_more = nullptr;
    Gtk::Menu           m_move_menu;
    sigc::connection    m_flash_timer;

    // ── Model ───────────────────────────────────────────────────────────────
    struct Item {
        std::string path, system_folder, game, system, dat_header, parent;
        // A CHD in <system folder>/<set>/ : `path` is the .chd file, and it
        // lands in <destination>/<set>/.
        bool        disk = false;
        std::string emulator = "fbneo";   // from the system folder's name
        std::string destination;      // resolved directory, empty when unmapped
        bool        dest_exists = false;
        uintmax_t   bytes = 0;
        int         files = 0, files_expected = 0;
        const RomManifest::Entry* entry = nullptr;   // into m_manifest, may be null
        bool        selected = true;
    };
    std::vector<Item>      m_items;
    RomManifest::Manifest  m_manifest;
    std::map<std::string, std::string> m_destinations;   // system folder → directory

    struct Columns : public Gtk::TreeModel::ColumnRecord {
        Gtk::TreeModelColumn<bool>          include;
        Gtk::TreeModelColumn<Glib::ustring> game, system, archive, parent, files, size, destination, fix;
        Gtk::TreeModelColumn<Glib::ustring> search_blob;
        Gtk::TreeModelColumn<bool>          mapped;
        Gtk::TreeModelColumn<unsigned int>  index;
        Columns() { add(include); add(game); add(system); add(archive); add(parent); add(files); add(size);
                    add(destination); add(fix); add(search_blob); add(mapped); add(index); }
    };
    Columns m_cols;
    Glib::RefPtr<Gtk::ListStore>       m_store;
    SettingsUi::ModelStack             m_models;    // filter + sort, rebuilt on every change
    Glib::ustring m_vis_system;                     // filter inputs, snapshotted per refilter
    std::string   m_vis_needle;
    struct StatusColours { Gdk::RGBA ok, warn, err, muted; bool ready = false; } m_colours;
    void ensure_colours();

    // ── Move job ────────────────────────────────────────────────────────────
    struct MoveJob {
        Paths paths;
        Collision collision = Collision::Replace;
        bool keep_replaced = true;
        std::vector<Item> items;
        int moved = 0, replaced = 0, skipped = 0, identical = 0, refused = 0, failed = 0;
        std::set<std::string> moved_emulators;   // whose libraries to rescan
    } m_job;
    std::thread      m_worker;
    Glib::Dispatcher m_progress_dispatcher;
    Glib::Dispatcher m_finished_dispatcher;
    mutable std::mutex       m_shared_mutex;
    std::atomic<double>      m_progress_value{0.0};
    std::atomic<bool>        m_cancelled{false};
    std::atomic<bool>        m_busy{false};
    std::string              m_current_message;
    std::vector<std::string> m_log_messages;

    sigc::signal<void, std::string> m_sig_scan;
    sigc::signal<void> m_sig_quarantine;
    sigc::signal<void, std::string> m_sig_outbox_path;
};
