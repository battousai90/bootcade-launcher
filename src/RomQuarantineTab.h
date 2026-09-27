// src/RomQuarantineTab.h
//
// The Quarantine tab of ROM Management : what was set aside, and why.
//
// Nothing arrives here on its own. A file is in quarantine because the
// audit found it wrong beyond repair, because Import could not recognise or
// read it, because the library already had it, because it was an entry no
// DAT needed, or because a move replaced it. The manifest written at that
// moment is what lets this tab say so; a file with no record is shown as
// exactly that. The tab moves files back to Import or to where they came
// from, deletes what the user chooses, or empties the folder : nothing is
// ever deleted on its own.
#pragma once

#include "DatabaseManager.h"
#include "RomManifest.h"
#include "SettingsUi.h"

#include <gtkmm.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class RomQuarantineTab : public Gtk::Box {
public:
    struct Paths {
        std::string quarantine;
        std::string inbox;
    };
    using PathsProvider = std::function<Paths()>;

    RomQuarantineTab(std::shared_ptr<DatabaseManager> db, PathsProvider paths);

    void refresh();
    // Files went back to the import folder : the owner may want to show it.
    sigc::signal<void, int>& signal_restored_to_import() { return m_sig_restored; }
    // A log line for the window's own log, when something is worth keeping.
    sigc::signal<void, std::string>& signal_log() { return m_sig_log; }
    // The user picked another quarantine folder here : the same key lives in
    // config.json, so whoever else displays it is told.
    sigc::signal<void, std::string>& signal_quarantine_path_changed() { return m_sig_path_changed; }

private:
    void build_header();
    void build_footer();
    void build_table();
    void populate();
    void update_summary();
    bool row_visible(const Gtk::TreeModel::const_iterator& it) const;
    void refilter();
    void on_row_toggled(const Glib::ustring& path);
    void set_all_checked(bool on);
    void on_context_menu(const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn* column, GdkEventButton* event);
    void update_action_buttons();
    void on_open_folder();
    void on_browse_folder();
    // Take a folder the user chose : store it, tell the others, show it.
    void apply_quarantine_path(const std::string& folder);
    void save_quarantine_path(const std::string& folder) const;
    void on_restore(bool to_origin);
    void on_delete_selected();
    void on_empty();
    void flash(const Glib::ustring& text);
    Gtk::TreeModel::Row source_row(const Gtk::TreeModel::Path& sorted_path) const;

    std::shared_ptr<DatabaseManager> m_db;
    PathsProvider m_paths;

    // ── Widgets ─────────────────────────────────────────────────────────────
    Gtk::Entry          m_entry_folder;
    Gtk::Button*        m_btn_browse = nullptr;
    Gtk::Button*        m_btn_open = nullptr;
    Gtk::Button*        m_btn_refresh = nullptr;
    Gtk::Button*        m_btn_restore = nullptr;
    Gtk::MenuButton*    m_btn_restore_more = nullptr;
    Gtk::Menu           m_restore_menu;
    Gtk::MenuItem*      m_item_restore_origin = nullptr;
    Gtk::Button*        m_btn_delete = nullptr;
    Gtk::Button*        m_btn_empty = nullptr;
    Gtk::Label          m_status;
    Gtk::Box            m_footer{Gtk::ORIENTATION_HORIZONTAL, 10};
    Gtk::Box            m_pills{Gtk::ORIENTATION_HORIZONTAL, 8};
    struct ReasonPill { std::string key; SettingsUi::Pill* pill; };
    std::vector<ReasonPill> m_reason_pills;
    SettingsUi::Pill*   m_pill_total = nullptr;
    Gtk::Button        *m_btn_select_all = nullptr, *m_btn_select_none = nullptr;
    SettingsUi::FilterBar* m_filter = nullptr;
    Gtk::ComboBoxText*     m_system_combo = nullptr;
    SettingsUi::Table*     m_table = nullptr;
    Gtk::Menu              m_context_menu;
    sigc::connection       m_flash_timer;

    // ── Model ───────────────────────────────────────────────────────────────
    struct Item {
        std::string path, rel, reason, game, system, origin, action, added_at;
        std::vector<std::string> details;
        uintmax_t bytes = 0;
        bool has_record = false;
        bool selected = false;
    };
    std::vector<Item>     m_items;
    RomManifest::Manifest m_manifest;

    struct Columns : public Gtk::TreeModel::ColumnRecord {
        Gtk::TreeModelColumn<bool>          include;
        Gtk::TreeModelColumn<Glib::ustring> reason, reason_key, game, system, origin, file, size, added, details;
        Gtk::TreeModelColumn<Glib::ustring> search_blob;
        Gtk::TreeModelColumn<unsigned int>  index;
        Columns() { add(include); add(reason); add(reason_key); add(game); add(system); add(origin); add(file);
                    add(size); add(added); add(details); add(search_blob); add(index); }
    };
    Columns m_cols;
    Glib::RefPtr<Gtk::ListStore>       m_store;
    SettingsUi::ModelStack             m_models;    // filter + sort, rebuilt on every change
    Glib::ustring m_vis_system;                     // filter inputs, snapshotted per refilter
    std::string   m_vis_needle;
    struct StatusColours { Gdk::RGBA ok, warn, err, muted, accent; bool ready = false; } m_colours;
    void ensure_colours();

    sigc::signal<void, int>         m_sig_restored;
    sigc::signal<void, std::string> m_sig_log;
    sigc::signal<void, std::string> m_sig_path_changed;
};
