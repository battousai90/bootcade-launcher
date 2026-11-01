// src/RomRebuildTab.h
//
// The Rebuild tab of ROM Management : clrmamepro's Rebuilder.
//
// Some DATs (a Bootcade group's, a DAT file, or the DATs of a folder, read
// for this run only), a source folder and a destination : the sets of those
// DATs are rebuilt into the destination as non-merged, split or merged, or
// exactly as the DAT lists them, from every file of the source that holds a
// piece of them. Preview shows, before anything is written, what each DAT
// can give and the plan archive by archive ; Rebuild carries it out
// (RomRebuild). Nothing is ever deleted : not a source, not the library, not
// a DAT group or its rules.
#pragma once

#include "DatabaseManager.h"
#include "RomRebuild.h"
#include "SettingsUi.h"

#include <gtkmm.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class RomRebuildTab : public Gtk::Box {
public:
    explicit RomRebuildTab(std::shared_ptr<DatabaseManager> db);
    ~RomRebuildTab() override;

    // The DAT tab changed the groups : the combo and the DAT list follow.
    void reload_groups();
    // Re-read the tab's own keys of config.json (rom_manager.rebuild_*).
    void reload_settings();
    bool busy() const { return m_busy.load(); }

private:
    void build_options();
    void build_results();
    void build_footer();
    void save_settings() const;

    // The DATs offered by the chosen origin, ticked as last time.
    void fill_dats();
    RomRebuild::Options options_from_ui() const;
    // A choice changed : the preview no longer describes what Rebuild would do.
    void invalidate();
    // After a preview : what each DAT can give, and the set types it cannot.
    void show_structure();
    void reset_modes();
    void on_preview_clicked();
    void on_rebuild_clicked();
    void worker_analyze();
    void worker_apply();
    void populate();
    void clear_plan(const Glib::ustring& status);
    void update_summary();
    bool row_visible(const Gtk::TreeModel::const_iterator& it) const;
    void refilter();
    void on_selection_changed();
    void update_action_buttons();
    void on_row_toggled(const Glib::ustring& path);
    void set_all_checked(bool on);
    // The archives Rebuild writes : the checked ones, or all when none is.
    int  to_write() const;
    Gtk::Window* toplevel();

    // ── Worker plumbing ─────────────────────────────────────────────────────
    enum class Job { None, Analyze, Apply };
    RomRebuild::Callbacks make_callbacks();
    void push_progress(double pct, const std::string& msg);
    void push_log(const std::string& msg);
    void on_progress_update();
    void on_worker_finished();
    void set_busy(bool busy);

    std::shared_ptr<DatabaseManager> m_db;

    // ── Widgets ─────────────────────────────────────────────────────────────
    Gtk::Box          m_top{Gtk::ORIENTATION_HORIZONTAL, SettingsUi::kCardSpacing};
    Gtk::ComboBoxText m_origin;          // group | file | folder
    Gtk::Stack        m_origin_stack;
    Gtk::ComboBoxText m_group;
    Gtk::Entry        m_entry_dat_file, m_entry_dat_folder;
    Gtk::Button       *m_btn_dat_file = nullptr, *m_btn_dat_folder = nullptr;
    Gtk::Box          m_dat_list{Gtk::ORIENTATION_VERTICAL, 2};
    std::vector<std::pair<std::string, Gtk::CheckButton*>> m_dat_checks;   // DAT, its box
    Gtk::Entry        m_entry_source, m_entry_dest;
    Gtk::Button       *m_btn_source = nullptr, *m_btn_dest = nullptr;
    Gtk::CheckButton  m_check_recursive;
    Gtk::RadioButton  m_radio_non_merged, m_radio_split, m_radio_merged, m_radio_listed;
    Gtk::ComboBoxText m_format;
    Gtk::CheckButton  m_check_recompress;
    Gtk::CheckButton  m_check_incomplete;
    Gtk::Label        m_status;
    Gtk::Label        m_structure;       // what each DAT can give, after a preview

    SettingsUi::Pill *m_pill_write = nullptr, *m_pill_already = nullptr, *m_pill_incomplete = nullptr,
                     *m_pill_impossible = nullptr, *m_pill_undetermined = nullptr, *m_pill_total = nullptr;
    SettingsUi::FilterBar*   m_filter = nullptr;
    Gtk::ComboBoxText*       m_dat_combo = nullptr;
    SettingsUi::Table*       m_table = nullptr;
    SettingsUi::DetailPanel* m_detail = nullptr;
    SettingsUi::LogPanel*    m_log = nullptr;

    Gtk::Box          m_footer{Gtk::ORIENTATION_HORIZONTAL, 10};
    Gtk::ProgressBar  m_progress;
    Gtk::Label        m_progress_label;
    Gtk::Button*      m_btn_cancel  = nullptr;
    Gtk::Button*      m_btn_preview = nullptr;
    Gtk::Button*      m_btn_rebuild = nullptr;
    Gtk::Button*      m_btn_select_all  = nullptr;
    Gtk::Button*      m_btn_select_none = nullptr;

    // ── Model ───────────────────────────────────────────────────────────────
    struct Columns : public Gtk::TreeModel::ColumnRecord {
        Gtk::TreeModelColumn<Glib::ustring> status;
        Gtk::TreeModelColumn<Glib::ustring> status_key;   // write|already|incomplete|impossible|undetermined
        Gtk::TreeModelColumn<Glib::ustring> set;
        Gtk::TreeModelColumn<Glib::ustring> dat;
        Gtk::TreeModelColumn<Glib::ustring> roms;
        Gtk::TreeModelColumn<Glib::ustring> sources;
        Gtk::TreeModelColumn<Glib::ustring> size;
        Gtk::TreeModelColumn<Glib::ustring> details;
        Gtk::TreeModelColumn<Glib::ustring> search_blob;
        Gtk::TreeModelColumn<unsigned int>  index;
        Gtk::TreeModelColumn<bool>          include;      // checked : written by Rebuild
        Gtk::TreeModelColumn<bool>          actionable;   // something to write, so it can be checked
        Columns() {
            add(status); add(status_key); add(set); add(dat); add(roms); add(sources);
            add(size); add(details); add(search_blob); add(index); add(include); add(actionable);
        }
    };
    Columns m_cols;
    Glib::RefPtr<Gtk::ListStore> m_store;
    SettingsUi::ModelStack       m_models;
    Glib::ustring m_vis_dat;
    std::string   m_vis_needle;

    struct StatusColours { Gdk::RGBA ok, warn, err, muted, accent; bool ready = false; } m_colours;
    void ensure_colours();

    // ── State ───────────────────────────────────────────────────────────────
    bool                    m_loading = false;   // filling the inputs : no save, no invalidate
    RomRebuild::Report      m_report;
    RomRebuild::ApplyResult m_apply_result;
    RomRebuild::Options     m_job_options;

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
};
