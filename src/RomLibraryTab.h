// src/RomLibraryTab.h
//
// The Library tab of ROM Management : what is actually wrong with the
// collection, and where.
//
// It scans nothing itself and launches nothing: it asks the main window for a
// scan, runs the audit (RomAudit, from the scan cache), and lays the report
// out as a flat table of sets with a detail panel listing the ROMs of the
// selected one. One action, Fix, hands every problem to where it is dealt
// with: repairable sets (misnamed, or rebuildable from the library) go to
// Import, unrepairable ones (wrong data), orphans and extra files go to
// quarantine. It never writes inside the library except to move a broken
// set out of it.
//
// Built entirely from the shared bricks of SettingsUi, so it belongs to the
// same system as Settings and Controller Configuration.
#pragma once

#include "DatSource.h"
#include "DatabaseManager.h"
#include "RomAudit.h"
#include "SettingsUi.h"

#include <gtkmm.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

class RomLibraryTab : public Gtk::Box {
public:
    // What the tab needs from the rest of the application, read fresh each
    // time (Settings may change them while the window is open).
    struct Paths {
        std::vector<std::string> roms_paths;
        std::string inbox;
        std::string quarantine;
    };
    using PathsProvider = std::function<Paths()>;

    RomLibraryTab(std::shared_ptr<DatabaseManager> db, PathsProvider paths);
    ~RomLibraryTab() override;

    // The owner ran a scan (or "Move to library" did): re-run the audit if one
    // was ever run this session, so the table reflects the library as it is.
    void refresh_after_scan();
    // The DAT groups changed (DAT tab) : the combo follows.
    void reload_groups();
    bool busy() const { return m_busy.load(); }

    // "Scan ROMs": the owner starts the scan with its usual confirmation.
    // Both scan signals carry the emulator of the DAT group : the library
    // to scan is that emulator's, its ROM directories and its sets.
    sigc::signal<void, std::string>& signal_rescan_requested() { return m_sig_rescan; }
    // Files were moved out of the library (quarantine): the owner should
    // rescan, silently, to keep statuses honest.
    sigc::signal<void, std::string>& signal_scan_requested()   { return m_sig_scan; }
    // Archives of repairable sets, already copied into the import folder by
    // Fix : the owner owns the Import tab and knows how to switch to it.
    sigc::signal<void, std::vector<std::string>>& signal_send_to_import() { return m_sig_send_to_import; }
    // Something happened worth a line in the shared log (owner decides where).
    sigc::signal<void, std::string>& signal_log() { return m_sig_log; }

private:
    // ── Layout ──────────────────────────────────────────────────────────────
    void build_header();
    void build_summary();
    void build_table();
    void build_detail();
    void build_footer();

    // ── Audit ───────────────────────────────────────────────────────────────
    void on_audit_clicked();
    void worker_audit();
    void populate();
    void update_summary();
    void update_last_audit_label();
    bool row_visible(const Gtk::TreeModel::const_iterator& it) const;
    void refilter();
    void on_selection_changed();
    void show_set_detail(const Gtk::TreeModel::Row& row);
    void show_orphan_detail(const Gtk::TreeModel::Row& row);

    // ── Actions ─────────────────────────────────────────────────────────────
    void on_row_toggled(const Glib::ustring& path);
    void set_all_checked(bool on);
    void on_context_menu(const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn* column, GdkEventButton* event);
    void copy_to_clipboard(const Glib::ustring& text, const Glib::ustring& what);
    std::string all_details_of(const Gtk::TreeModel::Row& row) const;
    void search_on_web(const Gtk::TreeModel::Row& row);
    void toggle_ignore(const Gtk::TreeModel::Row& row);
    // Fix : on the given rows, or when empty on the checked rows, or when
    // nothing is checked on every actionable row shown.
    void on_fix_clicked(std::vector<Gtk::TreeModel::Row> rows = {});
    void worker_fix();
    std::vector<Gtk::TreeModel::Row> fix_candidates() const;
    void on_export(int format);   // 0 text, 1 csv, 2 dat
    void update_action_buttons();
    std::vector<Gtk::TreeModel::Row> checked_rows() const;

    // ── Worker plumbing ─────────────────────────────────────────────────────
    enum class Job { None, Audit, Fix };
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
    Gtk::ComboBoxText   m_dat_group;
    std::vector<DatSource::Group> m_groups;      // active ones, in combo order
    bool                m_groups_loading = false;
    const DatSource::Group* current_group() const;
    void persist_group_choice();
    std::set<std::string> m_job_dat_sources;     // the group's files, for the worker
    std::string           m_job_emulator = "fbneo";  // the group's emulator, for the worker
    std::string           current_emulator() const;
    Gtk::Button*        m_btn_scan  = nullptr;
    Gtk::Button*        m_btn_audit = nullptr;
    Gtk::Label          m_last_audit;
    Gtk::Label          m_bios_line;
    Gtk::Box            m_pills{Gtk::ORIENTATION_HORIZONTAL, 8};
    SettingsUi::Pill*   m_pill_total     = nullptr;
    SettingsUi::Pill*   m_pill_correct   = nullptr;
    SettingsUi::Pill*   m_pill_missing   = nullptr;
    SettingsUi::Pill*   m_pill_incorrect = nullptr;
    SettingsUi::Pill*   m_pill_misnamed  = nullptr;
    SettingsUi::Pill*   m_pill_fixable   = nullptr;
    SettingsUi::Pill*   m_pill_orphan    = nullptr;
    SettingsUi::Pill*   m_pill_ignored   = nullptr;
    Gtk::MenuButton*    m_btn_export = nullptr;
    Gtk::Menu           m_export_menu;

    SettingsUi::FilterBar* m_filter = nullptr;
    Gtk::ComboBoxText*     m_system_combo = nullptr;
    SettingsUi::Table*     m_table = nullptr;
    SettingsUi::DetailPanel* m_detail = nullptr;
    Gtk::Menu              m_context_menu;

    Gtk::Box            m_footer{Gtk::ORIENTATION_HORIZONTAL, 10};
    Gtk::ProgressBar    m_progress;
    Gtk::Label          m_status;
    Gtk::Button*        m_btn_cancel = nullptr;
    Gtk::Button*        m_btn_fix    = nullptr;
    Gtk::Button*        m_btn_select_all = nullptr;
    Gtk::Button*        m_btn_select_none = nullptr;
    sigc::connection    m_flash_timer;

    // ── Model ───────────────────────────────────────────────────────────────
    enum Kind { KIND_SET = 0, KIND_ORPHAN = 1 };
    struct Columns : public Gtk::TreeModel::ColumnRecord {
        Gtk::TreeModelColumn<bool>          include;
        Gtk::TreeModelColumn<Glib::ustring> status;      // shown
        Gtk::TreeModelColumn<Glib::ustring> status_key;  // available|misnamed|fixable|incorrect|missing|orphan
        Gtk::TreeModelColumn<Glib::ustring> game;
        Gtk::TreeModelColumn<Glib::ustring> system;
        Gtk::TreeModelColumn<Glib::ustring> parent;
        Gtk::TreeModelColumn<Glib::ustring> expected;
        Gtk::TreeModelColumn<Glib::ustring> yours;
        Gtk::TreeModelColumn<Glib::ustring> details;
        Gtk::TreeModelColumn<Glib::ustring> search_blob; // lower-case haystack
        Gtk::TreeModelColumn<int>           kind;
        Gtk::TreeModelColumn<unsigned int>  index;       // into m_audit.games / m_audit.orphans
        Gtk::TreeModelColumn<bool>          repairable;
        Gtk::TreeModelColumn<bool>          ignored;
        Gtk::TreeModelColumn<bool>          has_extras;
        Gtk::TreeModelColumn<bool>          actionable;  // can be checked
        Columns() {
            add(include); add(status); add(status_key); add(game); add(system); add(parent);
            add(expected); add(yours); add(details); add(search_blob); add(kind); add(index);
            add(repairable); add(ignored); add(has_extras); add(actionable);
        }
    };
    Columns m_cols;
    Glib::RefPtr<Gtk::ListStore>       m_store;
    SettingsUi::ModelStack             m_models;    // filter + sort, rebuilt on every change
    Glib::ustring m_vis_system;                     // filter inputs, snapshotted per refilter
    std::string   m_vis_needle;
    Gtk::TreeModel::Row source_row(const Gtk::TreeModel::Path& sorted_path) const;

    // Status colours, read from the style sheet once the tab is on screen.
    struct StatusColours { Gdk::RGBA ok, warn, err, info, muted, accent; bool ready = false; } m_colours;
    void ensure_colours();

    // ── State ───────────────────────────────────────────────────────────────
    RomAudit::Report m_audit;
    bool m_audit_ever_run = false;
    Paths m_job_paths;   // snapshot for the worker

    // Fix job input, decided on the main thread ; counters filled by the worker.
    struct FixJob {
        struct Whole { std::string archive, dat_header, system; };
        struct Extras { std::string archive, system, dat_header; std::vector<std::string> entries; };
        std::vector<Whole>  whole;      // unrepairable sets (wrong data) → quarantine
        std::vector<Whole>  orphans;    // archives no DAT entry claims → quarantine
        std::vector<Extras> extras;     // entries pulled out of sound archives → quarantine
        std::vector<std::string> repairable;   // archives copied into the import folder
        std::vector<std::string> sent;         // what actually landed there (copied or already present)
        int moved = 0, cleaned = 0, copied = 0, failed = 0;
    } m_fix;

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

    sigc::signal<void, std::string> m_sig_rescan;
    sigc::signal<void, std::string> m_sig_scan;
    sigc::signal<void, std::vector<std::string>> m_sig_send_to_import;
    sigc::signal<void, std::string> m_sig_log;
};
