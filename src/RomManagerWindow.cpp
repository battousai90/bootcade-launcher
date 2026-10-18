// src/RomManagerWindow.cpp
#include "FileChooser.h"
#include "RomManagerWindow.h"
#include <functional>
#include "RomArchive.h"
#include "RomImportTab.h"
#include "RomLibraryTab.h"
#include "RomOutboxTab.h"
#include "RomQuarantineTab.h"
#include "RomDatTab.h"
#include "DatSource.h"
#include "RomResolve.h"
#include "RomManifest.h"
#include "SettingsUi.h"

#include "AppContext.h"
#include "ConfirmationDialog.h"
#include "GenerateDAT.h"
#include "i18n.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <set>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace {

// Same throttle as ROMScanDialog: emitting one dispatcher signal per processed file
// floods the GTK main loop and makes the window look hung.
constexpr int UI_DISPATCH_INTERVAL_MS = 100;

std::string human_size(uintmax_t bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    std::ostringstream os;
    os << std::fixed << std::setprecision(v < 10.0 && u > 0 ? 1 : 0) << v << ' ' << units[u];
    return os.str();
}

std::string join_preview(const std::vector<std::string>& items, size_t max_items) {
    std::string out;
    for (size_t i = 0; i < items.size() && i < max_items; ++i) {
        if (i) out += ", ";
        out += items[i];
    }
    if (items.size() > max_items)
        out += ", … (+" + std::to_string(items.size() - max_items) + ")";
    return out;
}

} // namespace

RomManagerWindow::RomManagerWindow(Gtk::Window& parent, std::shared_ptr<DatabaseManager> db)
    : m_db(db), m_parent(parent)
{
    namespace ui = SettingsUi;
    set_title(_("ROM Management"));
    // Au moins la taille de travail, et plus large si un onglet le demande :
    // une taille fixe (1400 x 900) rognait l'onglet DAT a droite.
    SettingsUi::fit_work_window(*this, &parent);
    SettingsUi::dim_behind(*this, parent);
    set_transient_for(parent);
    set_position(Gtk::WIN_POS_CENTER_ON_PARENT);
    get_style_context()->add_class("cc-window");
    get_style_context()->add_class("set-window");

    // ── Title bar : the Settings window's, same tile, same close ─────────
    auto* head = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 12);
    head->pack_start(*ui::tile("database.svg", 21, 36, /*accent=*/true), Gtk::PACK_SHRINK);
    auto* head_txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
    head_txt->set_valign(Gtk::ALIGN_CENTER);
    auto* head_title = Gtk::make_managed<Gtk::Label>(_("ROM Management"));
    head_title->set_xalign(0.0f);
    head_title->get_style_context()->add_class("set-head-title");
    m_header_sub.set_xalign(0.0f);
    m_header_sub.get_style_context()->add_class("set-head-sub");
    head_txt->pack_start(*head_title,  Gtk::PACK_SHRINK);
    head_txt->pack_start(m_header_sub, Gtk::PACK_SHRINK);
    head->pack_start(*head_txt, Gtk::PACK_SHRINK);

    auto* close = Gtk::make_managed<Gtk::Button>();
    close->set_image(*ui::image("bc-close.svg", 18));
    close->set_tooltip_text(_("Close"));
    close->get_style_context()->add_class("set-close");
    close->set_valign(Gtk::ALIGN_CENTER);
    close->signal_clicked().connect([this] { if (!busy()) hide(); });

    m_headerbar.set_show_close_button(false);
    m_headerbar.pack_start(*head);
    m_headerbar.pack_end(*close);
    m_headerbar.set_custom_title(*Gtk::make_managed<Gtk::Box>());
    m_headerbar.show_all();
    set_titlebar(m_headerbar);

    // ── Tabs ──────────────────────────────────────────────────────────────
    m_import = Gtk::make_managed<RomImportTab>(m_db, [this] {
        RomImportTab::Paths p;
        p.outbox     = config_string("outbox_path");
        p.quarantine = config_string("quarantine_path");
        p.emulator   = DatSource::library_group().emulator;
        p.roms_paths = read_roms_paths(p.emulator);
        return p;
    });
    m_import->signal_outbox_changed().connect([this] { m_outbox->refresh(); m_quarantine->refresh(); });

    m_outbox = Gtk::make_managed<RomOutboxTab>(m_db, [this] {
        RomOutboxTab::Paths p;
        p.outbox     = config_string("outbox_path");
        p.quarantine = config_string("quarantine_path");
        p.roms_paths = read_roms_paths(DatSource::library_group().emulator);
        return p;
    });
    m_outbox->signal_scan_requested().connect([this](std::string emulator) { m_sig_scan_requested.emit(emulator); });
    m_outbox->signal_quarantine_changed().connect([this] { m_quarantine->refresh(); });
    m_outbox->signal_outbox_path_changed().connect([this](std::string folder) { m_sig_outbox_path_changed.emit(folder); });

    m_quarantine = Gtk::make_managed<RomQuarantineTab>(m_db, [this] {
        RomQuarantineTab::Paths p;
        p.quarantine = config_string("quarantine_path");
        p.inbox      = m_import->inbox_path();
        return p;
    });
    m_quarantine->signal_log().connect([this](std::string line) { push_log(line); });
    m_quarantine->signal_quarantine_path_changed().connect([this](std::string folder) { m_sig_quarantine_path_changed.emit(folder); });
    // Files went back to the import folder : Import is where the user
    // decides what to do with them next.
    m_quarantine->signal_restored_to_import().connect([this](int) { show_tab("import"); });

    m_dat = Gtk::make_managed<RomDatTab>(m_db, [this] {
        RomDatTab::Env e;
        e.fbneo_executable = fbneo_executable();
        return e;
    });
    m_dat->signal_reload_database().connect([this](bool confirm, std::string emulator) { m_sig_update_dat.emit(confirm, emulator); });
    m_dat->signal_folder_changed().connect([this](std::string folder) { m_sig_dat_path_changed.emit(folder); });
    m_dat->signal_generate_requested().connect([this](std::string folder, std::string exe) {
        if (exe.empty()) {
            SettingsUi::notice(*this, _("This FinalBurn Neo is not installed."),
                               _("Install and set it in Settings › Emulator before generating DAT files."));
            return;
        }
        // GenerateDAT runs the emulator and shows its own dialogs; once it
        // returns, the folder has changed and the database follows.
        GenerateDAT::execute(*this, exe, folder);
        m_dat->refresh();
        m_dat->note_generated();
        m_sig_update_dat.emit(false, "fbneo");
    });

    m_library = Gtk::make_managed<RomLibraryTab>(m_db, [this] {
        RomLibraryTab::Paths p;
        p.roms_paths = read_roms_paths(DatSource::library_group().emulator);
        p.inbox      = m_import->inbox_path();
        p.quarantine = config_string("quarantine_path");
        return p;
    });
    m_library->signal_rescan_requested().connect([this](std::string emulator) { m_sig_rescan_requested.emit(emulator); });
    m_library->signal_scan_requested().connect([this](std::string emulator) { m_scan_pending = true; m_sig_scan_requested.emit(emulator); });
    m_library->signal_log().connect([this](std::string line) { push_log(line); });
    m_library->signal_send_to_import().connect(sigc::mem_fun(*this, &RomManagerWindow::on_send_to_import));
    // The DAT groups are the Library's choice of what "complete" means :
    // the combo follows every change made on the DAT tab.
    m_dat->signal_groups_changed().connect([this] { if (m_library) m_library->reload_groups(); });

    // Downloads : les manquants du dernier audit de Library.
    RomDownloadTab::Env denv;
    denv.report      = [this] { return m_library ? m_library->report() : nullptr; };
    denv.inbox       = [this] { return m_import ? m_import->inbox_path() : std::string(); };
    denv.group_label = [this] { return m_library ? m_library->group_label() : std::string(); };
    denv.outbox      = [this] { return config_string("outbox_path"); };
    denv.library_root = [this](const std::string& emulator) {
        const auto roots = read_roms_paths(emulator);
        return roots.empty() ? std::string() : roots.front();
    };
    m_downloads = Gtk::make_managed<RomDownloadTab>(denv);
    m_downloads->signal_audit_requested().connect([this] { if (m_library && !m_library->busy()) { show_tab("library"); m_library->start_audit(); } });
    m_downloads->signal_send_to_import().connect(sigc::mem_fun(*this, &RomManagerWindow::on_send_to_import));
    m_downloads->signal_log().connect([this](std::string line) { push_log(line); });
    m_downloads->signal_account_changed().connect([this] { m_sig_account_changed.emit(); });
    m_downloads->signal_scan_requested().connect([this](std::string emulator) { m_sig_scan_requested.emit(emulator); });
    m_downloads->signal_outbox_changed().connect([this] { if (m_outbox) m_outbox->refresh(); });
    m_library->signal_audited().connect([this] { if (m_downloads) m_downloads->refresh(); });

    m_tabbar.get_style_context()->add_class("set-tabbar");
    m_pages.set_transition_type(Gtk::STACK_TRANSITION_TYPE_NONE);
    m_pages.add(*m_library,       "library");
    m_pages.add(*m_import,        "import");
    m_pages.add(*m_outbox,        "outbox");
    m_pages.add(*m_quarantine,    "quarantine");
    m_pages.add(*m_dat,           "dat");
    m_pages.add(*m_downloads,     "downloads");
    add_tab("library",    "bc-folder.svg",   _("Library"),    _("Scan your ROM library and compare it with DAT files."));
    add_tab("import",     "bc-download.svg", _("Import"),     _("Analyse and repair ROMs before they reach your library."));
    add_tab("outbox",     "bc-package.svg",  _("Outbox"),     _("Repaired ROMs, ready to be moved to your library."));
    add_tab("quarantine", "bc-shield.svg",   _("Quarantine"), _("Files that could not be used, or were rejected."));
    add_tab("dat",        "bc-file.svg",     _("DAT"),        _("The DAT files your library is compared with."));
    add_tab("downloads",  "bc-cloud.svg",    _("Downloads"),  _("Download the sets your library is missing."));

    /* Les onglets defilent, la fenetre garde sa taille.
     *
     * Poses tels quels, ils imposaient leur taille minimale a la fenetre :
     * celle du plus grand des cinq, au-dela de 1400 x 900, et elle changeait
     * avec le contenu, si bien que la fenetre se redimensionnait et se
     * replacait au moindre clic. Dans une zone defilante, le contenu s'adapte
     * a la fenetre et non l'inverse ; chaque onglet garde sa propre taille. */
    m_pages.set_hhomogeneous(false);
    m_pages.set_vhomogeneous(false);
    auto* pages_scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    pages_scroll->set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    pages_scroll->set_shadow_type(Gtk::SHADOW_NONE);
    pages_scroll->add(m_pages);
    // La zone des onglets annonce leur largeur, et elle seule : les tableaux,
    // eux, defilent dans la leur. Sans quoi une colonne de longues phrases
    // demandait toute la largeur de l'ecran.
    pages_scroll->set_propagate_natural_width(true);

    auto* shell = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
    shell->pack_start(m_tabbar,      Gtk::PACK_SHRINK);
    shell->pack_start(*pages_scroll, Gtk::PACK_EXPAND_WIDGET);
    add(*shell);

    reload_settings();
    // Dans l'onglet DAT, les listes annoncent aussi leur largeur : leurs
    // colonnes sont courtes (noms de groupes, nombres, dates), et rognees elles
    // ne se lisaient plus. Pas dans Library ni Import : leur colonne Details
    // porte de longues phrases, qui demanderaient tout l'ecran.
    std::function<void(Gtk::Widget*)> propagate = [&](Gtk::Widget* w) {
        if (auto* sw = dynamic_cast<Gtk::ScrolledWindow*>(w))
            if (sw->get_name() != "no-propagate-width") sw->set_propagate_natural_width(true);
        if (auto* c = dynamic_cast<Gtk::Container*>(w))
            for (Gtk::Widget* child : c->get_children()) propagate(child);
    };
    if (m_dat) propagate(m_dat);
    show_all_children();
    show_tab("library");
}

void RomManagerWindow::add_tab(const std::string& id, const std::string& icon_file,
                               const std::string& label, const std::string& subtitle) {
    namespace ui = SettingsUi;
    auto* btn = Gtk::make_managed<Gtk::ToggleButton>();
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 9);
    box->set_halign(Gtk::ALIGN_CENTER);
    box->pack_start(*ui::image(icon_file, 17), Gtk::PACK_SHRINK);
    box->pack_start(*Gtk::make_managed<Gtk::Label>(label), Gtk::PACK_SHRINK);
    btn->add(*box);
    btn->get_style_context()->add_class("set-tab");
    btn->set_active(m_tabs.empty());
    // "clicked", not "toggled" : the same reasoning as SettingsPanel::add_tab.
    btn->signal_clicked().connect([this, id] { show_tab(id); });
    m_tabs.push_back({id, btn, subtitle});
    m_tabbar.pack_start(*btn, Gtk::PACK_SHRINK);
}

void RomManagerWindow::show_tab(const std::string& id) {
    if (m_tab_switching) return;
    m_tab_switching = true;
    for (auto& t : m_tabs) {
        t.button->set_active(t.id == id);
        if (t.id == id) m_header_sub.set_text(t.subtitle);
    }
    m_pages.set_visible_child(id);
    m_tab_switching = false;
    // Folders change behind these two tabs (Fix, Move, a manual drop) : what
    // they show is re-read every time they come to the front.
    if (id == "outbox"     && m_outbox     && !m_outbox->busy()) m_outbox->refresh();
    if (id == "quarantine" && m_quarantine)                      m_quarantine->refresh();
    if (id == "dat"        && m_dat        && !m_dat->busy())    m_dat->refresh();
    if (id == "downloads"  && m_downloads)                       m_downloads->update_group();
    grow_to_page();
}

/* La fenetre n'est jamais plus etroite que l'onglet affiche.
 *
 * Chaque onglet garde sa propre taille (pile non homogene) : la fenetre,
 * mesuree a l'ouverture sur Library, coupait l'onglet DAT a droite. Elle
 * s'elargit donc quand l'onglet le demande, sans jamais retrecir : elle ne
 * saute pas d'un onglet a l'autre. Bornee a 95 % de l'ecran. */
void RomManagerWindow::grow_to_page() {
    Glib::signal_idle().connect_once([this] {
        if (!get_visible()) return;
        Gtk::Requisition minimum, natural;
        get_preferred_size(minimum, natural);
        int w = 0, h = 0;
        get_size(w, h);
        int want = std::max(minimum.width, natural.width);
        Gdk::Rectangle work;
        if (auto win = get_window())
            if (auto display = Gdk::Display::get_default())
                if (auto monitor = display->get_monitor_at_window(win)) {
                    monitor->get_workarea(work);
                    if (work.get_width() > 0) want = std::min(want, work.get_width() * 95 / 100);
                }
        // Jamais plus large que la fenetre principale.
        int pw = 0, ph = 0;
        m_parent.get_size(pw, ph);
        if (pw > 40) want = std::min(want, pw - 24);   // marge : bordure et ombre
        if (want <= w) return;
        resize(want, h);
        // Elargie, elle reste au-dessus de la fenetre principale : recentree sur elle.
        int px = 0, py = 0, x = 0, y = 0;
        m_parent.get_position(px, py);
        get_position(x, y);
        move(px + (pw - want) / 2, y);
    });
}

// Library hands over the archives of sets it found repairable : Import takes
// the stage and analyses them straight away : or as soon as the scan Library
// asked for at the same time is done, since that scan rewrites the cache the
// analysis reads.
void RomManagerWindow::on_send_to_import(std::vector<std::string> archives) {
    if (m_scan_pending || (m_import && m_import->busy())) {
        m_pending_import.insert(m_pending_import.end(), archives.begin(), archives.end());
        return;
    }
    show_tab("import");
    m_import->receive(archives);
}

bool RomManagerWindow::busy() const {
    return (m_import && m_import->busy()) || (m_library && m_library->busy()) || (m_outbox && m_outbox->busy());
}

std::string RomManagerWindow::config_string(const char* key) const {
    nlohmann::json j;
    std::ifstream fi(AppContext::get_config_path());
    if (fi) { try { fi >> j; } catch (...) {} }
    if (j.contains("rom_manager") && j["rom_manager"].is_object()) {
        const auto& rm = j["rom_manager"];
        if (rm.contains(key) && rm[key].is_string()) return rm[key].get<std::string>();
    }
    return "";
}

// Every line an old-style tab (Outbox, Quarantine) or Library has to say goes
// to the Import log panel, the one log the window has until those tabs get
// their own : and to debug.log, which is what survives the window.
void RomManagerWindow::push_log(const std::string& msg) {
    std::cerr << "[ROM-MANAGER] " << msg << std::endl;
    if (m_import) m_import->log(msg);
}

RomManagerWindow::~RomManagerWindow() = default;

// ── Import tab ───────────────────────────────────────────────────────────────

void RomManagerWindow::on_browse(Gtk::Entry* entry) {
    BcFileChooser dialog(*this, _("Select Folder"), Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
    dialog.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dialog.add_button(_("Select"), Gtk::RESPONSE_OK);
    auto current = entry->get_text();
    if (!current.empty()) dialog.set_current_folder(current);
    if (dialog.run() == Gtk::RESPONSE_OK) {
        entry->set_text(dialog.get_filename());
    }
}

std::vector<std::string> RomManagerWindow::read_roms_paths(const std::string& emulator) const {
    return DatSource::roms_paths_for(emulator);
}

// ── Outbox tab ───────────────────────────────────────────────────────────────

std::string RomManagerWindow::fbneo_executable() const {
    // Owned by the Settings panel; read fresh rather than cached so a change made
    // there while this window stayed open is picked up.
    nlohmann::json j;
    std::ifstream fi(AppContext::get_config_path());
    if (fi) { try { fi >> j; } catch (...) { return {}; } }
    if (j.contains("fbneo_executable") && j["fbneo_executable"].is_string())
        return j["fbneo_executable"].get<std::string>();
    return {};
}

// Settings changed something the tabs read from config.json : each one
// re-reads what is its own.
void RomManagerWindow::reload_settings() {
    if (m_import) m_import->reload_settings();
    if (m_outbox) m_outbox->reload_settings();
    if (m_quarantine) m_quarantine->refresh();
    if (m_dat) m_dat->refresh();
    if (m_library) m_library->reload_groups();
}

void RomManagerWindow::refresh_after_scan() {
    m_scan_pending = false;
    if (m_outbox) m_outbox->refresh();
    if (m_quarantine) m_quarantine->refresh();
    if (m_library) { m_library->reload_groups(); m_library->refresh_after_scan(); }
    if (!m_pending_import.empty() && m_import && !m_import->busy()) {
        std::vector<std::string> archives;
        archives.swap(m_pending_import);
        show_tab("import");
        m_import->receive(archives);
    }
}

