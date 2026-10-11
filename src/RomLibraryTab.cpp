// src/RomLibraryTab.cpp
#include "RomLibraryTab.h"
#include "FileCopy.h"

#include "AppContext.h"
#include "ConfirmationDialog.h"
#include "RomCleanup.h"
#include "RomDownload.h"
#include "BootcadeAuth.h"
#include "RomManifest.h"
#include "i18n.h"

#include <giomm/appinfo.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
namespace ui = SettingsUi;

namespace {

constexpr int UI_DISPATCH_INTERVAL_MS = 100;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string crc_hex(unsigned long crc) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%08lx", crc);
    return buf;
}

std::string human_size(uintmax_t bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    std::ostringstream os;
    os << std::fixed << std::setprecision(v < 10.0 && u > 0 ? 1 : 0) << v << ' ' << units[u];
    return os.str();
}

std::string join(const std::vector<std::string>& items, const char* sep, size_t max_items = 0) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (max_items && i == max_items) { out += sep; out += "… (+" + std::to_string(items.size() - max_items) + ")"; break; }
        if (i) out += sep;
        out += items[i];
    }
    return out;
}

std::string format_time(int64_t t) {
    if (t <= 0) return "";
    std::time_t tt = (std::time_t)t;
    std::tm tm{};
    localtime_r(&tt, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

// The one word the table says about a set. The audit's status is coarse
// ("incorrect" covers a wrong entry name as well as wrong data) ; what the
// user needs to know is whether Fix can repair it, and how :
//   misnamed  : right data under other entry names, nothing wrong with it
//   fixable   : absent or wrong pieces, every one of which exists elsewhere
//               in the library
//   incorrect : wrong data (bad CRC) with no good copy anywhere : unrepairable
//   extra     : an archive of its own carrying files the DAT does not need,
//               and nothing else Fix can do : once Fix has taken them out,
//               the set is Correct, or Missing when a piece is still absent
//   missing   : pieces absent and nowhere to be found
bool has_extra_files(const RomAudit::GameEntry& g);
const char* status_key_of(const RomAudit::GameEntry& g) {
    if (g.status == "incorrect" && g.corrupt == 0) return "misnamed";
    if (g.repairable) return "fixable";
    if (g.status == "incorrect") return "incorrect";
    if (has_extra_files(g)) return "extra";
    return g.status == "available" ? "available" : "missing";
}

const char* status_label_of(const std::string& key) {
    if (key == "available") return N_("Correct");
    if (key == "misnamed")  return N_("Misnamed");
    if (key == "fixable")   return N_("Fixable");
    if (key == "incorrect") return N_("Incorrect");
    if (key == "extra")     return N_("Extra");
    return N_("Missing");
}

// What Fix can do with a set, if anything. Nothing for a CHD set : its
// "archive" is a folder of disk images, which Fix never moves.
// The archive Fix copies into Import for a repairable set : its own when it
// has one, otherwise the archive holding a good copy of one of its own ROMs,
// which Import recognises by content (a set left under its former name, say).
// A BIOS piece only as a last resort : a BIOS dump sits in hundreds of sets.
std::string import_source_of(const RomAudit::GameEntry& g) {
    if (g.is_chd || !g.repairable || g.ignored) return {};
    if (g.archive_found && !g.archive.empty()) return g.archive;
    std::string fallback;
    for (const auto& r : g.roms) {
        if (r.state != RomAudit::RomState::Absent && r.state != RomAudit::RomState::Corrupt) continue;
        if (r.found_in.empty()) continue;
        if (!r.inherited) return r.found_in;
        if (fallback.empty()) fallback = r.found_in;
    }
    return fallback;
}
// Missing or wrong, and not rebuildable from the library : Fix does that
// without spending the quota. Never a CHD, never an ignored set.
bool downloadable(const RomAudit::Report& rep, const RomAudit::GameEntry& g) {
    return rep.emulator == "fbneo" && !g.is_chd && !g.ignored && g.archive_is_own
        && !g.repairable && !g.dat_header.empty()
        && (g.status == "missing" || g.status == "incorrect");
}
bool can_send_to_import(const RomAudit::GameEntry& g) {
    return !import_source_of(g).empty();
}
// A set with CHDs besides its zip is judged by its zip alone here : a wrong
// or absent CHD must never send a sound zip to quarantine.
bool can_quarantine_whole(const RomAudit::GameEntry& g) {
    const std::string& zip = g.has_disks ? g.zip_status : g.status;
    return !g.is_chd && g.archive_is_own && zip == "incorrect" && !g.repairable && !g.ignored && g.archive_found && !g.archive.empty();
}
bool disks_not_right(const RomAudit::GameEntry& g) {
    for (const auto& r : g.roms) if (r.is_disk && r.state != RomAudit::RomState::Present) return true;
    return false;
}
bool has_extra_files(const RomAudit::GameEntry& g) {
    return !g.is_chd && g.archive_is_own && !g.extra_entries.empty() && g.archive_found && !g.archive.empty();
}

// A ROM's identity in the tables : its CRC, or a CHD's SHA1.
std::string hash_of(const RomAudit::RomEntry& r) { return r.is_disk ? r.sha1 : crc_hex(r.crc); }
std::string found_hash_of(const RomAudit::RomEntry& r) {
    if (r.state == RomAudit::RomState::Absent) return "-";
    return r.is_disk ? r.found_sha1 : crc_hex(r.found_crc);
}

const char* state_label(RomAudit::RomState s) {
    switch (s) {
        case RomAudit::RomState::Present:   return N_("Present");
        case RomAudit::RomState::WrongName: return N_("Wrong name");
        case RomAudit::RomState::Corrupt:   return N_("Corrupt");
        default:                            return N_("Absent");
    }
}

// XML text escaping for the DAT export.
std::string xml(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string csv(const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) { if (c == '"') out += '"'; out += c; }
    return out + "\"";
}

} // namespace

// ═══ Construction ═══════════════════════════════════════════════════════════

RomLibraryTab::RomLibraryTab(std::shared_ptr<DatabaseManager> db, PathsProvider paths)
    : Gtk::Box(Gtk::ORIENTATION_VERTICAL, ui::kCardSpacing), m_db(std::move(db)), m_paths(std::move(paths)) {
    get_style_context()->add_class("set-page");

    build_header();
    build_summary();
    build_table();
    build_detail();
    build_footer();

    m_progress_dispatcher.connect(sigc::mem_fun(*this, &RomLibraryTab::on_progress_update));
    m_finished_dispatcher.connect(sigc::mem_fun(*this, &RomLibraryTab::on_worker_finished));

    update_last_audit_label();
    update_summary();
    update_action_buttons();
    show_all_children();
    m_progress.hide();
    m_btn_cancel->hide();
}

RomLibraryTab::~RomLibraryTab() {
    if (m_worker.joinable()) {
        m_cancelled = true;
        m_worker.join();
    }
}

void RomLibraryTab::build_header() {
    // ── Library scan : the DAT group. The actions (Scan, Audit, Fix) are in
    // the bottom bar, where every tab keeps its actions. ──────────────────
    auto scan = ui::card("bc-search.svg", _("Library scan"),
                         _("Scan your ROM directories and compare them with DAT files."));
    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 10);
    body->set_margin_top(10);

    auto* group_line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 12);
    auto* group_label = ui::title_label(_("DAT group"));
    group_label->set_valign(Gtk::ALIGN_CENTER);
    group_line->pack_start(*group_label, Gtk::PACK_SHRINK);
    // The DAT groups the user organised in the DAT tab : the audit compares
    // the library with the chosen one's files, and nothing else.
    m_dat_group.set_size_request(ui::kFieldWidth, -1);
    m_dat_group.signal_changed().connect([this] {
        if (m_groups_loading) return;
        persist_group_choice();
        update_last_audit_label();
    });
    group_line->pack_start(m_dat_group, Gtk::PACK_SHRINK);
    reload_groups();
    body->pack_start(*group_line, Gtk::PACK_SHRINK);
    scan.body->pack_start(*body, Gtk::PACK_SHRINK);
    scan.frame->set_size_request(360, -1);
    m_top.pack_start(*scan.frame, Gtk::PACK_SHRINK);
}

void RomLibraryTab::build_summary() {
    auto sum = ui::card("bc-chart.svg", _("Scan summary"), _("Comparison result for your library."));
    m_last_audit.set_xalign(1.0f);
    m_last_audit.set_justify(Gtk::JUSTIFY_RIGHT);
    m_last_audit.get_style_context()->add_class("set-sub");
    m_last_audit.set_valign(Gtk::ALIGN_CENTER);
    sum.head->pack_end(m_last_audit, Gtk::PACK_SHRINK);

    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 10);
    body->set_margin_top(10);

    // The counters are the filter: every pill but Total narrows the table.
    m_pill_total     = Gtk::make_managed<ui::Pill>(_("Total"),     ui::PillTone::Neutral);
    m_pill_correct   = Gtk::make_managed<ui::Pill>(_("Correct"),   ui::PillTone::Ok,      true);
    m_pill_missing   = Gtk::make_managed<ui::Pill>(_("Missing"),   ui::PillTone::Error,   true);
    // Incorrect is wrong data (bad CRC, no good copy anywhere) : unrepairable.
    // Misnamed is the right data under another entry name, Fixable a set
    // whose broken pieces exist elsewhere in the library : both repairable.
    // One colour each : missing red, incorrect orange, misnamed blue.
    m_pill_incorrect = Gtk::make_managed<ui::Pill>(_("Incorrect"), ui::PillTone::Warn,    true);
    m_pill_misnamed  = Gtk::make_managed<ui::Pill>(_("Misnamed"),  ui::PillTone::Info,    true);
    m_pill_fixable   = Gtk::make_managed<ui::Pill>(_("Fixable"),   ui::PillTone::Accent,  true);
    m_pill_extra     = Gtk::make_managed<ui::Pill>(_("Extra"),     ui::PillTone::Neutral, true);
    m_pill_orphan    = Gtk::make_managed<ui::Pill>(_("Orphan"),    ui::PillTone::Neutral, true);
    m_pill_ignored   = Gtk::make_managed<ui::Pill>(_("Ignored"),   ui::PillTone::Neutral, true);
    m_pill_incorrect->set_tooltip_text(_("Wrong data (bad CRC) and no good copy anywhere in the library : cannot be repaired, Fix moves them to quarantine."));
    m_pill_misnamed->set_tooltip_text(_("Right data under the wrong entry names : Fix sends them to Import, which rebuilds them."));
    m_pill_fixable->set_tooltip_text(_("Absent or wrong pieces that exist elsewhere in the library : Fix sends them to Import, which rebuilds them."));
    m_pill_extra->set_tooltip_text(_("Files in the archive the DAT does not need : Fix moves them to quarantine. A set still lacking a ROM is Missing afterwards."));
    m_pill_orphan->set_tooltip_text(_("Archives no set of the DAT group claims : Fix moves them to quarantine."));
    for (auto* p : {m_pill_total, m_pill_correct, m_pill_missing, m_pill_incorrect, m_pill_misnamed,
                    m_pill_fixable, m_pill_extra, m_pill_orphan, m_pill_ignored}) {
        p->set_count(0);
        m_pills.add(*p);
    }
    /* Une ligne qui passe a la ligne. Dans une boite, les neuf pastilles
     * reclamaient a elles seules plus que la largeur de la fenetre : la page
     * defilait de cote et coupait le bord droit de la carte. */
    m_pills.set_selection_mode(Gtk::SELECTION_NONE);
    m_pills.set_homogeneous(false);
    m_pills.set_column_spacing(8);
    m_pills.set_row_spacing(8);
    m_pills.set_max_children_per_line(9);
    // Problems first : what the tab is for.
    for (auto* p : {m_pill_missing, m_pill_incorrect, m_pill_misnamed, m_pill_fixable, m_pill_extra, m_pill_orphan}) p->set_active(true);
    for (auto* p : {m_pill_correct, m_pill_missing, m_pill_incorrect, m_pill_misnamed, m_pill_fixable, m_pill_extra, m_pill_orphan, m_pill_ignored})
        p->signal_toggled().connect([this](bool) { refilter(); });
    body->pack_start(m_pills, Gtk::PACK_SHRINK);

    auto* under = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    m_status.set_xalign(0.0f);
    m_status.get_style_context()->add_class("set-sub");
    m_status.set_ellipsize(Pango::ELLIPSIZE_END);
    under->pack_start(m_status, Gtk::PACK_EXPAND_WIDGET);

    m_btn_export = Gtk::make_managed<Gtk::MenuButton>();
    auto* export_face = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    export_face->pack_start(*ui::image("bc-save.svg", ui::kIconButton), Gtk::PACK_SHRINK);
    export_face->pack_start(*Gtk::make_managed<Gtk::Label>(_("Export…")), Gtk::PACK_SHRINK);
    export_face->pack_start(*ui::image("bc-caret-down.svg", 14), Gtk::PACK_SHRINK);
    m_btn_export->add(*export_face);
    m_btn_export->set_valign(Gtk::ALIGN_CENTER);
    struct Fmt { const char* label; int id; };
    for (Fmt f : {Fmt{N_("Text report"), 0}, Fmt{N_("CSV (spreadsheet)"), 1}, Fmt{N_("Missing sets as DAT"), 2}}) {
        auto* item = Gtk::make_managed<Gtk::MenuItem>(_(f.label));
        int id = f.id;
        item->signal_activate().connect([this, id] { on_export(id); });
        m_export_menu.append(*item);
    }
    m_export_menu.show_all();
    m_btn_export->set_popup(m_export_menu);
    under->pack_end(*m_btn_export, Gtk::PACK_SHRINK);
    body->pack_start(*under, Gtk::PACK_SHRINK);

    m_bios_line.set_xalign(0.0f);
    m_bios_line.set_line_wrap(true);
    m_bios_line.get_style_context()->add_class("set-warn");
    m_bios_line.set_no_show_all(true);
    body->pack_start(m_bios_line, Gtk::PACK_SHRINK);

    sum.body->pack_start(*body, Gtk::PACK_SHRINK);
    m_top.pack_start(*sum.frame, Gtk::PACK_EXPAND_WIDGET);
    pack_start(m_top, Gtk::PACK_SHRINK);
}

void RomLibraryTab::build_table() {
    m_filter = Gtk::make_managed<ui::FilterBar>(_("Search (game, rom, filename, parent, crc…)"));
    m_system_combo = m_filter->add_combo(_("System:"));
    m_system_combo->append(_("All"));
    m_system_combo->set_active(0);
    m_filter->signal_changed().connect(sigc::mem_fun(*this, &RomLibraryTab::refilter));
    pack_start(*m_filter, Gtk::PACK_SHRINK);

    m_store = Gtk::ListStore::create(m_cols);
    m_models.sort_column = m_cols.game.index();
    m_models.sort_order  = Gtk::SORT_ASCENDING;

    m_table = Gtk::make_managed<ui::Table>(Gtk::SELECTION_MULTIPLE);
    m_models.attach(m_table->view(), m_store, sigc::mem_fun(*this, &RomLibraryTab::row_visible));
    m_table->add_check_column(m_cols.include, sigc::mem_fun(*this, &RomLibraryTab::on_row_toggled),
                              [this](bool on) { set_all_checked(on); });
    {
        // Status is painted with the application's state colours, read from
        // the sheet when the tab is on screen (see ensure_colours).
        auto* renderer = Gtk::make_managed<Gtk::CellRendererText>();
        auto* col = Gtk::make_managed<Gtk::TreeViewColumn>(_("Status"), *renderer);
        col->add_attribute(renderer->property_text(), m_cols.status);
        col->set_sort_column(m_cols.status_key);
        col->set_cell_data_func(*renderer, [this, renderer](Gtk::CellRenderer*, const Gtk::TreeModel::iterator& it) {
            ensure_colours();
            if (!m_colours.ready) return;
            const Glib::ustring key = (*it)[m_cols.status_key];
            const bool ignored = (*it)[m_cols.ignored];
            Gdk::RGBA c = m_colours.muted;
            if (ignored)                      c = m_colours.muted;
            else if (key == "available")      c = m_colours.ok;
            else if (key == "missing")        c = m_colours.err;
            else if (key == "incorrect")      c = m_colours.warn;
            else if (key == "misnamed")       c = m_colours.info;
            else if (key == "fixable")        c = m_colours.accent;
            else if (key == "extra")          c = m_colours.muted;
            else if (key == "orphan")         c = m_colours.muted;
            renderer->property_foreground_rgba() = c;
            renderer->property_weight() = ignored ? Pango::WEIGHT_NORMAL : Pango::WEIGHT_BOLD;
        });
        m_table->view().append_column(*col);
    }
    { ui::ColumnOptions o; o.expand = true; o.min_width = 220; m_table->add_text_column(_("Game / ROM"), m_cols.game, o); }
    m_table->add_text_column(_("System"), m_cols.system);
    m_table->add_text_column(_("Parent"), m_cols.parent);
    { ui::ColumnOptions o; o.mono = true; m_table->add_text_column(_("Expected file"), m_cols.expected, o); }
    { ui::ColumnOptions o; o.mono = true; o.expand = true; m_table->add_text_column(_("Your file"), m_cols.yours, o); }
    { ui::ColumnOptions o; o.expand = true; o.sortable = false; m_table->add_text_column(_("Details"), m_cols.details, o); }
    m_table->view().get_selection()->signal_changed().connect(sigc::mem_fun(*this, &RomLibraryTab::on_selection_changed));
    m_table->signal_context_menu().connect(sigc::mem_fun(*this, &RomLibraryTab::on_context_menu));
    m_table->set_size_request(-1, 140);   // never less than a few rows
}

void RomLibraryTab::build_detail() {
    m_detail = Gtk::make_managed<ui::DetailPanel>("bc-file.svg", _("Selected set"),
                                                  _("Every ROM of the selected set, and where it was found."), 110);
    m_detail->show_placeholder(_("Select a set to see its ROMs."));
    // Table above, detail below, a grip between the two to trade height.
    pack_start(*ui::splitter(*m_table, *m_detail, 230), Gtk::PACK_EXPAND_WIDGET);
}

void RomLibraryTab::build_footer() {
    m_progress.set_show_text(true);
    m_progress.set_size_request(220, -1);
    m_progress.set_valign(Gtk::ALIGN_CENTER);
    m_progress.set_no_show_all(true);
    m_btn_cancel = ui::button(_("Cancel"), "bc-close.svg");
    m_btn_cancel->set_no_show_all(true);
    m_btn_cancel->signal_clicked().connect([this] { m_cancelled = true; m_status.set_text(_("Cancelling…")); });
    m_footer.pack_start(m_progress, Gtk::PACK_SHRINK);
    m_footer.pack_start(*m_btn_cancel, Gtk::PACK_SHRINK);

    m_btn_select_all  = ui::button(_("Select all"));
    m_btn_select_none = ui::button(_("Select none"));
    m_btn_select_all->signal_clicked().connect([this] { set_all_checked(true); });
    m_btn_select_none->signal_clicked().connect([this] { set_all_checked(false); });
    m_footer.pack_start(*m_btn_select_all,  Gtk::PACK_SHRINK);
    m_footer.pack_start(*m_btn_select_none, Gtk::PACK_SHRINK);

    // The actions, bottom right, in the order they are used : scan, audit,
    // fix. Audit is the one to press first, so it carries the accent ; Fix
    // takes it over once there is something to fix.
    m_btn_scan = ui::button(_("Scan ROMs"), "bc-sync.svg");
    m_btn_scan->set_tooltip_text(_("Rescan every configured ROM directory for new or changed files."));
    m_btn_scan->signal_clicked().connect([this] { if (!m_busy) m_sig_rescan.emit(current_emulator()); });
    m_btn_audit = ui::button(_("Audit library"), "bc-chart.svg", ui::Tone::Accent);
    m_btn_audit->set_tooltip_text(_("Compare the last scan with the DAT group."));
    m_btn_audit->signal_clicked().connect(sigc::mem_fun(*this, &RomLibraryTab::on_audit_clicked));
    m_btn_fix = ui::button(_("Fix"), "bc-check.svg", ui::Tone::Accent);
    m_btn_fix->set_tooltip_text(_("Deal with every problem the audit found : misnamed and fixable sets are sent to Import "
                                  "to be rebuilt, unrepairable sets, orphans and extra files are moved to quarantine. "
                                  "Acts on the ticked rows shown : everything it can act on is ticked after an audit."));
    m_btn_fix->signal_clicked().connect([this] { on_fix_clicked(); });
    m_btn_download = ui::button(_("Download from Bootcade"), "bc-download.svg");
    m_btn_download->set_tooltip_text(_("Download the missing or wrong FinalBurn Neo sets from the Bootcade server, with "
                                       "your account and its daily quota. They go to Import, which checks and files them. "
                                       "Acts on the ticked rows shown : everything it can act on is ticked after an audit."));
    m_btn_download->set_no_show_all(true);
    m_btn_download->signal_clicked().connect([this] { on_download_clicked(); });
    m_footer.pack_end(*m_btn_download, Gtk::PACK_SHRINK);
    m_footer.pack_end(*m_btn_fix, Gtk::PACK_SHRINK);
    m_footer.pack_end(*m_btn_audit, Gtk::PACK_SHRINK);
    m_footer.pack_end(*m_btn_scan, Gtk::PACK_SHRINK);
    pack_start(m_footer, Gtk::PACK_SHRINK);
}

void RomLibraryTab::ensure_colours() {
    if (m_colours.ready || !get_toplevel() || !get_toplevel()->get_realized()) return;
    m_colours.ok     = ui::probe_color(*this, "set-ok");
    m_colours.warn   = ui::probe_color(*this, "set-warn");
    m_colours.err    = ui::probe_color(*this, "set-err");
    m_colours.info   = ui::probe_color(*this, "set-info");
    m_colours.muted  = ui::probe_color(*this, "set-sub");
    m_colours.accent = ui::probe_color(*this, "set-accent-text");
    m_colours.ready  = true;
}

// ═══ Audit ══════════════════════════════════════════════════════════════════

// Remembered in config.json : the scan's split pass and the audit read the
// chosen group's style from there.
void RomLibraryTab::persist_group_choice() {
    nlohmann::json j;
    const std::string path = AppContext::get_config_path();
    { std::ifstream fi(path); if (fi) { try { fi >> j; } catch (...) { j = nlohmann::json{}; } } }
    j["rom_manager"]["library_group"] = m_dat_group.get_active_id().raw();
    std::ofstream fo(path);
    if (fo) fo << j.dump(4);
}

const DatSource::Group* RomLibraryTab::current_group() const {
    std::string id = m_dat_group.get_active_id().raw();
    for (const auto& g : m_groups) if (g.id == id) return &g;
    return m_groups.empty() ? nullptr : &m_groups.front();
}

// The group's emulator decides whose ROM directories are scanned and
// audited, and whose sets they are compared with.
std::string RomLibraryTab::current_emulator() const {
    const auto* g = current_group();
    return g ? g->emulator : std::string("fbneo");
}

void RomLibraryTab::reload_groups() {
    m_groups_loading = true;
    std::string chosen = m_dat_group.get_active_id().raw();
    if (chosen.empty()) {
        nlohmann::json j;
        std::ifstream fi(AppContext::get_config_path());
        if (fi) { try { fi >> j; } catch (...) {} }
        if (j.contains("rom_manager") && j["rom_manager"].is_object() && j["rom_manager"].contains("library_group")
            && j["rom_manager"]["library_group"].is_string())
            chosen = j["rom_manager"]["library_group"].get<std::string>();
    }
    m_groups.clear();
    for (auto& g : DatSource::load_groups()) if (g.active) m_groups.push_back(std::move(g));
    m_dat_group.remove_all();
    for (const auto& g : m_groups) {
        size_t n = DatSource::selected_in_folder(g).size();
        m_dat_group.append(g.id, Glib::ustring::compose(n == 1 ? _("%1 (%2 DAT file)") : _("%1 (%2 DAT files)"), g.name, (int)n));
    }
    if (!m_dat_group.set_active_id(chosen) && !m_groups.empty()) m_dat_group.set_active(0);
    m_groups_loading = false;
    // The remembered group may have gone inactive or been deleted : what the
    // combo shows is what the scan and the audit must use.
    if (m_dat_group.get_active_id().raw() != chosen) persist_group_choice();
    update_last_audit_label();
}

void RomLibraryTab::on_audit_clicked() {
    if (m_busy) return;
    m_job_paths = m_paths();
    m_job_dat_sources.clear();
    if (const auto* g = current_group()) m_job_dat_sources = DatSource::selected_in_folder(*g);
    m_job_emulator = current_emulator();
    // The combo, not config.json : the group shown is the one audited.
    m_job_paths.roms_paths = DatSource::roms_paths_for(m_job_emulator);
    m_cancelled = false;
    m_job = Job::Audit;
    set_busy(true);
    m_status.set_text(_("Auditing…"));
    m_worker = std::thread(&RomLibraryTab::worker_audit, this);
}

void RomLibraryTab::worker_audit() {
    RomInbox::Callbacks cb = make_callbacks();
    // Everything, not only problems : the table filters, and "Correct" is a
    // pill like the others.
    RomAudit::Report rep = RomAudit::audit(m_db, m_job_paths.roms_paths, /*problems_only=*/false, cb,
                                           m_job_dat_sources, m_job_emulator);

    // Which of the sets it would offer the Bootcade server actually holds :
    // one HEAD each, free of quota. Only for a short list : a library that
    // lacks thousands of sets is asked at download time, set by set.
    std::map<std::string, bool> not_on_server;
    if (rep.emulator == "fbneo" && BootcadeAuth::signed_in() && !rep.cancelled) {
        std::vector<const RomAudit::GameEntry*> asked;
        for (const auto& g : rep.games) if (downloadable(rep, g)) asked.push_back(&g);
        if (!asked.empty() && asked.size() <= 200) {
            RomDownload::ServerView server;
            for (size_t i = 0; i < asked.size() && !m_cancelled; ++i) {
                if (cb.progress) cb.progress(100.0 * (double)i / (double)asked.size(), _("Asking the Bootcade server…"));
                const auto offer = server.check(asked[i]->dat_header, asked[i]->name);
                if (offer == RomDownload::Offer::Absent || offer == RomDownload::Offer::Outdated)
                    not_on_server[asked[i]->dat_header + "/" + asked[i]->name] = offer == RomDownload::Offer::Outdated;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lk(m_shared_mutex);
        m_audit = std::move(rep);
        m_not_on_server = std::move(not_on_server);
    }
    m_finished_dispatcher();
}

void RomLibraryTab::refresh_after_scan() {
    if (m_audit_ever_run && !m_busy) on_audit_clicked();
}

void RomLibraryTab::populate() {
    // Phase timings with BOOTCADE_WATCHDOG=1 : this runs on the GTK thread.
    static const bool perf_log = [] { const char* v = std::getenv("BOOTCADE_WATCHDOG"); return v && *v && std::string(v) != "0"; }();
    using clk = std::chrono::steady_clock;
    const auto t0 = clk::now();
    auto ms = [](clk::time_point a, clk::time_point b) { return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count(); };

    // Nothing attached while filling : see SettingsUi::ModelStack.
    m_models.detach(m_table->view());
    m_store->clear();
    const auto t_cleared = clk::now();

    std::set<std::string> systems;
    for (size_t i = 0; i < m_audit.games.size(); ++i) {
        const auto& g = m_audit.games[i];
        auto row = *(m_store->append());
        systems.insert(g.system);

        // A CHD set is a folder named after the set, not a zip.
        std::string expected = g.is_chd ? g.name + "/" : g.has_disks ? g.name + ".zip + " + g.name + "/" : g.name + ".zip";
        std::string yours = g.archive_found ? fs::path(g.archive).filename().string() + (g.is_chd ? "/" : "") : "-";

        const bool can_quarantine = can_quarantine_whole(g);
        const bool has_extras = has_extra_files(g);

        std::vector<std::string> bits;
        if (g.absent)  bits.push_back(Glib::ustring::compose(_("%1 absent"),   g.absent).raw());
        if (g.corrupt) bits.push_back(Glib::ustring::compose(_("%1 corrupt"),  g.corrupt).raw());
        if (g.wrong)   bits.push_back(Glib::ustring::compose(_("%1 misnamed"), g.wrong).raw());
        if (has_extras) bits.push_back(Glib::ustring::compose(_("%1 extra file(s) not needed by the DAT"), (int)g.extra_entries.size()).raw());
        if (g.is_chd && g.status != "available") bits.push_back(_("Fix not available for CHDs"));
        else if (g.misnamed_archive) bits.push_back(_("right content under another file name"));
        else if (g.repairable) bits.push_back(_("repairable from the library"));
        else if (!g.archive_found && (g.has_disks ? g.zip_status : g.status) != "available") bits.push_back(_("no archive found"));
        if (auto s = m_not_on_server.find(g.dat_header + "/" + g.name); s != m_not_on_server.end())
            bits.push_back(s->second ? _("outdated on the Bootcade server") : _("not available on the Bootcade server"));
        int inherited = 0;
        for (const auto& r : g.roms) if (!r.inherited_from.empty()) ++inherited;
        if (inherited) bits.push_back(Glib::ustring::compose(_("%1 from parent/BIOS"), inherited).raw());
        if (g.has_disks && disks_not_right(g)) bits.push_back(_("Fix not available for CHDs"));
        if (g.ignored) bits.insert(bits.begin(), _("ignored"));
        if (g.is_bios) bits.insert(bits.begin(), _("BIOS"));
        // En TETE : la colonne est tronquee, et c'est la seule information
        // qui dit au joueur quoi faire pour que le jeu demarre.
        if (!g.missing_dependency.empty() && g.status != "missing")
            bits.insert(bits.begin(), Glib::ustring::compose(_("\u26A0 cannot run : %1 is missing"),
                                                            g.missing_dependency).raw());

        const std::string key = status_key_of(g);
        row[m_cols.status]     = g.ignored ? Glib::ustring(_("Ignored")) : Glib::ustring(_(status_label_of(key)));
        row[m_cols.status_key] = key;
        row[m_cols.game]       = g.description.empty() ? g.name : g.name + "  (" + g.description + ")";
        row[m_cols.system]     = g.system;
        row[m_cols.parent]     = g.cloneof;
        row[m_cols.expected]   = expected;
        row[m_cols.yours]      = yours;
        row[m_cols.details]    = join(bits, ", ", 4);
        row[m_cols.kind]       = KIND_SET;
        row[m_cols.index]      = (unsigned int)i;
        row[m_cols.repairable] = g.repairable;
        row[m_cols.ignored]    = g.ignored;
        row[m_cols.has_extras] = has_extras;
        row[m_cols.actionable] = can_quarantine || has_extras || can_send_to_import(g);
        row[m_cols.checkable]  = row[m_cols.actionable] || can_download(g);
        row[m_cols.include]    = ticked_by_default(row);

        std::string blob = lower(g.name + ' ' + g.description + ' ' + expected + ' ' + yours + ' ' + g.cloneof + ' ' + g.system);
        for (const auto& r : g.roms) {
            blob += ' ' + lower(r.name) + ' ' + hash_of(r);
            if (!r.found_as.empty()) blob += ' ' + lower(r.found_as);
        }
        row[m_cols.search_blob] = blob;
    }

    for (size_t i = 0; i < m_audit.orphans.size(); ++i) {
        const auto& o = m_audit.orphans[i];
        std::string base = fs::path(o.path).filename().string();
        std::string folder = fs::path(o.path).parent_path().filename().string();
        int elsewhere = 0;
        for (const auto& e : o.entries) if (e.copy_elsewhere) ++elsewhere;
        auto row = *(m_store->append());
        row[m_cols.status]     = _("Orphan");
        row[m_cols.status_key] = "orphan";
        row[m_cols.game]       = base;
        row[m_cols.system]     = folder;
        row[m_cols.parent]     = "";
        row[m_cols.expected]   = "";
        row[m_cols.yours]      = base;
        Glib::ustring details = Glib::ustring::compose(
            _("Not in DAT : %1 file(s), %2 with a copy elsewhere in the library"), (int)o.entries.size(), elsewhere);
        if (m_audit.emulator != "fbneo") details += Glib::ustring(" · ") + _("Left unticked : tick it to quarantine it");
        row[m_cols.details]    = details;
        row[m_cols.kind]       = KIND_ORPHAN;
        row[m_cols.index]      = (unsigned int)i;
        row[m_cols.repairable] = false;
        row[m_cols.ignored]    = false;
        row[m_cols.has_extras] = false;
        row[m_cols.actionable] = true;
        row[m_cols.checkable]  = true;
        row[m_cols.include]    = ticked_by_default(row);
        std::string blob = lower(base + ' ' + folder);
        for (const auto& e : o.entries) blob += ' ' + lower(e.name) + ' ' + crc_hex(e.crc);
        row[m_cols.search_blob] = blob;
    }

    // Systems seen this time, current choice kept when still there.
    m_filter->set_combo_items(m_system_combo, _("All"), systems, m_system_combo->get_active_text());

    const auto t1 = clk::now();
    refilter();
    const auto t2 = clk::now();
    update_summary();
    update_action_buttons();
    m_detail->show_placeholder(_("Select a set to see its ROMs."));
    if (perf_log)
        std::cout << "[PERF] library populate: rows=" << m_store->children().size() << " clear=" << ms(t0, t_cleared) << "ms fill=" << ms(t_cleared, t1)
                  << "ms attach=" << ms(t1, t2) << "ms rest=" << ms(t2, clk::now()) << "ms" << std::endl;
}

void RomLibraryTab::update_summary() {
    // Counted by the table's own vocabulary (see status_key_of) : the
    // report's incorrect/missing/repairable overlap, these pills do not.
    int correct = 0, missing = 0, incorrect = 0, misnamed = 0, fixable = 0, extra = 0;
    for (const auto& g : m_audit.games) {
        if (g.ignored) continue;
        const std::string key = status_key_of(g);
        if (key == "available")      ++correct;
        else if (key == "missing")   ++missing;
        else if (key == "incorrect") ++incorrect;
        else if (key == "misnamed")  ++misnamed;
        else if (key == "extra")     ++extra;
        else                         ++fixable;
    }
    m_pill_total->set_count(m_audit.total);
    m_pill_correct->set_count(correct);
    m_pill_missing->set_count(missing);
    m_pill_incorrect->set_count(incorrect);
    m_pill_misnamed->set_count(misnamed);
    m_pill_fixable->set_count(fixable);
    m_pill_extra->set_count(extra);
    m_pill_orphan->set_count((long)m_audit.orphans.size());
    m_pill_ignored->set_count(m_audit.ignored);

    if (!m_audit_ever_run) {
        m_status.set_text(_("Run an audit to compare your library with the DAT group."));
    } else if (m_audit.pool_empty) {
        m_status.set_text(_("The scan cache is empty : run a ROM scan first."));
    } else {
        m_status.set_text(Glib::ustring::compose(
            _("%1 set(s) with a problem, of which %2 can be repaired from the library itself (%3 misnamed). Each DAT is read with its own merge mode."),
            m_audit.incorrect + m_audit.missing, misnamed + fixable, misnamed));
    }

    if (m_audit.missing_bios.empty()) {
        m_bios_line.hide();
    } else {
        std::vector<std::string> parts;
        for (const auto& b : m_audit.missing_bios)
            parts.push_back(Glib::ustring::compose(_("%1 (%2) : %3 dependent set(s)"), b.name, b.system, b.dependents).raw());
        // Plus de paragraphe : chaque ligne concernee le dit elle-meme
        // (colonne Details, « cannot run : BIOS msx.zip is missing »).
        (void)parts;
        m_bios_line.hide();
    }
}

void RomLibraryTab::update_last_audit_label() {
    int64_t t = m_db->getScanMetadata("last_audit_time", 0);
    const auto* g = current_group();
    int n = g ? (int)DatSource::selected_in_folder(*g).size() : 0;
    const Glib::ustring files = Glib::ustring::compose(n == 1 ? _("%1 DAT file") : _("%1 DAT files"), n);
    if (t <= 0) {
        m_last_audit.set_text(g ? Glib::ustring::compose(_("No audit yet\nGroup: %1, %2"), g->name, files)
                                : Glib::ustring(_("No audit yet")));
    } else {
        m_last_audit.set_text(Glib::ustring::compose(_("Last audit: %1\nCompared with %2 (%3)"),
                                                     format_time(t), files, g ? g->name : ""));
    }
}

bool RomLibraryTab::row_visible(const Gtk::TreeModel::const_iterator& it) const {
    const Gtk::TreeModel::Row row = *it;
    const Glib::ustring key = row[m_cols.status_key];
    const bool ignored = row[m_cols.ignored];

    bool wanted = false;
    if (ignored)                 wanted = m_pill_ignored->active();
    else if (key == "orphan")    wanted = m_pill_orphan->active();
    else if (key == "available") wanted = m_pill_correct->active();
    else if (key == "missing")   wanted = m_pill_missing->active();
    else if (key == "incorrect") wanted = m_pill_incorrect->active();
    else if (key == "misnamed")  wanted = m_pill_misnamed->active();
    else if (key == "fixable")   wanted = m_pill_fixable->active();
    else if (key == "extra")     wanted = m_pill_extra->active();
    if (!wanted) return false;

    if (!m_vis_system.empty() && row[m_cols.system] != m_vis_system) return false;
    if (!m_vis_needle.empty()) {
        const Glib::ustring blob = row[m_cols.search_blob];
        if (blob.raw().find(m_vis_needle) == std::string::npos) return false;
    }
    return true;
}

void RomLibraryTab::refilter() {
    // Rebuilt, not refiltered : see SettingsUi::ModelStack. The filter's
    // inputs are read once here, not once per row inside row_visible.
    m_vis_system = m_system_combo->get_active_row_number() > 0 ? m_system_combo->get_active_text() : Glib::ustring();
    m_vis_needle = lower(m_filter->search_text());
    m_models.detach(m_table->view());
    m_models.attach(m_table->view(), m_store, sigc::mem_fun(*this, &RomLibraryTab::row_visible));
    m_filter->set_summary(Glib::ustring::compose(_("%1 result(s) (%2 sets audited)"), m_models.visible_count(), m_audit.total));
    // "Fix all" counts what is shown.
    if (m_btn_fix) update_action_buttons();
}

Gtk::TreeModel::Row RomLibraryTab::source_row(const Gtk::TreeModel::Path& sorted_path) const {
    auto child = m_models.filter->convert_path_to_child_path(m_models.sort->convert_path_to_child_path(sorted_path));
    return *m_store->get_iter(child);
}

// ═══ Detail panel ═══════════════════════════════════════════════════════════

void RomLibraryTab::on_selection_changed() {
    auto rows = m_table->view().get_selection()->get_selected_rows();
    if (rows.empty()) { m_detail->show_placeholder(_("Select a set to see its ROMs.")); return; }
    Gtk::TreeModel::Row row = source_row(rows.front());
    if ((int)row[m_cols.kind] == KIND_ORPHAN) show_orphan_detail(row);
    else                                       show_set_detail(row);
}

namespace {
struct RomCols : public Gtk::TreeModel::ColumnRecord {
    Gtk::TreeModelColumn<Glib::ustring> state, expected, found_as, crc_expected, crc_found, size, found_in;
    RomCols() { add(state); add(expected); add(found_as); add(crc_expected); add(crc_found); add(size); add(found_in); }
};
}

void RomLibraryTab::show_set_detail(const Gtk::TreeModel::Row& row) {
    const auto& g = m_audit.games[(unsigned int)row[m_cols.index]];
    m_detail->set_title(g.description.empty() ? g.name : g.name + "  —  " + g.description);
    std::string sub = g.system;
    if (!g.cloneof.empty()) sub += Glib::ustring::compose(_("  ·  clone of %1"), g.cloneof).raw();
    if (g.archive_found) sub += "  ·  " + g.archive;
    m_detail->set_subtitle(sub);

    static RomCols cols;
    auto store = Gtk::ListStore::create(cols);
    for (const auto& r : g.roms) {
        auto rr = *(store->append());
        rr[cols.state]        = _(state_label(r.state));
        rr[cols.expected]     = r.name;
        rr[cols.found_as]     = r.found_as;
        rr[cols.crc_expected] = hash_of(r);
        rr[cols.crc_found]    = found_hash_of(r);
        rr[cols.size]         = r.is_disk ? std::string() : human_size(r.size);
        std::string where;
        if (r.is_disk)
            where = r.found_in.empty() ? std::string() : fs::path(r.found_in).parent_path().filename().string() + "/";
        else if (!r.inherited_from.empty())
            where = Glib::ustring::compose(_("from %1 (%2)"), r.inherited_from, fs::path(r.found_in).filename().string()).raw();
        else if (!r.found_in.empty())
            where = Glib::ustring::compose(_("good copy in %1"), fs::path(r.found_in).filename().string()).raw();
        else if (r.inherited && r.state == RomAudit::RomState::Absent)
            where = _("expected from parent/BIOS");
        rr[cols.found_in] = where;
    }
    for (const auto& x : g.extra_entries) {
        auto rr = *(store->append());
        rr[cols.state]    = _("Extra");
        rr[cols.expected] = "";
        rr[cols.found_as] = x;
        rr[cols.found_in] = _("not needed by the DAT");
    }

    auto* t = Gtk::make_managed<ui::Table>(Gtk::SELECTION_SINGLE);
    t->view().set_model(store);
    {
        auto* renderer = Gtk::make_managed<Gtk::CellRendererText>();
        auto* col = Gtk::make_managed<Gtk::TreeViewColumn>(_("State"), *renderer);
        col->add_attribute(renderer->property_text(), cols.state);
        col->set_cell_data_func(*renderer, [this, renderer](Gtk::CellRenderer*, const Gtk::TreeModel::iterator& it) {
            ensure_colours();
            if (!m_colours.ready) return;
            static RomCols c;
            const Glib::ustring s = (*it)[c.state];
            Gdk::RGBA colour = m_colours.muted;
            if (s == _("Present"))         colour = m_colours.ok;
            else if (s == _("Absent"))     colour = m_colours.err;
            else if (s == _("Extra"))      colour = m_colours.muted;
            else                           colour = m_colours.warn;
            renderer->property_foreground_rgba() = colour;
        });
        t->view().append_column(*col);
    }
    { ui::ColumnOptions o; o.mono = true; o.expand = true; t->add_text_column(_("Expected name"), cols.expected, o); }
    { ui::ColumnOptions o; o.mono = true; o.expand = true; t->add_text_column(_("Found as"), cols.found_as, o); }
    { ui::ColumnOptions o; o.mono = true;
      t->add_text_column(g.is_chd ? _("SHA1 expected") : g.has_disks ? _("CRC / SHA1 expected") : _("CRC expected"), cols.crc_expected, o);
      t->add_text_column(g.is_chd ? _("SHA1 found") : g.has_disks ? _("CRC / SHA1 found") : _("CRC found"), cols.crc_found, o); }
    { ui::ColumnOptions o; o.xalign = 1.0f; t->add_text_column(_("Size"), cols.size, o); }
    { ui::ColumnOptions o; o.expand = true; t->add_text_column(_("Found in"), cols.found_in, o); }
    m_detail->set_content(t);
}

void RomLibraryTab::show_orphan_detail(const Gtk::TreeModel::Row& row) {
    const auto& o = m_audit.orphans[(unsigned int)row[m_cols.index]];
    m_detail->set_title(fs::path(o.path).filename().string());
    m_detail->set_subtitle(Glib::ustring::compose(_("Not claimed by any set of the DAT group  ·  %1"), o.path));

    static RomCols cols;
    auto store = Gtk::ListStore::create(cols);
    for (const auto& e : o.entries) {
        auto rr = *(store->append());
        rr[cols.state]        = e.copy_elsewhere ? _("Copy elsewhere") : _("Unique");
        rr[cols.expected]     = "";
        rr[cols.found_as]     = e.name;
        rr[cols.crc_expected] = "";
        rr[cols.crc_found]    = crc_hex(e.crc);
        rr[cols.size]         = "";
        rr[cols.found_in]     = e.copy_elsewhere ? _("the same data exists in another archive") : _("no other copy in the library");
    }
    auto* t = Gtk::make_managed<ui::Table>(Gtk::SELECTION_SINGLE);
    t->view().set_model(store);
    t->add_text_column(_("State"), cols.state);
    { ui::ColumnOptions o; o.mono = true; o.expand = true; t->add_text_column(_("Entry"), cols.found_as, o); }
    { ui::ColumnOptions o; o.mono = true; t->add_text_column(_("CRC"), cols.crc_found, o); }
    { ui::ColumnOptions o; o.expand = true; t->add_text_column(_("Note"), cols.found_in, o); }
    m_detail->set_content(t);
}

// ═══ Checkboxes and actions ═════════════════════════════════════════════════

void RomLibraryTab::on_row_toggled(const Glib::ustring& path) {
    Gtk::TreeModel::Row row = source_row(Gtk::TreeModel::Path(path));
    if (!row[m_cols.checkable]) return;
    row[m_cols.include] = !row[m_cols.include];
    update_action_buttons();
}

void RomLibraryTab::set_all_checked(bool on) {
    /* Les lignes affichees, puis la vue detachee le temps de les modifier.
     * Attachee, chaque case changee relancait filtre et tri : sur 29 000
     * lignes, tout cocher figeait la fenetre une dizaine de secondes. */
    std::vector<Gtk::TreeModel::iterator> shown;
    for (const auto& frow : m_models.filter->children())
        shown.push_back(m_models.filter->convert_iter_to_child_iter(frow));
    m_models.detach(m_table->view());
    for (const auto& it : shown) {
        Gtk::TreeModel::Row row = *it;
        if (row[m_cols.checkable]) row[m_cols.include] = on;
    }
    m_models.attach(m_table->view(), m_store, sigc::mem_fun(*this, &RomLibraryTab::row_visible));
    update_action_buttons();
}

// The ticked rows the table shows : what one sees ticked is what gets acted on.
std::vector<Gtk::TreeModel::Row> RomLibraryTab::checked_rows() const {
    std::vector<Gtk::TreeModel::Row> out;
    if (!m_models.filter) return out;
    for (const auto& frow : m_models.filter->children()) {
        Gtk::TreeModel::Row row = *m_models.filter->convert_iter_to_child_iter(frow);
        if (row[m_cols.include]) out.push_back(row);
    }
    return out;
}

// Ticked once the audit is in : everything a button can act on, so the
// counts on the buttons are rows one can see and untick.
bool RomLibraryTab::ticked_by_default(const Gtk::TreeModel::Row& row) const {
    if (!row[m_cols.checkable] || row[m_cols.ignored]) return false;
    // A MAME library keeps zips no set of the DAT claims on purpose : the
    // ~350 devices that have no ROM of their own, which RomVault's MAME XML
    // still gives a zip. For MAME an orphan is fixed only when ticked by hand.
    return !((int)row[m_cols.kind] == KIND_ORPHAN && m_audit.emulator != "fbneo");
}

// The rows Fix acts on : a ticked set that can only be downloaded is
// Download's, not Fix's.
std::vector<Gtk::TreeModel::Row> RomLibraryTab::fix_candidates() const {
    std::vector<Gtk::TreeModel::Row> rows;
    for (const auto& row : checked_rows()) if (row[m_cols.actionable]) rows.push_back(row);
    return rows;
}

void RomLibraryTab::update_action_buttons() {
    if (m_table) m_table->refresh_header_check();
    const int n = (int)fix_candidates().size();
    m_btn_fix->set_label(n ? Glib::ustring::compose(_("Fix selected (%1)"), n)
                           : Glib::ustring(_("Fix")));
    m_btn_fix->set_sensitive(!m_busy && n > 0);

    // FinalBurn Neo only : MAME has no online feature yet.
    const int d = (int)download_candidates().size();
    const bool fbneo = m_audit_ever_run && m_audit.emulator == "fbneo";
    m_btn_download->set_visible(fbneo);
    m_btn_download->set_label(d ? Glib::ustring::compose(_("Download from Bootcade (%1)"), d)
                                : Glib::ustring(_("Download from Bootcade")));
    m_btn_download->set_sensitive(!m_busy && d > 0);
}

bool RomLibraryTab::can_download(const RomAudit::GameEntry& g) const {
    return downloadable(m_audit, g) && !m_not_on_server.count(g.dat_header + "/" + g.name);
}

std::vector<Gtk::TreeModel::Row> RomLibraryTab::download_candidates() const {
    std::vector<Gtk::TreeModel::Row> out;
    auto keep = [&](const Gtk::TreeModel::Row& row) {
        if ((int)row[m_cols.kind] == KIND_SET && can_download(m_audit.games[(unsigned int)row[m_cols.index]]))
            out.push_back(row);
    };
    for (const auto& row : checked_rows()) keep(row);
    return out;
}

void RomLibraryTab::on_download_clicked(std::vector<Gtk::TreeModel::Row> rows) {
    if (m_busy) return;
    if (rows.empty()) rows = download_candidates();
    m_dl = DownloadJob{};
    for (const auto& row : rows) {
        const auto& g = m_audit.games[(unsigned int)row[m_cols.index]];
        if (can_download(g)) m_dl.items.push_back({g.dat_header, g.name});
    }
    if (m_dl.items.empty()) { flash(_("Nothing to download : no missing or wrong FinalBurn Neo set is shown.")); return; }

    auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
    // Signed out : say so at once, rather than ask to confirm what cannot happen.
    if (BootcadeAuth::username().empty()) {
        if (top) ui::notice(*top, _("Download from Bootcade"), RomDownload::explain("not_signed_in"));
        return;
    }
    Paths p = m_paths();
    std::error_code ec;
    if (p.inbox.empty()) { if (top) ui::notice(*top, _("No import folder"), _("Set an import folder (Import tab) first.")); return; }
    fs::create_directories(p.inbox, ec);
    if (ec || !fs::is_directory(p.inbox, ec)) { if (top) ui::notice(*top, _("Import folder"), _("Could not create the import folder.")); return; }
    if (top) {
        const Glib::ustring text = Glib::ustring::compose(
            _("%1 set(s) will be downloaded from the Bootcade server into the import folder, which checks them and "
              "files them into your library.\n\nYour account's daily quota applies : downloads stop when it is used up, "
              "and the rest can be downloaded later."), (int)m_dl.items.size());
        ConfirmationDialog confirm(*top, _("Download from Bootcade?"), text, "bc-download.svg");
        if (!confirm.show_and_confirm()) return;
    }
    m_job_paths = p;
    m_cancelled = false;
    m_job = Job::Download;
    set_busy(true);
    m_status.set_text(_("Checking your account…"));
    m_worker = std::thread(&RomLibraryTab::worker_download, this);
}

void RomLibraryTab::worker_download() {
    // Whatever the way out (refusal, quota, cancel), the page is told once.
    [this] {
    // The quota first : a refusal is explained once, before anything moves.
    RomDownload::Quota q = RomDownload::fetch_quota();
    if (!q.answered) { m_dl.stopped = std::string(_("The Bootcade server did not answer : ")) + q.error; return; }
    if (!q.can_download) { m_dl.stopped = RomDownload::explain(q.reason); return; }
    push_log(Glib::ustring::compose(_("Bootcade account : %1 download(s) left today."), q.remaining).raw());

    int left = q.remaining;
    const int n = (int)m_dl.items.size();
    RomDownload::ServerView server;
    for (int i = 0; i < n && !m_cancelled; ++i) {
        const auto& item = m_dl.items[i];
        if (left <= 0) {
            m_dl.stopped = Glib::ustring::compose(_("Daily quota used up : %1 set(s) left to download later."), n - i).raw();
            break;
        }
        // The version the DAT asks for, or nothing : an older copy would
        // spend the quota on a set Import then turns down.
        const auto offer = server.check(item.dat_header, item.name);
        if (offer == RomDownload::Offer::Absent || offer == RomDownload::Offer::Outdated) {
            const std::string why = offer == RomDownload::Offer::Outdated ? _("outdated on the Bootcade server")
                                                                           : _("not available on the Bootcade server");
            m_dl.failed.push_back(item.name + " : " + why);
            push_log(item.name + " : " + why);
            continue;
        }
        const std::string label = Glib::ustring::compose(_("Downloading %1 (%2/%3)…"), item.name, i + 1, n).raw();
        push_progress((double)i / n, label);
        auto res = RomDownload::download(item.dat_header, item.name, m_job_paths.inbox, m_cancelled,
                                         [&](double f) { push_progress((i + f) / n, label); });
        if (res.ok) {
            m_dl.got.push_back(res.path);
            --left;
            push_log(Glib::ustring::compose(_("Downloaded %1."), item.name).raw());
            continue;
        }
        if (m_cancelled) break;
        m_dl.failed.push_back(item.name + " : " + res.reason);
        push_log(item.name + " : " + res.reason);
        // Une panne du serveur touche toutes les ROMs : continuer userait le
        // quota sur chacune sans rien recevoir.
        if (res.server_fault) {
            m_dl.stopped = _("The Bootcade server could not send the files : downloads were stopped. Please try again later.");
            break;
        }
        // Refused : ask the account why. Quota used up or access lost stops
        // everything ; anything else is this set's problem only.
        if (res.http_status == 401 || res.http_status == 403) {
            RomDownload::Quota again = RomDownload::fetch_quota();
            if (!again.can_download) { m_dl.stopped = RomDownload::explain(again.reason); break; }
            left = again.remaining;
        }
    }
    push_progress(1.0, "");
    }();
    m_finished_dispatcher();
}

void RomLibraryTab::copy_to_clipboard(const Glib::ustring& text, const Glib::ustring& what) {
    if (text.empty()) { flash(Glib::ustring::compose(_("Nothing to copy for %1."), what)); return; }
    Gtk::Clipboard::get()->set_text(text);
    flash(Glib::ustring::compose(_("Copied %1 to the clipboard."), what));
}

std::string RomLibraryTab::all_details_of(const Gtk::TreeModel::Row& row) const {
    std::ostringstream out;
    if ((int)row[m_cols.kind] == KIND_ORPHAN) {
        const auto& o = m_audit.orphans[(unsigned int)row[m_cols.index]];
        out << "Orphan archive: " << o.path << "\n";
        for (const auto& e : o.entries)
            out << "  " << e.name << "\tcrc=" << crc_hex(e.crc) << (e.copy_elsewhere ? "\t(copy elsewhere)" : "") << "\n";
        return out.str();
    }
    const auto& g = m_audit.games[(unsigned int)row[m_cols.index]];
    out << g.name << " (" << g.description << ")\n"
        << "system: " << g.system << "\n"
        << "status: " << g.status << (g.ignored ? " (ignored)" : "") << "\n";
    if (!g.cloneof.empty()) out << "parent: " << g.cloneof << "\n";
    out << "expected: " << g.name << (g.is_chd ? "/" : ".zip") << "\n"
        << "your file: " << (g.archive_found ? g.archive : "-") << "\n";
    for (const auto& r : g.roms) {
        if (r.is_disk) {
            out << "  " << _(state_label(r.state)) << "\t" << r.name << "\tsha1=" << r.sha1;
            if (r.state == RomAudit::RomState::Corrupt) out << "\tfound sha1=" << r.found_sha1;
            if (!r.found_in.empty()) out << "\tfile " << r.found_in;
            out << "\n";
            continue;
        }
        out << "  " << _(state_label(r.state)) << "\t" << r.name << "\tcrc=" << crc_hex(r.crc)
            << "\tsize=" << r.size;
        if (!r.found_as.empty()) out << "\tfound as " << r.found_as;
        if (r.state == RomAudit::RomState::Corrupt) out << "\tfound crc=" << crc_hex(r.found_crc);
        if (!r.inherited_from.empty()) out << "\tfrom " << r.inherited_from;
        else if (!r.found_in.empty()) out << "\tcopy in " << r.found_in;
        out << "\n";
    }
    for (const auto& x : g.extra_entries) out << "  extra\t" << x << "\n";
    return out.str();
}

void RomLibraryTab::search_on_web(const Gtk::TreeModel::Row& row) {
    Glib::ustring q = (int)row[m_cols.kind] == KIND_ORPHAN
        ? Glib::ustring(row[m_cols.yours])
        : Glib::ustring(row[m_cols.expected]) + " " + Glib::ustring(row[m_cols.system]);
    std::string uri = "https://duckduckgo.com/?q=" + Glib::uri_escape_string(q.raw());
    if (!ui::open_uri(uri)) flash(_("Could not open a browser."));
}

void RomLibraryTab::toggle_ignore(const Gtk::TreeModel::Row& row) {
    if ((int)row[m_cols.kind] != KIND_SET) return;
    auto& g = m_audit.games[(unsigned int)row[m_cols.index]];
    bool now_ignored = !g.ignored;
    bool ok = now_ignored ? m_db->ignoreSet(g.name, g.system, "", m_audit.emulator)
                          : m_db->unignoreSet(g.name, g.system, m_audit.emulator);
    if (!ok) { flash(_("Could not update the ignore list.")); return; }
    g.ignored = now_ignored;

    // Counters move with the set : an ignored set is in neither bucket of
    // problems, and never repairable.
    auto bucket = [&](int delta) {
        if (g.status == "available")      m_audit.available += delta;
        else if (g.status == "incorrect") m_audit.incorrect += delta;
        else                              m_audit.missing   += delta;
        if (g.repairable)                 m_audit.repairable += delta;
    };
    if (now_ignored) { bucket(-1); m_audit.ignored++; }
    else             { bucket(+1); m_audit.ignored--; }

    row[m_cols.ignored] = now_ignored;
    row[m_cols.status]  = now_ignored ? Glib::ustring(_("Ignored")) : Glib::ustring(_(status_label_of(status_key_of(g))));
    row[m_cols.actionable] = can_quarantine_whole(g) || has_extra_files(g) || can_send_to_import(g);
    row[m_cols.checkable]  = row[m_cols.actionable] || can_download(g);
    row[m_cols.include]    = ticked_by_default(row);
    Glib::ustring details = row[m_cols.details];
    const Glib::ustring tag = Glib::ustring(_("ignored")) + ", ";
    if (now_ignored) row[m_cols.details] = tag + details;
    else if (details.find(tag) == 0) row[m_cols.details] = details.substr(tag.size());
    update_summary();
    refilter();
    update_action_buttons();
    flash(now_ignored ? Glib::ustring::compose(_("%1 will not be reported as a problem any more."), g.name)
                      : Glib::ustring::compose(_("%1 is reported again."), g.name));
}

void RomLibraryTab::on_context_menu(const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn*, GdkEventButton* event) {
    Gtk::TreeModel::Row row = source_row(path);
    const bool is_set = (int)row[m_cols.kind] == KIND_SET;

    // Rebuilt each time: the entries depend on the row.
    ui::destroy_children(m_context_menu);
    auto add = [&](const Glib::ustring& label, std::function<void()> fn, bool enabled = true) {
        auto* item = Gtk::make_managed<Gtk::MenuItem>(label);
        item->set_sensitive(enabled);
        item->signal_activate().connect([fn] { fn(); });
        m_context_menu.append(*item);
    };
    auto sep = [&] { m_context_menu.append(*Gtk::make_managed<Gtk::SeparatorMenuItem>()); };

    const Glib::ustring name     = is_set ? Glib::ustring(m_audit.games[(unsigned int)row[m_cols.index]].name) : Glib::ustring(row[m_cols.yours]);
    const Glib::ustring expected = row[m_cols.expected];
    const Glib::ustring yours    = row[m_cols.yours];
    const Glib::ustring parent   = row[m_cols.parent];

    add(_("Copy game name"),         [this, name]     { copy_to_clipboard(name, _("the game name")); });
    add(_("Copy expected filename"), [this, expected] { copy_to_clipboard(expected, _("the expected filename")); }, !expected.empty());
    add(_("Copy your filename"),     [this, yours]    { copy_to_clipboard(yours, _("your filename")); }, yours != "-" && !yours.empty());
    add(_("Copy parent name"),       [this, parent]   { copy_to_clipboard(parent, _("the parent name")); }, !parent.empty());
    if (is_set) {
        const auto& g = m_audit.games[(unsigned int)row[m_cols.index]];
        std::vector<std::string> missing;
        for (const auto& r : g.roms) if (r.state == RomAudit::RomState::Absent) missing.push_back(r.name);
        add(_("Copy missing filenames"), [this, missing] { copy_to_clipboard(join(missing, "\n"), _("the missing filenames")); }, !missing.empty());
    }
    add(_("Copy all details"), [this, row] { copy_to_clipboard(all_details_of(row), _("all details")); });
    sep();
    add(_("Search on web"), [this, row] { search_on_web(row); });
    if (is_set) {
        sep();
        const auto& g = m_audit.games[(unsigned int)row[m_cols.index]];
        add(g.ignored ? _("Stop ignoring this set") : _("Ignore this set (do not report again)"),
            [this, row] { toggle_ignore(row); });
        if (can_download(g))
            add(_("Download from Bootcade"), [this, row] { on_download_clicked({row}); });
        if (can_send_to_import(g))
            add(_("Fix : send to Import to be rebuilt"), [this, row] { on_fix_clicked({row}); });
        else if (can_quarantine_whole(g))
            add(_("Fix : move to quarantine"), [this, row] { on_fix_clicked({row}); });
        else if (has_extra_files(g) && !g.ignored)
            add(_("Fix : extract the extra files to quarantine"), [this, row] { on_fix_clicked({row}); });
        else if ((g.is_chd && g.status != "available") || (g.has_disks && disks_not_right(g)))
            add(_("Fix not available for CHDs"), [] {}, false);
    } else {
        sep();
        add(_("Fix : move to quarantine"), [this, row] { on_fix_clicked({row}); });
    }
    m_context_menu.show_all();
    m_context_menu.popup_at_pointer((GdkEvent*)event);
}

void RomLibraryTab::on_fix_clicked(std::vector<Gtk::TreeModel::Row> rows) {
    if (m_busy) return;
    if (rows.empty()) rows = fix_candidates();

    // One button, the right destination per row :
    //  - a repairable set (misnamed, or every broken piece has a good copy
    //    elsewhere) is copied into the import folder, where Import rebuilds
    //    it from the library and the outbox brings it back ;
    //  - a set with wrong data and no good copy anywhere is moved out whole,
    //    to quarantine : nothing here can repair it ;
    //  - an otherwise sound archive carrying entries no DAT rom needs has
    //    just those entries extracted to quarantine, and stays in place ;
    //  - an orphan (an archive no set of the DAT group claims) goes to
    //    quarantine too : Restore to Import from there re-identifies it.
    m_fix = FixJob{};
    for (const auto& row : rows) {
        if ((int)row[m_cols.kind] == KIND_ORPHAN) {
            const auto& o = m_audit.orphans[(unsigned int)row[m_cols.index]];
            m_fix.orphans.push_back({o.path, fs::path(o.path).parent_path().filename().string(), Glib::ustring(row[m_cols.system]).raw()});
            continue;
        }
        const auto& g = m_audit.games[(unsigned int)row[m_cols.index]];
        std::string header = g.dat_header.empty() ? g.system : g.dat_header;
        if (g.misnamed_archive && !g.ignored) {
            m_fix.renamed.push_back(g.archive);
            continue;
        }
        if (can_send_to_import(g)) {
            std::string src = import_source_of(g);
            if (std::find(m_fix.repairable.begin(), m_fix.repairable.end(), src) == m_fix.repairable.end())
                m_fix.repairable.push_back(src);
        }
        else if (can_quarantine_whole(g))   m_fix.whole.push_back({g.archive, header, g.system});
        else if (has_extra_files(g) && !g.ignored) m_fix.extras.push_back({g.archive, g.system, header, g.extra_entries});
    }
    // An orphan that is also the good copy a repairable set is rebuilt from
    // stays put this time : moving it out first would leave nothing to copy.
    // Once the rebuilt set is in the library, the next audit offers it again.
    m_fix.orphans.erase(std::remove_if(m_fix.orphans.begin(), m_fix.orphans.end(), [&](const auto& o) {
        return std::find(m_fix.repairable.begin(), m_fix.repairable.end(), o.archive) != m_fix.repairable.end();
    }), m_fix.orphans.end());
    if (m_fix.whole.empty() && m_fix.orphans.empty() && m_fix.extras.empty() && m_fix.repairable.empty() &&
        m_fix.renamed.empty()) {
        flash(_("Nothing to fix : the audit found no repairable set, unrepairable set, orphan or extra file."));
        return;
    }

    auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
    Paths p = m_paths();
    std::error_code ec;
    const bool needs_quarantine = !m_fix.whole.empty() || !m_fix.extras.empty() || !m_fix.orphans.empty();
    if (needs_quarantine) {
        if (p.quarantine.empty()) { if (top) ui::notice(*top, _("No quarantine folder"), _("Set a quarantine directory in Settings › Library › ROM Management first.")); return; }
        fs::create_directories(p.quarantine, ec);
        if (ec || !fs::is_directory(p.quarantine, ec)) { if (top) ui::notice(*top, _("Quarantine folder"), _("Could not create the quarantine folder.")); return; }
    }
    if (!m_fix.repairable.empty() || !m_fix.renamed.empty()) {
        if (p.inbox.empty()) { if (top) ui::notice(*top, _("No import folder"), _("Set an import folder (Import tab) first.")); return; }
        fs::create_directories(p.inbox, ec);
        if (ec || !fs::is_directory(p.inbox, ec)) { if (top) ui::notice(*top, _("Import folder"), _("Could not create the import folder.")); return; }
    }

    int extra_files = 0;
    for (const auto& x : m_fix.extras) extra_files += (int)x.entries.size();
    Glib::ustring summary;
    if (!m_fix.renamed.empty())
        summary += Glib::ustring::compose(_("%1 set(s) under another file name → moved to Import, which files them under the right name\n"), (int)m_fix.renamed.size());
    if (!m_fix.repairable.empty())
        summary += Glib::ustring::compose(_("%1 repairable set(s) (misnamed, or pieces found elsewhere in the library) → copied to Import, which rebuilds them\n"), (int)m_fix.repairable.size());
    if (!m_fix.whole.empty())
        summary += Glib::ustring::compose(_("%1 unrepairable set(s) (wrong data, no good copy anywhere) → quarantine\n"), (int)m_fix.whole.size());
    if (!m_fix.extras.empty())
        summary += Glib::ustring::compose(_("%1 extra file(s) not needed by the DAT, extracted from %2 otherwise sound archive(s) → quarantine\n"), extra_files, (int)m_fix.extras.size());
    if (!m_fix.orphans.empty())
        summary += Glib::ustring::compose(_("%1 orphan archive(s) matching no set of the DAT group → quarantine\n"), (int)m_fix.orphans.size());
    summary += _("\nNothing is deleted : every file is moved or copied, never destroyed. The library itself is only ever written by Outbox › Move to library.");
    if (top) {
        ConfirmationDialog confirm(*top, _("Fix these items?"), summary, "bc-check.svg");
        if (!confirm.show_and_confirm()) return;
    }

    m_job_paths = p;
    m_cancelled = false;
    m_job = Job::Fix;
    set_busy(true);
    m_status.set_text(_("Fixing…"));
    m_worker = std::thread(&RomLibraryTab::worker_fix, this);
}

void RomLibraryTab::worker_fix() {
    RomInbox::Callbacks cb = make_callbacks();
    std::error_code ec;
    const std::string& quarantine = m_job_paths.quarantine;
    const std::string& inbox = m_job_paths.inbox;
    RomManifest::Manifest manifest = quarantine.empty() ? RomManifest::Manifest() : RomManifest::Manifest::load(quarantine);

    auto move_file = [&](const fs::path& src, const fs::path& dest) -> bool {
        std::error_code mec;
        fs::rename(src, dest, mec);
        if (!mec) return true;
        copy_file_robust(src, dest, false, mec);
        if (mec) return false;
        fs::remove(src, mec);
        return true;
    };

    const size_t total = m_fix.whole.size() + m_fix.extras.size() + m_fix.orphans.size() + m_fix.repairable.size() +
                         m_fix.renamed.size();
    size_t done = 0;
    auto step = [&](const std::string& what) { push_progress(100.0 * (double)(++done) / (double)total, what); };

    for (const auto& w : m_fix.whole) {
        if (m_cancelled) break;
        fs::path src(w.archive);
        fs::path dest_dir = fs::path(quarantine) / w.dat_header;
        fs::create_directories(dest_dir, ec);
        fs::path dest = dest_dir / src.filename();
        step(src.filename().string());
        if (fs::exists(dest, ec)) { m_fix.failed++; push_log("[FIX] quarantine already holds " + dest.string() + " : left alone"); continue; }
        if (!move_file(src, dest)) { m_fix.failed++; push_log("[FIX] FAILED to move " + src.string()); continue; }
        m_fix.moved++;
        RomManifest::Entry e;
        e.file = manifest.relative(dest.string());
        e.game = src.stem().string();
        e.system = w.system;
        e.dat_header = w.dat_header;
        e.reason = RomManifest::reason::BadCrc;
        e.origin = w.archive;
        e.action = RomManifest::action::Moved;
        e.details.push_back("wrong data, and no good copy anywhere else in the library");
        manifest.add(std::move(e));
        push_log("[FIX] unrepairable " + src.filename().string() + " -> quarantine");
    }

    for (const auto& x : m_fix.extras) {
        if (m_cancelled) break;
        step(fs::path(x.archive).filename().string());
        std::vector<std::string> written;
        bool ok = RomCleanup::extract_entries_to_quarantine(x.archive, x.entries, quarantine, cb, &written);
        ok ? m_fix.cleaned++ : m_fix.failed++;
        for (const auto& f : written) {
            RomManifest::Entry e;
            e.file = manifest.relative(f);
            e.game = fs::path(x.archive).stem().string();
            e.system = x.system;
            e.dat_header = x.dat_header;
            e.reason = RomManifest::reason::ExtraFiles;
            e.origin = x.archive;
            e.action = RomManifest::action::Extracted;
            e.details.push_back("entry no DAT rom of this set needs, taken out of an otherwise sound archive");
            manifest.add(std::move(e));
        }
    }

    // An orphan goes to quarantine whole, under the folder it came from, as
    // "unknown" : the same reason Import gives a file it cannot identify, so
    // the Quarantine tab offers it the same way out (Restore to Import).
    for (const auto& o : m_fix.orphans) {
        if (m_cancelled) break;
        fs::path src(o.archive);
        fs::path dest_dir = fs::path(quarantine) / (o.dat_header.empty() ? std::string("orphans") : o.dat_header);
        fs::create_directories(dest_dir, ec);
        fs::path dest = dest_dir / src.filename();
        step(src.filename().string());
        if (fs::exists(dest, ec)) { m_fix.failed++; push_log("[FIX] quarantine already holds " + dest.string() + " : left alone"); continue; }
        if (!move_file(src, dest)) { m_fix.failed++; push_log("[FIX] FAILED to move " + src.string()); continue; }
        m_fix.moved++;
        RomManifest::Entry e;
        e.file = manifest.relative(dest.string());
        e.game = src.stem().string();
        e.system = o.system;
        e.dat_header = o.dat_header;
        e.reason = RomManifest::reason::Unknown;
        e.origin = o.archive;
        e.action = RomManifest::action::Moved;
        e.details.push_back("matches no set of the DAT group, by name or by content");
        manifest.add(std::move(e));
        push_log("[FIX] orphan " + src.filename().string() + " -> quarantine");
    }

    // A set under another file name is MOVED to the import folder : copied,
    // it came back rebuilt under its right name and the old file stayed in
    // the library as an orphan.
    for (const auto& a : m_fix.renamed) {
        if (m_cancelled) break;
        fs::path src(a);
        fs::path dest = fs::path(inbox) / src.filename();
        step(src.filename().string());
        std::error_code cec;
        if (fs::exists(dest, cec)) { m_fix.failed++; push_log("[FIX] import folder already holds " + dest.string() + " : left alone"); continue; }
        if (!move_file(src, dest)) { m_fix.failed++; push_log("[FIX] FAILED to move " + src.string()); continue; }
        m_fix.relocated++;
        m_fix.sent.push_back(dest.string());
        push_log("[FIX] misnamed " + src.filename().string() + " -> import folder");
    }

    // A repairable set is COPIED into the import folder : the library keeps
    // its copy until Outbox › Move to library replaces it with the rebuilt one.
    for (const auto& a : m_fix.repairable) {
        if (m_cancelled) break;
        fs::path src(a);
        fs::path dest = fs::path(inbox) / src.filename();
        step(src.filename().string());
        std::error_code cec;
        if (fs::exists(dest, cec)) { m_fix.sent.push_back(dest.string()); push_log("[FIX] " + src.filename().string() + " already in the import folder"); continue; }
        copy_file_robust(src, dest, false, cec);
        if (cec) { m_fix.failed++; push_log("[FIX] could not copy " + src.string() + ": " + cec.message()); continue; }
        m_fix.copied++;
        m_fix.sent.push_back(dest.string());
        push_log("[FIX] repairable " + src.filename().string() + " -> import folder");
    }

    if (!quarantine.empty() && (m_fix.moved > 0 || m_fix.cleaned > 0) && !manifest.save())
        push_log("[FIX] could not write " + std::string(RomManifest::kFileName));
    m_finished_dispatcher();
}

// ═══ Export ═════════════════════════════════════════════════════════════════

void RomLibraryTab::on_export(int format) {
    if (!m_audit_ever_run) { flash(_("Run an audit first.")); return; }
    auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
    Gtk::FileChooserDialog dlg(_("Export audit"), Gtk::FILE_CHOOSER_ACTION_SAVE);
    if (top) dlg.set_transient_for(*top);
    dlg.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dlg.add_button(_("Save"),   Gtk::RESPONSE_OK);
    dlg.set_current_name(format == 0 ? "library-audit.txt" : format == 1 ? "library-audit.csv" : "missing-sets.dat");
    dlg.set_do_overwrite_confirmation(true);
    if (dlg.run() != Gtk::RESPONSE_OK) return;

    std::ofstream out(dlg.get_filename());
    if (!out) { if (top) ui::notice(*top, _("Could not write the file."), dlg.get_filename()); return; }

    if (format == 0) {
        out << "# ROM library audit : Bootcade\n";
        out << "# " << m_audit.total << " sets: " << m_audit.available << " correct, "
            << m_audit.incorrect << " incorrect, " << m_audit.missing << " missing, "
            << m_audit.repairable << " repairable from the library, "
            << m_audit.ignored << " ignored\n";
        for (const char* want : {"incorrect", "missing"}) {
            out << "\n\n=== " << want << " ===\n";
            for (const auto& g : m_audit.games) {
                if (g.status != want || g.ignored) continue;
                out << "\n" << g.name << "  (" << g.description << ")  [" << g.system << "]"
                    << (g.repairable ? "  repairable" : "") << "\n";
                if (g.archive_found) out << "    archive: " << g.archive << "\n";
                for (const auto& r : g.roms) {
                    if (r.state == RomAudit::RomState::Present) continue;
                    if (r.is_disk) out << "    " << _(state_label(r.state)) << "\t" << r.name << "\tsha1=" << r.sha1;
                    else out << "    " << _(state_label(r.state)) << "\t" << r.name << "\tcrc=" << crc_hex(r.crc) << "\tsize=" << r.size;
                    if (!r.found_as.empty()) out << "\tfound as " << r.found_as;
                    if (!r.found_in.empty()) out << "\tcopy in " << r.found_in;
                    out << "\n";
                }
            }
        }
        if (!m_audit.orphans.empty()) {
            out << "\n\n=== orphan archives ===\n";
            for (const auto& o : m_audit.orphans) out << o.path << "\n";
        }
    } else if (format == 1) {
        out << "status,game,description,system,parent,expected_file,your_file,rom,rom_state,crc_expected,crc_found,size,found_as,found_in,inherited_from\n";
        for (const auto& g : m_audit.games) {
            std::string status = g.ignored ? "ignored" : g.status;
            std::string yours = g.archive_found ? g.archive : "";
            if (g.roms.empty() || g.status == "available") {
                out << csv(status) << ',' << csv(g.name) << ',' << csv(g.description) << ',' << csv(g.system) << ','
                    << csv(g.cloneof) << ',' << csv(g.name + ".zip") << ',' << csv(yours) << ",,,,,,,,\n";
                continue;
            }
            for (const auto& r : g.roms) {
                out << csv(status) << ',' << csv(g.name) << ',' << csv(g.description) << ',' << csv(g.system) << ','
                    << csv(g.cloneof) << ',' << csv(g.name + (g.is_chd ? "/" : ".zip")) << ',' << csv(yours) << ','
                    << csv(r.name) << ',' << _(state_label(r.state)) << ',' << hash_of(r) << ','
                    << (r.state == RomAudit::RomState::Absent ? "" : found_hash_of(r)) << ',' << r.size << ','
                    << csv(r.found_as) << ',' << csv(r.found_in) << ',' << csv(r.inherited_from) << "\n";
            }
        }
        for (const auto& o : m_audit.orphans)
            out << "orphan,,," << csv(fs::path(o.path).parent_path().filename().string()) << ",,," << csv(o.path) << ",,,,,,,,\n";
    } else {
        // A datafile of what is missing : feed it to any ROM manager, or to
        // Import once the files are found.
        out << "<?xml version=\"1.0\"?>\n"
            << "<!DOCTYPE datafile PUBLIC \"-//Logiqx//DTD ROM Management Datafile//EN\" \"http://www.logiqx.com/Dats/datafile.dtd\">\n"
            << "<datafile>\n\t<header>\n\t\t<name>Bootcade - Missing sets</name>\n"
            << "\t\t<description>Sets missing or incorrect in the library, exported by Bootcade</description>\n"
            << "\t\t<version>" << format_time(std::time(nullptr)) << "</version>\n\t</header>\n";
        for (const auto& g : m_audit.games) {
            if (g.status == "available" || g.ignored) continue;
            out << "\t<game name=\"" << xml(g.name) << "\"";
            if (!g.cloneof.empty()) out << " cloneof=\"" << xml(g.cloneof) << "\" romof=\"" << xml(g.cloneof) << "\"";
            out << ">\n\t\t<description>" << xml(g.description) << "</description>\n";
            for (const auto& r : g.roms) {
                if (r.is_disk) {
                    // The DAT names a disk without its extension.
                    out << "\t\t<disk name=\"" << xml(fs::path(r.name).stem().string()) << "\" sha1=\"" << r.sha1 << "\"/>\n";
                    continue;
                }
                out << "\t\t<rom name=\"" << xml(r.name) << "\" size=\"" << r.size << "\" crc=\"" << crc_hex(r.crc) << "\""
                    << (r.state == RomAudit::RomState::Present ? " status=\"verified\"" : "") << "/>\n";
            }
            out << "\t</game>\n";
        }
        out << "</datafile>\n";
    }
    flash(Glib::ustring::compose(_("Exported to %1."), dlg.get_filename()));
}

// ═══ Worker plumbing ════════════════════════════════════════════════════════

RomInbox::Callbacks RomLibraryTab::make_callbacks() {
    RomInbox::Callbacks cb;
    cb.progress  = [this](double p, const std::string& m) { push_progress(p, m); };
    cb.log       = [this](const std::string& m) { push_log(m); };
    cb.cancelled = [this] { return m_cancelled.load(); };
    return cb;
}

void RomLibraryTab::push_progress(double pct, const std::string& msg) {
    {
        std::lock_guard<std::mutex> lk(m_shared_mutex);
        m_current_message = msg;
    }
    m_progress_value.store(pct);
    static thread_local auto last = std::chrono::steady_clock::now() - std::chrono::hours(1);
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() >= UI_DISPATCH_INTERVAL_MS) {
        last = now;
        m_progress_dispatcher();
    }
}

void RomLibraryTab::push_log(const std::string& msg) {
    std::cerr << "[LIBRARY] " << msg << std::endl;
    {
        std::lock_guard<std::mutex> lk(m_shared_mutex);
        m_log_messages.push_back(msg);
    }
    m_progress_dispatcher();
}

void RomLibraryTab::on_progress_update() {
    std::string message;
    std::vector<std::string> pending;
    {
        std::lock_guard<std::mutex> lk(m_shared_mutex);
        message = m_current_message;
        pending.swap(m_log_messages);
    }
    double pct = m_progress_value.load();
    m_progress.set_fraction(std::clamp(pct / 100.0, 0.0, 1.0));
    m_progress.set_text(std::to_string((int)pct) + "%");
    if (!message.empty() && m_busy) m_status.set_text(message);
    for (const auto& l : pending) m_sig_log.emit(l);
}

void RomLibraryTab::on_worker_finished() {
    if (m_worker.joinable()) m_worker.join();
    on_progress_update();

    if (m_job == Job::Audit) {
        m_audit_ever_run = true;
        m_db->setScanMetadata("last_audit_time", (int64_t)std::time(nullptr));
        update_last_audit_label();
        populate();
        AppContext::trim_heap();   // the audit's temporaries are gone : give the pages back
    } else if (m_job == Job::Download) {
        Glib::ustring status = m_dl.got.empty() && !m_dl.stopped.empty() ? Glib::ustring()
            : Glib::ustring::compose(_("Download : %1 set(s) received, sent to Import."), (int)m_dl.got.size());
        if (!m_dl.failed.empty()) status += Glib::ustring::compose(_(" %1 could not be downloaded."), (int)m_dl.failed.size());
        if (m_cancelled) status += _(" Cancelled.");
        if (!m_dl.stopped.empty()) status += (status.empty() ? "" : " ") + m_dl.stopped;
        m_job = Job::None;
        set_busy(false);
        flash(status);
        m_sig_log.emit(status.raw());
        /* Un bilan qui reste a l'ecran.
         *
         * Le seul message etait celui de la barre d'etat, efface au bout de
         * quelques secondes : quand aucune ROM n'arrivait (absente du serveur,
         * quota atteint), le joueur ne voyait rien se passer et cherchait
         * des fichiers qui n'existaient pas. Le bilan dit ce qui est arrive,
         * OU, et ce qui a echoue avec sa raison. */
        if (auto* top = dynamic_cast<Gtk::Window*>(get_toplevel())) {
            std::string text;
            if (!m_dl.got.empty())
                text += Glib::ustring::compose(_("%1 set(s) downloaded into the import folder:\n%2"),
                                               (int)m_dl.got.size(), m_job_paths.inbox).raw();
            if (!m_dl.failed.empty()) {
                if (!text.empty()) text += "\n\n";
                text += Glib::ustring::compose(_("%1 set(s) could not be downloaded:"), (int)m_dl.failed.size()).raw();
                const size_t shown = std::min<size_t>(m_dl.failed.size(), 8);
                for (size_t i = 0; i < shown; ++i) text += "\n• " + m_dl.failed[i];
                if (m_dl.failed.size() > shown)
                    text += "\n" + Glib::ustring::compose(_("…and %1 more, listed in the Import log."),
                                                          (int)(m_dl.failed.size() - shown)).raw();
            }
            if (!m_dl.stopped.empty()) text += (text.empty() ? "" : "\n\n") + m_dl.stopped;
            if (m_cancelled) text += (text.empty() ? "" : "\n\n") + std::string(_("The download was cancelled."));
            if (text.empty()) text = _("Nothing was downloaded.");
            const bool got = !m_dl.got.empty();
            if (got) {
                if (ui::offer(*top, _("Download from Bootcade"), text, _("Open import folder"),
                              "bc-download.svg", "bc-folder.svg"))
                    ui::open_uri(Glib::filename_to_uri(m_job_paths.inbox), top);
            } else {
                ui::notice(*top, _("Download from Bootcade"), text, "bc-warning.svg");
            }
        }
        if (!m_dl.got.empty()) m_sig_send_to_import.emit(m_dl.got);
        return;
    } else if (m_job == Job::Fix) {
        Glib::ustring status = Glib::ustring::compose(
            _("Fix : %1 archive(s) moved to quarantine, %2 cleaned of extra files, %3 repairable set(s) sent to Import."),
            m_fix.moved, m_fix.cleaned, (int)m_fix.sent.size());
        if (m_fix.failed) status += Glib::ustring::compose(_(" %1 item(s) could not be processed."), m_fix.failed);
        m_job = Job::None;
        set_busy(false);
        flash(status);
        m_sig_log.emit(status.raw());
        // The library changed under the audit : the owner rescans, then calls
        // refresh_after_scan(). Import takes over the copied sets ; when a
        // scan is pending the owner holds that until the scan is done.
        if (m_fix.moved || m_fix.cleaned || m_fix.relocated) m_sig_scan.emit(m_audit.emulator);
        if (!m_fix.sent.empty()) m_sig_send_to_import.emit(m_fix.sent);
        return;
    }
    m_job = Job::None;
    set_busy(false);
}

void RomLibraryTab::set_busy(bool busy) {
    m_busy = busy;
    m_btn_scan->set_sensitive(!busy);
    m_btn_audit->set_sensitive(!busy);
    m_btn_export->set_sensitive(!busy);
    m_btn_select_all->set_sensitive(!busy);
    m_btn_select_none->set_sensitive(!busy);
    if (m_btn_download) m_btn_download->set_sensitive(!busy);
    if (busy) { m_progress.set_fraction(0.0); m_progress.show(); m_btn_cancel->show(); }
    else      { m_progress.hide(); m_btn_cancel->hide(); }
    update_action_buttons();
}

void RomLibraryTab::flash(const Glib::ustring& text) {
    if (m_busy) return;
    m_status.set_text(text);
    m_flash_timer.disconnect();
    m_flash_timer = Glib::signal_timeout().connect([this] { if (!m_busy) update_summary(); return false; }, 3500);
}
