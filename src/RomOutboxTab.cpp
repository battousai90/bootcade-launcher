// src/RomOutboxTab.cpp
#include "RomOutboxTab.h"
#include "FileCopy.h"

#include "AppContext.h"
#include "ConfirmationDialog.h"
#include "DatParser.h"
#include "DatSource.h"
#include "RomArchive.h"
#include "RomResolve.h"
#include "i18n.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <unordered_set>

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

std::string join(const std::vector<std::string>& items, const char* sep) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) { if (i) out += sep; out += items[i]; }
    return out;
}

// "FinalBurn Neo - GBA Games" -> "GBA", "MAME ROMs (split)" -> "ROMs (split)" :
// the folder is the DAT header (RomResolve::expected_folder), so the rule
// DatParser applies to that header gives back the exact system string the
// database holds. A folder that follows neither naming is its own system.
std::string system_of_folder(const std::string& folder) {
    const std::string system = DatParser::extractSystemFromHeader(folder);
    return system == "Unknown" ? folder : system;
}

bool move_file(const fs::path& src, const fs::path& dest, std::string& error) {
    std::error_code ec;
    fs::rename(src, dest, ec);
    if (!ec) return true;
    std::error_code rec = ec;
    copy_file_robust(src, dest, true, ec);
    if (ec) { error = "copy failed: " + ec.message() + " (rename had failed: " + rec.message() + ")"; return false; }
    fs::remove(src, ec);
    return true;
}

// Same entries, same data : the two archives are the same set, however they
// were compressed.
bool same_contents(const std::string& a, const std::string& b) {
    std::vector<RomArchive::Entry> ea, eb;
    if (!RomArchive::read_entries(a, ea) || !RomArchive::read_entries(b, eb)) return false;
    if (ea.size() != eb.size()) return false;
    std::set<std::pair<std::string, unsigned long>> sa, sb;
    for (const auto& e : ea) sa.emplace(e.name, e.crc);
    for (const auto& e : eb) sb.emplace(e.name, e.crc);
    return sa == sb;
}

ui::LogPanel::Level level_of(const std::string& m) {
    if (m.rfind("REFUSED", 0) == 0 || m.rfind("FAILED", 0) == 0 || m.find("could not") != std::string::npos) return ui::LogPanel::Level::Error;
    if (m.rfind("moved", 0) == 0) return ui::LogPanel::Level::Ok;
    if (m.rfind("replaced", 0) == 0 || m.rfind("skipped", 0) == 0 || m.rfind("identical", 0) == 0) return ui::LogPanel::Level::Warn;
    return ui::LogPanel::Level::Info;
}

} // namespace

// ═══ Construction ═══════════════════════════════════════════════════════════

RomOutboxTab::RomOutboxTab(std::shared_ptr<DatabaseManager> db, PathsProvider paths)
    : Gtk::Box(Gtk::ORIENTATION_VERTICAL, ui::kCardSpacing), m_db(std::move(db)), m_paths(std::move(paths)) {
    get_style_context()->add_class("set-page");
    build_header();
    build_table();
    build_footer();
    m_progress_dispatcher.connect(sigc::mem_fun(*this, &RomOutboxTab::on_progress_update));
    m_finished_dispatcher.connect(sigc::mem_fun(*this, &RomOutboxTab::on_worker_finished));
    reload_settings();
    show_all_children();
    m_progress.hide();
    m_progress_label.hide();
    m_btn_cancel->hide();
}

RomOutboxTab::~RomOutboxTab() {
    if (m_worker.joinable()) {
        m_cancelled = true;
        m_worker.join();
    }
}

void RomOutboxTab::build_header() {
    auto card = ui::card("bc-package.svg", _("Outbox"),
                         _("Repaired sets, ready to be moved into your library. Verify them here first."));
    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 8);
    body->set_margin_top(10);
    // Le dossier se choisit la ou on regarde son contenu, comme le dossier
    // d'import dans l'onglet Import : meme ligne, meme bouton.
    auto* path_line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    m_entry_folder.set_hexpand(true);
    m_entry_folder.set_placeholder_text(_("No outbox folder set yet"));
    m_entry_folder.signal_activate().connect([this] { apply_outbox_path(m_entry_folder.get_text().raw()); });
    m_btn_browse = ui::button(_("Browse…"), "bc-folder.svg");
    m_btn_browse->signal_clicked().connect(sigc::mem_fun(*this, &RomOutboxTab::on_browse_folder));
    path_line->pack_start(m_entry_folder, Gtk::PACK_EXPAND_WIDGET);
    path_line->pack_start(*m_btn_browse, Gtk::PACK_SHRINK);
    body->pack_start(*path_line, Gtk::PACK_SHRINK);
    auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    m_btn_open = ui::button(_("Open folder"), "bc-external.svg");
    m_btn_open->signal_clicked().connect(sigc::mem_fun(*this, &RomOutboxTab::on_open_folder));
    m_btn_refresh = ui::button(_("Refresh"), "bc-sync.svg");
    m_btn_refresh->signal_clicked().connect(sigc::mem_fun(*this, &RomOutboxTab::refresh));
    actions->pack_start(*m_btn_open, Gtk::PACK_SHRINK);
    actions->pack_start(*m_btn_refresh, Gtk::PACK_SHRINK);
    body->pack_start(*actions, Gtk::PACK_SHRINK);
    auto* hint = ui::sub_label(_("Import writes the sets it repairs into this folder."));
    body->pack_start(*hint, Gtk::PACK_SHRINK);
    card.body->pack_start(*body, Gtk::PACK_SHRINK);
    card.frame->set_size_request(380, -1);
    m_top.pack_start(*card.frame, Gtk::PACK_EXPAND_WIDGET);

    auto opts = ui::card("gear.svg", _("Move options"), _("How sets are added to your library."));
    auto* obody = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 8);
    obody->set_margin_top(10);
    m_check_keep_replaced.set_label(_("Keep replaced library files in Quarantine"));
    m_check_keep_replaced.set_tooltip_text(_("A library archive overwritten by a move is kept in the quarantine, never deleted."));
    obody->pack_start(m_check_keep_replaced, Gtk::PACK_SHRINK);
    auto* coll = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    auto* coll_label = ui::title_label(_("If the destination exists"));
    coll_label->set_valign(Gtk::ALIGN_CENTER);
    m_combo_collision.append("replace",        _("Replace it (the outbox set is the verified one)"));
    m_combo_collision.append("skip_identical", _("Skip when identical, replace otherwise"));
    m_combo_collision.append("skip",           _("Skip, leave the set in the outbox"));
    m_combo_collision.set_active_id("replace");
    coll->pack_start(*coll_label, Gtk::PACK_SHRINK);
    coll->pack_start(m_combo_collision, Gtk::PACK_EXPAND_WIDGET);
    obody->pack_start(*coll, Gtk::PACK_SHRINK);
    auto* dest_line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    m_btn_destinations = ui::button(_("Destination mapping…"), "bc-folder.svg");
    m_btn_destinations->set_tooltip_text(_("Which ROM directory each system folder of the outbox goes to. By default, the directory with the same name."));
    m_btn_destinations->signal_clicked().connect(sigc::mem_fun(*this, &RomOutboxTab::on_edit_destinations));
    dest_line->pack_start(*m_btn_destinations, Gtk::PACK_SHRINK);
    auto* verify = ui::status_dot(_("Every set is verified against the DAT before it moves."), ui::State::Ok);
    dest_line->pack_start(*verify, Gtk::PACK_SHRINK);
    obody->pack_start(*dest_line, Gtk::PACK_SHRINK);
    opts.body->pack_start(*obody, Gtk::PACK_SHRINK);
    m_top.pack_start(*opts.frame, Gtk::PACK_EXPAND_WIDGET);
    pack_start(m_top, Gtk::PACK_SHRINK);

    m_check_keep_replaced.signal_toggled().connect([this] { save_settings(); });
    m_combo_collision.signal_changed().connect([this] { save_settings(); });
}

void RomOutboxTab::build_table() {
    auto* summary = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    m_pill_total    = Gtk::make_managed<ui::Pill>(_("Total"),    ui::PillTone::Neutral);
    m_pill_ready    = Gtk::make_managed<ui::Pill>(_("Ready"),    ui::PillTone::Ok,   true);
    m_pill_unmapped = Gtk::make_managed<ui::Pill>(_("No destination"), ui::PillTone::Warn, true);
    for (auto* p : {m_pill_total, m_pill_ready, m_pill_unmapped}) { p->set_count(0); m_pills.pack_start(*p, Gtk::PACK_SHRINK); }
    m_pill_ready->set_active(true);
    m_pill_unmapped->set_active(true);
    for (auto* p : {m_pill_ready, m_pill_unmapped}) p->signal_toggled().connect([this](bool) { refilter(); });
    summary->pack_start(m_pills, Gtk::PACK_SHRINK);
    m_status.set_xalign(0.0f);
    m_status.get_style_context()->add_class("set-sub");
    m_status.set_ellipsize(Pango::ELLIPSIZE_END);
    m_status.set_valign(Gtk::ALIGN_CENTER);
    summary->pack_start(m_status, Gtk::PACK_EXPAND_WIDGET);
    m_btn_select_all  = ui::button(_("Select all"));
    m_btn_select_none = ui::button(_("Select none"));
    m_btn_select_all->signal_clicked().connect([this] { set_all_checked(true); });
    m_btn_select_none->signal_clicked().connect([this] { set_all_checked(false); });
    summary->pack_end(*m_btn_select_none, Gtk::PACK_SHRINK);
    summary->pack_end(*m_btn_select_all, Gtk::PACK_SHRINK);
    pack_start(*summary, Gtk::PACK_SHRINK);

    m_filter = Gtk::make_managed<ui::FilterBar>(_("Search (game, file, destination…)"));
    m_system_combo = m_filter->add_combo(_("System:"));
    m_system_combo->append(_("All"));
    m_system_combo->set_active(0);
    m_filter->signal_changed().connect(sigc::mem_fun(*this, &RomOutboxTab::refilter));
    pack_start(*m_filter, Gtk::PACK_SHRINK);

    m_store = Gtk::ListStore::create(m_cols);
    m_models.sort_column = m_cols.game.index();
    m_models.sort_order  = Gtk::SORT_ASCENDING;

    m_table = Gtk::make_managed<ui::Table>(Gtk::SELECTION_MULTIPLE);
    m_models.attach(m_table->view(), m_store, sigc::mem_fun(*this, &RomOutboxTab::row_visible));
    m_table->add_check_column(m_cols.include, sigc::mem_fun(*this, &RomOutboxTab::on_row_toggled),
                              [this](bool on) { set_all_checked(on); });
    { ui::ColumnOptions o; o.expand = true; o.min_width = 180; m_table->add_text_column(_("Game / ROM"), m_cols.game, o); }
    m_table->add_text_column(_("System"), m_cols.system);
    { ui::ColumnOptions o; o.mono = true; m_table->add_text_column(_("Archive"), m_cols.archive, o); }
    m_table->add_text_column(_("Parent"), m_cols.parent);
    { ui::ColumnOptions o; o.xalign = 1.0f; m_table->add_text_column(_("Files"), m_cols.files, o); }
    { ui::ColumnOptions o; o.xalign = 1.0f; m_table->add_text_column(_("Size"), m_cols.size, o); }
    {
        auto* renderer = Gtk::make_managed<Gtk::CellRendererText>();
        renderer->property_ellipsize() = Pango::ELLIPSIZE_MIDDLE;
        auto* col = Gtk::make_managed<Gtk::TreeViewColumn>(_("Destination"), *renderer);
        col->add_attribute(renderer->property_text(), m_cols.destination);
        col->set_expand(true);
        col->set_resizable(true);
        col->set_cell_data_func(*renderer, [this, renderer](Gtk::CellRenderer*, const Gtk::TreeModel::iterator& it) {
            ensure_colours();
            if (!m_colours.ready) return;
            renderer->property_foreground_rgba() = (*it)[m_cols.mapped] ? m_colours.muted : m_colours.warn;
        });
        m_table->view().append_column(*col);
    }
    { ui::ColumnOptions o; o.expand = true; o.sortable = false; m_table->add_text_column(_("Fix performed"), m_cols.fix, o); }
    m_table->view().get_selection()->signal_changed().connect(sigc::mem_fun(*this, &RomOutboxTab::on_selection_changed));
    m_table->signal_context_menu().connect(sigc::mem_fun(*this, &RomOutboxTab::on_context_menu));
    m_table->set_size_request(-1, 140);   // never less than a few rows

    // Detail and log side by side under the table, a grip between the two
    // rows to trade height.
    auto* bottom = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, ui::kCardSpacing);
    bottom->set_homogeneous(true);
    m_detail = Gtk::make_managed<ui::DetailPanel>("bc-file.svg", _("Selected set"),
                                                  _("What was done to produce it, and what it holds."), 110);
    m_detail->show_placeholder(_("Select a set to see its details."));
    bottom->pack_start(*m_detail, Gtk::PACK_EXPAND_WIDGET);
    m_log = Gtk::make_managed<ui::LogPanel>(_("Outbox log"), _("Verification and move messages."));
    m_log->set_size_request(-1, 110);
    bottom->pack_start(*m_log, Gtk::PACK_EXPAND_WIDGET);
    pack_start(*ui::splitter(*m_table, *bottom, 230), Gtk::PACK_EXPAND_WIDGET);
}

void RomOutboxTab::build_footer() {
    m_progress.set_show_text(true);
    m_progress.set_size_request(220, -1);
    m_progress.set_valign(Gtk::ALIGN_CENTER);
    m_progress.set_no_show_all(true);
    m_progress_label.get_style_context()->add_class("set-sub");
    m_progress_label.set_ellipsize(Pango::ELLIPSIZE_END);
    m_progress_label.set_max_width_chars(48);
    m_progress_label.set_no_show_all(true);
    m_btn_cancel = ui::button(_("Cancel"), "bc-close.svg");
    m_btn_cancel->set_no_show_all(true);
    m_btn_cancel->signal_clicked().connect([this] { m_cancelled = true; m_progress_label.set_text(_("Cancelling…")); });
    m_footer.pack_start(m_progress, Gtk::PACK_SHRINK);
    m_footer.pack_start(m_progress_label, Gtk::PACK_SHRINK);
    m_footer.pack_start(*m_btn_cancel, Gtk::PACK_SHRINK);

    m_btn_move = ui::button(_("Move selected to Library"), "bc-right.svg", ui::Tone::Accent);
    m_btn_move->signal_clicked().connect([this] { on_move_clicked(false); });
    m_btn_move_more = Gtk::make_managed<Gtk::MenuButton>();
    m_btn_move_more->add(*ui::image("bc-caret-down.svg", 14));
    m_btn_move_more->get_style_context()->add_class("accent-button");
    auto* item = Gtk::make_managed<Gtk::MenuItem>(_("Move all ready sets to Library"));
    item->signal_activate().connect([this] { on_move_clicked(true); });
    m_move_menu.append(*item);
    m_move_menu.show_all();
    m_btn_move_more->set_popup(m_move_menu);
    m_footer.pack_end(*m_btn_move_more, Gtk::PACK_SHRINK);
    m_footer.pack_end(*m_btn_move, Gtk::PACK_SHRINK);
    pack_start(m_footer, Gtk::PACK_SHRINK);
}

void RomOutboxTab::ensure_colours() {
    if (m_colours.ready || !get_toplevel() || !get_toplevel()->get_realized()) return;
    m_colours.ok    = ui::probe_color(*this, "set-ok");
    m_colours.warn  = ui::probe_color(*this, "set-warn");
    m_colours.err   = ui::probe_color(*this, "set-err");
    m_colours.muted = ui::probe_color(*this, "set-sub");
    m_colours.ready = true;
}

// ═══ Settings ═══════════════════════════════════════════════════════════════

void RomOutboxTab::reload_settings() {
    nlohmann::json j;
    std::ifstream fi(AppContext::get_config_path());
    if (fi) { try { fi >> j; } catch (...) { j = nlohmann::json{}; } }
    nlohmann::json rm = (j.contains("rom_manager") && j["rom_manager"].is_object()) ? j["rom_manager"] : nlohmann::json::object();
    // The mapping first : the two widgets below save the settings as soon as
    // their value changes, mapping included. Read after them, the first
    // opening of the window wrote an empty mapping over the saved one.
    m_destinations.clear();
    if (rm.contains("destinations") && rm["destinations"].is_object())
        for (auto it = rm["destinations"].begin(); it != rm["destinations"].end(); ++it)
            if (it.value().is_string()) m_destinations[it.key()] = it.value().get<std::string>();
    m_check_keep_replaced.set_active(!(rm.contains("outbox_keep_replaced") && rm["outbox_keep_replaced"].is_boolean()) || rm["outbox_keep_replaced"].get<bool>());
    std::string coll = (rm.contains("outbox_collision") && rm["outbox_collision"].is_string()) ? rm["outbox_collision"].get<std::string>() : "replace";
    if (!m_combo_collision.set_active_id(coll)) m_combo_collision.set_active_id("replace");
    refresh();
}

void RomOutboxTab::save_settings() const {
    nlohmann::json j;
    const std::string path = AppContext::get_config_path();
    { std::ifstream fi(path); if (fi) { try { fi >> j; } catch (...) { j = nlohmann::json{}; } } }
    auto& rm = j["rom_manager"];
    rm["outbox_keep_replaced"] = m_check_keep_replaced.get_active();
    rm["outbox_collision"]     = m_combo_collision.get_active_id().raw();
    rm["destinations"]         = nlohmann::json::object();
    for (const auto& [k, v] : m_destinations) rm["destinations"][k] = v;
    std::ofstream fo(path);
    if (fo) fo << j.dump(4);
}

// config.json est aussi ecrit par le panneau de reglages : on relit le fichier
// entier, on ne change que cette cle, et on le reecrit. Un fichier illisible
// n'est pas reecrit du tout, plutot que remplace par un fichier presque vide.
void RomOutboxTab::save_outbox_path(const std::string& folder) const {
    nlohmann::json j;
    const std::string path = AppContext::get_config_path();
    { std::ifstream fi(path); if (fi) { try { fi >> j; } catch (...) { return; } } }
    j["rom_manager"]["outbox_path"] = folder;
    std::ofstream fo(path);
    if (fo) fo << j.dump(4);
}

void RomOutboxTab::apply_outbox_path(const std::string& folder) {
    if (m_busy) return;
    if (folder == m_paths().outbox) return;
    save_outbox_path(folder);
    m_entry_folder.set_text(folder);
    m_sig_outbox_path.emit(folder);
    // Sans cela l'ecran continuerait de montrer le contenu de l'ancien dossier.
    refresh();
}

void RomOutboxTab::on_browse_folder() {
    auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
    Gtk::FileChooserDialog dlg(_("Select the outbox folder"), Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
    if (top) dlg.set_transient_for(*top);
    dlg.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dlg.add_button(_("Select"), Gtk::RESPONSE_OK);
    const std::string current = m_entry_folder.get_text().raw();
    if (!current.empty()) dlg.set_filename(current);
    if (dlg.run() != Gtk::RESPONSE_OK) return;
    apply_outbox_path(dlg.get_filename());
}

// Which emulator a system folder of the outbox belongs to : the folder is
// named after the folder of a DAT, whose group says which emulator it
// describes.
std::string RomOutboxTab::emulator_of_folder(const std::string& system_folder) const {
    for (const auto& [file, reading] : RomResolve::dat_readings(m_db))
        if (!reading.emulator.empty() && lower(reading.folder) == lower(system_folder)) return reading.emulator;
    return "";
}

std::string RomOutboxTab::destination_for(const std::string& system_folder, const Paths&) const {
    auto it = m_destinations.find(system_folder);
    if (it != m_destinations.end() && !it->second.empty()) return it->second;
    return auto_destination(system_folder);
}

// Where a DAT's folder is in the library, the way the audit looks for it
// (RomResolve::CacheIndex::in_folder) : a ROM directory of that name, else a
// directory of that name under one of them, else, when the emulator has a
// single ROM directory, that directory's subfolder of that name. Among the ROM directories of the
// emulator the folder belongs to : a MAME set never lands in a FinalBurn Neo
// folder. Compared without case : the DAT "MAME" lands in a folder "Mame".
std::string RomOutboxTab::auto_destination(const std::string& system_folder) const {
    const auto roots = DatSource::roms_paths_for(emulator_of_folder(system_folder));
    for (const auto& root : roots)
        if (lower(fs::path(root).filename().string()) == lower(system_folder)) return root;
    std::error_code ec;
    for (const auto& root : roots) {
        for (auto e = fs::directory_iterator(root, ec); !ec && e != fs::directory_iterator(); e.increment(ec))
            if (e->is_directory(ec) && lower(e->path().filename().string()) == lower(system_folder))
                return e->path().string();
    }
    // Not in the library yet (the first set of a DAT) : with a single ROM
    // directory for the emulator there is only one place for it, created
    // when the first set moves in. With several, the user says which.
    if (roots.size() == 1 && !system_folder.empty()) return (fs::path(roots.front()) / system_folder).string();
    return "";
}

// A small table of "system folder → ROM directory", one row per DAT header
// the database knows plus whatever the outbox holds. An empty directory
// means "the folder of that name in the library" (auto_destination), which
// is what almost every row wants.
void RomOutboxTab::on_edit_destinations() {
    auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
    Paths p = m_paths();
    std::set<std::string> folders;
    for (const auto& h : m_db->getDatHeaders()) folders.insert(h);
    for (const auto& it : m_items) folders.insert(it.system_folder);
    for (const auto& [k, v] : m_destinations) folders.insert(k);

    Gtk::Dialog dlg;
    dlg.set_title(_("Destination mapping"));
    if (top) dlg.set_transient_for(*top);
    dlg.set_modal(true);
    dlg.set_decorated(false);
    dlg.get_style_context()->add_class("cc-window");
    dlg.get_style_context()->add_class("set-window");
    dlg.set_default_size(860, 520);
    auto* content = dlg.get_content_area();
    content->set_spacing(0);

    auto* page = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, ui::kCardSpacing);
    page->get_style_context()->add_class("set-page");
    auto card = ui::card("bc-folder.svg", _("Destination mapping"),
                         _("Where each system folder of the outbox is moved. Leave a row empty to use the ROM directory with the same name."));
    auto* rows = ui::rows();
    rows->set_margin_top(10);
    std::map<std::string, Gtk::Entry*> entries;
    for (const auto& f : folders) {
        auto* line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
        line->get_style_context()->add_class("set-row");
        auto* name = ui::title_label(f);
        name->set_size_request(300, -1);
        name->set_ellipsize(Pango::ELLIPSIZE_END);
        line->pack_start(*name, Gtk::PACK_SHRINK);
        auto* entry = Gtk::make_managed<Gtk::Entry>();
        entry->set_hexpand(true);
        auto it = m_destinations.find(f);
        if (it != m_destinations.end()) entry->set_text(it->second);
        const std::string by_name = auto_destination(f);
        entry->set_placeholder_text(by_name.empty() ? Glib::ustring(_("no ROM directory of that name : set one")) : Glib::ustring(by_name));
        line->pack_start(*entry, Gtk::PACK_EXPAND_WIDGET);
        auto* browse = ui::button(_("Browse…"), "bc-folder.svg");
        browse->signal_clicked().connect([&dlg, entry] {
            Gtk::FileChooserDialog fc(dlg, _("Select the destination directory"), Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
            fc.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
            fc.add_button(_("Select"), Gtk::RESPONSE_OK);
            if (fc.run() == Gtk::RESPONSE_OK) entry->set_text(fc.get_filename());
        });
        line->pack_start(*browse, Gtk::PACK_SHRINK);
        ui::add_row(rows, *line);
        entries[f] = entry;
    }
    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    scroll->add(*rows);
    card.body->pack_start(*scroll, Gtk::PACK_EXPAND_WIDGET);
    page->pack_start(*card.frame, Gtk::PACK_EXPAND_WIDGET);
    content->pack_start(*page, Gtk::PACK_EXPAND_WIDGET);

    auto* foot = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    foot->get_style_context()->add_class("cc-footer");
    auto* cancel = ui::button(_("Cancel"));
    cancel->signal_clicked().connect([&dlg] { dlg.response(Gtk::RESPONSE_CANCEL); });
    auto* save = ui::button(_("Save"), "bc-save.svg", ui::Tone::Accent);
    save->signal_clicked().connect([&dlg] { dlg.response(Gtk::RESPONSE_OK); });
    foot->pack_end(*save, Gtk::PACK_SHRINK);
    foot->pack_end(*cancel, Gtk::PACK_SHRINK);
    content->pack_start(*foot, Gtk::PACK_SHRINK);
    dlg.show_all_children();
    if (dlg.run() != Gtk::RESPONSE_OK) return;

    m_destinations.clear();
    for (const auto& [f, entry] : entries) {
        std::string v = entry->get_text().raw();
        if (!v.empty()) m_destinations[f] = v;
    }
    save_settings();
    refresh();
}

// ═══ Content ════════════════════════════════════════════════════════════════

void RomOutboxTab::refresh() {
    if (m_busy) return;
    Paths p = m_paths();
    std::error_code ec;
    m_items.clear();
    // Pas de set_text inconditionnel : l'utilisateur peut etre en train de
    // taper dans le champ pendant qu'un refresh arrive.
    if (m_entry_folder.get_text().raw() != p.outbox) m_entry_folder.set_text(p.outbox);
    if (p.outbox.empty() || !fs::is_directory(p.outbox, ec)) {
        m_manifest = RomManifest::Manifest();
        populate();
        return;
    }
    m_manifest = RomManifest::Manifest::load(p.outbox);
    if (m_manifest.reconcile() > 0) m_manifest.save();

    std::vector<fs::path> systems;
    for (auto it = fs::directory_iterator(p.outbox, ec); it != fs::directory_iterator(); ++it)
        if (it->is_directory(ec)) systems.push_back(it->path());
    std::sort(systems.begin(), systems.end());
    for (const auto& sysdir : systems) {
        std::vector<fs::path> zips;
        for (auto it = fs::directory_iterator(sysdir, ec); it != fs::directory_iterator(); ++it)
            if (it->is_regular_file(ec) && lower(it->path().extension().string()) == ".zip")
                zips.push_back(it->path());
        std::sort(zips.begin(), zips.end());
        const std::string folder = sysdir.filename().string();
        const std::string destination = destination_for(folder, p);
        for (const auto& z : zips) {
            Item item;
            item.path = z.string();
            item.system_folder = folder;
            item.game = z.stem().string();
            item.system = system_of_folder(folder);
            item.dat_header = folder;
            item.emulator = emulator_of_folder(folder);
            item.bytes = fs::file_size(z, ec);
            item.destination = destination;
            item.dest_exists = !destination.empty() && fs::exists(fs::path(destination) / z.filename(), ec);
            item.entry = m_manifest.find(m_manifest.relative(z.string()));
            Game g = m_db->getGame(item.game, item.system, item.emulator);
            if (!g.name.empty()) {
                item.parent = g.cloneof;
                if (!g.system.empty()) item.system = g.system;
                for (const auto& r : g.roms) if (!r.crc.empty()) ++item.files_expected;
            }
            std::vector<std::string> names;
            if (RomArchive::list_names(z.string(), names)) item.files = (int)names.size();
            m_items.push_back(std::move(item));
        }
        // CHDs, one folder per set as the DAT's layout puts them.
        std::vector<fs::path> disks;
        for (auto it = fs::directory_iterator(sysdir, ec); it != fs::directory_iterator(); ++it) {
            if (!it->is_directory(ec)) continue;
            for (auto f = fs::directory_iterator(it->path(), ec); f != fs::directory_iterator(); ++f)
                if (f->is_regular_file(ec) && lower(f->path().extension().string()) == ".chd") disks.push_back(f->path());
        }
        std::sort(disks.begin(), disks.end());
        for (const auto& d : disks) {
            Item item;
            item.disk = true;
            item.path = d.string();
            item.system_folder = folder;
            item.game = d.parent_path().filename().string();
            item.system = system_of_folder(folder);
            item.dat_header = folder;
            item.emulator = emulator_of_folder(folder);
            item.bytes = fs::file_size(d, ec);
            item.destination = destination;
            item.dest_exists = !destination.empty() && fs::exists(fs::path(destination) / item.game / d.filename(), ec);
            item.entry = m_manifest.find(m_manifest.relative(d.string()));
            item.files = item.files_expected = 1;
            Game g = m_db->getGame(item.game, item.system, item.emulator);
            if (!g.name.empty()) {
                item.parent = g.cloneof;
                if (!g.system.empty()) item.system = g.system;
            }
            m_items.push_back(std::move(item));
        }
    }
    populate();
}

void RomOutboxTab::populate() {
    m_models.detach(m_table->view());   // nothing attached while filling : see SettingsUi::ModelStack
    m_store->clear();
    std::set<std::string> systems;
    for (size_t i = 0; i < m_items.size(); ++i) {
        const auto& it = m_items[i];
        systems.insert(it.system);
        auto row = *(m_store->append());
        row[m_cols.include] = it.selected && !it.destination.empty();
        row[m_cols.game]    = it.game;
        row[m_cols.system]  = it.system;
        row[m_cols.archive] = it.disk ? it.game + "/" + fs::path(it.path).filename().string()
                                      : fs::path(it.path).filename().string();
        row[m_cols.parent]  = it.parent;
        row[m_cols.files]   = it.files_expected ? std::to_string(it.files) + "/" + std::to_string(it.files_expected) : std::to_string(it.files);
        row[m_cols.size]    = human_size(it.bytes);
        row[m_cols.mapped]  = !it.destination.empty();
        row[m_cols.destination] = it.destination.empty()
            ? Glib::ustring(_("no destination : map this system folder"))
            : Glib::ustring((fs::path(it.destination) / (it.disk ? fs::path(it.game) / fs::path(it.path).filename()
                                                                  : fs::path(it.path).filename())).string()
                            + (it.dest_exists ? _("  (exists)") : ""));
        std::string fix;
        if (it.entry) {
            fix = it.entry->action == RomManifest::action::Rebuilt ? _("rebuilt") : _("moved");
            if (!it.entry->details.empty()) fix += " · " + join(it.entry->details, " · ");
        } else {
            fix = _("added by hand (no record)");
        }
        row[m_cols.fix]   = fix;
        row[m_cols.index] = (unsigned int)i;
        row[m_cols.search_blob] = lower(it.game + ' ' + it.system + ' ' + fs::path(it.path).filename().string() + ' ' + it.destination + ' ' + it.parent);
    }
    m_filter->set_combo_items(m_system_combo, _("All"), systems, m_system_combo->get_active_text());
    refilter();
    update_summary();
    update_action_buttons();
    m_detail->set_title(_("Selected set"));
    m_detail->set_subtitle(_("What was done to produce it, and what it holds."));
    m_detail->show_placeholder(_("Select a set to see its details."));
}

void RomOutboxTab::update_summary() {
    int ready = 0, unmapped = 0;
    uintmax_t bytes = 0;
    for (const auto& it : m_items) { it.destination.empty() ? ++unmapped : ++ready; bytes += it.bytes; }
    m_pill_total->set_count((long)m_items.size());
    m_pill_ready->set_count(ready);
    m_pill_unmapped->set_count(unmapped);
    if (m_items.empty()) m_status.set_text(_("The outbox is empty."));
    else m_status.set_text(Glib::ustring::compose(_("%1 set(s), %2 · %3 with a destination, %4 without"),
                                                  (int)m_items.size(), human_size(bytes), ready, unmapped));
}

bool RomOutboxTab::row_visible(const Gtk::TreeModel::const_iterator& it) const {
    const Gtk::TreeModel::Row row = *it;
    bool mapped = row[m_cols.mapped];
    if (mapped && !m_pill_ready->active()) return false;
    if (!mapped && !m_pill_unmapped->active()) return false;
    if (!m_vis_system.empty() && row[m_cols.system] != m_vis_system) return false;
    const std::string& needle = m_vis_needle;
    if (!needle.empty()) {
        const Glib::ustring blob = row[m_cols.search_blob];
        if (blob.raw().find(needle) == std::string::npos) return false;
    }
    return true;
}

void RomOutboxTab::refilter() {
    // Rebuilt, not refiltered : see SettingsUi::ModelStack. The filter's
    // inputs are read once here, not once per row inside row_visible.
    m_vis_system = m_system_combo->get_active_row_number() > 0 ? m_system_combo->get_active_text() : Glib::ustring();
    m_vis_needle = lower(m_filter->search_text());
    m_models.detach(m_table->view());
    m_models.attach(m_table->view(), m_store, sigc::mem_fun(*this, &RomOutboxTab::row_visible));
    m_filter->set_summary(Glib::ustring::compose(_("%1 result(s)"), m_models.visible_count()));
}

Gtk::TreeModel::Row RomOutboxTab::source_row(const Gtk::TreeModel::Path& sorted_path) const {
    auto child = m_models.filter->convert_path_to_child_path(m_models.sort->convert_path_to_child_path(sorted_path));
    return *m_store->get_iter(child);
}

namespace {
struct EntryCols : public Gtk::TreeModel::ColumnRecord {
    Gtk::TreeModelColumn<Glib::ustring> name, crc, size;
    EntryCols() { add(name); add(crc); add(size); }
};
}

void RomOutboxTab::on_selection_changed() {
    auto rows = m_table->view().get_selection()->get_selected_rows();
    if (rows.empty()) { m_detail->show_placeholder(_("Select a set to see its details.")); return; }
    Gtk::TreeModel::Row row = source_row(rows.front());
    const auto& it = m_items[(unsigned int)row[m_cols.index]];
    m_detail->set_title(it.game + "  ·  " + it.system);
    std::string sub;
    if (it.entry) {
        sub = Glib::ustring::compose(_("%1 on %2 from %3"), it.entry->action, RomManifest::local_time(it.entry->added_at),
                                     fs::path(it.entry->origin).filename().string()).raw();
        if (!it.entry->details.empty()) sub += "  ·  " + join(it.entry->details, " · ");
    } else {
        sub = _("No record : this file was not produced by Import.");
    }
    m_detail->set_subtitle(sub);

    static EntryCols cols;
    auto store = Gtk::ListStore::create(cols);
    std::vector<RomArchive::Entry> entries;
    if (RomArchive::read_entries(it.path, entries)) {
        for (const auto& e : entries) {
            auto rr = *(store->append());
            rr[cols.name] = e.name;
            rr[cols.crc]  = crc_hex(e.crc);
            rr[cols.size] = human_size(e.size);
        }
    }
    auto* t = Gtk::make_managed<ui::Table>(Gtk::SELECTION_SINGLE);
    t->view().set_model(store);
    { ui::ColumnOptions o; o.mono = true; o.expand = true; t->add_text_column(_("Entry"), cols.name, o); }
    { ui::ColumnOptions o; o.mono = true; t->add_text_column(_("CRC"), cols.crc, o); }
    { ui::ColumnOptions o; o.xalign = 1.0f; t->add_text_column(_("Size"), cols.size, o); }
    m_detail->set_content(t);
}

void RomOutboxTab::on_row_toggled(const Glib::ustring& path) {
    Gtk::TreeModel::Row row = source_row(Gtk::TreeModel::Path(path));
    if (!row[m_cols.mapped]) return;
    bool on = !row[m_cols.include];
    row[m_cols.include] = on;
    m_items[(unsigned int)row[m_cols.index]].selected = on;
    update_action_buttons();
}

void RomOutboxTab::set_all_checked(bool on) {
    /* Les lignes affichees, puis la vue detachee le temps de les modifier.
     * Attachee, chaque case changee relancait filtre et tri : sur 29 000
     * lignes, tout cocher figeait la fenetre une dizaine de secondes. */
    std::vector<Gtk::TreeModel::iterator> shown;
    for (const auto& frow : m_models.filter->children())
        shown.push_back(m_models.filter->convert_iter_to_child_iter(frow));
    m_models.detach(m_table->view());
    for (const auto& it : shown) {
        Gtk::TreeModel::Row row = *it;
        if (!row[m_cols.mapped]) continue;
        row[m_cols.include] = on;
        m_items[(unsigned int)row[m_cols.index]].selected = on;
    }
    m_models.attach(m_table->view(), m_store, sigc::mem_fun(*this, &RomOutboxTab::row_visible));
    update_action_buttons();
}

void RomOutboxTab::update_action_buttons() {
    if (m_table) m_table->refresh_header_check();
    int selected = 0, ready = 0;
    for (const auto& it : m_items) {
        if (it.destination.empty()) continue;
        ++ready;
        if (it.selected) ++selected;
    }
    m_btn_move->set_label(selected ? Glib::ustring::compose(_("Move selected to Library (%1)"), selected) : Glib::ustring(_("Move selected to Library")));
    m_btn_move->set_sensitive(!m_busy && selected > 0);
    m_btn_move_more->set_sensitive(!m_busy && ready > 0);
}

void RomOutboxTab::on_context_menu(const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn*, GdkEventButton* event) {
    Gtk::TreeModel::Row row = source_row(path);
    const auto& it = m_items[(unsigned int)row[m_cols.index]];
    ui::destroy_children(m_context_menu);
    auto add = [&](const Glib::ustring& label, std::function<void()> fn, bool enabled = true) {
        auto* item = Gtk::make_managed<Gtk::MenuItem>(label);
        item->set_sensitive(enabled);
        item->signal_activate().connect([fn] { fn(); });
        m_context_menu.append(*item);
    };
    auto copy = [this](const Glib::ustring& text, const Glib::ustring& what) {
        if (text.empty()) return;
        Gtk::Clipboard::get()->set_text(text);
        flash(Glib::ustring::compose(_("Copied %1 to the clipboard."), what));
    };
    std::string game = it.game, archive = it.path, dest = it.destination, origin = it.entry ? it.entry->origin : "";
    add(_("Copy game name"),      [copy, game]    { copy(game, _("the game name")); });
    add(_("Copy archive path"),   [copy, archive] { copy(archive, _("the archive path")); });
    add(_("Copy destination"),    [copy, dest]    { copy(dest, _("the destination")); }, !dest.empty());
    add(_("Copy origin path"),    [copy, origin]  { copy(origin, _("the origin path")); }, !origin.empty());
    m_context_menu.append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());
    add(_("Destination mapping…"), [this] { on_edit_destinations(); });
    m_context_menu.show_all();
    m_context_menu.popup_at_pointer((GdkEvent*)event);
}

void RomOutboxTab::on_open_folder() {
    Paths p = m_paths();
    std::error_code ec;
    if (p.outbox.empty() || !fs::is_directory(p.outbox, ec)) { flash(_("No outbox folder to open.")); return; }
    if (!ui::open_uri(Glib::filename_to_uri(p.outbox))) flash(_("Could not open the folder."));
}

// ═══ Move ═══════════════════════════════════════════════════════════════════

void RomOutboxTab::on_move_clicked(bool all) {
    if (m_busy) return;
    m_job = MoveJob{};
    m_job.paths = m_paths();
    m_job.keep_replaced = m_check_keep_replaced.get_active();
    std::string coll = m_combo_collision.get_active_id().raw();
    m_job.collision = coll == "skip" ? Collision::Skip : coll == "skip_identical" ? Collision::SkipIdentical : Collision::Replace;
    for (auto& it : m_items) {
        if (it.destination.empty()) continue;
        if (all) it.selected = true;
        if (it.selected) m_job.items.push_back(it);
    }
    if (m_job.items.empty()) { flash(_("Check at least one set with a destination first.")); return; }
    if (all) for (auto& row : m_store->children()) if (row[m_cols.mapped]) row[m_cols.include] = true;

    int unmapped = 0;
    for (const auto& it : m_items) if (it.destination.empty()) ++unmapped;
    auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (m_job.keep_replaced && m_job.collision != Collision::Skip && m_job.paths.quarantine.empty()) {
        if (top) ui::notice(*top, _("No quarantine folder"), _("Keeping replaced files needs a quarantine directory : set one in Settings, or untick the option."));
        return;
    }
    int replacing = 0;
    for (const auto& it : m_job.items) if (it.dest_exists) ++replacing;
    Glib::ustring msg = Glib::ustring::compose(
        _("%1 set(s) will be moved from the outbox into your ROM directories, each verified against the DAT first.\n\nA ROM scan will run automatically afterwards."), (int)m_job.items.size());
    if (replacing) {
        if (m_job.collision == Collision::Skip)
            msg += Glib::ustring::compose(_("\n\n%1 of them already exist in the library and will be skipped."), replacing);
        else
            msg += Glib::ustring::compose(m_job.keep_replaced
                ? _("\n\n%1 of them replace a library file, which is kept in quarantine.")
                : _("\n\n%1 of them replace a library file, which is NOT kept."), replacing);
    }
    if (unmapped) msg += Glib::ustring::compose(_("\n\n%1 set(s) have no destination and stay in the outbox."), unmapped);
    if (top) {
        ConfirmationDialog confirm(*top, _("Move selected sets into the library?"), msg, "bc-package.svg");
        if (!confirm.show_and_confirm()) return;
    }
    m_cancelled = false;
    set_busy(true);
    m_worker = std::thread(&RomOutboxTab::worker_move, this);
}

void RomOutboxTab::worker_move() {
    RomInbox::Callbacks cb = make_callbacks();
    std::error_code ec;
    MoveJob& job = m_job;

    // The DAT is the ground truth for what "correct" means : not whatever
    // happens to already sit in the library. The archive must hold every
    // entry its set's DAT, read with that DAT's merge mode, lists for it
    // (DatLayout), exactly as the audit will judge it once moved.
    //
    // Per emulator : the outbox can hold sets of both, each checked against
    // its own DAT.
    push_progress(0.0, _("Indexing the library…"));
    std::map<std::string, std::unique_ptr<RomResolve::LayoutBook>> books;
    auto book_of = [&](const std::string& emulator) -> RomResolve::LayoutBook& {
        auto it = books.find(emulator);
        if (it == books.end())
            it = books.emplace(emulator, std::make_unique<RomResolve::LayoutBook>(m_db, emulator)).first;
        return *it->second;
    };
    auto verify = [&](const Item& it, std::string& reason) -> bool {
        RomResolve::LayoutBook& book = book_of(it.emulator);
        const Game* game = book.game(it.game, it.system);
        if (!game) { reason = "not found in the DAT for system \"" + it.system + "\""; return false; }
        const DatLayout::Archive* layout = book.archive_for(*game);
        if (it.disk) {
            // A CHD : its set's archive must list a disk of that name, and
            // the SHA1 its header declares must be that disk's.
            if (!layout || layout->name != game->name) { reason = "its DAT expects no disk in this folder"; return false; }
            const std::string name = fs::path(it.path).stem().string();
            const std::string sha1 = RomResolve::chd_header_sha1(it.path);
            if (sha1.empty()) { reason = "not a readable CHD"; return false; }
            for (const auto& d : layout->disks) {
                if (d.name != name) continue;
                if (d.sha1 == sha1) return true;
                reason = "SHA1 mismatch for " + name + ".chd (the DAT expects " + d.sha1 + ", the file declares " + sha1 + ")";
                return false;
            }
            reason = "its DAT lists no disk " + name + " for " + game->name;
            return false;
        }
        if (!layout || layout->entries.empty()) { reason = "its DAT expects no archive for this set"; return false; }
        if (layout->name != game->name) { reason = "its DAT puts it in " + layout->name + "'s archive"; return false; }
        std::vector<RomArchive::Entry> entries;
        if (!RomArchive::read_entries(it.path, entries)) { reason = "cannot read the archive"; return false; }
        RomResolve::Archive incoming;
        incoming.path = it.path;
        for (const auto& e : entries) incoming.add(e.name, e.crc);
        for (const auto& entry : layout->entries) {
            std::string found;
            const auto state = RomResolve::probe_rom(&incoming, entry.name, entry.crc, &found);
            if (state == RomResolve::RomState::Present) continue;
            if (state == RomResolve::RomState::Absent)       reason = "missing ROM required by the DAT: " + entry.name;
            else if (state == RomResolve::RomState::Corrupt) reason = "CRC mismatch for " + entry.name;
            else reason = "wrong entry name for " + entry.name + " (found as " + found + ")";
            return false;
        }
        return true;
    };

    RomManifest::Manifest outbox_manifest = RomManifest::Manifest::load(job.paths.outbox);
    RomManifest::Manifest quarantine_manifest = job.paths.quarantine.empty() ? RomManifest::Manifest() : RomManifest::Manifest::load(job.paths.quarantine);
    std::set<fs::path> touched_dirs;

    for (size_t i = 0; i < job.items.size(); ++i) {
        if (m_cancelled) break;
        const Item& it = job.items[i];
        fs::path src(it.path);
        const fs::path dest_dir = it.disk ? fs::path(it.destination) / it.game : fs::path(it.destination);
        fs::path dest = dest_dir / src.filename();
        push_progress(100.0 * (double)i / (double)job.items.size(), src.filename().string());
        touched_dirs.insert(src.parent_path());
        if (it.disk) touched_dirs.insert(src.parent_path().parent_path());

        std::string reason;
        if (!verify(it, reason)) {
            push_log("REFUSED " + src.filename().string() + " : " + reason);
            ++job.refused;
            continue;
        }
        fs::create_directories(dest_dir, ec);
        const bool exists = fs::exists(dest, ec);
        const std::string rel = outbox_manifest.relative(it.path);
        if (exists) {
            if (job.collision == Collision::Skip) {
                push_log("skipped " + src.filename().string() + " : already in the library");
                ++job.skipped;
                continue;
            }
            if (job.collision == Collision::SkipIdentical && same_contents(it.path, dest.string())) {
                // The library already holds exactly this : the outbox copy is
                // redundant, and dropping it is what "identical" means.
                fs::remove(src, ec);
                outbox_manifest.remove(rel, RomManifest::outcome::Deleted);
                push_log("identical " + src.filename().string() + " : the library copy is the same, outbox copy discarded");
                ++job.identical;
                continue;
            }
            if (job.keep_replaced) {
                fs::path qdir = fs::path(job.paths.quarantine) / "_replaced" / it.system_folder;
                fs::create_directories(qdir, ec);
                fs::path qdest = qdir / src.filename();
                std::error_code qec;
                if (fs::exists(qdest, qec)) {
                    // A previous replacement of the same set : keep both, the
                    // newer under a stamp.
                    qdest = qdir / (src.stem().string() + "." + RomManifest::now_iso() + src.extension().string());
                }
                std::string qerr;
                if (!move_file(dest, qdest, qerr)) {
                    push_log("FAILED " + src.filename().string() + " : could not keep the replaced file: " + qerr);
                    ++job.failed;
                    continue;
                }
                RomManifest::Entry e;
                e.file = quarantine_manifest.relative(qdest.string());
                e.game = it.game;
                e.system = it.system;
                e.dat_header = it.dat_header;
                e.reason = RomManifest::reason::Replaced;
                e.origin = dest.string();
                e.action = RomManifest::action::Moved;
                e.details.push_back("replaced by the outbox copy " + fs::path(it.path).filename().string());
                quarantine_manifest.add(std::move(e));
                push_log("replaced " + src.filename().string() + " : previous library copy kept in quarantine");
            } else {
                push_log("replaced " + src.filename().string() + " : previous library copy overwritten");
            }
            ++job.replaced;
        }
        std::string err;
        if (!move_file(src, dest, err)) {
            push_log("FAILED " + src.filename().string() + " : " + err);
            ++job.failed;
            continue;
        }
        outbox_manifest.remove(rel, RomManifest::outcome::MovedToLibrary);
        ++job.moved;
        job.moved_emulators.insert(it.emulator);
        push_log("moved " + src.filename().string() + " → " + dest.string());
    }

    // A system folder emptied by the move above shouldn't linger : once its
    // contents are in the library, the outbox goes back to being empty.
    // Deepest first : a set's CHD folder, then the system folder holding it.
    for (auto dir = touched_dirs.rbegin(); dir != touched_dirs.rend(); ++dir) {
        std::error_code rmec;
        if (fs::is_empty(*dir, rmec) && !rmec) fs::remove(*dir, rmec);
    }
    if (job.moved + job.identical > 0) outbox_manifest.save();
    if (job.replaced > 0 && job.keep_replaced) quarantine_manifest.save();
    push_log("done: " + std::to_string(job.moved) + " moved, " + std::to_string(job.replaced) + " replaced, " +
             std::to_string(job.identical) + " identical, " + std::to_string(job.skipped) + " skipped, " +
             std::to_string(job.refused) + " refused, " + std::to_string(job.failed) + " failed");
    m_finished_dispatcher();
}

// ═══ Worker plumbing ════════════════════════════════════════════════════════

RomInbox::Callbacks RomOutboxTab::make_callbacks() {
    RomInbox::Callbacks cb;
    cb.progress  = [this](double p, const std::string& m) { push_progress(p, m); };
    cb.log       = [this](const std::string& m) { push_log(m); };
    cb.cancelled = [this] { return m_cancelled.load(); };
    return cb;
}

void RomOutboxTab::push_progress(double pct, const std::string& msg) {
    { std::lock_guard<std::mutex> lk(m_shared_mutex); m_current_message = msg; }
    m_progress_value.store(pct);
    static thread_local auto last = std::chrono::steady_clock::now() - std::chrono::hours(1);
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() >= UI_DISPATCH_INTERVAL_MS) {
        last = now;
        m_progress_dispatcher();
    }
}

void RomOutboxTab::push_log(const std::string& msg) {
    std::cerr << "[OUTBOX] " << msg << std::endl;
    { std::lock_guard<std::mutex> lk(m_shared_mutex); m_log_messages.push_back(msg); }
    m_progress_dispatcher();
}

void RomOutboxTab::log(const std::string& line, ui::LogPanel::Level level) { m_log->append(line, level); }

void RomOutboxTab::on_progress_update() {
    std::string message;
    std::vector<std::string> pending;
    { std::lock_guard<std::mutex> lk(m_shared_mutex); message = m_current_message; pending.swap(m_log_messages); }
    double pct = m_progress_value.load();
    m_progress.set_fraction(std::clamp(pct / 100.0, 0.0, 1.0));
    m_progress.set_text(std::to_string((int)pct) + "%");
    if (!message.empty() && m_busy) m_progress_label.set_text(message);
    for (const auto& l : pending) m_log->append(l, level_of(l));
}

void RomOutboxTab::on_worker_finished() {
    if (m_worker.joinable()) m_worker.join();
    on_progress_update();
    set_busy(false);
    refresh();
    Glib::ustring status = Glib::ustring::compose(_("Moved %1 set(s) into the library"), m_job.moved);
    if (m_job.replaced)  status += Glib::ustring::compose(_(", %1 replaced"), m_job.replaced);
    if (m_job.identical) status += Glib::ustring::compose(_(", %1 identical (discarded)"), m_job.identical);
    if (m_job.skipped)   status += Glib::ustring::compose(_(", %1 skipped"), m_job.skipped);
    if (m_job.refused)   status += Glib::ustring::compose(_(", %1 refused by the DAT check"), m_job.refused);
    if (m_job.failed)    status += Glib::ustring::compose(_(", %1 failed"), m_job.failed);
    status += ".";
    flash(status);
    if (m_job.replaced && m_job.keep_replaced) m_sig_quarantine.emit();
    // One scan per library the move wrote into.
    if (m_job.moved > 0) for (const auto& emulator : m_job.moved_emulators) m_sig_scan.emit(emulator);
}

void RomOutboxTab::set_busy(bool busy) {
    m_busy = busy;
    m_btn_refresh->set_sensitive(!busy);
    m_btn_browse->set_sensitive(!busy);
    m_entry_folder.set_sensitive(!busy);
    m_btn_destinations->set_sensitive(!busy);
    m_btn_select_all->set_sensitive(!busy);
    m_btn_select_none->set_sensitive(!busy);
    m_check_keep_replaced.set_sensitive(!busy);
    m_combo_collision.set_sensitive(!busy);
    if (busy) { m_progress.set_fraction(0.0); m_progress.show(); m_progress_label.show(); m_btn_cancel->show(); }
    else      { m_progress.hide(); m_progress_label.hide(); m_btn_cancel->hide(); }
    update_action_buttons();
}

void RomOutboxTab::flash(const Glib::ustring& text) {
    if (m_busy) return;
    m_status.set_text(text);
    m_flash_timer.disconnect();
    m_flash_timer = Glib::signal_timeout().connect([this] { if (!m_busy) update_summary(); return false; }, 4000);
}
