// src/RomRebuildTab.cpp
#include "FileChooser.h"
#include "RomRebuildTab.h"

#include "AppContext.h"
#include "ConfirmationDialog.h"
#include "DatSource.h"
#include "RomArchive.h"
#include "i18n.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
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

nlohmann::json read_rom_manager() {
    nlohmann::json j;
    std::ifstream fi(AppContext::get_config_path());
    if (fi) { try { fi >> j; } catch (...) { j = nlohmann::json{}; } }
    return (j.contains("rom_manager") && j["rom_manager"].is_object()) ? j["rom_manager"] : nlohmann::json::object();
}

// The log level a RomRebuild message deserves, from the way it starts.
ui::LogPanel::Level level_of(const std::string& m) {
    if (m.rfind("FAILED", 0) == 0 || m.rfind("REFUSED", 0) == 0 || m.find("could not") != std::string::npos)
        return ui::LogPanel::Level::Error;
    if (m.rfind("WARNING", 0) == 0) return ui::LogPanel::Level::Warn;
    if (m.rfind("rebuilt", 0) == 0 || m.rfind("copied", 0) == 0 || m.rfind("normalised", 0) == 0) return ui::LogPanel::Level::Ok;
    if (m.rfind("kept", 0) == 0 || m.rfind("cleaned", 0) == 0) return ui::LogPanel::Level::Muted;
    return ui::LogPanel::Level::Info;
}

const char* status_key(RomRebuild::Status s) {
    switch (s) {
        case RomRebuild::Status::Build:
        case RomRebuild::Status::Copy:
        case RomRebuild::Status::Normalise:      return "write";
        case RomRebuild::Status::AlreadyThere:   return "already";
        case RomRebuild::Status::Incomplete:     return "incomplete";
        case RomRebuild::Status::Impossible:     return "impossible";
        case RomRebuild::Status::Undeterminable: return "undetermined";
    }
    return "impossible";
}

const char* status_label(RomRebuild::Status s) {
    switch (s) {
        case RomRebuild::Status::Build:          return N_("Rebuild");
        case RomRebuild::Status::Copy:           return N_("Copy");
        case RomRebuild::Status::Normalise:      return N_("Normalise");
        case RomRebuild::Status::AlreadyThere:   return N_("Already there");
        case RomRebuild::Status::Incomplete:     return N_("Incomplete");
        case RomRebuild::Status::Impossible:     return N_("Not possible");
        case RomRebuild::Status::Undeterminable: return N_("Not determinable");
    }
    return "";
}

// A set type, as the radio buttons name it.
Glib::ustring mode_label(const std::string& mode) {
    if (mode.empty())                     return _("As the DAT lists");
    if (mode == DatSource::kMergeSplit)  return _("Split");
    if (mode == DatSource::kMergeMerged) return _("Merged");
    return _("Non-merged");
}

// What a DAT does not say for a set type, in the player's words.
Glib::ustring missing_text(const RomRebuild::DatReport& r) {
    std::map<DatLayout::Missing, int> count;
    std::string example;
    for (const auto& p : r.problems) {
        ++count[p.kind];
        if (example.empty() && !p.set.empty()) example = p.set;
    }
    std::vector<Glib::ustring> lines;
    if (count.count(DatLayout::Missing::NoLinks))
        lines.push_back(_("it names no parent, clone, BIOS nor device : it lists fixed archives, which can only be rebuilt as it lists them"));
    if (count.count(DatLayout::Missing::NoDeviceRefs))
        lines.push_back(_("it describes devices but never says which set uses which (no device_ref)"));
    if (int n = count[DatLayout::Missing::MergeNotMarked])
        lines.push_back(Glib::ustring::compose(_("%1 set(s) list a ROM of their parent or BIOS without merge="), n));
    if (int n = count[DatLayout::Missing::ParentRomsNotListed])
        lines.push_back(Glib::ustring::compose(_("%1 set(s) inherit ROMs, but the DAT marks none as inherited (merge=)"), n));
    if (int n = count[DatLayout::Missing::LinkTargetMissing])
        lines.push_back(Glib::ustring::compose(_("%1 link(s) name a set no DAT read holds"), n));
    Glib::ustring out;
    for (size_t i = 0; i < lines.size(); ++i) out += (i ? " ; " : "") + lines[i];
    if (!example.empty()) out += Glib::ustring::compose(_(" (e.g. %1)"), example);
    const bool mame = !r.build.empty() || lower(r.label).rfind("mame", 0) == 0;
    if (mame)
        out += ". " + (r.build.empty() ? Glib::ustring(_("Use the -listxml of the same MAME version."))
                                       : Glib::ustring::compose(_("Use the -listxml of the same MAME build (%1)."), r.build));
    else
        out += ". " + Glib::ustring(_("Use a DAT that keeps parent/clone links and merge= marks."));
    return out;
}

} // namespace

// ═══ Construction ═══════════════════════════════════════════════════════════

RomRebuildTab::RomRebuildTab(std::shared_ptr<DatabaseManager> db)
    : Gtk::Box(Gtk::ORIENTATION_VERTICAL, ui::kCardSpacing), m_db(std::move(db)) {
    get_style_context()->add_class("set-page");

    build_options();
    build_results();
    build_footer();

    m_progress_dispatcher.connect(sigc::mem_fun(*this, &RomRebuildTab::on_progress_update));
    m_finished_dispatcher.connect(sigc::mem_fun(*this, &RomRebuildTab::on_worker_finished));

    reload_settings();
    update_summary();
    update_action_buttons();
    show_all_children();
    m_progress.hide();
    m_progress_label.hide();
    m_btn_cancel->hide();
    m_structure.hide();
}

RomRebuildTab::~RomRebuildTab() {
    if (m_worker.joinable()) {
        m_cancelled = true;
        m_worker.join();
    }
}

Gtk::Window* RomRebuildTab::toplevel() {
    return dynamic_cast<Gtk::Window*>(get_toplevel());
}

void RomRebuildTab::build_options() {
    auto browse = [this](Gtk::Entry& entry, const Glib::ustring& title, Gtk::FileChooserAction action) {
        BcFileChooser dlg(title, action);
        if (auto* top = toplevel()) dlg.set_transient_for(*top);
        dlg.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
        dlg.add_button(_("Select"), Gtk::RESPONSE_OK);
        if (!entry.get_text().empty()) dlg.set_filename(entry.get_text());
        if (dlg.run() == Gtk::RESPONSE_OK) entry.set_text(dlg.get_filename());
    };
    auto path_line = [&](Gtk::Entry& entry, Gtk::Button*& button, const Glib::ustring& placeholder,
                         const Glib::ustring& title, Gtk::FileChooserAction action) {
        auto* line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
        entry.set_hexpand(true);
        entry.set_placeholder_text(placeholder);
        button = ui::button(_("Browse…"), "bc-folder.svg");
        button->signal_clicked().connect([this, &entry, title, action, browse] { browse(entry, title, action); });
        line->pack_start(entry, Gtk::PACK_EXPAND_WIDGET);
        line->pack_start(*button, Gtk::PACK_SHRINK);
        return line;
    };

    // ── DATs : a group, a DAT file, or a folder of DATs ───────────────────
    auto dats = ui::card("bc-file.svg", _("DATs"), _("The sets to rebuild, and what they hold."));
    auto* dats_body = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 8);
    dats_body->set_margin_top(10);
    m_origin.append("group", _("Bootcade DAT group"));
    m_origin.append("file", _("DAT file"));
    m_origin.append("folder", _("DAT folder"));
    m_origin.set_tooltip_text(_("A DAT file or folder is read for this rebuild only : it is not added to Bootcade, "
                                "and no emulator is involved."));
    dats_body->pack_start(m_origin, Gtk::PACK_SHRINK);
    m_group.set_hexpand(true);
    m_origin_stack.add(m_group, "group");
    m_origin_stack.add(*path_line(m_entry_dat_file, m_btn_dat_file, _("/path/to/file.dat (or .xml, .zip, .7z)"),
                                  _("Select a DAT file"), Gtk::FILE_CHOOSER_ACTION_OPEN), "file");
    m_origin_stack.add(*path_line(m_entry_dat_folder, m_btn_dat_folder, _("/path/to/dats"),
                                  _("Select a folder of DATs"), Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER), "folder");
    m_origin_stack.set_vhomogeneous(false);
    dats_body->pack_start(m_origin_stack, Gtk::PACK_SHRINK);
    auto* dats_scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    dats_scroll->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    dats_scroll->set_min_content_height(52);
    dats_scroll->set_max_content_height(110);
    dats_scroll->set_propagate_natural_height(true);
    dats_scroll->add(m_dat_list);
    dats_body->pack_start(*dats_scroll, Gtk::PACK_SHRINK);
    dats.body->pack_start(*dats_body, Gtk::PACK_SHRINK);
    dats.frame->set_size_request(320, -1);
    m_top.pack_start(*dats.frame, Gtk::PACK_EXPAND_WIDGET);
    m_origin.signal_changed().connect([this] {
        m_origin_stack.set_visible_child(m_origin.get_active_id());
        if (m_loading) return;
        fill_dats();
        save_settings();
        invalidate();
    });
    m_group.signal_changed().connect([this] {
        if (m_loading) return;
        fill_dats();
        save_settings();
        invalidate();
    });
    for (auto* e : {&m_entry_dat_file, &m_entry_dat_folder}) {
        e->signal_changed().connect([this] {
            if (m_loading) return;
            fill_dats();
            save_settings();
            invalidate();
        });
    }

    // ── Folders ───────────────────────────────────────────────────────────
    auto dirs = ui::card("bc-folder.svg", _("Folders"), _("Where the ROMs are read, and where the sets are written."));
    auto* dirs_body = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 8);
    dirs_body->set_margin_top(10);
    // Each label on its field's line, all as wide as the widest : the card
    // stays as low as Import's, and the table keeps its height.
    auto labels = Gtk::SizeGroup::create(Gtk::SIZE_GROUP_HORIZONTAL);
    auto folder_line = [&](const Glib::ustring& label, Gtk::Entry& entry, Gtk::Button*& button,
                           const Glib::ustring& placeholder, const Glib::ustring& title) {
        auto* line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
        auto* name = Gtk::make_managed<Gtk::Label>(label);
        name->set_xalign(0.0f);
        labels->add_widget(*name);
        line->pack_start(*name, Gtk::PACK_SHRINK);
        line->pack_start(*path_line(entry, button, placeholder, title, Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER), Gtk::PACK_EXPAND_WIDGET);
        dirs_body->pack_start(*line, Gtk::PACK_SHRINK);
        entry.signal_changed().connect([this] { if (!m_loading) { save_settings(); invalidate(); } });
    };
    folder_line(_("Source"), m_entry_source, m_btn_source, _("/path/to/roms/to/rebuild"), _("Select the source folder"));
    m_check_recursive.set_label(_("Scan subfolders"));
    dirs_body->pack_start(m_check_recursive, Gtk::PACK_SHRINK);
    folder_line(_("Destination"), m_entry_dest, m_btn_dest, _("/path/to/rebuilt/sets"), _("Select the destination folder"));
    dirs.body->pack_start(*dirs_body, Gtk::PACK_SHRINK);
    dirs.frame->set_size_request(340, -1);
    m_top.pack_start(*dirs.frame, Gtk::PACK_EXPAND_WIDGET);

    // ── Output ────────────────────────────────────────────────────────────
    auto outc = ui::card("bc-package.svg", _("Output"), _("How the rebuilt sets are laid out and written."));
    auto* out_body = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 4);
    out_body->set_margin_top(10);
    auto* type_line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 12);
    type_line->pack_start(*Gtk::make_managed<Gtk::Label>(_("Set type")), Gtk::PACK_SHRINK);
    Gtk::RadioButton::Group group;
    for (auto* r : {&m_radio_non_merged, &m_radio_split, &m_radio_merged, &m_radio_listed}) r->set_group(group);
    m_radio_non_merged.set_label(_("Non-merged"));
    m_radio_split.set_label(_("Split"));
    m_radio_merged.set_label(_("Merged"));
    m_radio_listed.set_label(_("As the DAT lists"));
    reset_modes();
    for (auto* r : {&m_radio_non_merged, &m_radio_split, &m_radio_merged}) type_line->pack_start(*r, Gtk::PACK_SHRINK);
    out_body->pack_start(*type_line, Gtk::PACK_SHRINK);
    auto* listed_line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 12);
    auto* spacer = Gtk::make_managed<Gtk::Label>(_("Set type"));
    spacer->set_opacity(0.0);   // under the radios of the line above
    listed_line->pack_start(*spacer, Gtk::PACK_SHRINK);
    listed_line->pack_start(m_radio_listed, Gtk::PACK_SHRINK);
    out_body->pack_start(*listed_line, Gtk::PACK_SHRINK);

    auto* format_line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    format_line->pack_start(*Gtk::make_managed<Gtk::Label>(_("Format")), Gtk::PACK_SHRINK);
    m_format.append("zip", _("ZIP (TorrentZip)"));
    m_format.append("7z", "7z");
    format_line->pack_start(m_format, Gtk::PACK_SHRINK);
    m_check_recompress.set_label(_("Recompress archives"));
    m_check_recompress.set_tooltip_text(_("Write every archive anew. Off, an archive that already conforms is copied or kept "
                                          "as it is, and a ZIP with the right content that is not a TorrentZip is normalised."));
    format_line->pack_start(m_check_recompress, Gtk::PACK_SHRINK);
    out_body->pack_start(*format_line, Gtk::PACK_SHRINK);
    m_check_incomplete.set_label(_("Write incomplete sets"));
    m_check_incomplete.set_tooltip_text(_("An incomplete set is written with the ROMs found, and the report lists what it misses : "
                                          "a later rebuild completes it. Off, it is only reported."));
    out_body->pack_start(m_check_incomplete, Gtk::PACK_SHRINK);
    out_body->pack_start(*ui::sub_label(_("Sources are never modified : replaced archives are kept in _replaced/.")), Gtk::PACK_SHRINK);
    outc.body->pack_start(*out_body, Gtk::PACK_SHRINK);
    m_top.pack_start(*outc.frame, Gtk::PACK_EXPAND_WIDGET);

    pack_start(m_top, Gtk::PACK_SHRINK);

    m_check_recursive.signal_toggled().connect([this] { save_settings(); invalidate(); });
    m_check_recompress.signal_toggled().connect([this] { save_settings(); invalidate(); });
    m_check_incomplete.signal_toggled().connect([this] { save_settings(); invalidate(); });
    for (auto* r : {&m_radio_non_merged, &m_radio_split, &m_radio_merged, &m_radio_listed})
        r->signal_toggled().connect([this] { save_settings(); invalidate(); });
    m_format.signal_changed().connect([this] { save_settings(); invalidate(); });
}

void RomRebuildTab::build_results() {
    auto* summary = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    auto* pills = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    m_pill_write        = Gtk::make_managed<ui::Pill>(_("To write"),         ui::PillTone::Accent,  true);
    m_pill_already      = Gtk::make_managed<ui::Pill>(_("Already there"),    ui::PillTone::Ok,      true);
    m_pill_incomplete   = Gtk::make_managed<ui::Pill>(_("Incomplete"),       ui::PillTone::Warn,    true);
    m_pill_impossible   = Gtk::make_managed<ui::Pill>(_("Not possible"),     ui::PillTone::Error,   true);
    m_pill_undetermined = Gtk::make_managed<ui::Pill>(_("Not determinable"), ui::PillTone::Info,    true);
    m_pill_total        = Gtk::make_managed<ui::Pill>(_("Total"),            ui::PillTone::Neutral);
    for (auto* p : {m_pill_write, m_pill_already, m_pill_incomplete, m_pill_impossible, m_pill_undetermined, m_pill_total}) {
        p->set_count(0);
        pills->pack_start(*p, Gtk::PACK_SHRINK);
    }
    // Not possible is most of a big DAT against a small folder : shown on demand.
    for (auto* p : {m_pill_write, m_pill_already, m_pill_incomplete, m_pill_undetermined}) p->set_active(true);
    for (auto* p : {m_pill_write, m_pill_already, m_pill_incomplete, m_pill_impossible, m_pill_undetermined})
        p->signal_toggled().connect([this](bool) { refilter(); });
    summary->pack_start(*pills, Gtk::PACK_SHRINK);
    m_status.set_xalign(0.0f);
    m_status.get_style_context()->add_class("set-sub");
    m_status.set_ellipsize(Pango::ELLIPSIZE_END);
    m_status.set_valign(Gtk::ALIGN_CENTER);
    // Ellipsized when the window is narrow : the whole line on hover.
    m_status.property_label().signal_changed().connect([this] { m_status.set_tooltip_text(m_status.get_text()); });
    summary->pack_start(m_status, Gtk::PACK_EXPAND_WIDGET);
    pack_start(*summary, Gtk::PACK_SHRINK);

    // What each DAT can give : filled by a preview.
    m_structure.set_xalign(0.0f);
    m_structure.set_line_wrap(true);
    m_structure.set_line_wrap_mode(Pango::WRAP_WORD_CHAR);
    m_structure.set_selectable(true);
    m_structure.set_no_show_all(true);
    pack_start(m_structure, Gtk::PACK_SHRINK);

    m_filter = Gtk::make_managed<ui::FilterBar>(_("Search (set, rom, crc…)"));
    m_dat_combo = m_filter->add_combo(_("DAT:"));
    m_dat_combo->append(_("All"));
    m_dat_combo->set_active(0);
    m_filter->signal_changed().connect(sigc::mem_fun(*this, &RomRebuildTab::refilter));
    // On the search line : the counters above keep their room.
    m_btn_select_all  = ui::button(_("Select all"));
    m_btn_select_none = ui::button(_("Select none"));
    m_btn_select_all->signal_clicked().connect([this] { set_all_checked(true); });
    m_btn_select_none->signal_clicked().connect([this] { set_all_checked(false); });
    m_filter->pack_end(*m_btn_select_none, Gtk::PACK_SHRINK);
    m_filter->pack_end(*m_btn_select_all, Gtk::PACK_SHRINK);
    pack_start(*m_filter, Gtk::PACK_SHRINK);

    m_store = Gtk::ListStore::create(m_cols);
    m_models.sort_column = m_cols.set.index();
    m_models.sort_order  = Gtk::SORT_ASCENDING;

    m_table = Gtk::make_managed<ui::Table>(Gtk::SELECTION_SINGLE);
    m_models.attach(m_table->view(), m_store, sigc::mem_fun(*this, &RomRebuildTab::row_visible));
    m_table->add_check_column(m_cols.include, sigc::mem_fun(*this, &RomRebuildTab::on_row_toggled),
                              [this](bool on) { set_all_checked(on); });
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
            if (key == "write")           c = m_colours.accent;
            else if (key == "already")    c = m_colours.ok;
            else if (key == "incomplete") c = m_colours.warn;
            else if (key == "impossible") c = m_colours.err;
            renderer->property_foreground_rgba() = c;
            renderer->property_weight() = key == "already" ? Pango::WEIGHT_NORMAL : Pango::WEIGHT_BOLD;
        });
        m_table->view().append_column(*col);
    }
    { ui::ColumnOptions o; o.expand = true; o.min_width = 200; m_table->add_text_column(_("Set"), m_cols.set, o); }
    m_table->add_text_column(_("DAT"), m_cols.dat);
    { ui::ColumnOptions o; o.xalign = 1.0f; m_table->add_text_column(_("ROMs"), m_cols.roms, o); }
    { ui::ColumnOptions o; o.xalign = 1.0f; m_table->add_text_column(_("Sources"), m_cols.sources, o); }
    { ui::ColumnOptions o; o.xalign = 1.0f; m_table->add_text_column(_("Size"), m_cols.size, o); }
    { ui::ColumnOptions o; o.expand = true; o.sortable = false; m_table->add_text_column(_("Details"), m_cols.details, o); }
    m_table->view().get_selection()->signal_changed().connect(sigc::mem_fun(*this, &RomRebuildTab::on_selection_changed));
    m_table->set_size_request(-1, 140);

    auto* bottom = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, ui::kCardSpacing);
    bottom->set_homogeneous(true);
    m_detail = Gtk::make_managed<ui::DetailPanel>("bc-file.svg", _("Selected set"),
                                                  _("Every piece of the selected archive, and where it comes from."), 110);
    m_detail->show_placeholder(_("Select a set to see its pieces."));
    bottom->pack_start(*m_detail, Gtk::PACK_EXPAND_WIDGET);
    m_log = Gtk::make_managed<ui::LogPanel>(_("Rebuild log"), _("Details of the analysis and of the rebuild."));
    m_log->set_size_request(-1, 110);
    bottom->pack_start(*m_log, Gtk::PACK_EXPAND_WIDGET);
    pack_start(*ui::splitter(*m_table, *bottom, 230), Gtk::PACK_EXPAND_WIDGET);
}

void RomRebuildTab::build_footer() {
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

    // The DAT is the contract : the emulator may still need what another
    // DAT holds, or not run a set at all.
    auto* note = ui::sub_label(_("Rebuilt to match the DAT : whether the emulator runs a set is another matter."));
    note->set_ellipsize(Pango::ELLIPSIZE_END);
    m_footer.pack_start(*note, Gtk::PACK_EXPAND_WIDGET);

    m_btn_rebuild = ui::button(_("Rebuild"), "bc-check.svg", ui::Tone::Accent);
    m_btn_rebuild->set_tooltip_text(_("Write the checked archives into the destination ; when none is checked, every archive the preview lists as to write."));
    m_btn_rebuild->signal_clicked().connect(sigc::mem_fun(*this, &RomRebuildTab::on_rebuild_clicked));
    m_btn_preview = ui::button(_("Preview"), "bc-search.svg");
    m_btn_preview->set_tooltip_text(_("Read the DATs and the source, and show what Rebuild would write, without writing anything."));
    m_btn_preview->signal_clicked().connect(sigc::mem_fun(*this, &RomRebuildTab::on_preview_clicked));
    m_footer.pack_end(*m_btn_rebuild, Gtk::PACK_SHRINK);
    m_footer.pack_end(*m_btn_preview, Gtk::PACK_SHRINK);
    pack_start(m_footer, Gtk::PACK_SHRINK);
}

void RomRebuildTab::ensure_colours() {
    if (m_colours.ready || !get_toplevel() || !get_toplevel()->get_realized()) return;
    m_colours.ok     = ui::probe_color(*this, "set-ok");
    m_colours.warn   = ui::probe_color(*this, "set-warn");
    m_colours.err    = ui::probe_color(*this, "set-err");
    m_colours.muted  = ui::probe_color(*this, "set-sub");
    m_colours.accent = ui::probe_color(*this, "set-accent-text");
    m_colours.ready  = true;
}

// ═══ DATs, settings ═════════════════════════════════════════════════════════

void RomRebuildTab::reload_groups() {
    if (m_busy) return;
    const bool was_loading = m_loading;
    m_loading = true;
    std::string chosen = m_group.get_active_id().raw();
    if (chosen.empty()) {
        const auto rm = read_rom_manager();
        if (rm.contains("rebuild_group") && rm["rebuild_group"].is_string()) chosen = rm["rebuild_group"].get<std::string>();
        else chosen = DatSource::library_group().id;
    }
    m_group.remove_all();
    // Every group : an active one is read from the database, an inactive one
    // (one active group per emulator) from its DAT files, and left inactive.
    for (const auto& g : DatSource::load_groups())
        m_group.append(g.id, g.active ? g.name : Glib::ustring::compose(_("%1 (inactive, read from its files)"), g.name).raw());
    if (!m_group.set_active_id(chosen)) m_group.set_active(0);
    fill_dats();
    m_loading = was_loading;
}

void RomRebuildTab::fill_dats() {
    const std::string origin = m_origin.get_active_id().raw();
    // The DATs ticked last time, when the same DATs were offered.
    const auto rm = read_rom_manager();
    const std::string where = origin == "group" ? m_group.get_active_id().raw()
                            : origin == "file"  ? m_entry_dat_file.get_text().raw()
                                                : m_entry_dat_folder.get_text().raw();
    std::set<std::string> ticked;
    const bool remembered = rm.contains("rebuild_dats_of") && rm["rebuild_dats_of"].is_string()
                         && rm["rebuild_dats_of"].get<std::string>() == origin + ":" + where
                         && rm.contains("rebuild_dats") && rm["rebuild_dats"].is_array();
    if (remembered)
        for (const auto& d : rm["rebuild_dats"]) if (d.is_string()) ticked.insert(d.get<std::string>());

    ui::destroy_children(m_dat_list);
    m_dat_checks.clear();
    std::vector<std::string> dats;
    std::error_code ec;
    if (origin == "group") {
        if (!where.empty()) dats = RomRebuild::dats_of_group(m_db, where);
    } else if (origin == "file") {
        if (!where.empty() && fs::is_regular_file(where, ec)) {
            if (RomArchive::looks_like_archive(where)) {
                std::vector<std::string> names;
                if (RomArchive::list_names(where, names))
                    for (const auto& n : names) {
                        const std::string ext = lower(fs::path(n).extension().string());
                        if (ext == ".dat" || ext == ".xml") dats.push_back(where + '|' + n);
                    }
            } else {
                dats.push_back(where);
            }
        }
    } else if (!where.empty() && fs::is_directory(where, ec)) {
        dats = RomRebuild::dats_of_folder(where);
    }
    for (const auto& dat : dats) {
        const size_t bar = dat.find('|');
        const std::string shown = bar == std::string::npos ? fs::path(dat).filename().string()
                                                           : fs::path(dat.substr(0, bar)).filename().string() + "  ›  " + dat.substr(bar + 1);
        auto* c = Gtk::make_managed<Gtk::CheckButton>(shown);
        c->set_tooltip_text(dat);
        c->set_active(!remembered || ticked.count(dat));
        c->signal_toggled().connect([this] { if (!m_loading) { save_settings(); invalidate(); } });
        m_dat_list.pack_start(*c, Gtk::PACK_SHRINK);
        m_dat_checks.push_back({dat, c});
    }
    if (dats.empty())
        m_dat_list.pack_start(*ui::sub_label(origin == "group" ? _("No DAT of this group is loaded : update the DATs on the DAT tab.")
                                                               : _("No DAT here : a .dat, a .xml (-listxml), or an archive holding them.")),
                              Gtk::PACK_SHRINK);
    m_dat_list.show_all();
}

void RomRebuildTab::reload_settings() {
    m_loading = true;
    const auto rm = read_rom_manager();
    auto str  = [&](const char* k, const std::string& d) { return (rm.contains(k) && rm[k].is_string())  ? rm[k].get<std::string>() : d; };
    auto flag = [&](const char* k, bool d)               { return (rm.contains(k) && rm[k].is_boolean()) ? rm[k].get<bool>() : d; };
    m_entry_source.set_text(str("rebuild_source", ""));
    m_entry_dest.set_text(str("rebuild_dest", ""));
    m_entry_dat_file.set_text(str("rebuild_dat_file", ""));
    m_entry_dat_folder.set_text(str("rebuild_dat_folder", ""));
    m_check_recursive.set_active(flag("rebuild_recursive", true));
    const std::string mode = str("rebuild_mode", DatSource::kMergeNonMerged);
    if (mode == DatSource::kMergeSplit)       m_radio_split.set_active(true);
    else if (mode == DatSource::kMergeMerged) m_radio_merged.set_active(true);
    else if (mode == "as-listed")             m_radio_listed.set_active(true);
    else                                      m_radio_non_merged.set_active(true);
    if (!m_format.set_active_id(str("rebuild_format", "zip"))) m_format.set_active_id("zip");
    m_check_recompress.set_active(flag("rebuild_recompress", false));
    m_check_incomplete.set_active(flag("rebuild_incomplete", true));
    if (!m_origin.set_active_id(str("rebuild_origin", "group"))) m_origin.set_active_id("group");
    m_origin_stack.set_visible_child(m_origin.get_active_id());
    reload_groups();
    m_loading = false;
}

void RomRebuildTab::save_settings() const {
    if (m_loading) return;
    // Read-modify-write, like every other config.json writer in the app. The
    // tab's own keys only : never a DAT group nor its rules.
    nlohmann::json j;
    const std::string path = AppContext::get_config_path();
    { std::ifstream fi(path); if (fi) { try { fi >> j; } catch (...) { j = nlohmann::json{}; } } }
    auto& rm = j["rom_manager"];
    const RomRebuild::Options o = options_from_ui();
    const std::string origin = m_origin.get_active_id().raw();
    const std::string where = origin == "group" ? o.group_id
                            : origin == "file"  ? m_entry_dat_file.get_text().raw()
                                                : m_entry_dat_folder.get_text().raw();
    rm["rebuild_origin"]     = origin;
    if (origin == "group") rm["rebuild_group"] = o.group_id;
    rm["rebuild_dat_file"]   = m_entry_dat_file.get_text().raw();
    rm["rebuild_dat_folder"] = m_entry_dat_folder.get_text().raw();
    rm["rebuild_dats_of"]    = origin + ":" + where;
    rm["rebuild_dats"]       = o.dats;
    rm["rebuild_source"]     = o.source_dir;
    rm["rebuild_dest"]       = o.dest_dir;
    rm["rebuild_recursive"]  = o.recursive;
    rm["rebuild_mode"]       = o.mode.empty() ? std::string("as-listed") : o.mode;
    rm["rebuild_format"]     = o.format == RomBuild::Format::SevenZip ? "7z" : "zip";
    rm["rebuild_recompress"] = o.recompress;
    rm["rebuild_incomplete"] = o.write_incomplete;
    rm.erase("rebuild_remove_matched");
    std::ofstream fo(path);
    if (fo) fo << j.dump(4);
}

RomRebuild::Options RomRebuildTab::options_from_ui() const {
    RomRebuild::Options o;
    const std::string origin = m_origin.get_active_id().raw();
    o.origin   = origin == "group" ? RomRebuild::DatOrigin::Group : RomRebuild::DatOrigin::Files;
    o.group_id = m_group.get_active_id().raw();
    for (const auto& [dat, check] : m_dat_checks) {
        if (o.origin == RomRebuild::DatOrigin::Files) o.files.push_back(dat);
        if (check->get_active()) o.dats.push_back(dat);
    }
    o.source_dir = m_entry_source.get_text().raw();
    o.dest_dir   = m_entry_dest.get_text().raw();
    o.recursive  = m_check_recursive.get_active();
    o.mode       = m_radio_split.get_active()  ? DatSource::kMergeSplit
                 : m_radio_merged.get_active() ? DatSource::kMergeMerged
                 : m_radio_listed.get_active() ? ""
                                               : DatSource::kMergeNonMerged;
    o.format     = m_format.get_active_id() == "7z" ? RomBuild::Format::SevenZip : RomBuild::Format::Zip;
    o.recompress = m_check_recompress.get_active();
    o.write_incomplete = m_check_incomplete.get_active();
    return o;
}

void RomRebuildTab::invalidate() {
    if (m_loading || m_busy) return;
    if (m_report.archives.empty() && m_report.dats.empty()) return;
    reset_modes();
    clear_plan(_("The settings changed : preview again to see what Rebuild would write."));
}

void RomRebuildTab::reset_modes() {
    m_radio_non_merged.set_tooltip_text(_("Every set holds everything it runs on : its parent's, BIOS's and devices' ROMs included."));
    m_radio_split.set_tooltip_text(_("Every set holds only its own ROMs : the parent's and the BIOS's stay in theirs."));
    m_radio_merged.set_tooltip_text(_("A parent's archive holds its clones too ; clones have none."));
    m_radio_listed.set_tooltip_text(_("Each archive exactly as the DAT lists it, whatever its links."));
    for (auto* r : {&m_radio_non_merged, &m_radio_split, &m_radio_merged, &m_radio_listed}) r->set_sensitive(!m_busy);
}

void RomRebuildTab::show_structure() {
    if (m_report.dats.empty()) { m_structure.hide(); return; }
    auto yes_no = [](bool b) { return Glib::ustring(b ? "✓" : "✗"); };
    Glib::ustring text;
    bool warn = false;
    for (const auto& r : m_report.dats) {
        if (!text.empty()) text += "\n";
        text += "<b>" + Glib::Markup::escape_text(r.label) + "</b>";
        if (!r.build.empty()) text += "  " + Glib::Markup::escape_text(r.build);
        text += "  ·  " + Glib::ustring::compose(_("%1 sets, %2 clones"), (int)r.sets, (int)r.clones)
              + "  ·  " + Glib::ustring(_("links")) + " " + yes_no(r.clones + r.romofs + r.device_refs > 0)
              + "  ·  " + Glib::ustring(_("inherited ROMs marked")) + " " + yes_no(r.merge_roms > 0)
              + "  ·  " + Glib::ustring(_("devices")) + " " + (r.isdevice == 0 ? Glib::ustring("—") : yes_no(r.device_refs > 0))
              + "  →  ";
        for (int m = 1; m < 4; ++m)
            text += (m > 1 ? "  " : "") + Glib::Markup::escape_text(mode_label(RomRebuild::kModes[m])) + " " + yes_no(r.possible[m]);
        if (!r.problems.empty()) {
            warn = true;
            text += "\n<span weight=\"bold\">" + Glib::Markup::escape_text(
                        Glib::ustring::compose(r.whole ? _("%1 is not possible with this DAT : ") : _("%1 : some sets cannot be told : "),
                                               mode_label(m_report.options.mode)))
                  + "</span>" + Glib::Markup::escape_text(missing_text(r));
        }
    }
    m_structure.set_markup(text);
    auto ctx = m_structure.get_style_context();
    if (warn) { ctx->remove_class("set-sub"); ctx->add_class("set-warn"); }
    else      { ctx->remove_class("set-warn"); ctx->add_class("set-sub"); }
    m_structure.show();

    // A set type some chosen DAT cannot give at all : greyed, with the reason,
    // until the DATs change.
    Gtk::RadioButton* radios[4] = {&m_radio_listed, &m_radio_split, &m_radio_non_merged, &m_radio_merged};
    for (int m = 1; m < 4; ++m) {
        Glib::ustring why;
        for (const auto& r : m_report.dats)
            if (!r.possible[m]) why += (why.empty() ? "" : ", ") + r.label;
        if (why.empty() || radios[m]->get_active()) continue;
        radios[m]->set_sensitive(false);
        radios[m]->set_tooltip_text(Glib::ustring::compose(_("Not possible with %1 : preview with this set type to see why."), why));
    }
}

// ═══ Preview / Rebuild ══════════════════════════════════════════════════════

void RomRebuildTab::on_preview_clicked() {
    if (m_busy) return;
    const RomRebuild::Options o = options_from_ui();
    auto* top = toplevel();
    std::error_code ec;
    auto refuse = [&](const Glib::ustring& title, const Glib::ustring& text) {
        if (top) ui::notice(*top, title, text);
    };
    if (o.dats.empty())
        return refuse(_("No DAT chosen"), _("Choose a DAT group, a DAT file or a folder of DATs, and tick at least one DAT."));
    if (o.source_dir.empty() || !fs::is_directory(o.source_dir, ec))
        return refuse(_("No source folder"), _("Select the folder holding the ROMs to rebuild."));
    if (o.dest_dir.empty())
        return refuse(_("No destination folder"), _("Select the folder the sets are written into."));
    const fs::path src_c = RomBuild::weak_canonical(o.source_dir), dest_c = RomBuild::weak_canonical(o.dest_dir);
    if (RomBuild::is_under(src_c, dest_c))
        return refuse(_("Source inside the destination"),
                      _("The source folder cannot be the destination, nor a folder inside it."));
    save_settings();
    m_log->clear();
    reset_modes();
    clear_plan(_("Reading the DATs and the source folder…"));
    m_cancelled = false;
    m_job_options = o;
    m_job = Job::Analyze;
    set_busy(true);
    m_worker = std::thread(&RomRebuildTab::worker_analyze, this);
}

void RomRebuildTab::worker_analyze() {
    RomRebuild::Report rep = RomRebuild::analyze(m_db, m_job_options, make_callbacks());
    {
        std::lock_guard<std::mutex> lk(m_shared_mutex);
        m_report = std::move(rep);
    }
    m_finished_dispatcher();
}

int RomRebuildTab::to_write() const {
    int n = 0;
    for (const auto& a : m_report.archives) n += RomRebuild::chosen(m_report, a);
    return n;
}

void RomRebuildTab::on_rebuild_clicked() {
    if (m_busy) return;
    const int writes = to_write();
    if (writes == 0) return;
    int all = 0, replacing = 0, partial = 0;
    for (const auto& a : m_report.archives) {
        all += RomRebuild::writes(a);
        if (!RomRebuild::chosen(m_report, a)) continue;
        replacing += a.replaces;
        partial   += a.partial;
    }
    Glib::ustring what = writes < all
        ? Glib::ustring::compose(_("%1 checked archive(s) of %2 → written into %3\n"), writes, all, m_report.options.dest_dir)
        : Glib::ustring::compose(_("%1 archive(s) → written into %2\n"), writes, m_report.options.dest_dir);
    if (replacing)
        what += Glib::ustring::compose(_("%1 archive(s) already in the destination are replaced, the previous ones kept in %2/\n"),
                                       replacing, RomRebuild::kReplacedSubdir);
    if (partial)
        what += Glib::ustring::compose(_("%1 incomplete set(s) are written with the ROMs found : the report lists what they miss.\n"),
                                       partial);
    if (m_report.incomplete && !m_report.options.write_incomplete)
        what += Glib::ustring::compose(_("%1 incomplete set(s) are not written : the report lists what they miss.\n"), m_report.incomplete);
    if (m_report.undeterminable)
        what += Glib::ustring::compose(_("%1 set(s) the DAT cannot tell are not written.\n"), m_report.undeterminable);
    int refused = 0;
    for (const auto& r : m_report.dats) refused += r.whole;
    if (refused)
        what += Glib::ustring::compose(_("%1 DAT(s) cannot give this set type : nothing of them is written.\n"), refused);
    what += _("\nThe source files are not modified.");
    what += _("\nThis can take a long time : every ROM written is read, recompressed as a TorrentZip and checked. "
              "For a large collection, count several hours.");
    if (auto* top = toplevel()) {
        ConfirmationDialog confirm(*top, _("Rebuild these sets?"), what, "bc-check.svg");
        if (!confirm.show_and_confirm()) return;
    }
    m_cancelled = false;
    m_job = Job::Apply;
    set_busy(true);
    m_worker = std::thread(&RomRebuildTab::worker_apply, this);
}

void RomRebuildTab::worker_apply() {
    RomRebuild::ApplyResult res = RomRebuild::apply(m_report, make_callbacks());
    {
        std::lock_guard<std::mutex> lk(m_shared_mutex);
        m_apply_result = std::move(res);
    }
    m_finished_dispatcher();
}

// ═══ Table ══════════════════════════════════════════════════════════════════

void RomRebuildTab::clear_plan(const Glib::ustring& status) {
    m_models.detach(m_table->view());   // nothing attached while filling : see SettingsUi::ModelStack
    m_store->clear();
    m_report = RomRebuild::Report{};
    refilter();
    update_summary();
    update_action_buttons();
    m_structure.hide();
    m_detail->set_title(_("Selected set"));
    m_detail->set_subtitle(_("Every piece of the selected archive, and where it comes from."));
    m_detail->show_placeholder(_("Select a set to see its pieces."));
    m_status.set_text(status);
}

void RomRebuildTab::populate() {
    m_models.detach(m_table->view());
    m_store->clear();
    std::set<std::string> dats;
    for (size_t i = 0; i < m_report.archives.size(); ++i) {
        const auto& a = m_report.archives[i];
        dats.insert(a.folder);
        auto row = *(m_store->append());
        std::vector<std::string> bits;
        switch (a.status) {
            case RomRebuild::Status::Build:
                bits.push_back(a.archive_ok ? _("only its CHDs to add")
                                            : Glib::ustring::compose(_("from %1 file(s)"), a.sources).raw());
                break;
            case RomRebuild::Status::Copy:
                bits.push_back(Glib::ustring::compose(_("copied as it is from %1"), fs::path(a.from).filename().string()).raw());
                break;
            case RomRebuild::Status::Normalise:
                bits.push_back(Glib::ustring::compose(_("%1 has the right content : written again as a TorrentZip"),
                                                      fs::path(a.from).filename().string()).raw());
                break;
            case RomRebuild::Status::AlreadyThere:
                bits.push_back(_("the destination already holds it"));
                break;
            case RomRebuild::Status::Undeterminable:
                bits.push_back(Glib::ustring::compose(_("the DAT does not say : %1"), a.why).raw());
                break;
            case RomRebuild::Status::Incomplete:
            case RomRebuild::Status::Impossible: {
                std::string first;
                for (const auto& p : a.pieces) if (!p.found) { first = p.name; break; }
                if (first.empty()) for (const auto& d : a.disks) if (!d.found) { first = d.name + ".chd"; break; }
                bits.push_back(Glib::ustring::compose(_("%1 missing"), a.missing).raw() + (first.empty() ? "" : " : " + first + (a.missing > 1 ? ", …" : "")));
                break;
            }
        }
        if (a.status == RomRebuild::Status::Incomplete && m_report.options.write_incomplete)
            bits.push_back(a.partial ? _("written with the ROMs found") : _("the destination already holds the ROMs found"));
        if (a.replaces && (a.status == RomRebuild::Status::Build || a.status == RomRebuild::Status::Copy
                           || a.status == RomRebuild::Status::Normalise || a.partial))
            bits.push_back(Glib::ustring::compose(_("replaces the archive there (kept in %1/)"), RomRebuild::kReplacedSubdir).raw());
        for (const auto& n : a.needs) bits.push_back(Glib::ustring::compose(_("needs %1"), n).raw());
        if (!a.disks.empty()) bits.push_back(Glib::ustring::compose(_("%1 CHD(s)"), (int)a.disks.size()).raw());

        std::string details;
        for (size_t k = 0; k < bits.size(); ++k) details += (k ? " · " : "") + bits[k];
        row[m_cols.status]     = _(status_label(a.status));
        row[m_cols.status_key] = status_key(a.status);
        row[m_cols.set]        = a.description.empty() ? a.set : a.set + "  (" + a.description + ")";
        row[m_cols.dat]        = a.folder;
        row[m_cols.roms]       = std::to_string(a.found) + "/" + std::to_string(a.found + a.missing);
        row[m_cols.sources]    = a.sources ? std::to_string(a.sources) : std::string("—");
        row[m_cols.size]       = human_size(a.bytes);
        row[m_cols.details]    = details;
        row[m_cols.index]      = (unsigned int)i;
        row[m_cols.actionable] = RomRebuild::writes(a);
        row[m_cols.include]    = a.selected;
        std::string blob = lower(a.set + ' ' + a.description + ' ' + a.folder);
        for (const auto& p : a.pieces) blob += ' ' + lower(p.name) + ' ' + crc_hex(p.crc);
        for (const auto& d : a.disks) blob += ' ' + lower(d.name) + ' ' + d.sha1;
        row[m_cols.search_blob] = blob;
    }
    m_filter->set_combo_items(m_dat_combo, _("All"), dats, m_dat_combo->get_active_text());
    refilter();
    update_summary();
    update_action_buttons();
    show_structure();
}

void RomRebuildTab::update_summary() {
    const int write = m_report.build + m_report.copy + m_report.normalise;
    m_pill_write->set_count(write);
    m_pill_already->set_count(m_report.already);
    m_pill_incomplete->set_count(m_report.incomplete);
    m_pill_impossible->set_count(m_report.impossible);
    m_pill_undetermined->set_count(m_report.undeterminable);
    m_pill_total->set_count((long)m_report.archives.size());
    if (m_report.archives.empty() && m_report.dats.empty()) {
        m_status.set_text(_("Preview reads the DATs and the source folder, and shows what Rebuild would write."));
        return;
    }
    Glib::ustring text = Glib::ustring::compose(_("%1 to write (%2 copied, %3 normalised) · %4 source file(s) read"),
                                                write, m_report.copy, m_report.normalise, (int)m_report.source_files);
    if (m_report.partial)
        text += " · " + Glib::ustring::compose(_("%1 incomplete written with the ROMs found"), m_report.partial);
    if (!m_report.unreadable.empty())
        text += " · " + Glib::ustring::compose(_("%1 unreadable"), (int)m_report.unreadable.size());
    m_status.set_text(text);
}

bool RomRebuildTab::row_visible(const Gtk::TreeModel::const_iterator& it) const {
    const Gtk::TreeModel::Row row = *it;
    const Glib::ustring key = row[m_cols.status_key];
    const bool wanted = (key == "write"        && m_pill_write->active())
                     || (key == "already"      && m_pill_already->active())
                     || (key == "incomplete"   && m_pill_incomplete->active())
                     || (key == "impossible"   && m_pill_impossible->active())
                     || (key == "undetermined" && m_pill_undetermined->active());
    if (!wanted) return false;
    if (!m_vis_dat.empty() && row[m_cols.dat] != m_vis_dat) return false;
    if (!m_vis_needle.empty()) {
        const Glib::ustring blob = row[m_cols.search_blob];
        if (blob.raw().find(m_vis_needle) == std::string::npos) return false;
    }
    return true;
}

void RomRebuildTab::refilter() {
    // Rebuilt, not refiltered : see SettingsUi::ModelStack.
    m_vis_dat = m_dat_combo->get_active_row_number() > 0 ? m_dat_combo->get_active_text() : Glib::ustring();
    m_vis_needle = lower(m_filter->search_text());
    m_models.detach(m_table->view());
    m_models.attach(m_table->view(), m_store, sigc::mem_fun(*this, &RomRebuildTab::row_visible));
    m_filter->set_summary(Glib::ustring::compose(_("%1 result(s)"), m_models.visible_count()));
}

namespace {
struct PieceCols : public Gtk::TreeModel::ColumnRecord {
    Gtk::TreeModelColumn<Glib::ustring> state, name, sum, size, source;
    Gtk::TreeModelColumn<int> found;   // 1 found, 0 missing
    PieceCols() { add(state); add(name); add(sum); add(size); add(source); add(found); }
};
}

void RomRebuildTab::on_selection_changed() {
    auto rows = m_table->view().get_selection()->get_selected_rows();
    if (rows.empty()) { m_detail->show_placeholder(_("Select a set to see its pieces.")); return; }
    Gtk::TreeModel::Row row = *m_store->get_iter(m_models.store_path(rows.front()));
    const auto& a = m_report.archives[(unsigned int)row[m_cols.index]];
    m_detail->set_title(a.description.empty() ? a.set : a.set + "  —  " + a.description);
    const std::string where = !a.dest_path.empty() ? a.dest_path : a.disk_dir;
    m_detail->set_subtitle(a.folder + "  →  " + fs::path(where).filename().string());

    static PieceCols cols;
    auto store = Gtk::ListStore::create(cols);
    auto source_of = [](const std::string& container, const std::string& entry) {
        const std::string file = fs::path(container).filename().string();
        return entry.empty() || entry == file ? file : file + "  ›  " + entry;
    };
    for (const auto& p : a.pieces) {
        auto r = *(store->append());
        r[cols.found]  = p.found ? 1 : 0;
        r[cols.state]  = !p.found ? _("Missing") : p.container.empty() ? _("Empty ROM") : _("Found");
        r[cols.name]   = p.owner.empty() || p.name.rfind(p.owner + "/", 0) == 0 ? p.name : p.name + "  (" + p.owner + ")";
        r[cols.sum]    = crc_hex(p.crc);
        r[cols.size]   = human_size(p.size);
        r[cols.source] = p.found && !p.container.empty() ? source_of(p.container, p.entry) : std::string();
    }
    for (const auto& d : a.disks) {
        auto r = *(store->append());
        r[cols.found]  = d.found ? 1 : 0;
        r[cols.state]  = !d.found ? _("Missing") : d.present ? _("Already there") : _("Found");
        r[cols.name]   = d.name + ".chd";
        r[cols.sum]    = d.sha1;
        r[cols.size]   = "";
        r[cols.source] = d.file.empty() ? std::string() : fs::path(d.file).filename().string();
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
            static PieceCols c;
            renderer->property_foreground_rgba() = (int)(*it)[c.found] ? m_colours.ok : m_colours.err;
        });
        t->view().append_column(*col);
    }
    { ui::ColumnOptions o; o.mono = true; o.expand = true; t->add_text_column(_("Name"), cols.name, o); }
    { ui::ColumnOptions o; o.mono = true; t->add_text_column(_("CRC / SHA1"), cols.sum, o); }
    { ui::ColumnOptions o; o.xalign = 1.0f; t->add_text_column(_("Size"), cols.size, o); }
    { ui::ColumnOptions o; o.expand = true; t->add_text_column(_("Source"), cols.source, o); }
    m_detail->set_content(t);
}

void RomRebuildTab::on_row_toggled(const Glib::ustring& path) {
    if (m_busy) return;
    Gtk::TreeModel::Row row = *m_store->get_iter(m_models.store_path(Gtk::TreeModel::Path(path)));
    if (!row[m_cols.actionable]) return;
    const bool on = !row[m_cols.include];
    row[m_cols.include] = on;
    m_report.archives[(unsigned int)row[m_cols.index]].selected = on;
    update_action_buttons();
}

void RomRebuildTab::set_all_checked(bool on) {
    if (m_busy) return;
    // The rows shown, the view detached while they change : attached, each
    // change refiltered and resorted the whole list (RomImportTab, the same).
    std::vector<Gtk::TreeModel::iterator> shown;
    for (const auto& frow : m_models.filter->children())
        shown.push_back(m_models.filter->convert_iter_to_child_iter(frow));
    m_models.detach(m_table->view());
    for (const auto& it : shown) {
        Gtk::TreeModel::Row row = *it;
        if (!row[m_cols.actionable]) continue;
        row[m_cols.include] = on;
        m_report.archives[(unsigned int)row[m_cols.index]].selected = on;
    }
    m_models.attach(m_table->view(), m_store, sigc::mem_fun(*this, &RomRebuildTab::row_visible));
    update_action_buttons();
}

void RomRebuildTab::update_action_buttons() {
    if (m_table) m_table->refresh_header_check();
    for (auto* b : {m_btn_select_all, m_btn_select_none}) if (b) b->set_sensitive(!m_busy && !m_report.archives.empty());
    const int writes = to_write();
    m_btn_rebuild->set_label(writes ? Glib::ustring::compose(_("Rebuild (%1)"), writes) : Glib::ustring(_("Rebuild")));
    m_btn_rebuild->set_sensitive(!m_busy && writes > 0);
    m_btn_preview->set_sensitive(!m_busy);
}

// ═══ Worker plumbing ════════════════════════════════════════════════════════

RomRebuild::Callbacks RomRebuildTab::make_callbacks() {
    RomRebuild::Callbacks cb;
    cb.progress  = [this](double p, const std::string& m) { push_progress(p, m); };
    cb.log       = [this](const std::string& m) { push_log(m); };
    cb.cancelled = [this] { return m_cancelled.load(); };
    return cb;
}

void RomRebuildTab::push_progress(double pct, const std::string& msg) {
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

void RomRebuildTab::push_log(const std::string& msg) {
    // main.cpp redirects std::cerr into debug.log : what the engine says
    // survives the window.
    std::cerr << "[REBUILD] " << msg << std::endl;
    {
        std::lock_guard<std::mutex> lk(m_shared_mutex);
        m_log_messages.push_back(msg);
    }
    m_progress_dispatcher();
}

void RomRebuildTab::on_progress_update() {
    std::string message;
    std::vector<std::string> pending;
    {
        std::lock_guard<std::mutex> lk(m_shared_mutex);
        message = m_current_message;
        pending.swap(m_log_messages);
    }
    const double pct = m_progress_value.load();
    m_progress.set_fraction(std::clamp(pct / 100.0, 0.0, 1.0));
    m_progress.set_text(std::to_string((int)pct) + "%");
    if (!message.empty() && m_busy) m_progress_label.set_text(message);
    for (const auto& l : pending) m_log->append(l, level_of(l));
}

void RomRebuildTab::on_worker_finished() {
    if (m_worker.joinable()) m_worker.join();
    on_progress_update();

    if (m_job == Job::Analyze) {
        if (!m_report.error.empty()) {
            m_log->append("FAILED  " + m_report.error, ui::LogPanel::Level::Error);
            const std::string error = m_report.error;
            clear_plan(Glib::ustring::compose(_("Nothing to preview : %1."), error));
        } else if (m_report.cancelled) {
            clear_plan(_("Preview cancelled."));
        } else {
            populate();
            for (const auto& u : m_report.unreadable)
                m_log->append("  unreadable " + u, ui::LogPanel::Level::Warn);
        }
        AppContext::trim_heap();
    } else if (m_job == Job::Apply) {
        for (const auto& e : m_apply_result.errors) m_log->append("  ! " + e, ui::LogPanel::Level::Error);
        for (const auto& u : m_apply_result.unreadable) m_log->append("  unreadable, left out : " + u, ui::LogPanel::Level::Warn);
        if (!m_apply_result.report_path.empty())
            m_log->append("report written to " + m_apply_result.report_path);
        Glib::ustring end = m_apply_result.disk_full ? Glib::ustring(_(" (stopped : the destination is full)"))
                          : m_apply_result.cancelled ? Glib::ustring(_(" (cancelled)")) : Glib::ustring();
        // The plan is stale now that files were written : the table empties
        // until the next preview, and says so.
        clear_plan(Glib::ustring::compose(
            _("Rebuild done%1: %2 rebuilt, %3 copied, %4 normalised, %5 incomplete, %6 CHD(s), %7 failed · %8 written · %9 kept in _replaced. Preview again to refresh."),
            end, m_apply_result.built, m_apply_result.copied, m_apply_result.normalised, m_apply_result.partial,
            m_apply_result.disks, m_apply_result.failed, human_size(m_apply_result.bytes_written), m_apply_result.replaced));
        if (m_apply_result.disk_full)
            if (auto* top = toplevel())
                ui::notice(*top, _("The destination is full"),
                           _("The rebuild stopped : no room is left in the destination. Every archive already written is complete, "
                             "and none was replaced by a partial one."));
    }
    m_job = Job::None;
    set_busy(false);
}

void RomRebuildTab::set_busy(bool busy) {
    m_busy = busy;
    for (Gtk::Widget* w : std::vector<Gtk::Widget*>{&m_origin, &m_origin_stack, &m_dat_list, &m_entry_source, &m_entry_dest,
                                                    m_btn_source, m_btn_dest, &m_check_recursive, &m_format, &m_check_recompress,
                                                    &m_check_incomplete})
        w->set_sensitive(!busy);
    // The set types keep what the preview said of them.
    for (auto* r : {&m_radio_non_merged, &m_radio_split, &m_radio_merged, &m_radio_listed})
        if (busy || r->get_tooltip_text().find(_("Not possible with")) == Glib::ustring::npos) r->set_sensitive(!busy);
    if (busy) { m_progress.set_fraction(0.0); m_progress.show(); m_progress_label.show(); m_btn_cancel->show(); }
    else      { m_progress.hide(); m_progress_label.hide(); m_btn_cancel->hide(); }
    update_action_buttons();
}
