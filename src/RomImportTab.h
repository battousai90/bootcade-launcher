// src/RomImportTab.h
//
// The Import tab of ROM Management : where files brought from outside are
// analysed and, when possible, turned into correct sets.
//
// It reads an import folder, asks RomInbox what each file is worth against
// the DAT group (and, optionally, what the existing library can lend), and
// shows the plan set by set : complete as it is, rebuildable, incomplete,
// already in the library, unknown. Fix carries the plan out into the outbox;
// nothing here ever writes inside the library. Every option on the screen
// drives a real branch of RomInbox::Options : none is decorative.
#pragma once

#include "DatabaseManager.h"
#include "RomInbox.h"
#include "SettingsUi.h"

#include <gtkmm.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class RomImportTab : public Gtk::Box {
public:
    struct Paths {
        std::string outbox;
        std::string quarantine;
        std::vector<std::string> roms_paths;   // the library, for RomInbox::Options
        std::string emulator = "fbneo";         // the library group's : whose sets the inbox is matched to
    };
    using PathsProvider = std::function<Paths()>;

    RomImportTab(std::shared_ptr<DatabaseManager> db, PathsProvider paths);
    ~RomImportTab() override;

    // Re-read the tab's own keys of config.json (rom_manager.*).
    void reload_settings();
    void save_settings() const;

    std::string inbox_path() const { return m_entry_inbox.get_text().raw(); }
    bool busy() const { return m_busy.load(); }

    // Library hands over archives of repairable sets : copied into the import
    // folder, then analysed straight away.
    void receive(const std::vector<std::string>& archives);
    // A line for the log panel, from anywhere in the window.
    void log(const std::string& line, SettingsUi::LogPanel::Level level = SettingsUi::LogPanel::Level::Info);

    // Fix wrote into the outbox : the Outbox tab should refresh.
    sigc::signal<void>& signal_outbox_changed() { return m_sig_outbox_changed; }

private:
    void build_options();
    void build_results();
    void build_footer();

    RomInbox::Options options_from_ui() const;
    void on_analyze_clicked();
    void on_fix_clicked();
    // What Fix would do right now, beyond the sets : files the analysis
    // could do nothing with, moved to quarantine when the option says so.
    struct Housekeeping { int unknown = 0, duplicates = 0; bool enabled = false; };
    Housekeeping housekeeping() const;
    void worker_analyze();
    void worker_apply();
    void populate();
    void update_summary();
    bool row_visible(const Gtk::TreeModel::const_iterator& it) const;
    void refilter();
    void on_selection_changed();
    void on_row_toggled(const Glib::ustring& path);
    void set_all_checked(bool on);
    void on_context_menu(const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn* column, GdkEventButton* event);
    void on_export(int format);   // 0 text, 1 csv, 2 dat
    void update_action_buttons();
    Gtk::TreeModel::Row source_row(const Gtk::TreeModel::Path& sorted_path) const;

    // ── Worker plumbing ─────────────────────────────────────────────────────
    enum class Job { None, Analyze, Apply };
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
    Gtk::Box          m_top{Gtk::ORIENTATION_HORIZONTAL, SettingsUi::kCardSpacing};
    Gtk::Entry        m_entry_inbox;
    Gtk::Button*      m_btn_browse = nullptr;
    Gtk::CheckButton  m_check_recursive, m_check_archives, m_check_loose;
    Gtk::CheckButton  m_check_use_library, m_check_rebuild_correct;
    Gtk::RadioButton  m_radio_subfolder, m_radio_delete, m_radio_keep;
    Gtk::CheckButton  m_check_quarantine_rejects;
    Gtk::Label        m_status;

    Gtk::Box          m_pills{Gtk::ORIENTATION_HORIZONTAL, 8};
    SettingsUi::Pill *m_pill_valid = nullptr, *m_pill_fixable = nullptr, *m_pill_missing = nullptr,
                     *m_pill_unknown = nullptr, *m_pill_already = nullptr, *m_pill_ignored = nullptr,
                     *m_pill_total = nullptr;
    Gtk::Button       *m_btn_select_all = nullptr, *m_btn_select_none = nullptr;

    SettingsUi::FilterBar*   m_filter = nullptr;
    Gtk::ComboBoxText*       m_system_combo = nullptr;
    SettingsUi::Table*       m_table = nullptr;
    SettingsUi::DetailPanel* m_detail = nullptr;
    SettingsUi::LogPanel*    m_log = nullptr;
    Gtk::Menu                m_context_menu;

    Gtk::Box          m_footer{Gtk::ORIENTATION_HORIZONTAL, 10};
    Gtk::ProgressBar  m_progress;
    Gtk::Label        m_progress_label;
    Gtk::Button*      m_btn_cancel  = nullptr;
    Gtk::Button*      m_btn_analyze = nullptr;
    Gtk::Button*      m_btn_fix     = nullptr;
    Gtk::MenuButton*  m_btn_export  = nullptr;
    Gtk::Menu         m_export_menu;
    sigc::connection  m_flash_timer;

    // ── Model ───────────────────────────────────────────────────────────────
    enum Kind { KIND_SET = 0, KIND_UNKNOWN, KIND_UNSUPPORTED, KIND_IGNORED, KIND_DUPLICATE };
    struct Columns : public Gtk::TreeModel::ColumnRecord {
        Gtk::TreeModelColumn<bool>          include;
        Gtk::TreeModelColumn<bool>          actionable;
        Gtk::TreeModelColumn<Glib::ustring> status;
        Gtk::TreeModelColumn<Glib::ustring> status_key;   // valid|fixable|missing|already|unknown|ignored
        Gtk::TreeModelColumn<Glib::ustring> game;
        Gtk::TreeModelColumn<Glib::ustring> system;
        Gtk::TreeModelColumn<Glib::ustring> file;
        Gtk::TreeModelColumn<Glib::ustring> size;
        Gtk::TreeModelColumn<Glib::ustring> source;       // repair source
        Gtk::TreeModelColumn<Glib::ustring> details;
        Gtk::TreeModelColumn<Glib::ustring> search_blob;
        Gtk::TreeModelColumn<int>           kind;
        Gtk::TreeModelColumn<unsigned int>  index;
        Columns() {
            add(include); add(actionable); add(status); add(status_key); add(game); add(system);
            add(file); add(size); add(source); add(details); add(search_blob); add(kind); add(index);
        }
    };
    Columns m_cols;
    Glib::RefPtr<Gtk::ListStore>       m_store;
    SettingsUi::ModelStack             m_models;    // filter + sort, rebuilt on every change
    Glib::ustring m_vis_system;                     // filter inputs, snapshotted per refilter
    std::string   m_vis_needle;

    struct StatusColours { Gdk::RGBA ok, warn, err, muted, accent; bool ready = false; } m_colours;
    void ensure_colours();

    // ── State ───────────────────────────────────────────────────────────────
    RomInbox::Report      m_report;
    RomInbox::ApplyResult m_apply_result;
    std::string           m_job_inbox, m_job_outbox;
    RomInbox::Options     m_job_options;

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

    sigc::signal<void> m_sig_outbox_changed;
};
