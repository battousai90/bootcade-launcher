// src/RomDownloadTab.h
//
// Onglet Downloads du ROM Manager : les jeux qui manquent a la bibliotheque
// (d'apres l'audit de l'onglet Library), ce que la source choisie en propose,
// le dossier ou ils arrivent, et la file des telechargements, avec leur
// progression et de quoi les annuler un par un ou tous.
#pragma once

#include "Pager.h"
#include <gtkmm.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "DownloadSources.h"
#include "RomAudit.h"
#include "SettingsUi.h"

class RomDownloadTab : public Gtk::Box {
public:
    struct Env {
        std::function<const RomAudit::Report*()> report;   // nul tant qu'aucun audit n'a tourne
        std::function<std::string()> inbox;                // le dossier d'import
        std::function<std::string()> group_label;          // « FinalBurn Neo Nightly · 66adff7 »
        std::function<std::string()> outbox;               // le dossier Outbox
        std::function<std::string(const std::string&)> library_root;   // 1er dossier de ROMs de l'emulateur
    };
    explicit RomDownloadTab(Env env);
    ~RomDownloadTab() override;

    // L'audit de Library a change : la liste des manquants se refait.
    void refresh();
    // Le groupe de DAT et sa version, relus quand l'onglet revient devant.
    void update_group() { m_group.set_text(m_env.group_label ? m_env.group_label() : std::string()); }
    bool busy() const { return m_running.load(); }

    sigc::signal<void, std::vector<std::string>>& signal_send_to_import() { return m_sig_import; }
    sigc::signal<void>& signal_audit_requested() { return m_sig_audit; }
    sigc::signal<void, std::string>& signal_log() { return m_sig_log; }
    sigc::signal<void>& signal_account_changed() { return m_sig_account; }
    // Des jeux sont arrives dans la bibliotheque (a rescanner) ou dans l'Outbox.
    sigc::signal<void, std::string>& signal_scan_requested() { return m_sig_scan; }
    sigc::signal<void>& signal_outbox_changed() { return m_sig_outbox; }

    static constexpr int kMaxAtOnce = 200;

private:
    struct Set {
        std::string dat_header, name, description, system, emulator, status;
        bool is_chd = false;
        std::vector<unsigned long> crcs;
        bool owned = false;   // deja dans la bibliotheque (ou reparable depuis elle)
    };
    enum class Avail { Unknown, Yes, Verified, Unverified, No, Outdated };

    void build_top();
    void build_table();
    void build_queue();
    void build_footer();

    void reload_sources();
    DownloadSources::Source current_source() const;
    bool eligible(const Set& s, const DownloadSources::Source& src) const;
    void populate();
    void refilter();
    bool row_visible(const Gtk::TreeModel::const_iterator& it) const;
    void update_buttons();
    void update_quota();
    void start_availability_check();
    void on_toggled(const Glib::ustring& path);
    void set_all(bool on);
    std::string destination(const Set& s) const;   // selon le choix « Download to »
    std::string target() const { return m_target.get_active_id(); }
    void update_target_ui();

    void enqueue_selected();
    void start_worker();
    void worker();
    void on_worker_event();
    void refresh_queue_rows();

    Env m_env;
    std::vector<Set> m_sets;
    std::string m_emulator;

    // ── Source et dossier ────────────────────────────────────────────────
    Gtk::ComboBoxText m_source;
    std::vector<DownloadSources::Source> m_sources;
    bool m_sources_loading = false;
    Gtk::Label m_account;      // compte et quota de la source
    Gtk::Box   m_conn{Gtk::ORIENTATION_HORIZONTAL, 0};   // « ● Connected as … » a cote de Manage
    Gtk::LevelBar m_quota_bar;
    void show_connection(const std::string& user, bool connected);
    void show_quota(int used, int limit);
    Gtk::Label m_group;
    Gtk::ComboBoxText m_target;
    Gtk::Box*  m_folder_row = nullptr;
    Gtk::Entry m_folder;
    Gtk::Label m_folder_note;

    // ── Liste des manquants ──────────────────────────────────────────────
    SettingsUi::FilterBar* m_filter = nullptr;
    Gtk::ComboBoxText*     m_system_combo = nullptr;
    // Les filtres : un jeu est dans UNE de ces categories ; « deja possede » s'y
    // ajoute pour ceux qu'on a deja.
    Gtk::FlowBox           m_pills;
    // Au milieu de la liste pendant qu'elle se prepare : sans lui, l'ecran semble fige.
    Gtk::Overlay           m_overlay;
    Gtk::Box               m_loading{Gtk::ORIENTATION_HORIZONTAL, 12};
    Gtk::Spinner           m_spinner;
    Gtk::Label             m_loading_text;
    void set_loading(bool on, const Glib::ustring& text = Glib::ustring());
    Gtk::CheckButton *m_p_invalid = nullptr, *m_p_all = nullptr;
    void set_filter_counts(int invalid, int all);
    struct Colours { Gdk::RGBA ok, err, muted; bool ready = false; } m_colours;
    void ensure_colours();
    void style_cell(Gtk::CellRenderer* renderer, const Gtk::TreeModel::iterator& it, bool crc_column);
    SettingsUi::Table*     m_table = nullptr;
    SettingsUi::ModelStack m_models;
    SettingsUi::Pager*     m_pager = nullptr;
    Gtk::Box*              m_list_box = nullptr;
    void attach_models();
    void on_page_changed();
    struct Columns : public Gtk::TreeModel::ColumnRecord {
        Gtk::TreeModelColumn<bool>          include;
        Gtk::TreeModelColumn<Glib::ustring> game, system, status, avail, crc, size, search;
        Gtk::TreeModelColumn<int>           avail_key;   // Avail, ou -1 : source qui ne le propose pas
        Gtk::TreeModelColumn<unsigned int>  index;
        Gtk::TreeModelColumn<bool>          selectable, owned;
        Gtk::TreeModelColumn<int>           cat;        // 0 disponible, 1 autre version, 2 pas verifie, 3 absent
        Gtk::TreeModelColumn<int>           crc_state;  // 0 OK, 1 KO, 2 inconnu
        Columns() { add(include); add(game); add(system); add(status); add(avail); add(crc); add(size); add(search);
                    add(avail_key); add(index); add(selectable); add(owned); add(cat); add(crc_state); }
    } m_cols;
    Glib::RefPtr<Gtk::ListStore> m_store;
    Glib::ustring m_vis_system;
    std::string   m_vis_needle;
    Gtk::Label    m_list_status;
    Gtk::Button*  m_btn_audit = nullptr;
    Gtk::Button*  m_btn_refresh = nullptr;

    // Ce que la source a, par jeu (cle systeme/nom), pour la source m_avail_for.
    std::map<std::string, Avail> m_avail;
    std::map<std::string, std::string> m_size, m_check_size;   // taille sur la source
    std::map<std::string, std::pair<int, int>> m_crcinfo, m_check_crcinfo;   // CRC du DAT dans le fichier : trouves / attendus
    std::string m_avail_for;
    std::thread m_check;
    std::atomic<bool> m_check_cancel{false};
    std::atomic<bool> m_checking{false};
    std::map<std::string, Avail> m_check_result;
    std::string m_check_for;
    int m_check_gen = 0;                       // fil GTK
    std::atomic<int> m_check_finished{-1};
    Glib::Dispatcher m_check_done;

    // ── File des telechargements ─────────────────────────────────────────
    enum class State { Queued, Running, Done, Failed, Cancelled };
    struct Item {
        Set set;
        DownloadSources::Source source;
        std::string dest;
        std::string target;        // inbox | outbox | library | folder
        bool force = false;        // autre version choisie a la main : on ne la jette pas
        State state = State::Queued;
        double progress = 0.0;
        std::string note;
        std::string path;
        // Widgets : fil GTK seulement.
        Gtk::ListBoxRow* row = nullptr;
        Gtk::Label* label = nullptr;
        Gtk::Label* state_label = nullptr;
        Gtk::ProgressBar* bar = nullptr;
        Gtk::Button* cancel = nullptr;
    };
    std::vector<std::shared_ptr<Item>> m_queue;   // m_qmutex
    std::mutex m_qmutex;
    Gtk::ListBox m_queue_list;
    Gtk::Label   m_queue_summary;
    Gtk::Button* m_btn_cancel_all = nullptr;
    Gtk::Button* m_btn_clear = nullptr;
    std::thread m_worker;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_cancel_current{false};
    std::atomic<bool> m_stop_all{false};
    Glib::Dispatcher m_worker_event;
    std::vector<std::string> m_received;          // m_qmutex : archives a passer a Import
    std::vector<std::string> m_log_lines;         // m_qmutex

    Gtk::Button* m_btn_download = nullptr;

    sigc::signal<void, std::vector<std::string>> m_sig_import;
    sigc::signal<void> m_sig_audit;
    sigc::signal<void, std::string> m_sig_log;
    sigc::signal<void> m_sig_account;
    sigc::signal<void, std::string> m_sig_scan;
    sigc::signal<void> m_sig_outbox;
    std::vector<std::string> m_to_library;      // m_qmutex : emulateurs dont la bibliotheque a recu des jeux
    bool m_outbox_got = false;                  // m_qmutex
    std::vector<std::string> m_wrong_keys;      // m_qmutex : jeux recus dans une autre version
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
