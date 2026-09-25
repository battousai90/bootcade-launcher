// src/RomManagerWindow.h
//
// Dedicated, non-modal window gathering everything that manipulates ROM files:
// the inbox → outbox import/rebuild flow, a view of the produced outbox, and DAT
// management (generation from FBNeo, database refresh).
//
// It never writes inside the configured roms_paths. Analyse/Fix produce a separate
// outbox tree laid out like the DATs; the user then decides whether to copy it over
// or simply add the outbox to their ROM directories and run a normal scan.
#pragma once

#include <gtkmm.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "DatabaseManager.h"
#include "RomImportTab.h"
#include "RomLibraryTab.h"
#include "RomOutboxTab.h"
#include "RomQuarantineTab.h"
#include "RomDatTab.h"

class RomManagerWindow : public Gtk::Window {
public:
    RomManagerWindow(Gtk::Window& parent, std::shared_ptr<DatabaseManager> db);
    virtual ~RomManagerWindow();

    // Re-read config.json. Called by the owner before presenting the window so a
    // path changed in the Settings panel meanwhile is picked up.
    void reload_settings();

    // Called by the owner once a ROM scan it started finishes : covers the
    // "Move to library" loop (move → scan → the audit should show the set as
    // fixed now, not still flagged) as well as any other scan. Outbox/Quarantine
    // are cheap to refresh unconditionally; the audit only re-runs if one was
    // already performed this session, so this never starts an unrequested scan.
    void refresh_after_scan();

    // Bring one tab to the front: "library", "import", "outbox", "quarantine", "dat".
    void show_tab(const std::string& id);

    // Emitted when the user changes the DAT directory here, so the Settings panel
    // (which owns the same config.json key) can stay in sync.
    sigc::signal<void, std::string>& signal_dat_path_changed() { return m_sig_dat_path_changed; }
    // Same story for the two folders the ROM tabs own : the outbox is chosen on
    // the Outbox tab, the quarantine on the Quarantine tab, and both keys live
    // in the same config.json anyone else may be showing.
    sigc::signal<void, std::string>& signal_outbox_path_changed()     { return m_sig_outbox_path_changed; }
    sigc::signal<void, std::string>& signal_quarantine_path_changed() { return m_sig_quarantine_path_changed; }
    // Emitted when the database must be rebuilt from the DAT files : true asks
    // the owner to confirm first (the explicit menu entry), false follows a
    // change the DAT tab just made to the folder.
    // (confirm, emulator whose games reload ; empty = every emulator)
    sigc::signal<void, bool, std::string>& signal_update_dat() { return m_sig_update_dat; }
    // Emitted after "Move to library" moves files in place : no new path to add,
    // just a rescan of the existing ROM directories.
    // Both scan signals carry the emulator whose library is to be scanned.
    sigc::signal<void, std::string>& signal_scan_requested()   { return m_sig_scan_requested; }
    // Distinct from the above: that one fires as the tail of "Move to library"
    // and must not interrupt the flow, this one is a button the user pressed
    // and goes through the same confirmation the header button always had.
    sigc::signal<void, std::string>& signal_rescan_requested() { return m_sig_rescan_requested; }

private:
    // ── Shell : title bar, tabs ──────────────────────────────────────────────
    void add_tab(const std::string& id, const std::string& icon_file,
                 const std::string& label, const std::string& subtitle);

    // ── Tab construction ─────────────────────────────────────────────────────

    // Library found repairable sets : copy their archives into the inbox and
    // let Import analyse them.
    void on_send_to_import(std::vector<std::string> archives);

    // FBNeo executable path, owned by the Settings panel and read from config.json.
    std::string fbneo_executable() const;

    // The ROM directories of one emulator's library, from config.json
    // (DatSource::roms_paths_for). Every job of this window works for the
    // emulator of the library DAT group (DatSource::library_group). Called on
    // the GTK main thread : the roots belong to the Settings panel, not to
    // this window.
    std::vector<std::string> read_roms_paths(const std::string& emulator) const;

    void on_browse(Gtk::Entry* entry);

    std::shared_ptr<DatabaseManager> m_db;
    Gtk::Window& m_parent;

    // ── Layout ───────────────────────────────────────────────────────────────
    Gtk::HeaderBar m_headerbar;
    Gtk::Label     m_header_sub;
    Gtk::Box       m_tabbar{Gtk::ORIENTATION_HORIZONTAL, 6};
    Gtk::Stack     m_pages;
    struct Tab { std::string id; Gtk::ToggleButton* button; std::string subtitle; };
    std::vector<Tab> m_tabs;
    bool m_tab_switching = false;
    // Library's Fix asked for a scan AND handed sets to Import : the scan
    // rewrites the cache Import reads, so the hand-over waits for it.
    bool m_scan_pending = false;
    std::vector<std::string> m_pending_import;

    RomImportTab*  m_import  = nullptr;
    RomLibraryTab* m_library = nullptr;
    RomOutboxTab*  m_outbox  = nullptr;
    RomQuarantineTab* m_quarantine = nullptr;
    RomDatTab*     m_dat     = nullptr;
    // A string of the "rom_manager" object in config.json.
    std::string config_string(const char* key) const;

    // A tab is working : nothing that moves files may start, and the window
    // stays open.
    bool busy() const;
    // Lines from the tabs that have no log of their own yet go to Import's.
    void push_log(const std::string& msg);


    sigc::signal<void, std::string> m_sig_dat_path_changed;
    sigc::signal<void, std::string> m_sig_outbox_path_changed;
    sigc::signal<void, std::string> m_sig_quarantine_path_changed;
    sigc::signal<void, bool, std::string> m_sig_update_dat;
    sigc::signal<void, std::string> m_sig_scan_requested;
    sigc::signal<void, std::string> m_sig_rescan_requested;
};
