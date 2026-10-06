// src/RomDatTab.cpp
#include "RomDatTab.h"

#include "AppContext.h"
#include "ConfirmationDialog.h"
#include "EmulatorRegistry.h"
#include "DatParser.h"
#include "GenerateDAT.h"
#include "MameCatalog.h"
#include "MameSooner.h"
#include "i18n.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <iostream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
namespace ui = SettingsUi;

namespace {

constexpr int UI_DISPATCH_INTERVAL_MS = 100;
// Ticks in a row are one reload : the database rebuild is long enough not to
// run it once per click.
constexpr int RELOAD_DEBOUNCE_MS = 2500;

std::string human_size(uintmax_t bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    std::ostringstream os;
    os << std::fixed << std::setprecision(v < 10.0 && u > 0 ? 1 : 0) << v << ' ' << units[u];
    return os.str();
}

std::string thousands(long n) {
    std::string digits = std::to_string(n < 0 ? -n : n), out;
    int k = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it, ++k) {
        if (k && k % 3 == 0) out.insert(0, " ");
        out.insert(0, 1, *it);
    }
    return (n < 0 ? "-" : "") + out;
}

// « www.progettosnaps.net » pour une adresse : ce que l'ecran montre comme
// source d'un fichier pris a une adresse du joueur.
std::string host_of(const std::string& url) {
    auto b = url.find("://");
    b = b == std::string::npos ? 0 : b + 3;
    const auto e = url.find('/', b);
    return url.substr(b, e == std::string::npos ? std::string::npos : e - b);
}

std::string short_date(const std::string& iso) {
    if (iso.size() < 16) return iso;
    return iso.substr(0, 10) + " " + iso.substr(11, 5);
}

// "FinalBurn Neo - Arcade Games" → "Arcade", "MAME ROMs (split)" →
// "ROMs (split)" : la regle du launcher, celle que DatParser applique pour
// remplir la colonne system. Un en-tete qu'elle ne sait pas lire s'affiche
// tel quel.
std::string system_of_header(const std::string& header) {
    const std::string s = DatParser::extractSystemFromHeader(header);
    return s == "Unknown" ? header : s;
}

std::string file_mtime_iso(const std::string& path) {
    std::error_code ec;
    auto ftime = fs::last_write_time(path, ec);
    if (ec) return "";
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    std::time_t t = std::chrono::system_clock::to_time_t(sctp);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// A "label : value" grid, the DAT information panel's material.
Gtk::Label* grid_row(Gtk::Grid& grid, int row, const std::string& label, std::map<std::string, Gtk::Label*>& values, const std::string& key) {
    auto* k = ui::sub_label(label);
    k->set_line_wrap(false);
    k->set_xalign(0.0f);
    grid.attach(*k, 0, row, 1, 1);
    auto* v = Gtk::make_managed<Gtk::Label>();
    v->set_xalign(0.0f);
    v->set_ellipsize(Pango::ELLIPSIZE_MIDDLE);
    v->set_selectable(true);
    v->set_halign(Gtk::ALIGN_FILL);
    // Bounded natural width : a long path ellipsizes instead of widening the
    // whole panel at the table's expense.
    v->set_max_width_chars(30);
    grid.attach(*v, 1, row, 1, 1);
    values[key] = v;
    return v;
}

// One field, in Bootcade's own dress : the name of a group. False when
// dismissed or left empty.
bool prompt_name(Gtk::Window* parent, const std::string& title, const std::string& subtitle, std::string& value) {
    Gtk::Dialog dlg;
    dlg.set_title(title);
    if (parent) dlg.set_transient_for(*parent);
    dlg.set_modal(true);
    dlg.set_resizable(false);
    dlg.set_decorated(false);
    dlg.get_style_context()->add_class("cc-window");
    dlg.get_style_context()->add_class("set-window");
    dlg.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);
    auto* content = dlg.get_content_area();
    content->set_spacing(0);

    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 14);
    body->get_style_context()->add_class("set-notice");
    auto* head = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 15);
    head->pack_start(*ui::tile("bc-file.svg", ui::kIconSection, ui::kTileSection), Gtk::PACK_SHRINK);
    auto* txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 4);
    txt->set_valign(Gtk::ALIGN_CENTER);
    txt->pack_start(*ui::card_title_label(title), Gtk::PACK_SHRINK);
    if (!subtitle.empty()) {
        auto* sub = ui::sub_label(subtitle);
        sub->set_max_width_chars(52);
        txt->pack_start(*sub, Gtk::PACK_SHRINK);
    }
    head->pack_start(*txt, Gtk::PACK_EXPAND_WIDGET);
    body->pack_start(*head, Gtk::PACK_SHRINK);
    auto* entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_text(value);
    entry->set_placeholder_text(_("Group name"));
    entry->set_size_request(380, -1);
    entry->set_activates_default(true);
    body->pack_start(*entry, Gtk::PACK_SHRINK);
    content->pack_start(*body, Gtk::PACK_EXPAND_WIDGET);

    auto* foot = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    foot->get_style_context()->add_class("cc-footer");
    auto* cancel = ui::button(_("Cancel"));
    cancel->signal_clicked().connect([&dlg] { dlg.response(Gtk::RESPONSE_CANCEL); });
    auto* ok = ui::button(_("Save"), "bc-save.svg", ui::Tone::Accent);
    ok->set_can_default(true);
    ok->signal_clicked().connect([&dlg] { dlg.response(Gtk::RESPONSE_OK); });
    foot->pack_end(*ok, Gtk::PACK_SHRINK);
    foot->pack_end(*cancel, Gtk::PACK_SHRINK);
    content->pack_start(*foot, Gtk::PACK_SHRINK);
    dlg.show_all_children();
    dlg.set_default(*ok);
    entry->grab_focus();
    if (dlg.run() != Gtk::RESPONSE_OK) return false;
    std::string v = entry->get_text().raw();
    while (!v.empty() && isspace((unsigned char)v.back())) v.pop_back();
    while (!v.empty() && isspace((unsigned char)v.front())) v.erase(v.begin());
    if (v.empty()) return false;
    value = v;
    return true;
}

} // namespace

// ═══ Construction ═══════════════════════════════════════════════════════════

RomDatTab::RomDatTab(std::shared_ptr<DatabaseManager> db, EnvProvider env)
    : Gtk::Box(Gtk::ORIENTATION_VERTICAL, ui::kCardSpacing), m_db(std::move(db)), m_env(std::move(env)) {
    get_style_context()->add_class("set-page");
    m_groups = DatSource::load_groups();

    build_groups_column();
    build_group_card();
    build_source_card();
    m_main.pack_start(m_top, Gtk::PACK_SHRINK);
    build_table();
    build_footer();
    m_columns.pack_start(m_main, Gtk::PACK_EXPAND_WIDGET);
    pack_start(m_columns, Gtk::PACK_EXPAND_WIDGET);

    m_progress_dispatcher.connect(sigc::mem_fun(*this, &RomDatTab::on_progress_update));
    m_finished_dispatcher.connect(sigc::mem_fun(*this, &RomDatTab::on_worker_finished));
    show_all_children();
    m_progress.hide();
    m_progress_label.hide();
    m_btn_cancel->hide();
    refresh();
}

RomDatTab::~RomDatTab() {
    m_reload_timer.disconnect();
    m_flash_timer.disconnect();
    if (m_worker.joinable()) {
        m_cancelled = true;
        m_worker.join();
    }
}

// The left column : one row per group, the current one highlighted.
void RomDatTab::build_groups_column() {
    auto card = ui::card("bc-folder.svg", _("DAT groups"), _("Named selections of DAT files."));
    // Un bouton, plus un menu : l'emulateur du nouveau groupe se choisit dans
    // sa carte, comme son dossier et son style de sets.
    m_btn_add_group = ui::button(_("Add group"), "bc-plus.svg");
    m_btn_add_group->set_halign(Gtk::ALIGN_START);
    m_btn_add_group->set_margin_top(10);
    m_btn_add_group->signal_clicked().connect(sigc::mem_fun(*this, &RomDatTab::on_add_group));

    m_group_list.set_selection_mode(Gtk::SELECTION_SINGLE);
    m_group_list.get_style_context()->add_class("set-rows");
    m_group_list.signal_row_selected().connect([this](Gtk::ListBoxRow* row) {
        if (!row) return;
        size_t idx = (size_t)row->get_index();
        if (idx >= m_groups.size() || idx == m_current) return;
        m_current = idx;
        // Off the signal : refresh rebuilds the very rows being selected.
        Glib::signal_idle().connect_once([this] { refresh(); });
    });
    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    scroll->add(m_group_list);
    scroll->set_margin_top(10);
    card.body->pack_start(*scroll, Gtk::PACK_EXPAND_WIDGET);
    card.body->pack_start(*m_btn_add_group, Gtk::PACK_SHRINK);
    card.frame->set_size_request(268, -1);
    card.frame->set_hexpand(false);
    m_columns.pack_start(*card.frame, Gtk::PACK_SHRINK);
}

void RomDatTab::build_group_card() {
    m_group_card = ui::card("bc-file.svg", group().name, "");
    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 8);
    body->set_margin_top(10);

    auto* folder_line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    auto* folder_label = ui::title_label(_("Folder"));
    folder_label->set_valign(Gtk::ALIGN_CENTER);
    m_entry_folder.set_hexpand(true);
    m_entry_folder.set_placeholder_text(_("/path/to/dat/files"));
    m_entry_folder.signal_activate().connect([this] {
        if (m_entry_folder.get_text().raw() == group().folder) return;
        group().folder = m_entry_folder.get_text().raw();
        save_groups();
        if (m_current == 0) m_sig_folder.emit(group().folder);
        refresh();
        groups_changed();
    });
    m_btn_browse = ui::button(_("Browse…"), "bc-folder.svg");
    m_btn_browse->signal_clicked().connect(sigc::mem_fun(*this, &RomDatTab::on_browse_folder));
    folder_line->pack_start(*folder_label, Gtk::PACK_SHRINK);
    folder_line->pack_start(m_entry_folder, Gtk::PACK_EXPAND_WIDGET);
    folder_line->pack_start(*m_btn_browse, Gtk::PACK_SHRINK);
    body->pack_start(*folder_line, Gtk::PACK_SHRINK);



    // L'emulateur appartient au groupe, pas a la source : c'est lui qui dit
    // quel catalogue le groupe decrit, donc quel executable produit ses DAT
    // et contre quelles regles l'audit juge. La liste vient du registre : un
    // emulateur de plus y apparait sans qu'on touche a cet ecran.
    auto* emu_line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    auto* emu_label = ui::title_label(_("Emulator"));
    emu_label->set_valign(Gtk::ALIGN_CENTER);
    for (const auto& e : EmulatorRegistry::all())
        m_combo_emulator.append(e.id, e.name + "  —  " + e.tagline);
    m_combo_emulator.set_tooltip_text(_("Which emulator this group describes. It says what generates its DAT files and which catalogue the audit judges."));
    m_combo_emulator.signal_changed().connect([this] {
        std::string v = m_combo_emulator.get_active_id().raw();
        if (v.empty() || v == group().emulator) return;
        group().emulator = v;
        // Change d'emulateur, le groupe actif y devient le seul actif ; son
        // ancien emulateur garde (ou retrouve) le sien.
        DatSource::one_active_per_emulator(m_groups, group().active ? m_current : DatSource::kNoGroup);
        save_groups();
        rebuild_group_list();
        apply_source_ui();
        show_source_info();
        show_file_info(-1);
        groups_changed();
    });
    emu_line->pack_start(*emu_label, Gtk::PACK_SHRINK);
    emu_line->pack_start(m_combo_emulator, Gtk::PACK_EXPAND_WIDGET);
    body->pack_start(*emu_line, Gtk::PACK_SHRINK);

    // The actions are built here, with the group they act on, and packed
    // in the bottom bar (build_footer) like every tab's actions.
    m_btn_primary = ui::button("", "bc-download.svg", ui::Tone::Accent);
    m_btn_primary->signal_clicked().connect(sigc::mem_fun(*this, &RomDatTab::on_primary_action));
    m_btn_check = ui::button(_("Check for updates"), "bc-sync.svg");
    m_btn_check->signal_clicked().connect(sigc::mem_fun(*this, &RomDatTab::on_check_updates));
    m_btn_add = ui::button(_("Add DAT files…"), "bc-plus.svg");
    m_btn_add->signal_clicked().connect(sigc::mem_fun(*this, &RomDatTab::on_add_files));
    m_btn_more = Gtk::make_managed<Gtk::MenuButton>();
    m_btn_more->add(*ui::image("more.svg", 16));
    m_btn_more->set_tooltip_text(_("More"));
    auto* reload = Gtk::make_managed<Gtk::MenuItem>(_("Reload database from DAT files"));
    reload->signal_activate().connect([this] { m_sig_reload.emit(true, ""); });
    auto* open = Gtk::make_managed<Gtk::MenuItem>(_("Open folder"));
    open->signal_activate().connect(sigc::mem_fun(*this, &RomDatTab::on_open_folder));
    m_more_menu.append(*reload);
    m_more_menu.append(*open);
    m_more_menu.show_all();
    m_btn_more->set_popup(m_more_menu);
    m_actions.pack_start(*m_btn_check, Gtk::PACK_SHRINK);
    m_actions.pack_start(*m_btn_add, Gtk::PACK_SHRINK);
    m_actions.pack_start(*m_btn_more, Gtk::PACK_SHRINK);
    m_actions.pack_start(*m_btn_primary, Gtk::PACK_SHRINK);

    m_status.set_xalign(0.0f);
    m_status.get_style_context()->add_class("set-sub");
    m_status.set_ellipsize(Pango::ELLIPSIZE_END);
    body->pack_start(m_status, Gtk::PACK_SHRINK);
    m_group_card.body->pack_start(*body, Gtk::PACK_SHRINK);

    m_group_sub.set_xalign(1.0f);
    m_group_sub.get_style_context()->add_class("set-sub");
    m_group_sub.set_valign(Gtk::ALIGN_CENTER);
    m_group_card.head->pack_end(m_group_sub, Gtk::PACK_SHRINK);
    m_top.pack_start(*m_group_card.frame, Gtk::PACK_EXPAND_WIDGET);
}

void RomDatTab::build_source_card() {
    auto card = ui::card("bc-cloud.svg", _("DAT source"),
                         _("Where this group's DAT files come from. Groups may share one."));
    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 6);
    body->set_margin_top(10);
    Gtk::RadioButton::Group grp;
    for (auto* r : {&m_radio_emulator, &m_radio_http, &m_radio_url, &m_radio_site, &m_radio_folder})
        r->set_group(grp);
    // Une seule ligne pour les executables : lequel lancer se lit dans
    // l'emulateur du groupe, la source n'a pas a le redire. L'adresse du
    // serveur Bootcade est dans le code : rien a saisir.
    m_radio_emulator.set_label(_("Emulator executable — generates the DAT files from the emulator this group describes"));
    m_radio_http.set_label(_("Bootcade server — downloads the DAT files published by the Bootcade server"));
    m_radio_url.set_label(_("Custom URL — downloads a DAT file, or an archive of DAT files, from an address you give"));
    m_radio_site.set_label(_("DAT site — downloads the DAT files from the site that publishes them"));
    m_radio_folder.set_label(_("Local folder — you put the DAT files there yourself"));
    for (auto* r : {&m_radio_emulator, &m_radio_http, &m_radio_url, &m_radio_site, &m_radio_folder}) {
        // Les libelles passent a la ligne : sur une seule ligne, le plus long
        // imposait sa largeur a la carte et la coupait au bord de la fenetre.
        if (auto* lbl = dynamic_cast<Gtk::Label*>(r->get_child())) {
            lbl->set_line_wrap(true);
            lbl->set_max_width_chars(56);
            lbl->set_xalign(0.0f);
        }
        body->pack_start(*r, Gtk::PACK_SHRINK);
        r->signal_toggled().connect([this, r] { if (r->get_active()) on_source_changed(); });
    }

    // Custom URL : l'adresse, et si une archive doit etre decompressee.
    m_url_line.set_spacing(8);
    m_url_line.set_no_show_all(true);
    auto* url_label = ui::sub_label(_("URL"));
    url_label->set_line_wrap(false);
    url_label->set_valign(Gtk::ALIGN_CENTER);
    m_entry_url.set_hexpand(true);
    m_entry_url.set_placeholder_text(_("https://… a .dat, .xml, .zip or .7z file"));
    auto commit_url = [this] {
        if (group().id != m_url_group) return;
        if (m_entry_url.get_text().raw() == group().url) return;
        group().url = m_entry_url.get_text().raw();
        save_groups();
        show_source_info();
        apply_source_ui();
    };
    m_entry_url.signal_activate().connect(commit_url);
    m_entry_url.signal_focus_out_event().connect([commit_url](GdkEventFocus*) { commit_url(); return false; });
    m_check_unzip.set_label(_("Unzip archives"));
    m_check_unzip.set_tooltip_text(_("Unpack the DAT files of a .zip or .7z into the folder. Unticked, the archive is kept as it is."));
    m_check_unzip.signal_toggled().connect([this] {
        if (m_source_filling || group().id != m_url_group) return;
        group().unzip = m_check_unzip.get_active();
        save_groups();
    });
    m_url_line.pack_start(*url_label, Gtk::PACK_SHRINK);
    m_url_line.pack_start(m_entry_url, Gtk::PACK_EXPAND_WIDGET);
    m_url_line.pack_start(m_check_unzip, Gtk::PACK_SHRINK);
    url_label->show();
    m_entry_url.show();
    m_check_unzip.show();
    body->pack_start(m_url_line, Gtk::PACK_SHRINK);

    // DAT site : les sites qui publient les DAT de l'emulateur du groupe,
    // pris a l'adresse de leurs auteurs (DatSource::sites()).
    m_site_line.set_spacing(8);
    m_site_line.set_no_show_all(true);
    auto* site_label = ui::sub_label(_("Site"));
    site_label->set_line_wrap(false);
    site_label->set_valign(Gtk::ALIGN_CENTER);
    m_combo_site.set_hexpand(true);
    m_combo_site.signal_changed().connect([this] {
        if (m_source_filling || group().id != m_url_group) return;
        const std::string id = m_combo_site.get_active_id().raw();
        if (id.empty() || id == group().site) return;
        group().site = id;
        save_groups();
        show_source_info();
        apply_source_ui();
    });
    m_site_line.pack_start(*site_label, Gtk::PACK_SHRINK);
    m_site_line.pack_start(m_combo_site, Gtk::PACK_EXPAND_WIDGET);
    site_label->show();
    m_combo_site.show();
    body->pack_start(m_site_line, Gtk::PACK_SHRINK);
    m_source_hint.set_xalign(0.0f);
    m_source_hint.set_line_wrap(true);
    m_source_hint.get_style_context()->add_class("set-sub");
    body->pack_start(m_source_hint, Gtk::PACK_SHRINK);
    card.body->pack_start(*body, Gtk::PACK_SHRINK);
    card.frame->set_size_request(440, -1);
    m_top.pack_start(*card.frame, Gtk::PACK_EXPAND_WIDGET);
}

void RomDatTab::build_table() {
    auto* middle = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, ui::kCardSpacing);

    m_store = Gtk::ListStore::create(m_cols);
    m_table = Gtk::make_managed<ui::Table>(Gtk::SELECTION_SINGLE);
    m_table->view().set_model(m_store);
    {
        auto* renderer = Gtk::make_managed<Gtk::CellRendererToggle>();
        renderer->set_activatable(true);
        renderer->signal_toggled().connect(sigc::mem_fun(*this, &RomDatTab::on_in_group_toggled));
        auto* col = Gtk::make_managed<Gtk::TreeViewColumn>(_("In group"), *renderer);
        col->add_attribute(renderer->property_active(), m_cols.in_group);
        m_table->view().append_column(*col);
    }
    {
        auto* renderer = Gtk::make_managed<Gtk::CellRendererText>();
        renderer->property_ellipsize() = Pango::ELLIPSIZE_MIDDLE;
        auto* col = Gtk::make_managed<Gtk::TreeViewColumn>(_("DAT file"), *renderer);
        col->add_attribute(renderer->property_text(), m_cols.file);
        // The names all share a long prefix : a bounded, resizable column
        // keeps the counts on screen at the window's default size.
        col->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
        col->set_fixed_width(290);
        col->set_resizable(true);
        col->set_sort_column(m_cols.file);
        col->set_cell_data_func(*renderer, [this, renderer](Gtk::CellRenderer*, const Gtk::TreeModel::iterator& it) {
            ensure_colours();
            if (!m_colours.ready) return;
            bool in = (*it)[m_cols.in_group], disk = (*it)[m_cols.on_disk];
            const Glib::ustring upd = (*it)[m_cols.update];
            Gdk::RGBA c = m_colours.muted;
            if (in) c = (disk && upd.empty()) ? m_colours.ok : m_colours.warn;
            renderer->property_foreground_rgba() = c;
        });
        m_table->view().append_column(*col);
    }
    m_table->add_text_column(_("System"), m_cols.system);
    { ui::ColumnOptions o; o.xalign = 1.0f; m_table->add_text_column(_("Sets"), m_cols.sets, o); }
    { ui::ColumnOptions o; o.xalign = 1.0f; m_table->add_text_column(_("ROM entries"), m_cols.roms, o); }
    m_table->add_text_column(_("Version / Date"), m_cols.version);
    { ui::ColumnOptions o; o.xalign = 1.0f; m_table->add_text_column(_("Size"), m_cols.size, o); }
    { ui::ColumnOptions o; o.expand = true; o.sortable = false; m_table->add_text_column(_("Update"), m_cols.update, o); }
    m_table->view().get_selection()->signal_changed().connect(sigc::mem_fun(*this, &RomDatTab::on_selection_changed));
    m_table->set_hexpand(true);
    middle->pack_start(*m_table, Gtk::PACK_EXPAND_WIDGET);

    // ── Right column : DAT information, Source information, preview ──────
    // Fixed width : the table is what needs the room, and a long value in the
    // panel must ellipsize, not push the table aside.
    m_info_column.set_size_request(360, -1);
    m_info_column.set_hexpand(false);
    auto info = ui::card("bc-info.svg", _("DAT information"), "");
    m_info_grid.set_column_spacing(14);
    m_info_grid.set_row_spacing(3);
    m_info_grid.set_hexpand(false);
    m_info_grid.set_margin_top(8);
    int r = 0;
    for (auto kv : {std::pair<const char*, const char*>{"file", N_("File name")}, {"group", N_("Group")}, {"system", N_("System")},
                    {"sets", N_("Sets")}, {"roms", N_("ROM entries")}, {"version", N_("Version")}, {"date", N_("Date")},
                    {"size", N_("File size")}, {"sha256", N_("SHA-256")}, {"path", N_("Path")}, {"source", N_("Source")}})
        grid_row(m_info_grid, r++, _(kv.second), m_info_values, kv.first);
    info.body->pack_start(m_info_grid, Gtk::PACK_SHRINK);
    m_btn_open_in_folder = ui::button(_("Open in folder"), "bc-external.svg");
    m_btn_open_in_folder->set_halign(Gtk::ALIGN_START);
    m_btn_open_in_folder->set_margin_top(8);
    m_btn_open_in_folder->signal_clicked().connect(sigc::mem_fun(*this, &RomDatTab::on_open_folder));
    info.body->pack_start(*m_btn_open_in_folder, Gtk::PACK_SHRINK);
    m_info_column.pack_start(*fold_card(info, "dat_info", [this] {
        return m_info_values["file"]->get_text().raw();
    }), Gtk::PACK_SHRINK);

    // How this DAT is read : a rule of its own, never one of the group's.
    auto rule = ui::card("bc-sliders.svg", _("How this DAT is read"), "");
    m_rule_grid.set_column_spacing(14);
    m_rule_grid.set_row_spacing(3);
    m_rule_grid.set_hexpand(false);
    m_rule_grid.set_margin_top(8);
    r = 0;
    for (auto kv : {std::pair<const char*, const char*>{"links", N_("Links between sets")}, {"declared", N_("Declared by the DAT")},
                    {"mode", N_("Read as")}})
        grid_row(m_rule_grid, r++, _(kv.second), m_rule_values, kv.first);
    rule.body->pack_start(m_rule_grid, Gtk::PACK_SHRINK);
    m_combo_merge.append(DatSource::kMergeSplit,     _("Split — a clone holds only its own ROMs"));
    m_combo_merge.append(DatSource::kMergeNonMerged, _("Non-merged — every set holds all it needs"));
    m_combo_merge.append(DatSource::kMergeMerged,    _("Merged — clones inside their parent's archive"));
    m_combo_merge.set_tooltip_text(_("How the linked sets of this DAT are laid out in its folder. Applies to this DAT only."));
    m_combo_merge.signal_changed().connect(sigc::mem_fun(*this, &RomDatTab::store_dat_rule));
    auto* merge_label = ui::title_label(_("Merge mode"));
    merge_label->set_valign(Gtk::ALIGN_CENTER);
    merge_label->show();   // its line is hidden from show_all : shown with it
    m_rule_merge_line.pack_start(*merge_label, Gtk::PACK_SHRINK);
    m_rule_merge_line.pack_start(m_combo_merge, Gtk::PACK_EXPAND_WIDGET);
    m_rule_merge_line.set_margin_top(8);
    m_rule_merge_line.set_no_show_all(true);
    rule.body->pack_start(m_rule_merge_line, Gtk::PACK_SHRINK);
    m_check_override.set_label(_("Use this mode instead of the one the DAT declares"));
    m_check_override.set_no_show_all(true);
    m_check_override.signal_toggled().connect(sigc::mem_fun(*this, &RomDatTab::store_dat_rule));
    rule.body->pack_start(m_check_override, Gtk::PACK_SHRINK);
    auto* folder_row = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    auto* dat_folder_label = ui::title_label(_("Folder"));
    dat_folder_label->set_valign(Gtk::ALIGN_CENTER);
    m_entry_dat_folder.set_tooltip_text(_("The folder of the library this DAT's sets belong in. Empty : named after the DAT's header."));
    m_entry_dat_folder.signal_changed().connect(sigc::mem_fun(*this, &RomDatTab::store_dat_rule));
    folder_row->pack_start(*dat_folder_label, Gtk::PACK_SHRINK);
    folder_row->pack_start(m_entry_dat_folder, Gtk::PACK_EXPAND_WIDGET);
    folder_row->set_margin_top(6);
    rule.body->pack_start(*folder_row, Gtk::PACK_SHRINK);
    m_info_column.pack_start(*fold_card(rule, "dat_rule", [this] {
        return m_rule_values["mode"]->get_text().raw();
    }), Gtk::PACK_SHRINK);

    auto src = ui::card("bc-cloud.svg", _("Source information"), "");
    m_source_grid.set_column_spacing(14);
    m_source_grid.set_row_spacing(3);
    m_source_grid.set_hexpand(false);
    m_source_grid.set_margin_top(8);
    r = 0;
    for (auto kv : {std::pair<const char*, const char*>{"kind", N_("Type")}, {"where", N_("Location")}, {"status", N_("Status")},
                    {"shared", N_("Shared with")}, {"last_check", N_("Last check")}, {"last_update", N_("Last update")}})
        grid_row(m_source_grid, r++, _(kv.second), m_source_values, kv.first);
    src.body->pack_start(m_source_grid, Gtk::PACK_SHRINK);
    m_info_column.pack_start(*fold_card(src, "dat_source", [this] {
        return m_source_values["kind"]->get_text().raw();
    }), Gtk::PACK_SHRINK);

    auto prev = ui::card("bc-file.svg", _("File preview (header)"), "");
    m_preview_buffer = Gtk::TextBuffer::create();
    m_preview.set_buffer(m_preview_buffer);
    m_preview.set_editable(false);
    m_preview.set_cursor_visible(false);
    m_preview.set_wrap_mode(Gtk::WRAP_NONE);
    m_preview.get_style_context()->add_class("set-mono");
    m_preview.get_style_context()->add_class("set-log");
    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    scroll->get_style_context()->add_class("set-log-frame");
    scroll->add(m_preview);
    scroll->set_margin_top(8);
    prev.body->pack_start(*scroll, Gtk::PACK_EXPAND_WIDGET);
    // Ouverte, elle prend la hauteur qui reste ; fermee, elle n'est plus que
    // son en-tete (refresh_folds).
    m_info_column.pack_start(*fold_card(prev, "dat_preview", [this] {
        return m_info_values["system"]->get_text().raw();
    }), Gtk::PACK_EXPAND_WIDGET);

    middle->pack_start(m_info_column, Gtk::PACK_SHRINK);
    m_main.pack_start(*middle, Gtk::PACK_EXPAND_WIDGET);
}

Gtk::Widget* RomDatTab::fold_card(const ui::Card& card, const std::string& key,
                                  std::function<std::string()> describe) {
    // Replies par defaut : ce sont des fiches de consultation, et ouvertes
    // elles poussaient l'onglet hors de la fenetre. Le choix du joueur, lui,
    // est retenu.
    bool open = false;
    {
        nlohmann::json j;
        std::ifstream in(AppContext::get_config_path());
        if (in) { try { in >> j; } catch (...) {} }
        if (j.is_object() && j.contains("rom_dat_sections") && j["rom_dat_sections"].is_object()
            && j["rom_dat_sections"].contains(key) && j["rom_dat_sections"][key].is_boolean())
            open = j["rom_dat_sections"][key].get<bool>();
    }
    m_folds[key] = ui::fold(card, open, std::move(describe), [this, key] {
        refresh_folds();
        // Ecrit au geste : cet ecran n'a pas de bouton Enregistrer.
        nlohmann::json j;
        const std::string path = AppContext::get_config_path();
        { std::ifstream in(path); if (in) { try { in >> j; } catch (...) {} } }
        if (!j.is_object()) return;
        j["rom_dat_sections"][key] = m_folds[key].body->get_reveal_child();
        std::ofstream out(path);
        if (out) out << j.dump(4);
    });
    return card.frame;
}

void RomDatTab::refresh_folds() {
    for (const auto& [key, f] : m_folds) {
        ui::refresh_fold(f);
        // L'apercu remplit la colonne quand il est ouvert ; ferme, il rend
        // la place, sans quoi son cadre resterait etire autour de rien.
        if (key == "dat_preview" && f.body) {
            auto* frame = f.body->get_parent();
            const bool open = f.body->get_reveal_child();
            if (frame) {
                m_info_column.child_property_expand(*frame) = open;
                m_info_column.child_property_fill(*frame) = open;
            }
        }
    }
}

void RomDatTab::build_footer() {
    m_footer_label.set_xalign(0.0f);
    m_footer_label.set_ellipsize(Pango::ELLIPSIZE_END);
    m_footer_label.get_style_context()->add_class("set-sub");
    m_footer.pack_start(m_footer_label, Gtk::PACK_EXPAND_WIDGET);
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
    m_footer.pack_end(m_actions, Gtk::PACK_SHRINK);
    m_footer.pack_end(*m_btn_cancel, Gtk::PACK_SHRINK);
    m_footer.pack_end(m_progress_label, Gtk::PACK_SHRINK);
    m_footer.pack_end(m_progress, Gtk::PACK_SHRINK);
    m_main.pack_start(m_footer, Gtk::PACK_SHRINK);
}

void RomDatTab::ensure_colours() {
    if (m_colours.ready || !get_toplevel() || !get_toplevel()->get_realized()) return;
    m_colours.ok    = ui::probe_color(*this, "set-ok");
    m_colours.warn  = ui::probe_color(*this, "set-warn");
    m_colours.err   = ui::probe_color(*this, "set-err");
    m_colours.muted = ui::probe_color(*this, "set-sub");
    m_colours.ready = true;
}

// ═══ Groups ═════════════════════════════════════════════════════════════════

void RomDatTab::save_groups() { DatSource::save_groups(m_groups); }

// Le seul endroit de l'ecran ou un emulateur est nomme. Tout le reste passe
// par le registre et par cette table : un troisieme emulateur capable
// d'ecrire ses DAT s'ajoute ici, et le bouton, l'infobulle et la fiche de
// source le suivent sans retouche.
const std::map<std::string, RomDatTab::Backend>& RomDatTab::backends() {
    // Construite a la premiere demande : les libelles passent par la
    // traduction, prete seulement apres l'initialisation de i18n.
    static const std::map<std::string, Backend> table = {
        {"fbneo", {
            [](RomDatTab& t) { return t.m_env().fbneo_executable; },
            // L'executable de FBNeo est un reglage, detenu par le proprietaire
            // de l'onglet : c'est lui qui lance et qui montre les dialogues.
            [](RomDatTab& t) { t.m_sig_generate.emit(t.group().folder); },
            N_("Run the installed %1 with -dat, writing its DAT files into the folder."),
            N_("No %1 executable configured : set it in Settings › Emulator."),
        }},
        {"mame", {
            // Le MAME des Settings, canal compris : les DAT generes decrivent
            // celui avec lequel on joue.
            [](RomDatTab&) { return MameSooner::resolve_executable(); },
            [](RomDatTab& t) { t.on_generate_mame(); },
            N_("Read the machine list from the installed %1 and write its DAT files into the folder."),
            N_("%1 was not found on this system : install it, then try again."),
        }},
    };
    return table;
}

const RomDatTab::Backend* RomDatTab::backend() const {
    auto it = backends().find(group().emulator);
    return it == backends().end() ? nullptr : &it->second;
}

std::string RomDatTab::executable_of(const std::string& emulator) {
    // Une seule recherche par session et par emulateur : trouver MAME coute
    // un `which`, et l'ecran redemande le chemin a chaque rafraichissement.
    auto cached = m_exe_cache.find(emulator);
    if (cached != m_exe_cache.end()) return cached->second;
    auto it = backends().find(emulator);
    std::string exe = (it == backends().end()) ? std::string() : it->second.locate(*this);
    m_exe_cache[emulator] = exe;
    return exe;
}

void RomDatTab::groups_changed() {
    rebuild_group_list();
    m_sig_groups.emit();
}

void RomDatTab::schedule_reload(const std::string& emulator) {
    // A reload already waiting for another emulator now covers both.
    if (!m_reload_timer.connected()) m_reload_emulator = emulator;
    else if (m_reload_emulator != emulator) m_reload_emulator.clear();
    m_reload_timer.disconnect();
    m_reload_timer = Glib::signal_timeout().connect([this] {
        m_sig_reload.emit(false, m_reload_emulator);
        return false;
    }, RELOAD_DEBOUNCE_MS);
}

std::string RomDatTab::emulator_of_group(const std::string& id) const {
    for (const auto& g : m_groups)
        if (g.id == id) return g.emulator;
    return "";
}

bool RomDatTab::reload_if_union_changed(const std::vector<std::string>& before,
                                        const std::string& emulator) {
    if (before == union_files()) return false;
    schedule_reload(emulator);
    return true;
}

void RomDatTab::select_group(size_t index) {
    if (index >= m_groups.size()) return;
    m_current = index;
    refresh();
}

// One row per group : its name, what it holds, whether it is active, and
// its own menu.
void RomDatTab::rebuild_group_list() {
    ui::destroy_children(m_group_list);
    for (size_t i = 0; i < m_groups.size(); ++i) {
        const auto& g = m_groups[i];
        auto* line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 9);
        line->get_style_context()->add_class("set-listrow");
        line->pack_start(*ui::tile("bc-file.svg", 15, 26), Gtk::PACK_SHRINK);
        auto* txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 1);
        txt->set_valign(Gtk::ALIGN_CENTER);
        auto* name = ui::title_label(g.name);
        name->set_ellipsize(Pango::ELLIPSIZE_END);
        txt->pack_start(*name, Gtk::PACK_SHRINK);
        // The second line : the state dot, how many files, how many sets.
        long sets = 0;
        auto selected = DatSource::selected_in_folder(g);
        for (const auto& f : selected) { auto st = m_stats.find(f); if (st != m_stats.end()) sets += st->second.games; }
        std::string sub = Glib::ustring::compose(selected.size() == 1 ? _("%1 file") : _("%1 files"), (int)selected.size()).raw();
        if (sets) sub += " · " + Glib::ustring::compose(sets == 1 ? _("%1 set") : _("%1 sets"), thousands(sets)).raw();
        // Deux groupes peuvent porter les memes noms de sets (mslug) : dire
        // quel emulateur chacun decrit evite de les confondre.
        sub += " · " + EmulatorRegistry::display_name(g.emulator);
        sub = (g.active ? std::string(_("Active")) : std::string(_("Inactive"))) + " · " + sub;
        auto* state = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 6);
        auto* dot = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 0);
        dot->get_style_context()->add_class("set-dot");
        dot->get_style_context()->add_class(g.active ? "set-ok" : "set-off");
        dot->set_valign(Gtk::ALIGN_CENTER);
        dot->set_tooltip_text(g.active ? _("Active : its DAT files are loaded and it can be audited.")
                                       : _("Inactive : loads nothing, not offered for audit."));
        // Un seul groupe actif par emulateur : l'interrupteur en allume un et
        // eteint l'autre. Celui qui est allume ne s'eteint pas seul : il faut
        // toujours un groupe pour juger la collection de cet emulateur.
        auto* sw = Gtk::make_managed<Gtk::Switch>();
        sw->set_active(g.active);
        sw->set_valign(Gtk::ALIGN_CENTER);
        sw->set_sensitive(!g.active && !m_busy);
        sw->set_tooltip_text(g.active
            ? Glib::ustring::compose(_("The active %1 group. Turn another %1 group on to replace it."),
                                     EmulatorRegistry::display_name(g.emulator)).raw()
            : Glib::ustring::compose(_("Use this group for %1 : the other %1 group is turned off."),
                                     EmulatorRegistry::display_name(g.emulator)).raw());
        sw->property_active().signal_changed().connect([this, i, sw] {
            if (!sw->get_active()) return;
            // Hors du signal : on reconstruit la liste qui porte l'interrupteur.
            Glib::signal_idle().connect_once([this, i] { on_toggle_group_active(i); });
        });
        state->pack_start(*dot, Gtk::PACK_SHRINK);
        auto* subl = ui::sub_label(sub);
        subl->set_line_wrap(false);
        subl->set_ellipsize(Pango::ELLIPSIZE_END);
        state->pack_start(*subl, Gtk::PACK_SHRINK);
        txt->pack_start(*state, Gtk::PACK_SHRINK);
        line->pack_start(*txt, Gtk::PACK_EXPAND_WIDGET);

        auto* more = Gtk::make_managed<Gtk::MenuButton>();
        more->add(*ui::image("more.svg", 14));
        more->set_valign(Gtk::ALIGN_CENTER);
        more->get_style_context()->add_class("set-rowmenu");
        more->set_tooltip_text(_("Group actions"));
        line->pack_start(*sw, Gtk::PACK_SHRINK);
        auto* menu = Gtk::make_managed<Gtk::Menu>();
        auto* rename = Gtk::make_managed<Gtk::MenuItem>(_("Rename…"));
        rename->signal_activate().connect([this, i] { on_rename_group(i); });
        auto* del = Gtk::make_managed<Gtk::MenuItem>(_("Delete group…"));
        del->set_sensitive(m_groups.size() > 1);
        del->signal_activate().connect([this, i] { on_delete_group(i); });
        menu->append(*rename);
        menu->append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());
        menu->append(*del);
        menu->show_all();
        more->set_popup(*menu);
        line->pack_start(*more, Gtk::PACK_SHRINK);
        auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
        row->add(*line);
        m_group_list.append(*row);
    }
    m_group_list.show_all();
    if (auto* row = m_group_list.get_row_at_index((int)m_current)) m_group_list.select_row(*row);
}

void RomDatTab::on_add_group() {
    std::string name;
    auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (!prompt_name(top, _("Add a DAT group"),
                     _("It starts with the current group's emulator, source and folder, and every DAT file of it. Change its emulator or its folder in its card, and untick the DAT files it should not use."),
                     name)) return;
    DatSource::Group g;
    g.id        = DatSource::make_id(name, m_groups);
    g.name      = name;
    g.folder    = group().folder;
    g.source    = group().source;
    g.emulator  = group().emulator;
    g.url       = group().url;
    g.rules     = group().rules;   // same folder, same DATs : each keeps its rule
    g.all_files = true;
    // Le groupe courant reste celui de son emulateur : le nouveau nait eteint,
    // son interrupteur le met en service.
    g.active    = false;
    auto before = union_files();
    m_groups.push_back(std::move(g));
    save_groups();
    m_current = m_groups.size() - 1;
    refresh();
    m_sig_groups.emit();
    flash(Glib::ustring::compose(_("Group \"%1\" created, turned off : set it up, then turn it on to use it instead of the current one."), name));
    // Every file of the source : a file no other group loaded joins the
    // database.
    reload_if_union_changed(before, group().emulator);
}

void RomDatTab::on_rename_group(size_t index) {
    if (index >= m_groups.size()) return;
    std::string name = m_groups[index].name;
    auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (!prompt_name(top, _("Rename the group"), "", name) || name == m_groups[index].name) return;
    m_groups[index].name = name;
    save_groups();
    groups_changed();
    if (index == m_current) m_group_card.title->set_text(name);
}

void RomDatTab::on_delete_group(size_t index) {
    if (index >= m_groups.size() || m_groups.size() <= 1) return;
    auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (top) {
        ConfirmationDialog confirm(*top, _("Delete this DAT group?"),
            Glib::ustring::compose(_("\"%1\" will be removed. Its DAT files stay on disk : only the selection is deleted."), m_groups[index].name), "bc-trash.svg");
        if (!confirm.show_and_confirm()) return;
    }
    auto before = union_files();
    const std::string erased_emulator = m_groups[index].emulator;
    m_groups.erase(m_groups.begin() + (long)index);
    if (m_current >= m_groups.size()) m_current = m_groups.size() - 1;
    else if (index < m_current) --m_current;
    // Le groupe actif supprime : un autre du meme emulateur prend le relais.
    DatSource::one_active_per_emulator(m_groups);
    save_groups();
    refresh();
    m_sig_groups.emit();
    reload_if_union_changed(before, erased_emulator);
}

void RomDatTab::on_toggle_group_active(size_t index) {
    if (index >= m_groups.size() || m_groups[index].active) return;
    auto before = union_files();
    std::string replaced;
    for (const auto& g : m_groups)
        if (g.active && g.emulator == m_groups[index].emulator) replaced = g.name;
    DatSource::one_active_per_emulator(m_groups, index);
    save_groups();
    refresh();
    groups_changed();
    const bool reloads = reload_if_union_changed(before, m_groups[index].emulator);
    const auto& name = m_groups[index].name;
    flash(replaced.empty()
        ? Glib::ustring::compose(_("\"%1\" is active."), name)
        : reloads ? Glib::ustring::compose(_("\"%1\" is active, \"%2\" is turned off. The database reloads in a moment…"), name, replaced)
                  : Glib::ustring::compose(_("\"%1\" is active, \"%2\" is turned off."), name, replaced));
}

// ═══ Source ═════════════════════════════════════════════════════════════════

// The buttons say what the chosen source can do, and nothing else.
void RomDatTab::apply_source_ui() {
    const auto& g = group();
    switch (g.source) {
        case DatSource::Kind::Emulator: {
            // Un seul bouton : l'emulateur du groupe dit quoi lancer, et la
            // table des backends dit ce qu'il faut avoir installe.
            const Glib::ustring name = EmulatorRegistry::display_name(g.emulator);
            const Backend* b = backend();
            const std::string exe = b ? executable_of(g.emulator) : std::string();
            m_btn_primary->set_label(Glib::ustring::compose(_("Generate from %1"), name));
            m_btn_primary->set_image(*ui::image("bc-generate-dat.svg", ui::kIconButton));
            m_btn_primary->set_sensitive(!m_busy && !exe.empty());
            const Glib::ustring unavailable =
                b ? Glib::ustring::compose(_(b->missing), name)
                  : Glib::ustring::compose(_("%1 does not generate its own DAT files : use an HTTP source or a local folder."), name);
            m_btn_primary->set_tooltip_text(exe.empty() ? unavailable
                                                        : Glib::ustring::compose(_(b->ready), name));
            m_btn_check->hide();
            m_url_line.hide();
            m_site_line.hide();
            m_source_hint.set_text(exe.empty() ? unavailable
                                               : Glib::ustring::compose(_("Executable: %1"), exe));
            break;
        }
        case DatSource::Kind::Http: {
            const std::string url = DatSource::manifest_url(g);
            m_btn_primary->set_label(_("Download DATs"));
            m_btn_primary->set_image(*ui::image("bc-download.svg", ui::kIconButton));
            m_btn_primary->set_sensitive(!m_busy && !url.empty());
            m_btn_primary->set_tooltip_text(_("Fetch the manifest and download this group's DAT files that are missing here or differ (SHA-256), each verified before it replaces the local one."));
            m_btn_check->show();
            m_btn_check->set_sensitive(!m_busy && !url.empty());
            m_url_line.hide();
            m_site_line.hide();
            m_source_hint.set_text(url.empty()
                ? Glib::ustring::compose(_("The Bootcade server publishes no DAT files for %1."),
                                         EmulatorRegistry::display_name(g.emulator)).raw()
                : g.emulator == "mame"
                ? std::string(_("The MAME DAT files of the version chosen in Settings › Emulator (official release or Sooner). Changes are detected by SHA-256."))
                : std::string(_("The server publishes dat-manifest.json next to the files. Changes are detected by SHA-256; version and date are informative.")));
            break;
        }
        case DatSource::Kind::Url:
            m_btn_primary->set_label(_("Download"));
            m_btn_primary->set_image(*ui::image("bc-download.svg", ui::kIconButton));
            m_btn_primary->set_sensitive(!m_busy && !g.url.empty() && !g.folder.empty());
            m_btn_primary->set_tooltip_text(_("Download the file at this address into the folder, then tick the DAT files this group uses."));
            m_btn_check->hide();
            m_url_line.show();
            m_site_line.hide();
            m_source_hint.set_text(_("A DAT file is saved as it is. An archive is unpacked when Unzip archives is ticked."));
            break;
        case DatSource::Kind::Site: {
            // Seuls les sites qui publient des DAT pour l'emulateur du groupe.
            m_source_filling = true;
            m_combo_site.remove_all();
            for (const auto& site : DatSource::sites())
                if (g.emulator == site.emulator) m_combo_site.append(site.label, _(site.label));
            if (!m_combo_site.set_active_id(g.site)) m_combo_site.set_active(-1);
            m_source_filling = false;
            const bool chosen = !m_combo_site.get_active_id().empty();
            m_btn_primary->set_label(_("Download"));
            m_btn_primary->set_image(*ui::image("bc-download.svg", ui::kIconButton));
            m_btn_primary->set_sensitive(!m_busy && chosen && !g.folder.empty());
            m_btn_primary->set_tooltip_text(_("Download DAT files from the site that publishes them, unpack them into the folder, then tick the ones this group uses."));
            m_btn_check->hide();
            m_url_line.hide();
            m_site_line.show();
            m_source_hint.set_text(chosen ? _("Fetched from the author's own address, at its newest version. Each file keeps its source on screen.")
                                          : _("Choose the site to download from."));
            break;
        }
        default:
            m_btn_primary->set_label(_("Rescan folder"));
            m_btn_primary->set_image(*ui::image("bc-sync.svg", ui::kIconButton));
            m_btn_primary->set_sensitive(!m_busy);
            m_btn_primary->set_tooltip_text(_("Re-read the folder and reload the database from the DAT files it holds."));
            m_btn_check->hide();
            m_url_line.hide();
            m_site_line.hide();
            m_source_hint.set_text(_("Put DAT files in the folder yourself, or use Add DAT files…, then rescan."));
            break;
    }
    m_btn_primary->set_always_show_image(true);
}

void RomDatTab::on_source_changed() {
    DatSource::Kind kind = m_radio_emulator.get_active() ? DatSource::Kind::Emulator
                         : m_radio_http.get_active()     ? DatSource::Kind::Http
                         : m_radio_url.get_active()      ? DatSource::Kind::Url
                         : m_radio_site.get_active()     ? DatSource::Kind::Site
                                                         : DatSource::Kind::Folder;
    if (kind == group().source) return;
    group().source = kind;
    save_groups();
    apply_source_ui();
    show_source_info();
    groups_changed();
}

void RomDatTab::on_browse_folder() {
    auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
    Gtk::FileChooserDialog dlg(_("Select the DAT folder"), Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
    if (top) dlg.set_transient_for(*top);
    dlg.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dlg.add_button(_("Select"), Gtk::RESPONSE_OK);
    if (!group().folder.empty()) dlg.set_filename(group().folder);
    if (dlg.run() != Gtk::RESPONSE_OK) return;
    if (dlg.get_filename() == group().folder) return;
    group().folder = dlg.get_filename();
    m_entry_folder.set_text(group().folder);
    save_groups();
    if (m_current == 0) m_sig_folder.emit(group().folder);
    refresh();
    groups_changed();
}

void RomDatTab::on_add_files() {
    auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (group().folder.empty()) { if (top) ui::notice(*top, _("No DAT folder"), _("Choose the group's folder first.")); return; }
    Gtk::FileChooserDialog dlg(_("Add DAT files"), Gtk::FILE_CHOOSER_ACTION_OPEN);
    if (top) dlg.set_transient_for(*top);
    dlg.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dlg.add_button(_("Add"), Gtk::RESPONSE_OK);
    dlg.set_select_multiple(true);
    auto filter = Gtk::FileFilter::create();
    filter->set_name(_("DAT files"));
    filter->add_pattern("*.dat");
    // A Logiqx DAT saved as .xml (Pleasuredome), or a raw MAME -listxml file
    // (progettosnaps) : DatParser tells them apart by their root.
    filter->add_pattern("*.xml");
    dlg.add_filter(filter);
    if (dlg.run() != Gtk::RESPONSE_OK) return;
    std::error_code ec;
    fs::create_directories(group().folder, ec);
    int copied = 0;
    std::vector<std::string> provided = provided_files();
    for (const auto& f : dlg.get_filenames()) {
        fs::path src(f), dest = fs::path(group().folder) / src.filename();
        if (fs::exists(dest, ec) && fs::equivalent(src, dest, ec)) continue;
        fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) { push_log("could not copy " + f + ": " + ec.message()); ec.clear(); continue; }
        ++copied;
        // A file added by hand is meant for this group.
        const std::string name = src.filename().string();
        if (std::find(provided.begin(), provided.end(), name) == provided.end()) provided.push_back(name);
        group().set_selected(name, true, provided);
    }
    if (copied == 0) { flash(_("No file added.")); return; }
    group().last_update = DatSource::now_iso();
    save_groups();
    refresh();
    m_sig_groups.emit();
    flash(Glib::ustring::compose(_("%1 DAT file(s) added. Reloading the database…"), copied));
    m_sig_reload.emit(false, group().emulator);
}

void RomDatTab::on_primary_action() {
    switch (group().source) {
        // Quoi lancer se lit dans l'emulateur du groupe, jamais dans le type
        // de source : la table des backends porte le geste de chacun.
        case DatSource::Kind::Emulator:
            if (const Backend* b = backend()) b->generate(*this);
            break;
        case DatSource::Kind::Http:     on_download(); break;
        case DatSource::Kind::Url:      on_download_url(); break;
        case DatSource::Kind::Site:
            for (size_t i = 0; i < DatSource::sites().size(); ++i)
                if (group().site == DatSource::sites()[i].label && group().emulator == DatSource::sites()[i].emulator)
                    on_download_site(i);
            break;
        default:                        on_rescan(); break;
    }
}

void RomDatTab::on_generate_mame() {
    auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (!top) return;
    if (group().folder.empty()) {
        ui::notice(*top, _("No DAT folder"), _("Choose the group's folder first."));
        return;
    }
    // GenerateDAT tient la fenetre de progression et ses dialogues ; quand il
    // rend la main, le dossier a change et la base suit, comme apres une
    // generation depuis FBNeo.
    GenerateDAT::execute_mame(*top, executable_of("mame"), group().folder, nullptr);
    refresh();
    m_sig_reload.emit(false, group().emulator);
}

void RomDatTab::on_rescan() {
    refresh();
    if (m_items.empty()) { flash(_("The folder holds no DAT file.")); return; }
    group().last_update = DatSource::now_iso();
    save_groups();
    flash(_("Reloading the database from the folder…"));
    m_sig_reload.emit(false, group().emulator);
}

void RomDatTab::on_check_updates() {
    if (m_busy || DatSource::manifest_url(group()).empty()) return;
    m_job_url = DatSource::manifest_url(group());
    m_job_folder = group().folder;
    m_job_group_id = group().id;
    m_job_group = group();
    m_job_error.clear();
    m_cancelled = false;
    m_job = Job::Check;
    set_busy(true);
    m_progress_label.set_text(_("Checking…"));
    m_worker = std::thread(&RomDatTab::worker_check, this);
}

void RomDatTab::on_download() {
    if (m_busy || DatSource::manifest_url(group()).empty()) return;
    if (group().folder.empty()) {
        auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
        if (top) ui::notice(*top, _("No DAT folder"), _("Choose the group's folder first."));
        return;
    }
    m_job_url = DatSource::manifest_url(group());
    m_job_folder = group().folder;
    m_job_group_id = group().id;
    m_job_group = group();
    m_job_error.clear();
    m_job_downloaded = m_job_failed = 0;
    m_cancelled = false;
    m_job = Job::Download;
    set_busy(true);
    m_progress_label.set_text(_("Downloading…"));
    m_worker = std::thread(&RomDatTab::worker_download, this);
}

std::vector<std::string> RomDatTab::provided_files() const {
    std::set<std::string> names;
    for (const auto& f : DatSource::list_folder(group().folder)) names.insert(f);
    if (m_last_compare_group == group().id)
        for (const auto& c : m_last_compare) if (c.state != DatSource::State::LocalOnly) names.insert(c.name);
    return std::vector<std::string>(names.begin(), names.end());
}

void RomDatTab::on_in_group_toggled(const Glib::ustring& path) {
    auto it = m_store->get_iter(path);
    if (!it) return;
    bool now = !(*it)[m_cols.in_group];
    (*it)[m_cols.in_group] = now;
    const std::string name = Glib::ustring((*it)[m_cols.file]).raw();
    const bool on_disk = (*it)[m_cols.on_disk];
    auto before = union_files();
    group().set_selected(name, now, provided_files());
    unsigned int idx = (*it)[m_cols.index];
    if (idx < m_items.size()) m_items[idx].in_group = now;
    save_groups();
    update_summary();
    show_file_info(idx < m_items.size() ? (int)idx : -1);
    groups_changed();
    // A file on disk may change what the database holds; one not
    // downloaded yet only changes what Download will fetch.
    if (on_disk && reload_if_union_changed(before, group().emulator)) {
        flash(now ? Glib::ustring::compose(_("%1 joins the group. The database reloads in a moment…"), name)
                  : Glib::ustring::compose(_("%1 leaves the group. The database reloads in a moment…"), name));
    } else if (on_disk) {
        flash(now ? Glib::ustring::compose(_("%1 joins the group (already loaded for another group)."), name)
                  : Glib::ustring::compose(_("%1 leaves the group (another group keeps it loaded)."), name));
    } else {
        flash(now ? Glib::ustring::compose(_("%1 joins the group : Download DATs will fetch it."), name)
                  : Glib::ustring::compose(_("%1 will not be downloaded for this group."), name));
    }
}

void RomDatTab::on_open_folder() {
    std::error_code ec;
    if (group().folder.empty() || !fs::is_directory(group().folder, ec)) { flash(_("No DAT folder to open.")); return; }
    try { Gio::AppInfo::launch_default_for_uri(Glib::filename_to_uri(group().folder)); }
    catch (const Glib::Error& e) { flash(Glib::ustring::compose(_("Could not open the folder: %1"), e.what())); }
}

// ═══ Content ════════════════════════════════════════════════════════════════

void RomDatTab::refresh() {
    if (m_busy) return;
    // Le MAME retenu peut avoir change dans les Settings (canal Sooner).
    m_exe_cache.clear();
    // Re-read : the groups (config.json), the folder, the database.
    std::string keep_id = m_groups.empty() ? "" : group().id;
    m_groups = DatSource::load_groups();
    if (m_groups.empty()) return;   // load_groups always builds one; belt and braces
    if (m_current >= m_groups.size()) m_current = 0;
    for (size_t i = 0; i < m_groups.size(); ++i) if (m_groups[i].id == keep_id) m_current = i;
    m_stats = m_db->getDatFileStats();
    rebuild_group_list();

    const auto& g = group();
    m_group_card.title->set_text(g.name);
    m_entry_folder.set_text(g.folder);
    m_url_group = g.id;
    m_source_filling = true;
    m_entry_url.set_text(g.url);
    m_check_unzip.set_active(g.unzip);
    m_source_filling = false;
    // Un groupe peut porter un emulateur que cette version ignore : mieux
    // vaut laisser le selecteur vide que lui en faire dire un autre.
    if (!m_combo_emulator.set_active_id(g.emulator)) m_combo_emulator.set_active(-1);
    // Radios follow the model without re-entering on_source_changed.
    switch (g.source) {
        case DatSource::Kind::Emulator: m_radio_emulator.set_active(true); break;
        case DatSource::Kind::Http:     m_radio_http.set_active(true); break;
        case DatSource::Kind::Url:      m_radio_url.set_active(true); break;
        case DatSource::Kind::Site:     m_radio_site.set_active(true); break;
        default:                        m_radio_folder.set_active(true); break;
    }

    m_items.clear();
    std::error_code ec;
    const bool compared = (m_last_compare_group == g.id);
    for (const auto& name : DatSource::list_folder(g.folder)) {
        Item item;
        item.name = name;
        item.path = (fs::path(g.folder) / name).string();
        item.size = fs::file_size(item.path, ec);
        item.in_group = g.selects(name);
        item.on_disk = true;
        DatSource::Header h = DatSource::read_header(item.path, 14);
        item.header_name = h.name;
        item.version = h.version;
        item.date = h.date.empty() ? file_mtime_iso(item.path) : h.date;
        item.system = system_of_header(h.name);
        auto st = m_stats.find(item.name);
        if (st != m_stats.end()) {
            item.games    = st->second.games;
            item.roms     = st->second.roms;
            item.linked   = st->second.linked;
            item.declared = st->second.declared;
        }
        // What the last Check said about it, when one was made for this group.
        if (compared)
            for (const auto& c : m_last_compare)
                if (c.name == item.name) { item.remote_known = true; item.remote_state = c.state; item.remote_version = c.remote_version; item.remote_date = c.remote_date; item.sha256 = c.local_sha256; }
        m_items.push_back(std::move(item));
    }
    // Files the server has and the folder lacks : rows too, so they can be
    // taken into the group and downloaded.
    if (compared) {
        for (const auto& c : m_last_compare) {
            if (c.state != DatSource::State::Missing) continue;
            Item item;
            item.name = c.name;
            item.in_group = g.selects(c.name);
            item.on_disk = false;
            item.size = c.remote_size;
            item.version = c.remote_version;
            item.date = c.remote_date;
            item.remote_known = true;
            item.remote_state = c.state;
            m_items.push_back(std::move(item));
        }
    }
    populate();
    apply_source_ui();
    show_source_info();
}

void RomDatTab::populate() {
    m_store->clear();
    for (size_t i = 0; i < m_items.size(); ++i) {
        const auto& it = m_items[i];
        auto row = *(m_store->append());
        row[m_cols.in_group] = it.in_group;
        row[m_cols.on_disk]  = it.on_disk;
        row[m_cols.file]     = it.name;
        row[m_cols.system]   = it.system;
        row[m_cols.sets]     = it.games ? thousands(it.games) : "";
        row[m_cols.roms]     = it.roms ? thousands(it.roms) : "";
        row[m_cols.version]  = it.version + (it.date.empty() ? "" : "  ·  " + short_date(it.date));
        row[m_cols.size]     = human_size(it.size);
        Glib::ustring update;
        if (!it.on_disk) update = it.in_group ? _("not downloaded yet") : _("on the server, not in this group");
        else if (it.remote_known) {
            switch (it.remote_state) {
                case DatSource::State::Outdated:  update = Glib::ustring::compose(_("update available (%1, %2)"), it.remote_version, short_date(it.remote_date)); break;
                case DatSource::State::LocalOnly: update = _("not on the server"); break;
                default: break;
            }
        }
        row[m_cols.update] = update;
        row[m_cols.index]  = (unsigned int)i;
    }
    update_summary();
    show_file_info(-1);
}

void RomDatTab::update_summary() {
    int in = 0, on_disk = 0; long sets = 0, roms = 0;
    for (const auto& it : m_items) {
        if (!it.on_disk) continue;
        ++on_disk;
        if (it.in_group) { ++in; sets += it.games; roms += it.roms; }
    }
    m_footer_label.set_text(Glib::ustring::compose(_("%1 of %2 DAT files in this group  ·  %3 sets  ·  %4 ROM entries"),
                                                   in, on_disk, thousands(sets), thousands(roms)));
    m_group_sub.set_text(group().active ? Glib::ustring::compose(in == 1 ? _("%1 DAT file") : _("%1 DAT files"), in)
                                        : Glib::ustring(_("inactive group")));
    int updates = 0;
    const bool compared = (m_last_compare_group == group().id);
    if (compared)
        for (const auto& c : m_last_compare)
            if (group().selects(c.name) && (c.state == DatSource::State::Outdated || c.state == DatSource::State::Missing)) ++updates;
    if (m_items.empty()) m_status.set_text(_("The folder holds no DAT file yet."));
    else if (compared) m_status.set_text(updates ? Glib::ustring::compose(_("%1 update(s) available on the server for this group."), updates)
                                                 : Glib::ustring(_("Every DAT file of this group is up to date with the server.")));
    else m_status.set_text(Glib::ustring::compose(_("%1 DAT file(s) in the folder, %2 in this group."), on_disk, in));
}

void RomDatTab::on_selection_changed() {
    auto it = m_table->view().get_selection()->get_selected();
    if (!it) { show_file_info(-1); return; }
    unsigned int idx = (*it)[m_cols.index];
    show_file_info(idx < m_items.size() ? (int)idx : -1);
}

void RomDatTab::show_file_info(int index) {
    auto set = [&](const char* key, const std::string& v) { m_info_values[key]->set_text(v); };
    show_dat_rule(index);
    if (index < 0 || index >= (int)m_items.size()) {
        for (auto& [k, l] : m_info_values) l->set_text("");
        m_preview_buffer->set_text("");
        m_btn_open_in_folder->set_sensitive(false);
        refresh_folds();
        return;
    }
    const auto& it = m_items[index];
    set("file", it.name);
    set("group", group().name + (it.in_group ? "" : "  (" + std::string(_("not in this group")) + ")"));
    set("system", it.header_name.empty() ? it.system : it.header_name);
    set("sets", it.games ? thousands(it.games) : std::string(it.on_disk ? _("not loaded") : _("not downloaded")));
    set("roms", it.roms ? thousands(it.roms) : "");
    set("version", it.version);
    set("date", short_date(it.date));
    set("size", human_size(it.size) + "  (" + thousands((long)it.size) + " B)");
    std::string sum = !it.on_disk ? "" : (it.sha256.empty() ? DatSource::sha256_of(it.path) : it.sha256);
    set("sha256", sum.empty() ? "" : sum.substr(0, 16) + "…");
    m_info_values["sha256"]->set_tooltip_text(sum);
    set("path", it.on_disk ? it.path : std::string(_("not on disk")));
    set("source", group().source == DatSource::Kind::Emulator
                      ? Glib::ustring::compose(_("Generated by %1"), EmulatorRegistry::display_name(group().emulator)).raw()
                  : group().source == DatSource::Kind::Http ? std::string(_("Downloaded from the Bootcade server"))
                  : !DatSource::source_of(group().folder, it.name).empty()
                      ? Glib::ustring::compose(_("Downloaded from %1"), DatSource::source_of(group().folder, it.name)).raw()
                      : std::string(_("Local folder")));
    std::string preview;
    if (it.on_disk) for (const auto& l : DatSource::read_header(it.path, 14).preview) preview += l + "\n";
    m_preview_buffer->set_text(preview);
    m_btn_open_in_folder->set_sensitive(it.on_disk);
    refresh_folds();
}

// A merge mode, as the screen names it.
static std::string merge_label(const std::string& mode) {
    if (mode == DatSource::kMergeSplit)     return _("Split");
    if (mode == DatSource::kMergeNonMerged) return _("Non-merged");
    if (mode == DatSource::kMergeMerged)    return _("Merged");
    return "";
}

void RomDatTab::show_dat_rule(int index) {
    m_rule_filling = true;
    m_rule_item = index;
    const bool known = index >= 0 && index < (int)m_items.size();
    const Item* it = known ? &m_items[index] : nullptr;
    const DatSource::DatRule* rule = nullptr;
    if (it) {
        auto r = group().rules.find(it->name);
        if (r != group().rules.end()) rule = &r->second;
    }
    auto set = [&](const char* key, const std::string& v) { m_rule_values[key]->set_text(v); };
    if (!it || !it->games) {
        // Not loaded yet : what the DAT holds is not known.
        set("links", it ? std::string(_("known once the DAT is loaded")) : std::string());
        set("declared", "");
        set("mode", "");
    } else {
        DatSource::DatTraits traits{it->linked, it->declared};
        const std::string mode = DatSource::effective_merge(traits, rule);
        set("links", it->linked ? std::string(_("parents, shared ROMs, devices"))
                                : std::string(_("none : every set is read as it is")));
        set("declared", it->declared.empty() ? std::string(_("nothing")) : merge_label(it->declared));
        set("mode", mode.empty() ? std::string(_("as it is")) : merge_label(mode));
    }
    const bool linked = it && it->games && it->linked;
    m_rule_merge_line.set_visible(linked);
    m_combo_merge.set_visible(linked);
    m_check_override.set_visible(linked && !it->declared.empty());
    // What the DAT is read with : the mode it declares, unless overridden.
    const bool overridden = rule && rule->override_dat;
    const bool declares   = it && !it->declared.empty();
    m_combo_merge.set_active_id(declares && !overridden ? it->declared
                                : rule && !rule->merge.empty() ? rule->merge
                                : std::string(DatSource::kMergeSplit));
    m_check_override.set_active(overridden);
    // With a mode the DAT declares, the choice only counts when overriding.
    m_combo_merge.set_sensitive(linked && (!declares || overridden));
    m_entry_dat_folder.set_sensitive(it != nullptr);
    m_entry_dat_folder.set_text(rule ? rule->folder : std::string());
    m_entry_dat_folder.set_placeholder_text(it ? it->header_name : std::string());
    m_rule_filling = false;
}

void RomDatTab::store_dat_rule() {
    if (m_rule_filling || m_rule_item < 0 || m_rule_item >= (int)m_items.size()) return;
    const Item& it = m_items[m_rule_item];
    DatSource::DatRule& rule = group().rules[it.name];
    // The list shows the DAT's own mode while it is not overridden : only a
    // choice the user can make is kept as theirs.
    if (m_combo_merge.get_sensitive()) rule.merge = m_combo_merge.get_active_id().raw();
    rule.override_dat = m_check_override.get_active();
    rule.folder       = m_entry_dat_folder.get_text().raw();
    save_groups();
    show_dat_rule(m_rule_item);
    m_sig_groups.emit();
}

void RomDatTab::show_source_info() {
    const auto& g = group();
    auto set = [&](const char* key, const std::string& v) { m_source_values[key]->set_text(v); };
    switch (g.source) {
        case DatSource::Kind::Emulator: {
            const Glib::ustring name = EmulatorRegistry::display_name(g.emulator);
            const Backend* b = backend();
            const std::string exe = b ? executable_of(g.emulator) : std::string();
            set("kind", Glib::ustring::compose(_("%1 executable"), name).raw());
            set("where", exe.empty() ? std::string(_("not found")) : exe);
            set("status", !b  ? std::string(_("Cannot generate DAT files"))
                       : exe.empty() ? std::string(_("Not available"))
                                     : std::string(_("Available")));
            break;
        }
        case DatSource::Kind::Http:
            set("kind", _("Bootcade server (dat-manifest.json)"));
            set("where", DatSource::manifest_url(g));
            set("status", DatSource::manifest_url(g).empty() ? std::string(_("No URL"))
                        : (m_last_compare_group == g.id && !m_last_manifest_generated.empty()
                           ? Glib::ustring::compose(_("Manifest generated %1"), short_date(m_last_manifest_generated)).raw()
                           : std::string(_("Not checked this session"))));
            break;
        case DatSource::Kind::Url:
            set("kind", _("Custom URL"));
            set("where", g.url);
            set("status", g.url.empty() ? std::string(_("No URL"))
                        : g.unzip ? std::string(_("Archives unzipped")) : std::string(_("Archives kept as they are")));
            break;
        case DatSource::Kind::Site:
            set("kind", _("DAT site"));
            set("where", g.site.empty() ? std::string(_("No site chosen")) : std::string(_(g.site.c_str())));
            set("status", g.site.empty() ? std::string(_("Not available")) : std::string(_("Available")));
            break;
        default:
            set("kind", _("Local folder"));
            set("where", g.folder);
            set("status", g.folder.empty() ? _("No folder") : _("Ready"));
            break;
    }
    // The other groups drawing on the same source : same folder, same kind,
    // same server when there is one.
    std::string shared;
    for (const auto& o : m_groups) {
        if (o.id == g.id || o.folder != g.folder || o.source != g.source) continue;
        if (g.source == DatSource::Kind::Http && DatSource::manifest_url(o) != DatSource::manifest_url(g)) continue;
        shared += (shared.empty() ? "" : ", ") + o.name;
    }
    set("shared", shared.empty() ? std::string(_("no other group")) : shared);
    set("last_check", g.last_check.empty() ? _("never") : short_date(g.last_check));
    set("last_update", g.last_update.empty() ? _("never") : short_date(g.last_update));
    refresh_folds();
}

// ═══ Worker ═════════════════════════════════════════════════════════════════

void RomDatTab::worker_check() {
    DatSource::Manifest m;
    std::string err;
    push_progress(10.0, _("Fetching the manifest…"));
    if (!DatSource::fetch_manifest(m_job_url, m, err)) {
        m_job_error = err;
        m_finished_dispatcher();
        return;
    }
    push_progress(40.0, _("Comparing with the folder…"));
    auto cmp = DatSource::compare(m_job_folder, m);
    {
        std::lock_guard<std::mutex> lk(m_shared_mutex);
        m_job_manifest = std::move(m);
        m_last_compare = std::move(cmp);
        m_last_compare_group = m_job_group_id;
    }
    push_progress(100.0, _("Done."));
    m_finished_dispatcher();
}

void RomDatTab::worker_download() {
    DatSource::Manifest m;
    std::string err;
    push_progress(2.0, _("Fetching the manifest…"));
    if (!DatSource::fetch_manifest(m_job_url, m, err)) {
        m_job_error = err;
        m_finished_dispatcher();
        return;
    }
    auto cmp = DatSource::compare(m_job_folder, m);
    // Only what this group selects : another group sharing the folder gets
    // its own files when it asks.
    std::vector<const DatSource::RemoteFile*> todo;
    for (const auto& c : cmp) {
        if (c.state != DatSource::State::Outdated && c.state != DatSource::State::Missing) continue;
        if (!m_job_group.selects(c.name)) continue;
        for (const auto& f : m.files) if (f.name == c.name) todo.push_back(&f);
    }
    if (todo.empty()) push_log("every DAT file of this group is already up to date (SHA-256)");
    for (size_t i = 0; i < todo.size(); ++i) {
        if (m_cancelled) break;
        const auto& f = *todo[i];
        double base = 5.0 + 90.0 * (double)i / (double)todo.size();
        double span = 90.0 / (double)todo.size();
        std::string e;
        bool ok = DatSource::download(m_job_url, f, m_job_folder, e,
            [&](double p, const std::string& n) { push_progress(base + span * p / 100.0, n); },
            [this] { return m_cancelled.load(); });
        if (ok) { ++m_job_downloaded; push_log("downloaded " + f.name + " (" + f.version + ")"); }
        else    { ++m_job_failed; push_log("FAILED " + f.name + ": " + e + " : local file kept"); }
    }
    // What the folder looks like now, for the table.
    cmp = DatSource::compare(m_job_folder, m);
    {
        std::lock_guard<std::mutex> lk(m_shared_mutex);
        m_job_manifest = std::move(m);
        m_last_compare = std::move(cmp);
        m_last_compare_group = m_job_group_id;
    }
    push_progress(100.0, _("Done."));
    m_finished_dispatcher();
}

// Custom URL : le meme travail qu'un site, a l'adresse du joueur.
void RomDatTab::on_download_url() {
    if (m_busy || group().url.empty()) return;
    on_download_site(kCustomUrl);
}

void RomDatTab::on_download_site(size_t site) {
    if (m_busy || (site != kCustomUrl && site >= DatSource::sites().size())) return;
    if (group().folder.empty()) {
        auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
        if (top) ui::notice(*top, _("No DAT folder"), _("Choose the group's folder first."));
        return;
    }
    m_job_site = site;
    m_job_folder = group().folder;
    m_job_group_id = group().id;
    m_job_group = group();
    m_job_before = DatSource::list_folder(group().folder);
    m_job_written.clear();
    m_job_error.clear();
    m_cancelled = false;
    m_job = Job::Site;
    m_job_custom_url = site == kCustomUrl ? group().url : std::string();
    m_job_unzip = site == kCustomUrl ? group().unzip : true;
    set_busy(true);
    m_progress_label.set_text(Glib::ustring::compose(_("Downloading from %1…"),
        site == kCustomUrl ? host_of(m_job_custom_url) : std::string(DatSource::sites()[site].source)));
    m_worker = std::thread(&RomDatTab::worker_site, this);
}

void RomDatTab::worker_site() {
    std::string err, url;
    if (m_job_site == kCustomUrl) {
        url = m_job_custom_url;
    } else {
        const auto& site = DatSource::sites()[m_job_site];
        push_progress(2.0, _("Looking for the newest version…"));
        // A site that cannot be read keeps the address the menu knows.
        url = DatSource::latest_url(site.url, err);
        if (!err.empty()) push_log(std::string("newest version unknown (") + err + "), trying " + url);
    }
    std::vector<std::string> written;
    err.clear();
    const bool ok = DatSource::fetch_direct(url, m_job_folder, written, err,
        [this](double p, const std::string& n) { push_progress(5.0 + 0.9 * p, n); },
        [this] { return m_cancelled.load(); }, m_job_unzip);
    if (!ok) m_job_error = err;
    m_job_url = url;
    m_job_written = std::move(written);
    push_progress(100.0, _("Done."));
    m_finished_dispatcher();
}

void RomDatTab::push_progress(double pct, const std::string& msg) {
    { std::lock_guard<std::mutex> lk(m_shared_mutex); m_current_message = msg; }
    m_progress_value.store(pct);
    static thread_local auto last = std::chrono::steady_clock::now() - std::chrono::hours(1);
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() >= UI_DISPATCH_INTERVAL_MS) {
        last = now;
        m_progress_dispatcher();
    }
}

void RomDatTab::push_log(const std::string& msg) {
    std::cerr << "[DAT] " << msg << std::endl;
    { std::lock_guard<std::mutex> lk(m_shared_mutex); m_log_messages.push_back(msg); }
    m_progress_dispatcher();
}

void RomDatTab::on_progress_update() {
    std::string message;
    std::vector<std::string> pending;
    { std::lock_guard<std::mutex> lk(m_shared_mutex); message = m_current_message; pending.swap(m_log_messages); }
    double pct = m_progress_value.load();
    m_progress.set_fraction(std::clamp(pct / 100.0, 0.0, 1.0));
    m_progress.set_text(std::to_string((int)pct) + "%");
    if (!message.empty() && m_busy) m_progress_label.set_text(message);
    if (!pending.empty()) m_status.set_text(pending.back());
}

void RomDatTab::on_worker_finished() {
    if (m_worker.joinable()) m_worker.join();
    on_progress_update();
    Job job = m_job;
    m_job = Job::None;
    set_busy(false);

    if (job == Job::Site) {
        // Une adresse du joueur se note comme un site : son hote est la source.
        const std::string host = host_of(m_job_url);
        const DatSource::Site custom{"", "Custom URL", host.c_str(), m_job_url.c_str(), m_job_url.c_str()};
        const DatSource::Site& site = m_job_site == kCustomUrl ? custom : DatSource::sites()[m_job_site];
        if (!m_job_written.empty()) DatSource::record_source(m_job_folder, m_job_written, site, m_job_url);
        if (!m_job_error.empty()) {
            refresh();
            flash(Glib::ustring::compose(_("Could not download from %1: %2"), site.source, m_job_error));
            return;
        }
        // A pack brings several DATs (progettosnaps : MAME, arcade, MAMEUI,
        // HBMAME...) : a group taking every file would load the same machines
        // several times. The group keeps what it had ; the user ticks the new
        // ones it uses.
        bool reload = false;
        for (auto& g : m_groups) {
            if (g.id != m_job_group_id) continue;
            g.last_update = DatSource::now_iso();
            if (m_job_written.size() > 1 && g.all_files) {
                std::vector<std::string> keep;
                for (const auto& f : m_job_before)
                    if (std::find(m_job_written.begin(), m_job_written.end(), f) == m_job_written.end()
                        || g.selects(f)) keep.push_back(f);
                g.all_files = false;
                g.files = keep;
            }
            for (const auto& f : m_job_written) if (g.selects(f)) reload = true;
        }
        save_groups();
        refresh();
        if (reload) {
            flash(Glib::ustring::compose(_("%1 DAT file(s) downloaded from %2. Reloading the database…"),
                                         m_job_written.size(), site.source));
            m_sig_groups.emit();
            m_sig_reload.emit(false, emulator_of_group(m_job_group_id));
        } else {
            flash(Glib::ustring::compose(_("%1 DAT file(s) downloaded from %2 : tick the ones this group uses."),
                                         m_job_written.size(), site.source));
        }
        return;
    }

    if (!m_job_error.empty()) {
        m_last_compare.clear();
        m_last_compare_group.clear();
        refresh();
        flash(Glib::ustring::compose(_("Could not read the DAT source: %1"), m_job_error));
        return;
    }
    m_last_manifest_generated = m_job_manifest.generated;
    // The group the job was for, even if the user moved to another one
    // meanwhile.
    for (auto& g : m_groups) {
        if (g.id != m_job_group_id) continue;
        g.last_check = DatSource::now_iso();
        if (job == Job::Download && m_job_downloaded > 0) g.last_update = DatSource::now_iso();
    }
    save_groups();
    refresh();

    const auto& g = group();
    int outdated = 0, missing = 0;
    if (m_last_compare_group == g.id)
        for (const auto& c : m_last_compare) {
            if (!g.selects(c.name)) continue;
            if (c.state == DatSource::State::Outdated) ++outdated;
            if (c.state == DatSource::State::Missing)  ++missing;
        }
    if (job == Job::Check) {
        flash(outdated + missing ? Glib::ustring::compose(_("%1 DAT file(s) of this group differ from the server and %2 are missing here : Download DATs to update."), outdated, missing)
                                 : Glib::ustring(_("Every DAT file of this group is up to date with the server.")));
    } else {
        Glib::ustring msg = Glib::ustring::compose(_("%1 DAT file(s) downloaded"), m_job_downloaded);
        if (m_job_failed) msg += Glib::ustring::compose(_(", %1 failed (local files kept)"), m_job_failed);
        if (m_job_downloaded > 0) { msg += _(". Reloading the database…"); flash(msg); m_sig_groups.emit(); m_sig_reload.emit(false, emulator_of_group(m_job_group_id)); }
        else flash(msg + ".");
    }
}

void RomDatTab::set_busy(bool busy) {
    m_busy = busy;
    for (auto* w : std::vector<Gtk::Widget*>{m_btn_browse, m_btn_add, m_btn_more, m_btn_add_group, &m_group_list,
                                             &m_entry_folder, &m_combo_emulator,
                                             &m_radio_emulator, &m_radio_http, &m_radio_url, &m_radio_site,
                                             &m_radio_folder, &m_entry_url, &m_check_unzip, &m_combo_site})
        w->set_sensitive(!busy);
    if (busy) { m_progress.set_fraction(0.0); m_progress.show(); m_progress_label.show(); m_btn_cancel->show(); }
    else      { m_progress.hide(); m_progress_label.hide(); m_btn_cancel->hide(); }
    apply_source_ui();
}

void RomDatTab::flash(const Glib::ustring& text) {
    m_status.set_text(text);
    m_flash_timer.disconnect();
    m_flash_timer = Glib::signal_timeout().connect([this] { if (!m_busy) update_summary(); return false; }, 6000);
}
