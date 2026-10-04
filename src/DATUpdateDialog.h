// src/DATUpdateDialog.h
#pragma once
#include <gtkmm.h>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include "DatabaseManager.h"
#include "SettingsUi.h"

class DATUpdateDialog : public Gtk::Dialog {
public:
    // `files` : the DAT files to build the database from (DatSource::files_to_load :
    // the union of what active groups select). `dat_path` is only shown.
    // `emulator` : the one whose games are rebuilt ("fbneo", "mame") ; the
    // other one's games and statuses are left as they are. Empty rebuilds
    // every emulator. `files` must then be that emulator's DATs only
    // (files_for_update).
    DATUpdateDialog(Gtk::Window& parent, std::shared_ptr<DatabaseManager> db, const std::string& dat_path,
                    std::vector<std::string> files, std::string emulator = "");

    // The DAT files an update of `emulator` loads : those of the active groups
    // describing it (every active group when empty).
    static std::vector<std::string> files_for_update(const std::string& emulator,
                                                     std::vector<std::string>* conflicts = nullptr);
    virtual ~DATUpdateDialog();

    void start_update();
    // What was left out before loading (DatSource::files_to_load) : shown in
    // the log, not only in the terminal.
    void note_conflicts(const std::vector<std::string>& conflicts);
    bool was_cancelled() const { return m_cancelled.load(); }
    // The games were wiped and (partly) reloaded : whoever shows them must
    // read them again, even when the update was stopped half-way.
    bool database_changed() const { return m_database_changed.load(); }

private:
    using Level = SettingsUi::LogPanel::Level;

    void on_cancel_clicked();
    void on_close_requested();
    void update_progress(double percentage, const std::string& current_file, const std::string& message);
    void add_log_message(const std::string& message, Level level = Level::Info);

    // Threading
    void worker_thread();
    void on_progress_update();
    void on_update_finished();

    std::shared_ptr<DatabaseManager> m_db;
    std::string m_dat_path;
    std::vector<std::string> m_files;
    std::string m_emulator;   // empty = every emulator
    std::atomic<bool> m_cancelled{false};
    std::atomic<bool> m_failed{false};
    std::atomic<bool> m_database_changed{false};

    // UI Components
    Gtk::Box m_main_box{Gtk::ORIENTATION_VERTICAL, 0};
    Gtk::Box m_body{Gtk::ORIENTATION_VERTICAL, 14};
    Gtk::Label* m_step_label = nullptr;   // l'etape en cours, dans l'en-tete

    // Progress section
    Gtk::Box m_progress_box{Gtk::ORIENTATION_VERTICAL, 6};
    Gtk::ProgressBar m_progress_bar;
    Gtk::Label m_percentage_label;

    // Log section
    SettingsUi::LogPanel* m_log = nullptr;

    // Buttons
    Gtk::Button* m_cancel_button = nullptr;
    Gtk::Button* m_close_button  = nullptr;

    // Threading
    std::thread m_worker_thread;
    // Le minuteur de fermeture automatique : arme a la fin d'une mise a jour
    // reussie, coupe au plus tard par le destructeur pour qu'il ne puisse pas
    // se declencher sur une boite deja detruite.
    sigc::connection m_autoclose;
    Glib::Dispatcher m_progress_dispatcher;
    Glib::Dispatcher m_finished_dispatcher;

    // Shared data : writes from worker thread, reads from main thread.
    // m_shared_mutex protects m_current_file, m_current_message and m_log_messages.
    // m_current_progress and m_update_finished are atomics for lock-free access.
    mutable std::mutex m_shared_mutex;
    std::atomic<double> m_current_progress{0.0};
    std::string m_current_file;
    std::string m_current_message;
    std::vector<std::pair<std::string, Level>> m_log_messages;
    std::atomic<bool> m_update_finished{false};
};
