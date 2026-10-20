// src/RomFolderDialog.h : le dossier de ROMs de Bootcade, dans une fenetre de Bootcade.
//
// Le joueur y voit ce que contient le dossier, y entre et en sort, y cree des
// dossiers, et y DEPOSE ses archives depuis son explorateur : elles sont
// copiees ici, sans ouvrir une autre fenetre hors du theme de l'application.
#pragma once

#include "SettingsUi.h"

#include <gtkmm.h>

#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class RomFolderDialog : public Gtk::Dialog {
public:
    static constexpr int kScan = 100;   // reponse : le joueur veut lancer le scan

    RomFolderDialog(Gtk::Window& parent, const std::string& root);
    ~RomFolderDialog() override;

private:
    struct Cols : public Gtk::TreeModel::ColumnRecord {
        Cols() { add(name); add(kind); add(size); add(is_dir); }
        Gtk::TreeModelColumn<Glib::ustring> name, kind, size;
        Gtk::TreeModelColumn<bool>          is_dir;
    };

    void navigate(const std::filesystem::path& dir);
    void refresh();
    void on_row_activated(const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn*);
    void on_up();
    void on_new_folder();
    void on_open_in_manager();
    void on_drop(const Glib::RefPtr<Gdk::DragContext>& ctx, int, int, const Gtk::SelectionData& data,
                 guint info, guint time);
    void start_copy(std::vector<std::filesystem::path> sources);
    void on_copy_progress();
    void on_copy_done();
    void set_busy(bool busy);

    Cols                           m_cols;
    Glib::RefPtr<Gtk::ListStore>   m_store;
    SettingsUi::Table*             m_table = nullptr;
    Gtk::Label*                    m_where = nullptr;
    Gtk::Label*                    m_hint = nullptr;
    Gtk::Button*                   m_up = nullptr;
    Gtk::Button*                   m_new = nullptr;
    Gtk::Button*                   m_scan = nullptr;
    std::filesystem::path          m_root, m_cur;

    Glib::Dispatcher               m_progress_dispatcher, m_done_dispatcher;
    std::mutex                     m_mutex;
    std::string                    m_status;   // sous m_mutex
    int                            m_copied = 0, m_skipped = 0, m_failed = 0;
    bool                           m_busy = false;
    std::thread                    m_worker;
};
