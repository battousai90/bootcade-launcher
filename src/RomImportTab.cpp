// src/RomImportTab.cpp
#include "RomImportTab.h"

#include "AppContext.h"
#include "ConfirmationDialog.h"
#include "RomArchive.h"
#include "i18n.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
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

std::string file_size_of(const std::string& path) {
    std::error_code ec;
    auto n = fs::file_size(path, ec);
    return ec ? std::string("-") : human_size(n);
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

// The log level a RomInbox message deserves, from the way it starts.
ui::LogPanel::Level level_of(const std::string& m) {
    if (m.rfind("FAILED", 0) == 0 || m.find("could not") != std::string::npos) return ui::LogPanel::Level::Error;
    if (m.find("WARNING:") != std::string::npos) return ui::LogPanel::Level::Warn;
    if (m.rfind("moved", 0) == 0 || m.rfind("rebuilt", 0) == 0) return ui::LogPanel::Level::Ok;
    if (m.rfind("quarantined", 0) == 0 || m.rfind("consumed", 0) == 0) return ui::LogPanel::Level::Warn;
    if (m.rfind("  ", 0) == 0) return ui::LogPanel::Level::Muted;
    return ui::LogPanel::Level::Info;
}

} // namespace

// ═══ Construction ═══════════════════════════════════════════════════════════

RomImportTab::RomImportTab(std::shared_ptr<DatabaseManager> db, PathsProvider paths)
    : Gtk::Box(Gtk::ORIENTATION_VERTICAL, ui::kCardSpacing), m_db(std::move(db)), m_paths(std::move(paths)) {
    get_style_context()->add_class("set-page");

    build_options();
    build_results();
    build_footer();

    m_progress_dispatcher.connect(sigc::mem_fun(*this, &RomImportTab::on_progress_update));
    m_finished_dispatcher.connect(sigc::mem_fun(*this, &RomImportTab::on_worker_finished));

    reload_settings();
    update_summary();
    update_action_buttons();
    show_all_children();
    m_progress.hide();
    m_progress_label.hide();
    m_btn_cancel->hide();
}

RomImportTab::~RomImportTab() {
    if (m_worker.joinable()) {
        m_cancelled = true;
        m_worker.join();
    }
}

void RomImportTab::build_options() {
    // ── Import source ─────────────────────────────────────────────────────
    auto src = ui::card("bc-folder.svg", _("Import source"),
                        _("The folder containing ROMs to analyse and repair."));
    auto* src_body = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 8);
    src_body->set_margin_top(10);
    auto* path_line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    m_entry_inbox.set_hexpand(true);
    m_entry_inbox.set_placeholder_text(_("/path/to/roms/to/sort"));
    m_btn_browse = ui::button(_("Browse…"), "bc-folder.svg");
    m_btn_browse->signal_clicked().connect([this] {
        auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
        Gtk::FileChooserDialog dlg(_("Select the import folder"), Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
        if (top) dlg.set_transient_for(*top);
        dlg.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
        dlg.add_button(_("Select"), Gtk::RESPONSE_OK);
        if (!inbox_path().empty()) dlg.set_filename(inbox_path());
        if (dlg.run() == Gtk::RESPONSE_OK) { m_entry_inbox.set_text(dlg.get_filename()); save_settings(); }
    });
    path_line->pack_start(m_entry_inbox, Gtk::PACK_EXPAND_WIDGET);
    path_line->pack_start(*m_btn_browse, Gtk::PACK_SHRINK);
    src_body->pack_start(*path_line, Gtk::PACK_SHRINK);

    auto* checks = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 4);
    m_check_recursive.set_label(_("Scan subdirectories recursively"));
    m_check_archives.set_label(_("Include archives (zip, 7z, rar)"));
    m_check_loose.set_label(_("Include loose files (uncompressed ROMs)"));
    for (auto* c : {&m_check_recursive, &m_check_archives, &m_check_loose}) checks->pack_start(*c, Gtk::PACK_SHRINK);
    src_body->pack_start(*checks, Gtk::PACK_SHRINK);
    src.body->pack_start(*src_body, Gtk::PACK_SHRINK);
    src.frame->set_size_request(380, -1);
    m_top.pack_start(*src.frame, Gtk::PACK_EXPAND_WIDGET);

    // ── Repair options : each one is a real branch of RomInbox ────────────
    auto rep = ui::card("gear.svg", _("Repair options"),
                        _("How sets are analysed and rebuilt."));
    auto* rep_body = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 6);
    rep_body->set_margin_top(10);
    m_check_use_library.set_label(_("Use existing Library ROMs"));
    m_check_use_library.set_tooltip_text(_("Borrow missing pieces from the archives your library already holds (never modified)."));
    m_check_rebuild_correct.set_label(_("Rebuild already-correct sets too"));
    m_check_rebuild_correct.set_tooltip_text(_("Rewrite even a perfect archive: DAT entry names, deflate, nothing the DAT does not list."));
    rep_body->pack_start(m_check_use_library, Gtk::PACK_SHRINK);
    rep_body->pack_start(m_check_rebuild_correct, Gtk::PACK_SHRINK);

    // How each set is laid out is its DAT's rule (ROM Manager › DAT) : one
    // DAT split, another non-merged, each set follows its own.

    auto* out = ui::sub_label(_("Output: ZIP, loaded by every emulator Bootcade runs; 7z and rar sources are converted."));
    rep_body->pack_start(*out, Gtk::PACK_SHRINK);
    rep.body->pack_start(*rep_body, Gtk::PACK_SHRINK);
    m_top.pack_start(*rep.frame, Gtk::PACK_EXPAND_WIDGET);

    // ── After repair ──────────────────────────────────────────────────────
    auto after = ui::card("bc-file.svg", _("After repair"),
                          _("What happens to the source files once Fix is done."));
    auto* after_body = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 4);
    after_body->set_margin_top(10);
    Gtk::RadioButton::Group group;
    m_radio_subfolder.set_group(group);
    m_radio_delete.set_group(group);
    m_radio_keep.set_group(group);
    m_radio_subfolder.set_label(Glib::ustring::compose(_("Move processed source files to %1/"), RomInbox::kProcessedSubdir));
    m_radio_delete.set_label(_("Delete processed source files"));
    m_radio_delete.get_style_context()->add_class("set-danger");
    m_radio_keep.set_label(_("Keep processed source files in place"));
    m_check_quarantine_rejects.set_label(_("Move unknown, unreadable and duplicate files to Quarantine"));
    for (auto* w : std::vector<Gtk::Widget*>{&m_radio_subfolder, &m_radio_delete, &m_radio_keep})
        after_body->pack_start(*w, Gtk::PACK_SHRINK);
    after_body->pack_start(*ui::hairline(), Gtk::PACK_SHRINK);
    after_body->pack_start(m_check_quarantine_rejects, Gtk::PACK_SHRINK);
    after.body->pack_start(*after_body, Gtk::PACK_SHRINK);
    m_top.pack_start(*after.frame, Gtk::PACK_EXPAND_WIDGET);

    pack_start(m_top, Gtk::PACK_SHRINK);

    // Any change persists : these are the user's habits, not one-shot inputs.
    for (auto* c : {&m_check_recursive, &m_check_archives, &m_check_loose, &m_check_use_library,
                    &m_check_rebuild_correct, &m_check_quarantine_rejects})
        c->signal_toggled().connect([this] { save_settings(); });
    // Whether Fix has anything to do depends on that option too.
    m_check_quarantine_rejects.signal_toggled().connect([this] { if (m_btn_fix) update_action_buttons(); });
    for (auto* r : {&m_radio_subfolder, &m_radio_delete, &m_radio_keep})
        r->signal_toggled().connect([this] { save_settings(); });
    m_entry_inbox.signal_activate().connect([this] { save_settings(); });
}

void RomImportTab::build_results() {
    auto* summary = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    m_pill_valid   = Gtk::make_managed<ui::Pill>(_("Valid"),              ui::PillTone::Ok,      true);
    m_pill_fixable = Gtk::make_managed<ui::Pill>(_("Fixable"),            ui::PillTone::Accent,  true);
    m_pill_missing = Gtk::make_managed<ui::Pill>(_("Missing"),            ui::PillTone::Error,   true);
    m_pill_unknown = Gtk::make_managed<ui::Pill>(_("Unknown"),            ui::PillTone::Warn,    true);
    m_pill_already = Gtk::make_managed<ui::Pill>(_("Already in library"), ui::PillTone::Neutral, true);
    m_pill_ignored = Gtk::make_managed<ui::Pill>(_("Ignored"),            ui::PillTone::Neutral, true);
    m_pill_total   = Gtk::make_managed<ui::Pill>(_("Total"),              ui::PillTone::Neutral);
    for (auto* p : {m_pill_valid, m_pill_fixable, m_pill_missing, m_pill_unknown, m_pill_already, m_pill_ignored, m_pill_total}) {
        p->set_count(0);
        m_pills.pack_start(*p, Gtk::PACK_SHRINK);
    }
    for (auto* p : {m_pill_valid, m_pill_fixable, m_pill_missing, m_pill_unknown, m_pill_already}) p->set_active(true);
    for (auto* p : {m_pill_valid, m_pill_fixable, m_pill_missing, m_pill_unknown, m_pill_already, m_pill_ignored})
        p->signal_toggled().connect([this](bool) { refilter(); });
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

    m_filter = Gtk::make_managed<ui::FilterBar>(_("Search (game, file, rom, crc…)"));
    m_system_combo = m_filter->add_combo(_("System:"));
    m_system_combo->append(_("All"));
    m_system_combo->set_active(0);
    m_filter->signal_changed().connect(sigc::mem_fun(*this, &RomImportTab::refilter));
    pack_start(*m_filter, Gtk::PACK_SHRINK);

    m_store = Gtk::ListStore::create(m_cols);
    m_models.sort_column = m_cols.game.index();
    m_models.sort_order  = Gtk::SORT_ASCENDING;

    m_table = Gtk::make_managed<ui::Table>(Gtk::SELECTION_MULTIPLE);
    m_models.attach(m_table->view(), m_store, sigc::mem_fun(*this, &RomImportTab::row_visible));
    m_table->add_check_column(m_cols.include, sigc::mem_fun(*this, &RomImportTab::on_row_toggled));
    {
        auto* renderer = Gtk::make_managed<Gtk::CellRendererText>();
        auto* col = Gtk::make_managed<Gtk::TreeViewColumn>(_("Status"), *renderer);
        col->add_attribute(renderer->property_text(), m_cols.status);
        col->set_sort_column(m_cols.status_key);
        col->set_cell_data_func(*renderer, [this, renderer](Gtk::CellRenderer*, const Gtk::TreeModel::iterator& it) {
            ensure_colours();
            if (!m_colours.ready) return;
            const Glib::ustring key = (*it)[m_cols.status_key];
            Gdk::RGBA c = m_colours.muted;
            if (key == "valid")        c = m_colours.ok;
            else if (key == "fixable") c = m_colours.accent;
            else if (key == "missing") c = m_colours.err;
            else if (key == "unknown") c = m_colours.warn;
            renderer->property_foreground_rgba() = c;
            renderer->property_weight() = (key == "already" || key == "ignored") ? Pango::WEIGHT_NORMAL : Pango::WEIGHT_BOLD;
        });
        m_table->view().append_column(*col);
    }
    { ui::ColumnOptions o; o.expand = true; o.min_width = 200; m_table->add_text_column(_("Game / ROM"), m_cols.game, o); }
    m_table->add_text_column(_("System"), m_cols.system);
    { ui::ColumnOptions o; o.mono = true; o.expand = true; m_table->add_text_column(_("File name"), m_cols.file, o); }
    { ui::ColumnOptions o; o.xalign = 1.0f; m_table->add_text_column(_("Size"), m_cols.size, o); }
    m_table->add_text_column(_("Repair source"), m_cols.source);
    { ui::ColumnOptions o; o.expand = true; o.sortable = false; m_table->add_text_column(_("Details"), m_cols.details, o); }
    m_table->view().get_selection()->signal_changed().connect(sigc::mem_fun(*this, &RomImportTab::on_selection_changed));
    m_table->signal_context_menu().connect(sigc::mem_fun(*this, &RomImportTab::on_context_menu));
    m_table->set_size_request(-1, 140);   // never less than a few rows

    // Detail and log side by side under the table, a grip between the two
    // rows to trade height : the log is worth reading tall after a Fix.
    auto* bottom = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, ui::kCardSpacing);
    bottom->set_homogeneous(true);
    m_detail = Gtk::make_managed<ui::DetailPanel>("bc-file.svg", _("Selected set"),
                                                  _("Every piece of the selected set, and where it comes from."), 110);
    m_detail->show_placeholder(_("Select a set to see its pieces."));
    bottom->pack_start(*m_detail, Gtk::PACK_EXPAND_WIDGET);
    m_log = Gtk::make_managed<ui::LogPanel>(_("Import log"), _("Details of the analysis and repair process."));
    m_log->set_size_request(-1, 110);
    bottom->pack_start(*m_log, Gtk::PACK_EXPAND_WIDGET);
    pack_start(*ui::splitter(*m_table, *bottom, 230), Gtk::PACK_EXPAND_WIDGET);
}

void RomImportTab::build_footer() {
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

    m_btn_export = Gtk::make_managed<Gtk::MenuButton>();
    auto* export_face = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    export_face->pack_start(*ui::image("bc-save.svg", ui::kIconButton), Gtk::PACK_SHRINK);
    export_face->pack_start(*Gtk::make_managed<Gtk::Label>(_("Export missing list…")), Gtk::PACK_SHRINK);
    export_face->pack_start(*ui::image("bc-chevron-down.svg", 14), Gtk::PACK_SHRINK);
    m_btn_export->add(*export_face);
    struct Fmt { const char* label; int id; };
    for (Fmt f : {Fmt{N_("Text list"), 0}, Fmt{N_("CSV (spreadsheet)"), 1}, Fmt{N_("Missing sets as DAT"), 2}}) {
        auto* item = Gtk::make_managed<Gtk::MenuItem>(_(f.label));
        int id = f.id;
        item->signal_activate().connect([this, id] { on_export(id); });
        m_export_menu.append(*item);
    }
    m_export_menu.show_all();
    m_btn_export->set_popup(m_export_menu);
    m_footer.pack_start(*m_btn_export, Gtk::PACK_SHRINK);

    // Fix is the primary action : the checked sets, or every fixable one
    // when none is checked, plus the housekeeping the options ask for.
    m_btn_fix = ui::button(_("Fix"), "bc-check.svg", ui::Tone::Accent);
    m_btn_fix->set_tooltip_text(_("Write the valid and fixable sets into the outbox, then tidy the import folder : "
                                  "sources fully used are processed as chosen, unknown, unreadable and duplicate files "
                                  "go to quarantine when the option is on. Acts on the checked sets, or on every fixable "
                                  "set when none is checked."));
    m_btn_fix->signal_clicked().connect([this] { on_fix_clicked(); });
    m_btn_analyze = ui::button(_("Analyse"), "bc-search.svg");
    m_btn_analyze->signal_clicked().connect(sigc::mem_fun(*this, &RomImportTab::on_analyze_clicked));
    m_footer.pack_end(*m_btn_fix, Gtk::PACK_SHRINK);
    m_footer.pack_end(*m_btn_analyze, Gtk::PACK_SHRINK);
    pack_start(m_footer, Gtk::PACK_SHRINK);
}

void RomImportTab::ensure_colours() {
    if (m_colours.ready || !get_toplevel() || !get_toplevel()->get_realized()) return;
    m_colours.ok     = ui::probe_color(*this, "set-ok");
    m_colours.warn   = ui::probe_color(*this, "set-warn");
    m_colours.err    = ui::probe_color(*this, "set-err");
    m_colours.muted  = ui::probe_color(*this, "set-sub");
    m_colours.accent = ui::probe_color(*this, "set-accent-text");
    m_colours.ready  = true;
}

// ═══ Settings ═══════════════════════════════════════════════════════════════

void RomImportTab::reload_settings() {
    nlohmann::json j;
    std::ifstream fi(AppContext::get_config_path());
    if (fi) { try { fi >> j; } catch (...) { j = nlohmann::json{}; } }
    nlohmann::json rm = (j.contains("rom_manager") && j["rom_manager"].is_object()) ? j["rom_manager"] : nlohmann::json::object();
    auto str  = [&](const char* k, const std::string& d) { return (rm.contains(k) && rm[k].is_string())  ? rm[k].get<std::string>() : d; };
    auto flag = [&](const char* k, bool d)               { return (rm.contains(k) && rm[k].is_boolean()) ? rm[k].get<bool>() : d; };

    m_entry_inbox.set_text(str("inbox_path", ""));
    m_check_recursive.set_active(flag("inbox_recursive", true));
    m_check_archives.set_active(flag("import_include_archives", true));
    m_check_loose.set_active(flag("import_include_loose", true));
    m_check_use_library.set_active(flag("import_use_library", true));
    m_check_rebuild_correct.set_active(flag("import_rebuild_correct", false));
    std::string processed = str("import_processed", "subfolder");
    if (processed == "delete")    m_radio_delete.set_active(true);
    else if (processed == "keep") m_radio_keep.set_active(true);
    else                          m_radio_subfolder.set_active(true);
    m_check_quarantine_rejects.set_active(flag("import_quarantine_rejects", true));
}

void RomImportTab::save_settings() const {
    // Read-modify-write, like every other config.json writer in the app.
    nlohmann::json j;
    const std::string path = AppContext::get_config_path();
    { std::ifstream fi(path); if (fi) { try { fi >> j; } catch (...) { j = nlohmann::json{}; } } }
    auto& rm = j["rom_manager"];
    rm["inbox_path"]                = inbox_path();
    rm["inbox_recursive"]           = m_check_recursive.get_active();
    rm["import_include_archives"]   = m_check_archives.get_active();
    rm["import_include_loose"]      = m_check_loose.get_active();
    rm["import_use_library"]        = m_check_use_library.get_active();
    rm["import_rebuild_correct"]    = m_check_rebuild_correct.get_active();
    rm.erase("import_style");   // the merge mode is each DAT's now
    rm["import_processed"]          = m_radio_delete.get_active() ? "delete" : m_radio_keep.get_active() ? "keep" : "subfolder";
    rm["import_quarantine_rejects"] = m_check_quarantine_rejects.get_active();
    std::ofstream fo(path);
    if (fo) fo << j.dump(4);
}

RomInbox::Options RomImportTab::options_from_ui() const {
    RomInbox::Options o;
    o.recursive        = m_check_recursive.get_active();
    o.include_archives = m_check_archives.get_active();
    o.include_loose    = m_check_loose.get_active();
    o.use_library      = m_check_use_library.get_active();
    o.rebuild_correct  = m_check_rebuild_correct.get_active();
    Paths p = m_paths();
    o.processed = m_radio_delete.get_active() ? RomInbox::Options::Processed::Delete
                : m_radio_keep.get_active()   ? RomInbox::Options::Processed::Keep
                                              : RomInbox::Options::Processed::Subfolder;
    o.quarantine_rejects = m_check_quarantine_rejects.get_active();
    o.quarantine_dir     = p.quarantine;
    o.roms_paths         = p.roms_paths;
    o.emulator           = p.emulator;
    return o;
}

// ═══ Analyse / Fix ══════════════════════════════════════════════════════════

void RomImportTab::on_analyze_clicked() {
    if (m_busy) return;
    auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
    const std::string inbox = inbox_path();
    Paths p = m_paths();
    std::error_code ec;
    if (inbox.empty() || !fs::is_directory(inbox, ec)) {
        if (top) ui::notice(*top, _("No import folder"), _("Select a valid import folder first."));
        return;
    }
    if (p.outbox.empty()) {
        if (top) ui::notice(*top, _("No outbox folder"), _("Set an outbox directory first (Outbox tab)."));
        return;
    }
    if (fs::exists(p.outbox, ec) && fs::equivalent(inbox, p.outbox, ec)) {
        if (top) ui::notice(*top, _("Same folder"), _("The import folder and the outbox must be two different folders."));
        return;
    }
    save_settings();
    m_log->clear();
    m_models.detach(m_table->view());   // nothing attached while filling : see SettingsUi::ModelStack
    m_store->clear();
    refilter();
    m_report = RomInbox::Report{};
    m_cancelled = false;
    m_job_inbox   = inbox;
    m_job_outbox  = p.outbox;
    m_job_options = options_from_ui();
    m_job = Job::Analyze;
    set_busy(true);
    m_worker = std::thread(&RomImportTab::worker_analyze, this);
}

void RomImportTab::worker_analyze() {
    RomInbox::Callbacks cb = make_callbacks();
    RomInbox::Report rep = RomInbox::analyze(m_job_inbox, m_job_outbox, m_db, m_job_options, cb);
    {
        std::lock_guard<std::mutex> lk(m_shared_mutex);
        m_report = std::move(rep);
    }
    m_finished_dispatcher();
}

// Counted the way RomInbox::apply() will act : unrecognized and unreadable
// files, plus archives that triggered nothing but "already in library".
RomImportTab::Housekeeping RomImportTab::housekeeping() const {
    Housekeeping h;
    h.enabled = m_check_quarantine_rejects.get_active() && !m_paths().quarantine.empty();
    h.unknown = (int)(m_report.unrecognized.size() + m_report.unsupported.size());
    std::set<std::string> duplicates(m_report.already_have.begin(), m_report.already_have.end());
    struct Seen { bool already = false, useful = false; };
    std::map<std::string, Seen> by_trigger;
    for (const auto& s : m_report.sets) {
        Seen& v = by_trigger[s.trigger_archive];
        if (s.action == RomInbox::Action::AlreadyInLibrary) v.already = true;
        if (s.action == RomInbox::Action::Move || s.action == RomInbox::Action::Rebuild) v.useful = true;
    }
    for (const auto& [archive, v] : by_trigger) if (v.already && !v.useful) duplicates.insert(archive);
    h.duplicates = (int)duplicates.size();
    return h;
}

void RomImportTab::on_fix_clicked() {
    if (m_busy) return;
    // The checked sets ; or, when none is checked, every set that can be
    // written : what is shown is what gets done.
    int checked = 0, fixable = 0;
    for (const auto& s : m_report.sets) {
        bool actionable = s.action == RomInbox::Action::Move || s.action == RomInbox::Action::Rebuild;
        if (!actionable) continue;
        ++fixable;
        if (s.selected) ++checked;
    }
    if (checked == 0 && fixable > 0) {
        for (auto& s : m_report.sets)
            s.selected = s.action == RomInbox::Action::Move || s.action == RomInbox::Action::Rebuild;
        for (auto& row : m_store->children())
            if (row[m_cols.actionable]) row[m_cols.include] = true;
    }
    const int selected = checked ? checked : fixable;
    const Housekeeping h = housekeeping();
    const int rejects = h.enabled ? h.unknown + h.duplicates : 0;
    if (selected == 0 && rejects == 0) {
        flash(h.unknown + h.duplicates > 0
              ? _("Nothing to write, and the unknown and duplicate files stay : tick \"Move … to Quarantine\" and set a quarantine folder to tidy them.")
              : _("Nothing to fix : analyse the import folder first."));
        return;
    }

    auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
    const Glib::ustring processed = m_radio_delete.get_active() ? Glib::ustring(_("deleted"))
                                  : m_radio_keep.get_active()   ? Glib::ustring(_("kept in place"))
                                  : Glib::ustring::compose(_("moved to %1/"), RomInbox::kProcessedSubdir);
    Glib::ustring what;
    if (selected)
        what += Glib::ustring::compose(_("%1 set(s) → written into the outbox (%2), source files fully used are %3\n"),
                                       selected, m_job_outbox, processed);
    else
        what += _("No set to write.\n");
    if (h.enabled) {
        if (h.unknown)    what += Glib::ustring::compose(_("%1 unknown or unreadable file(s) → quarantine\n"), h.unknown);
        if (h.duplicates) what += Glib::ustring::compose(_("%1 duplicate(s) the library already holds, complete → quarantine\n"), h.duplicates);
    } else if (h.unknown + h.duplicates) {
        what += Glib::ustring::compose(_("%1 unknown or duplicate file(s) are left in the import folder (the quarantine option is off, or no quarantine folder is set)\n"),
                                       h.unknown + h.duplicates);
    }
    what += _("\nThe library itself is never modified : Outbox › Move to library does that.");
    if (top) {
        ConfirmationDialog confirm(*top, _("Fix these items?"), what, "bc-check.svg");
        if (!confirm.show_and_confirm()) return;
    }
    // The plan carries the options it was analysed with; only the after-repair
    // choices may have changed since, and those are read now.
    RomInbox::Options now = options_from_ui();
    m_report.options.processed          = now.processed;
    m_report.options.quarantine_rejects = now.quarantine_rejects;
    m_report.options.quarantine_dir     = now.quarantine_dir;
    m_cancelled = false;
    m_job = Job::Apply;
    set_busy(true);
    m_worker = std::thread(&RomImportTab::worker_apply, this);
}

void RomImportTab::worker_apply() {
    RomInbox::Callbacks cb = make_callbacks();
    RomInbox::ApplyResult res = RomInbox::apply(m_report, cb);
    {
        std::lock_guard<std::mutex> lk(m_shared_mutex);
        m_apply_result = std::move(res);
    }
    m_finished_dispatcher();
}

// ═══ Table ══════════════════════════════════════════════════════════════════

void RomImportTab::populate() {
    m_models.detach(m_table->view());   // nothing attached while filling : see SettingsUi::ModelStack
    m_store->clear();
    std::set<std::string> systems;

    for (size_t i = 0; i < m_report.sets.size(); ++i) {
        const auto& s = m_report.sets[i];
        systems.insert(s.system);
        auto row = *(m_store->append());
        const bool actionable = s.action == RomInbox::Action::Move || s.action == RomInbox::Action::Rebuild;

        const char* key; const char* label;
        switch (s.action) {
            case RomInbox::Action::Move:             key = "valid";   label = N_("Valid");   break;
            case RomInbox::Action::Rebuild:          key = "fixable"; label = N_("Fixable"); break;
            case RomInbox::Action::AlreadyInLibrary: key = "already"; label = N_("Already in library"); break;
            default:                                 key = "missing"; label = N_("Missing"); break;
        }

        int from_import = 0, from_library = 0;
        for (const auto& p : s.pieces) {
            if (!p.resolved) continue;
            p.src.from_inbox ? ++from_import : ++from_library;
        }
        Glib::ustring source = from_library == 0 ? Glib::ustring(_("Import"))
                             : from_import == 0  ? Glib::ustring(_("Library"))
                                                 : Glib::ustring(_("Import + Library"));
        if (s.action == RomInbox::Action::AlreadyInLibrary) source = "";

        std::vector<std::string> bits;
        switch (s.action) {
            case RomInbox::Action::Move:    bits.push_back(_("matches the DAT, relocated as-is")); break;
            case RomInbox::Action::Rebuild:
                if (from_library) bits.push_back(Glib::ustring::compose(_("%1 piece(s) from the library"), from_library).raw());
                if (s.renamed_entries) bits.push_back(Glib::ustring::compose(_("%1 renamed"), s.renamed_entries).raw());
                if (!s.extra_entries.empty()) bits.push_back(Glib::ustring::compose(_("%1 extra entry(ies) left out"), (int)s.extra_entries.size()).raw());
                if (!RomArchive::is_zip(s.trigger_archive)) bits.push_back(_("converted to ZIP"));
                if (bits.empty()) bits.push_back(_("rebuilt to the DAT layout"));
                break;
            case RomInbox::Action::AlreadyInLibrary: bits.push_back(_("the library already holds this set, complete")); break;
            default:
                bits.push_back(Glib::ustring::compose(_("%1 ROM(s) missing"), (int)s.missing.size()).raw());
                if (!s.missing.empty()) bits.push_back(s.missing.front().name + (s.missing.size() > 1 ? ", …" : ""));
                break;
        }

        row[m_cols.include]    = actionable && s.selected;
        row[m_cols.actionable] = actionable;
        row[m_cols.status]     = _(label);
        row[m_cols.status_key] = key;
        row[m_cols.game]       = s.description.empty() ? s.game_name : s.game_name + "  (" + s.description + ")";
        row[m_cols.system]     = s.system;
        row[m_cols.file]       = fs::path(s.trigger_archive).filename().string();
        row[m_cols.size]       = file_size_of(s.trigger_archive);
        row[m_cols.source]     = source;
        row[m_cols.details]    = join(bits, ", ", 4);
        row[m_cols.kind]       = KIND_SET;
        row[m_cols.index]      = (unsigned int)i;
        std::string blob = lower(s.game_name + ' ' + s.description + ' ' + s.system + ' ' + fs::path(s.trigger_archive).filename().string());
        for (const auto& p : s.pieces) blob += ' ' + lower(p.target_name) + ' ' + crc_hex(p.crc) + ' ' + lower(p.src.entry);
        for (const auto& m : s.missing) blob += ' ' + lower(m.name) + ' ' + crc_hex(m.crc);
        row[m_cols.search_blob] = blob;
    }

    auto add_file_row = [&](const std::string& path, int kind, const char* key, const Glib::ustring& label,
                            const Glib::ustring& details, unsigned int index) {
        auto row = *(m_store->append());
        std::string base = fs::path(path).filename().string();
        row[m_cols.include]    = false;
        row[m_cols.actionable] = false;
        row[m_cols.status]     = label;
        row[m_cols.status_key] = key;
        row[m_cols.game]       = base;
        row[m_cols.system]     = "";
        row[m_cols.file]       = base;
        row[m_cols.size]       = file_size_of(path);
        row[m_cols.source]     = "";
        row[m_cols.details]    = details;
        row[m_cols.kind]       = kind;
        row[m_cols.index]      = index;
        row[m_cols.search_blob] = lower(base);
    };
    for (size_t i = 0; i < m_report.unrecognized.size(); ++i) {
        // The CRC is the whole diagnosis of an unknown file : with it the
        // player can search the DATs, a forum or a wiki ; without it "not in
        // DAT" is a dead end.
        Glib::ustring details = _("Not in DAT (neither by name nor by content)");
        const std::string crcs = i < m_report.unrecognized_crcs.size() ? m_report.unrecognized_crcs[i] : std::string();
        if (!crcs.empty()) details += "  ·  CRC " + crcs;
        add_file_row(m_report.unrecognized[i], KIND_UNKNOWN, "unknown", _("Unknown"), details, (unsigned)i);
        if (!crcs.empty()) {
            auto last = m_store->children().end(); --last;
            (*last)[m_cols.search_blob] = Glib::ustring((*last)[m_cols.search_blob]) + " " + crcs;
        }
    }
    for (size_t i = 0; i < m_report.unsupported.size(); ++i)
        add_file_row(m_report.unsupported[i], KIND_UNSUPPORTED, "unknown", _("Unreadable"), _("Corrupt archive, or a format no reader could open"), (unsigned)i);
    for (size_t i = 0; i < m_report.already_have.size(); ++i)
        add_file_row(m_report.already_have[i], KIND_DUPLICATE, "already", _("Already in library"), _("Every set in this archive is already in the library, complete"), (unsigned)i);
    for (size_t i = 0; i < m_report.ignored.size(); ++i)
        add_file_row(m_report.ignored[i], KIND_IGNORED, "ignored", _("Ignored"), _("Not a ROM file (readme, cue sheet, image…)"), (unsigned)i);

    m_filter->set_combo_items(m_system_combo, _("All"), systems, m_system_combo->get_active_text());
    refilter();
    update_summary();
    update_action_buttons();
    m_detail->set_title(_("Selected set"));
    m_detail->set_subtitle(_("Every piece of the selected set, and where it comes from."));
    m_detail->show_placeholder(_("Select a set to see its pieces."));
}

void RomImportTab::update_summary() {
    int valid = 0, fixable = 0, missing = 0, already = (int)m_report.already_have.size();
    for (const auto& s : m_report.sets) {
        switch (s.action) {
            case RomInbox::Action::Move:             ++valid;   break;
            case RomInbox::Action::Rebuild:          ++fixable; break;
            case RomInbox::Action::AlreadyInLibrary: ++already; break;
            default:                                 ++missing; break;
        }
    }
    int unknown = (int)(m_report.unrecognized.size() + m_report.unsupported.size());
    int ignored = (int)m_report.ignored.size();
    m_pill_valid->set_count(valid);
    m_pill_fixable->set_count(fixable);
    m_pill_missing->set_count(missing);
    m_pill_unknown->set_count(unknown);
    m_pill_already->set_count(already);
    m_pill_ignored->set_count(ignored);
    m_pill_total->set_count(valid + fixable + missing + already + unknown + ignored);

    if (m_report.sets.empty() && m_report.unrecognized.empty() && m_report.unsupported.empty()) {
        m_status.set_text(_("Analyse the import folder to see what it holds."));
    } else if (m_report.library_pool_empty) {
        m_status.set_text(_("The library index is empty : run a ROM scan first, or sets that could be rebuilt will look incomplete."));
    } else {
        m_status.set_text(Glib::ustring::compose(_("%1 valid, %2 fixable, %3 incomplete · each set laid out as its DAT says"),
                                                 valid, fixable, missing));
    }
}

bool RomImportTab::row_visible(const Gtk::TreeModel::const_iterator& it) const {
    const Gtk::TreeModel::Row row = *it;
    const Glib::ustring key = row[m_cols.status_key];
    bool wanted = (key == "valid"   && m_pill_valid->active())
               || (key == "fixable" && m_pill_fixable->active())
               || (key == "missing" && m_pill_missing->active())
               || (key == "unknown" && m_pill_unknown->active())
               || (key == "already" && m_pill_already->active())
               || (key == "ignored" && m_pill_ignored->active());
    if (!wanted) return false;
    if (!m_vis_system.empty() && row[m_cols.system] != m_vis_system) return false;
    const std::string& needle = m_vis_needle;
    if (!needle.empty()) {
        const Glib::ustring blob = row[m_cols.search_blob];
        if (blob.raw().find(needle) == std::string::npos) return false;
    }
    return true;
}

void RomImportTab::refilter() {
    // Rebuilt, not refiltered : see SettingsUi::ModelStack. The filter's
    // inputs are read once here, not once per row inside row_visible.
    m_vis_system = m_system_combo->get_active_row_number() > 0 ? m_system_combo->get_active_text() : Glib::ustring();
    m_vis_needle = lower(m_filter->search_text());
    m_models.detach(m_table->view());
    m_models.attach(m_table->view(), m_store, sigc::mem_fun(*this, &RomImportTab::row_visible));
    m_filter->set_summary(Glib::ustring::compose(_("%1 result(s)"), m_models.visible_count()));
}

Gtk::TreeModel::Row RomImportTab::source_row(const Gtk::TreeModel::Path& sorted_path) const {
    auto child = m_models.filter->convert_path_to_child_path(m_models.sort->convert_path_to_child_path(sorted_path));
    return *m_store->get_iter(child);
}

namespace {
struct PieceCols : public Gtk::TreeModel::ColumnRecord {
    Gtk::TreeModelColumn<Glib::ustring> action, expected, found_as, crc, size, source;
    PieceCols() { add(action); add(expected); add(found_as); add(crc); add(size); add(source); }
};
}

void RomImportTab::on_selection_changed() {
    auto rows = m_table->view().get_selection()->get_selected_rows();
    if (rows.empty()) { m_detail->show_placeholder(_("Select a set to see its pieces.")); return; }
    Gtk::TreeModel::Row row = source_row(rows.front());
    if ((int)row[m_cols.kind] != KIND_SET) {
        m_detail->set_title(Glib::ustring(row[m_cols.file]));
        m_detail->set_subtitle(Glib::ustring(row[m_cols.details]));
        m_detail->show_placeholder(_("Nothing to rebuild from this file."));
        return;
    }
    const auto& s = m_report.sets[(unsigned int)row[m_cols.index]];
    m_detail->set_title(s.description.empty() ? s.game_name : s.game_name + "  —  " + s.description);
    m_detail->set_subtitle(s.system + "  ·  " + fs::path(s.trigger_archive).filename().string() + "  →  "
                           + fs::path(s.dest_path).parent_path().filename().string() + "/" + fs::path(s.dest_path).filename().string());

    static PieceCols cols;
    auto store = Gtk::ListStore::create(cols);
    for (const auto& p : s.pieces) {
        auto rr = *(store->append());
        Glib::ustring action, source;
        if (!p.resolved) {
            action = _("Missing");
            source = _("found nowhere");
        } else {
            std::string where = fs::path(p.src.container).filename().string() + (p.src.from_inbox ? _(" (import)") : _(" (library)"));
            if (p.src.container == s.trigger_archive && p.src.entry == p.target_name) action = _("Keep");
            else if (p.src.container == s.trigger_archive)                            action = _("Rename");
            else                                                                       action = p.src.from_inbox ? _("From import") : _("From library");
            source = where;
        }
        rr[cols.action]   = action;
        rr[cols.expected] = p.target_name;
        rr[cols.found_as] = (p.resolved && p.src.entry != p.target_name) ? Glib::ustring(p.src.entry) : Glib::ustring();
        rr[cols.crc]      = p.sha1.empty() ? crc_hex(p.crc) : p.sha1;
        rr[cols.size]     = human_size(p.size);
        rr[cols.source]   = source;
    }
    for (const auto& x : s.extra_entries) {
        auto rr = *(store->append());
        // Same name as a missing piece : the player HAS the file they were
        // after, in another build. That is a mismatch to fix, not a surplus,
        // and both numbers are what lets them find the right one.
        const RomInbox::PiecePlan* wanted = nullptr;
        for (const auto& p : s.pieces)
            if (!p.resolved && lower(p.target_name) == lower(x.name)) { wanted = &p; break; }
        // Another name, but the size and extension of a missing piece : the
        // same kind of file, in another build ("game.ngp" next to the DAT's
        // long name). Said as a probability, since two files of one size are
        // not proof, but far more useful than "not needed".
        const RomInbox::PiecePlan* likely = nullptr;
        if (!wanted)
            for (const auto& p : s.pieces)
                if (!p.resolved && p.size == x.size && x.size > 0
                    && lower(fs::path(p.target_name).extension().string()) == lower(fs::path(x.name).extension().string())) { likely = &p; break; }
        rr[cols.found_as] = x.name;
        rr[cols.crc]      = crc_hex(x.crc);
        rr[cols.size]     = human_size(x.size);
        if (wanted) {
            rr[cols.action]   = _("Wrong CRC");
            rr[cols.expected] = wanted->target_name;
            rr[cols.source]   = Glib::ustring::compose(_("expected %1 : another build of the same file"), crc_hex(wanted->crc));
        } else if (likely) {
            rr[cols.action]   = _("Wrong CRC");
            rr[cols.expected] = likely->target_name;
            rr[cols.source]   = Glib::ustring::compose(_("expected %1 : same size, other content, probably another build"), crc_hex(likely->crc));
        } else {
            rr[cols.action]   = _("Left out");
            rr[cols.source]   = _("not needed by the DAT");
        }
    }
    auto* t = Gtk::make_managed<ui::Table>(Gtk::SELECTION_SINGLE);
    t->view().set_model(store);
    {
        auto* renderer = Gtk::make_managed<Gtk::CellRendererText>();
        auto* col = Gtk::make_managed<Gtk::TreeViewColumn>(_("Action"), *renderer);
        col->add_attribute(renderer->property_text(), cols.action);
        col->set_cell_data_func(*renderer, [this, renderer](Gtk::CellRenderer*, const Gtk::TreeModel::iterator& it) {
            ensure_colours();
            if (!m_colours.ready) return;
            static PieceCols c;
            const Glib::ustring a = (*it)[c.action];
            Gdk::RGBA colour = m_colours.muted;
            if (a == _("Keep")) colour = m_colours.ok;
            else if (a == _("Missing") || a == _("Wrong CRC")) colour = m_colours.err;
            else if (a == _("Rename") || a == _("From import") || a == _("From library")) colour = m_colours.accent;
            renderer->property_foreground_rgba() = colour;
        });
        t->view().append_column(*col);
    }
    { ui::ColumnOptions o; o.mono = true; o.expand = true; t->add_text_column(_("Expected name"), cols.expected, o); }
    { ui::ColumnOptions o; o.mono = true; o.expand = true; t->add_text_column(_("Found as"), cols.found_as, o); }
    { ui::ColumnOptions o; o.mono = true; t->add_text_column(s.disk ? _("SHA1") : _("CRC"), cols.crc, o); }
    { ui::ColumnOptions o; o.xalign = 1.0f; t->add_text_column(_("Size"), cols.size, o); }
    { ui::ColumnOptions o; o.expand = true; t->add_text_column(_("Source"), cols.source, o); }
    m_detail->set_content(t);
}

// ═══ Checkboxes, context menu, actions ══════════════════════════════════════

void RomImportTab::on_row_toggled(const Glib::ustring& path) {
    Gtk::TreeModel::Row row = source_row(Gtk::TreeModel::Path(path));
    if (!row[m_cols.actionable]) return;
    bool on = !row[m_cols.include];
    row[m_cols.include] = on;
    m_report.sets[(unsigned int)row[m_cols.index]].selected = on;
    update_action_buttons();
}

void RomImportTab::set_all_checked(bool on) {
    for (const auto& frow : m_models.filter->children()) {
        Gtk::TreeModel::Row row = *m_models.filter->convert_iter_to_child_iter(frow);
        if (!row[m_cols.actionable]) continue;
        row[m_cols.include] = on;
        m_report.sets[(unsigned int)row[m_cols.index]].selected = on;
    }
    update_action_buttons();
}

void RomImportTab::update_action_buttons() {
    int selected = 0, fixable = 0;
    for (const auto& s : m_report.sets) {
        bool actionable = s.action == RomInbox::Action::Move || s.action == RomInbox::Action::Rebuild;
        if (!actionable) continue;
        ++fixable;
        if (s.selected) ++selected;
    }
    const Housekeeping h = housekeeping();
    const int rejects = h.enabled ? h.unknown + h.duplicates : 0;
    const int n = selected ? selected : fixable;
    m_btn_fix->set_label(selected ? Glib::ustring::compose(_("Fix selected (%1)"), selected)
                       : fixable  ? Glib::ustring::compose(_("Fix all (%1)"), fixable)
                                  : Glib::ustring(_("Fix")));
    m_btn_fix->set_sensitive(!m_busy && (n > 0 || rejects > 0));
    m_btn_export->set_sensitive(!m_busy && !m_report.sets.empty());
}

void RomImportTab::on_context_menu(const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn*, GdkEventButton* event) {
    Gtk::TreeModel::Row row = source_row(path);
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
    const Glib::ustring file = row[m_cols.file];
    if ((int)row[m_cols.kind] == KIND_SET) {
        const auto& s = m_report.sets[(unsigned int)row[m_cols.index]];
        std::vector<std::string> missing;
        for (const auto& m : s.missing) missing.push_back(m.name);
        add(_("Copy game name"),         [copy, s] { copy(s.game_name, _("the game name")); });
        add(_("Copy file name"),         [copy, file] { copy(file, _("the file name")); });
        add(_("Copy missing filenames"), [copy, missing] { copy(join(missing, "\n"), _("the missing filenames")); }, !missing.empty());
        add(_("Copy destination path"),  [copy, s] { copy(s.dest_path, _("the destination path")); });
        m_context_menu.append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());
        add(_("Search on web"), [this, s] {
            std::string uri = "https://duckduckgo.com/?q=" + Glib::uri_escape_string(s.game_name + " " + s.system + " rom");
            try { Gio::AppInfo::launch_default_for_uri(uri); } catch (const Glib::Error& e) { flash(e.what()); }
        });
    } else {
        add(_("Copy file name"), [copy, file] { copy(file, _("the file name")); });
        add(_("Search on web"), [this, file] {
            std::string uri = "https://duckduckgo.com/?q=" + Glib::uri_escape_string(fs::path(file.raw()).stem().string() + " rom");
            try { Gio::AppInfo::launch_default_for_uri(uri); } catch (const Glib::Error& e) { flash(e.what()); }
        });
    }
    m_context_menu.show_all();
    m_context_menu.popup_at_pointer((GdkEvent*)event);
}

void RomImportTab::receive(const std::vector<std::string>& archives) {
    if (m_busy) return;
    auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
    const std::string inbox = inbox_path();
    std::error_code ec;
    if (inbox.empty()) {
        if (top) ui::notice(*top, _("No import folder"), _("Set an import folder first."));
        return;
    }
    fs::create_directories(inbox, ec);
    int copied = 0, skipped = 0;
    for (const auto& a : archives) {
        fs::path src(a);
        fs::path dest = fs::path(inbox) / src.filename();
        if (fs::exists(dest, ec)) { ++skipped; continue; }
        fs::copy_file(src, dest, ec);
        if (ec) { log("could not copy " + a + ": " + ec.message(), ui::LogPanel::Level::Error); ec.clear(); ++skipped; }
        else    { ++copied; log("from library: " + src.filename().string() + " → import folder"); }
    }
    if (copied) flash(Glib::ustring::compose(_("%1 set(s) copied from the library into the import folder (%2 already there)."), copied, skipped));
    else        flash(Glib::ustring::compose(_("%1 set(s) from the library are in the import folder : analysing."), skipped));
    on_analyze_clicked();
}

// ═══ Export ═════════════════════════════════════════════════════════════════

void RomImportTab::on_export(int format) {
    if (m_report.sets.empty()) { flash(_("Analyse first.")); return; }
    auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
    Gtk::FileChooserDialog dlg(_("Export missing list"), Gtk::FILE_CHOOSER_ACTION_SAVE);
    if (top) dlg.set_transient_for(*top);
    dlg.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dlg.add_button(_("Save"),   Gtk::RESPONSE_OK);
    dlg.set_current_name(format == 0 ? "missing-roms.txt" : format == 1 ? "missing-roms.csv" : "missing-sets.dat");
    dlg.set_do_overwrite_confirmation(true);
    if (dlg.run() != Gtk::RESPONSE_OK) return;
    std::ofstream out(dlg.get_filename());
    if (!out) { if (top) ui::notice(*top, _("Could not write the file."), dlg.get_filename()); return; }

    std::vector<const RomInbox::SetPlan*> incomplete;
    for (const auto& s : m_report.sets) if (s.action == RomInbox::Action::Incomplete) incomplete.push_back(&s);

    if (format == 0) {
        out << "# Missing ROMs : Bootcade\n# import folder: " << m_job_inbox << "\n";
        std::string current_system;
        for (const auto* s : incomplete) {
            if (s->system != current_system) { current_system = s->system; out << "\n[" << current_system << "]\n"; }
            out << "  " << s->game_name;
            if (!s->description.empty()) out << "  (" << s->description << ")";
            out << "\n";
            for (const auto& m : s->missing) out << "      " << m.name << "\tcrc=" << crc_hex(m.crc) << "\tsize=" << m.size << "\n";
        }
    } else if (format == 1) {
        out << "game,description,system,source_file,missing_rom,crc,size\n";
        for (const auto* s : incomplete)
            for (const auto& m : s->missing)
                out << csv(s->game_name) << ',' << csv(s->description) << ',' << csv(s->system) << ','
                    << csv(fs::path(s->trigger_archive).filename().string()) << ',' << csv(m.name) << ',' << crc_hex(m.crc) << ',' << m.size << "\n";
    } else {
        out << "<?xml version=\"1.0\"?>\n"
            << "<!DOCTYPE datafile PUBLIC \"-//Logiqx//DTD ROM Management Datafile//EN\" \"http://www.logiqx.com/Dats/datafile.dtd\">\n"
            << "<datafile>\n\t<header>\n\t\t<name>Bootcade - Incomplete imports</name>\n"
            << "\t\t<description>Sets the import could not complete, exported by Bootcade</description>\n\t</header>\n";
        for (const auto* s : incomplete) {
            out << "\t<game name=\"" << xml(s->game_name) << "\">\n\t\t<description>" << xml(s->description) << "</description>\n";
            for (const auto& p : s->pieces)
                out << "\t\t<rom name=\"" << xml(p.target_name) << "\" size=\"" << p.size << "\" crc=\"" << crc_hex(p.crc) << "\""
                    << (p.resolved ? " status=\"verified\"" : "") << "/>\n";
            out << "\t</game>\n";
        }
        out << "</datafile>\n";
    }
    flash(Glib::ustring::compose(_("Exported to %1."), dlg.get_filename()));
}

// ═══ Worker plumbing ════════════════════════════════════════════════════════

RomInbox::Callbacks RomImportTab::make_callbacks() {
    RomInbox::Callbacks cb;
    cb.progress  = [this](double p, const std::string& m) { push_progress(p, m); };
    cb.log       = [this](const std::string& m) { push_log(m); };
    cb.cancelled = [this] { return m_cancelled.load(); };
    return cb;
}

void RomImportTab::push_progress(double pct, const std::string& msg) {
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

void RomImportTab::push_log(const std::string& msg) {
    // main.cpp redirects std::cerr into debug.log : what the engine says
    // survives the window.
    std::cerr << "[IMPORT] " << msg << std::endl;
    {
        std::lock_guard<std::mutex> lk(m_shared_mutex);
        m_log_messages.push_back(msg);
    }
    m_progress_dispatcher();
}

void RomImportTab::log(const std::string& line, ui::LogPanel::Level level) {
    m_log->append(line, level);
}

void RomImportTab::on_progress_update() {
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
    if (!message.empty() && m_busy) m_progress_label.set_text(message);
    for (const auto& l : pending) {
        // The engine's debug traces are for debug.log, not for the screen.
        if (l.rfind("[INBOX-DEBUG]", 0) == 0) continue;
        m_log->append(l, level_of(l));
    }
}

void RomImportTab::on_worker_finished() {
    if (m_worker.joinable()) m_worker.join();
    on_progress_update();

    if (m_job == Job::Analyze) {
        populate();
        AppContext::trim_heap();
    } else if (m_job == Job::Apply) {
        for (const auto& e : m_apply_result.errors) m_log->append("  ! " + e, ui::LogPanel::Level::Error);
        m_sig_outbox_changed.emit();
        // The plan is stale now that files have moved : the table empties
        // until the next analysis, and says so.
        m_models.detach(m_table->view());
        m_store->clear();
        m_report = RomInbox::Report{};
        refilter();
        update_summary();
        m_detail->set_title(_("Selected set"));
        m_detail->set_subtitle(_("Every piece of the selected set, and where it comes from."));
        m_detail->show_placeholder(_("Select a set to see its pieces."));
        m_status.set_text(Glib::ustring::compose(
            _("Fix done: %1 moved, %2 rebuilt, %3 failed · %4 source(s) processed · %5 quarantined. Re-run Analyse to refresh."),
            m_apply_result.moved, m_apply_result.rebuilt, m_apply_result.failed,
            m_apply_result.consumed_archives, m_apply_result.quarantined));
    }
    m_job = Job::None;
    set_busy(false);
}

void RomImportTab::set_busy(bool busy) {
    m_busy = busy;
    m_btn_analyze->set_sensitive(!busy);
    m_btn_browse->set_sensitive(!busy);
    m_entry_inbox.set_sensitive(!busy);
    m_btn_select_all->set_sensitive(!busy);
    m_btn_select_none->set_sensitive(!busy);
    for (auto* c : {&m_check_recursive, &m_check_archives, &m_check_loose, &m_check_use_library, &m_check_rebuild_correct})
        c->set_sensitive(!busy);
    if (busy) { m_progress.set_fraction(0.0); m_progress.show(); m_progress_label.show(); m_btn_cancel->show(); }
    else      { m_progress.hide(); m_progress_label.hide(); m_btn_cancel->hide(); }
    update_action_buttons();
}

void RomImportTab::flash(const Glib::ustring& text) {
    if (m_busy) return;
    m_status.set_text(text);
    m_flash_timer.disconnect();
    m_flash_timer = Glib::signal_timeout().connect([this] { if (!m_busy) update_summary(); return false; }, 3500);
}
