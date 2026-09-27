// src/RomQuarantineTab.cpp
#include "RomQuarantineTab.h"

#include "AppContext.h"
#include "ConfirmationDialog.h"
#include "i18n.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
namespace ui = SettingsUi;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
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

// The reasons a file can be here, in the order the pills show them. The last
// one is not a reason but the absence of one.
struct ReasonDef { const char* key; const char* label; ui::PillTone tone; };
const ReasonDef kReasons[] = {
    { RomManifest::reason::Unknown,     N_("Unknown"),     ui::PillTone::Warn    },
    { RomManifest::reason::BadCrc,      N_("Bad CRC"),     ui::PillTone::Error   },
    { RomManifest::reason::Unsupported, N_("Unsupported"), ui::PillTone::Warn    },
    { RomManifest::reason::Duplicate,   N_("Duplicate"),   ui::PillTone::Accent  },
    { RomManifest::reason::ExtraFiles,  N_("Extra files"), ui::PillTone::Neutral },
    { RomManifest::reason::Replaced,    N_("Replaced"),    ui::PillTone::Ok      },
    { "none",                           N_("No record"),   ui::PillTone::Neutral },
};

const ReasonDef& reason_def(const std::string& key) {
    for (const auto& r : kReasons) if (key == r.key) return r;
    return kReasons[sizeof(kReasons) / sizeof(kReasons[0]) - 1];
}

// "2026-09-12T00:35:52Z" → "2026-09-12 00:35" : enough for a date column.
std::string short_date(const std::string& iso) {
    return RomManifest::local_time(iso);
}

bool move_file(const fs::path& src, const fs::path& dest, std::string& error) {
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    fs::rename(src, dest, ec);
    if (!ec) return true;
    fs::copy_file(src, dest, ec);
    if (ec) { error = ec.message(); return false; }
    fs::remove(src, ec);
    return true;
}

// Empty folders left behind by a move or a delete are swept, up to the root.
void prune_empty_dirs(const fs::path& from, const fs::path& root) {
    std::error_code ec;
    fs::path dir = from;
    while (dir != root && dir.string().rfind(root.string(), 0) == 0 && dir.string().size() > root.string().size()) {
        if (!fs::is_empty(dir, ec) || ec) break;
        fs::remove(dir, ec);
        dir = dir.parent_path();
    }
}

} // namespace

// ═══ Construction ═══════════════════════════════════════════════════════════

RomQuarantineTab::RomQuarantineTab(std::shared_ptr<DatabaseManager> db, PathsProvider paths)
    : Gtk::Box(Gtk::ORIENTATION_VERTICAL, ui::kCardSpacing), m_db(std::move(db)), m_paths(std::move(paths)) {
    get_style_context()->add_class("set-page");
    build_header();
    build_table();
    build_footer();
    show_all_children();
    refresh();
}

void RomQuarantineTab::build_header() {
    auto card = ui::card("bc-shield.svg", _("Quarantine"),
                         _("Files that could not be used or were rejected during import and repair. "
                           "Restore them to Import, delete them, or keep them for later review."));
    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 8);
    body->set_margin_top(10);
    // Le dossier se choisit la ou on regarde son contenu, comme le dossier
    // d'import dans l'onglet Import : meme ligne, meme bouton.
    auto* path_line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    m_entry_folder.set_hexpand(true);
    m_entry_folder.set_placeholder_text(_("No quarantine folder set yet"));
    m_entry_folder.signal_activate().connect([this] { apply_quarantine_path(m_entry_folder.get_text().raw()); });
    m_btn_browse = ui::button(_("Browse…"), "bc-folder.svg");
    m_btn_browse->signal_clicked().connect(sigc::mem_fun(*this, &RomQuarantineTab::on_browse_folder));
    path_line->pack_start(m_entry_folder, Gtk::PACK_EXPAND_WIDGET);
    path_line->pack_start(*m_btn_browse, Gtk::PACK_SHRINK);
    body->pack_start(*path_line, Gtk::PACK_SHRINK);
    auto* line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    m_btn_open = ui::button(_("Open folder"), "bc-external.svg");
    m_btn_open->signal_clicked().connect(sigc::mem_fun(*this, &RomQuarantineTab::on_open_folder));
    m_btn_refresh = ui::button(_("Refresh"), "bc-sync.svg");
    m_btn_refresh->signal_clicked().connect(sigc::mem_fun(*this, &RomQuarantineTab::refresh));
    line->pack_start(*m_btn_open, Gtk::PACK_SHRINK);
    line->pack_start(*m_btn_refresh, Gtk::PACK_SHRINK);
    auto* hint = ui::sub_label(_("Nothing here is deleted on its own."));
    hint->set_valign(Gtk::ALIGN_CENTER);
    line->pack_start(*hint, Gtk::PACK_SHRINK);
    body->pack_start(*line, Gtk::PACK_SHRINK);
    card.body->pack_start(*body, Gtk::PACK_SHRINK);
    pack_start(*card.frame, Gtk::PACK_SHRINK);
}

// The actions, bottom right, like every tab of the window.
void RomQuarantineTab::build_footer() {
    m_btn_restore = ui::button(_("Restore selected to Import"), "bc-restore.svg", ui::Tone::Accent);
    m_btn_restore->signal_clicked().connect([this] { on_restore(false); });
    m_btn_restore_more = Gtk::make_managed<Gtk::MenuButton>();
    m_btn_restore_more->add(*ui::image("bc-chevron-down.svg", 14));
    m_btn_restore_more->get_style_context()->add_class("accent-button");
    m_item_restore_origin = Gtk::make_managed<Gtk::MenuItem>(_("Restore selected to original location"));
    m_item_restore_origin->signal_activate().connect([this] { on_restore(true); });
    m_restore_menu.append(*m_item_restore_origin);
    m_restore_menu.show_all();
    m_btn_restore_more->set_popup(m_restore_menu);
    m_btn_delete = ui::button(_("Delete selected"), "bc-trash-red.svg", ui::Tone::Danger);
    m_btn_delete->signal_clicked().connect(sigc::mem_fun(*this, &RomQuarantineTab::on_delete_selected));
    m_btn_empty = ui::button(_("Empty quarantine"), "bc-trash.svg", ui::Tone::Danger);
    m_btn_empty->signal_clicked().connect(sigc::mem_fun(*this, &RomQuarantineTab::on_empty));
    m_footer.pack_end(*m_btn_empty, Gtk::PACK_SHRINK);
    m_footer.pack_end(*m_btn_delete, Gtk::PACK_SHRINK);
    m_footer.pack_end(*m_btn_restore_more, Gtk::PACK_SHRINK);
    m_footer.pack_end(*m_btn_restore, Gtk::PACK_SHRINK);
    pack_start(m_footer, Gtk::PACK_SHRINK);
}

void RomQuarantineTab::build_table() {
    auto* summary = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    m_pill_total = Gtk::make_managed<ui::Pill>(_("All"), ui::PillTone::Neutral);
    m_pill_total->set_count(0);
    m_pills.pack_start(*m_pill_total, Gtk::PACK_SHRINK);
    for (const auto& r : kReasons) {
        auto* p = Gtk::make_managed<ui::Pill>(_(r.label), r.tone, true);
        p->set_count(0);
        p->set_active(true);
        p->signal_toggled().connect([this](bool) { refilter(); });
        m_pills.pack_start(*p, Gtk::PACK_SHRINK);
        m_reason_pills.push_back({r.key, p});
    }
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

    m_filter = Gtk::make_managed<ui::FilterBar>(_("Search (game, file, origin…)"));
    m_system_combo = m_filter->add_combo(_("System:"));
    m_system_combo->append(_("All"));
    m_system_combo->set_active(0);
    m_filter->signal_changed().connect(sigc::mem_fun(*this, &RomQuarantineTab::refilter));
    pack_start(*m_filter, Gtk::PACK_SHRINK);

    m_store = Gtk::ListStore::create(m_cols);
    m_models.sort_column = m_cols.added.index();
    m_models.sort_order  = Gtk::SORT_DESCENDING;

    m_table = Gtk::make_managed<ui::Table>(Gtk::SELECTION_MULTIPLE);
    m_models.attach(m_table->view(), m_store, sigc::mem_fun(*this, &RomQuarantineTab::row_visible));
    m_table->add_check_column(m_cols.include, sigc::mem_fun(*this, &RomQuarantineTab::on_row_toggled));
    {
        auto* renderer = Gtk::make_managed<Gtk::CellRendererText>();
        auto* col = Gtk::make_managed<Gtk::TreeViewColumn>(_("Reason"), *renderer);
        col->add_attribute(renderer->property_text(), m_cols.reason);
        col->set_sort_column(m_cols.reason_key);
        col->set_cell_data_func(*renderer, [this, renderer](Gtk::CellRenderer*, const Gtk::TreeModel::iterator& it) {
            ensure_colours();
            if (!m_colours.ready) return;
            const Glib::ustring key = (*it)[m_cols.reason_key];
            Gdk::RGBA c = m_colours.muted;
            switch (reason_def(key.raw()).tone) {
                case ui::PillTone::Ok:     c = m_colours.ok; break;
                case ui::PillTone::Warn:   c = m_colours.warn; break;
                case ui::PillTone::Error:  c = m_colours.err; break;
                case ui::PillTone::Accent: c = m_colours.accent; break;
                default: break;
            }
            renderer->property_foreground_rgba() = c;
            renderer->property_weight() = key == "none" ? Pango::WEIGHT_NORMAL : Pango::WEIGHT_BOLD;
        });
        m_table->view().append_column(*col);
    }
    { ui::ColumnOptions o; o.expand = true; o.min_width = 160; m_table->add_text_column(_("Game / File"), m_cols.game, o); }
    m_table->add_text_column(_("System"), m_cols.system);
    {
        auto* renderer = Gtk::make_managed<Gtk::CellRendererText>();
        renderer->property_ellipsize() = Pango::ELLIPSIZE_MIDDLE;
        auto* col = Gtk::make_managed<Gtk::TreeViewColumn>(_("Original location"), *renderer);
        col->add_attribute(renderer->property_text(), m_cols.origin);
        col->set_expand(true);
        col->set_resizable(true);
        col->set_sort_column(m_cols.origin);
        m_table->view().append_column(*col);
    }
    { ui::ColumnOptions o; o.mono = true; m_table->add_text_column(_("File name"), m_cols.file, o); }
    { ui::ColumnOptions o; o.xalign = 1.0f; m_table->add_text_column(_("Size"), m_cols.size, o); }
    m_table->add_text_column(_("Date added"), m_cols.added);
    { ui::ColumnOptions o; o.expand = true; o.sortable = false; m_table->add_text_column(_("Details"), m_cols.details, o); }
    m_table->signal_context_menu().connect(sigc::mem_fun(*this, &RomQuarantineTab::on_context_menu));
    pack_start(*m_table, Gtk::PACK_EXPAND_WIDGET);
}

void RomQuarantineTab::ensure_colours() {
    if (m_colours.ready || !get_toplevel() || !get_toplevel()->get_realized()) return;
    m_colours.ok     = ui::probe_color(*this, "set-ok");
    m_colours.warn   = ui::probe_color(*this, "set-warn");
    m_colours.err    = ui::probe_color(*this, "set-err");
    m_colours.muted  = ui::probe_color(*this, "set-sub");
    m_colours.accent = ui::probe_color(*this, "set-accent-text");
    m_colours.ready  = true;
}

// ═══ Content ════════════════════════════════════════════════════════════════

void RomQuarantineTab::refresh() {
    Paths p = m_paths();
    std::error_code ec;
    m_items.clear();
    // Pas de set_text inconditionnel : l'utilisateur peut etre en train de
    // taper dans le champ pendant qu'un refresh arrive.
    if (m_entry_folder.get_text().raw() != p.quarantine) m_entry_folder.set_text(p.quarantine);
    if (p.quarantine.empty() || !fs::is_directory(p.quarantine, ec)) {
        m_manifest = RomManifest::Manifest();
        populate();
        return;
    }
    m_manifest = RomManifest::Manifest::load(p.quarantine);
    if (m_manifest.reconcile() > 0) m_manifest.save();

    for (auto it = fs::recursive_directory_iterator(p.quarantine, ec); it != fs::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file(ec)) continue;
        const std::string name = it->path().filename().string();
        if (RomManifest::Manifest::is_manifest_file(name)) continue;
        Item item;
        item.path  = it->path().string();
        item.rel   = m_manifest.relative(item.path);
        item.bytes = fs::file_size(it->path(), ec);
        if (const auto* e = m_manifest.find(item.rel)) {
            item.has_record = true;
            item.reason   = e->reason.empty() ? "none" : e->reason;
            item.game     = e->game;
            item.system   = e->system;
            item.origin   = e->origin;
            item.action   = e->action;
            item.added_at = e->added_at;
            item.details  = e->details;
        } else {
            item.reason = "none";
            item.game   = it->path().stem().string();
            // The folder it sits in is the only hint left about its system.
            std::string folder = it->path().parent_path().filename().string();
            if (folder.rfind("_", 0) != 0 && it->path().parent_path() != fs::path(p.quarantine)) item.system = folder;
            auto ftime = fs::last_write_time(it->path(), ec);
            if (!ec) {
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
                std::time_t t = std::chrono::system_clock::to_time_t(sctp);
                std::tm tm{};
                gmtime_r(&t, &tm);
                char buf[32];
                std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
                item.added_at = buf;
            }
        }
        m_items.push_back(std::move(item));
    }
    populate();
}

void RomQuarantineTab::populate() {
    m_models.detach(m_table->view());   // nothing attached while filling : see SettingsUi::ModelStack
    m_store->clear();
    std::set<std::string> systems;
    for (size_t i = 0; i < m_items.size(); ++i) {
        const auto& it = m_items[i];
        if (!it.system.empty()) systems.insert(it.system);
        auto row = *(m_store->append());
        row[m_cols.include]    = it.selected;
        row[m_cols.reason]     = _(reason_def(it.reason).label);
        row[m_cols.reason_key] = it.reason;
        row[m_cols.game]       = it.game;
        row[m_cols.system]     = it.system;
        row[m_cols.origin]     = it.origin.empty() ? Glib::ustring(it.has_record ? "" : _("unknown")) : Glib::ustring(fs::path(it.origin).parent_path().string());
        row[m_cols.file]       = fs::path(it.path).filename().string();
        row[m_cols.size]       = human_size(it.bytes);
        row[m_cols.added]      = short_date(it.added_at);
        row[m_cols.details]    = it.has_record ? join(it.details, " · ") : _("no record : not put here by Bootcade");
        row[m_cols.index]      = (unsigned int)i;
        row[m_cols.search_blob] = lower(it.game + ' ' + it.system + ' ' + it.origin + ' ' + fs::path(it.path).filename().string() + ' ' + join(it.details, " "));
    }
    m_filter->set_combo_items(m_system_combo, _("All"), systems, m_system_combo->get_active_text());
    refilter();
    update_summary();
    update_action_buttons();
}

void RomQuarantineTab::update_summary() {
    uintmax_t bytes = 0;
    std::map<std::string, long> by_reason;
    for (const auto& it : m_items) { bytes += it.bytes; by_reason[it.reason]++; }
    m_pill_total->set_count((long)m_items.size());
    for (auto& rp : m_reason_pills) rp.pill->set_count(by_reason[rp.key]);
    m_status.set_text(m_items.empty() ? Glib::ustring(_("The quarantine is empty."))
                                      : Glib::ustring::compose(_("%1 file(s), %2"), (int)m_items.size(), human_size(bytes)));
}

bool RomQuarantineTab::row_visible(const Gtk::TreeModel::const_iterator& it) const {
    const Gtk::TreeModel::Row row = *it;
    const Glib::ustring key = row[m_cols.reason_key];
    bool wanted = false;
    for (const auto& rp : m_reason_pills) if (rp.key == key.raw()) { wanted = rp.pill->active(); break; }
    if (!wanted) return false;
    if (!m_vis_system.empty() && row[m_cols.system] != m_vis_system) return false;
    const std::string& needle = m_vis_needle;
    if (!needle.empty()) {
        const Glib::ustring blob = row[m_cols.search_blob];
        if (blob.raw().find(needle) == std::string::npos) return false;
    }
    return true;
}

void RomQuarantineTab::refilter() {
    // Rebuilt, not refiltered : see SettingsUi::ModelStack. The filter's
    // inputs are read once here, not once per row inside row_visible.
    m_vis_system = m_system_combo->get_active_row_number() > 0 ? m_system_combo->get_active_text() : Glib::ustring();
    m_vis_needle = lower(m_filter->search_text());
    m_models.detach(m_table->view());
    m_models.attach(m_table->view(), m_store, sigc::mem_fun(*this, &RomQuarantineTab::row_visible));
    m_filter->set_summary(Glib::ustring::compose(_("%1 result(s)"), m_models.visible_count()));
}

Gtk::TreeModel::Row RomQuarantineTab::source_row(const Gtk::TreeModel::Path& sorted_path) const {
    auto child = m_models.filter->convert_path_to_child_path(m_models.sort->convert_path_to_child_path(sorted_path));
    return *m_store->get_iter(child);
}

void RomQuarantineTab::on_row_toggled(const Glib::ustring& path) {
    Gtk::TreeModel::Row row = source_row(Gtk::TreeModel::Path(path));
    bool on = !row[m_cols.include];
    row[m_cols.include] = on;
    m_items[(unsigned int)row[m_cols.index]].selected = on;
    update_action_buttons();
}

void RomQuarantineTab::set_all_checked(bool on) {
    for (const auto& frow : m_models.filter->children()) {
        Gtk::TreeModel::Row row = *m_models.filter->convert_iter_to_child_iter(frow);
        row[m_cols.include] = on;
        m_items[(unsigned int)row[m_cols.index]].selected = on;
    }
    update_action_buttons();
}

void RomQuarantineTab::update_action_buttons() {
    int selected = 0, restorable_to_origin = 0;
    for (const auto& it : m_items) {
        if (!it.selected) continue;
        ++selected;
        // Only a whole file moved here can go back where it was : an entry
        // extracted out of an archive would have to be put back inside it.
        if (it.has_record && !it.origin.empty() && it.action != RomManifest::action::Extracted) ++restorable_to_origin;
    }
    m_btn_restore->set_label(selected ? Glib::ustring::compose(_("Restore selected to Import (%1)"), selected) : Glib::ustring(_("Restore selected to Import")));
    m_btn_restore->set_sensitive(selected > 0);
    m_btn_restore_more->set_sensitive(selected > 0);
    m_item_restore_origin->set_sensitive(restorable_to_origin > 0);
    m_item_restore_origin->set_label(restorable_to_origin
        ? Glib::ustring::compose(_("Restore selected to original location (%1)"), restorable_to_origin)
        : Glib::ustring(_("Restore selected to original location")));
    m_btn_delete->set_sensitive(selected > 0);
    m_btn_empty->set_sensitive(!m_items.empty());
}

void RomQuarantineTab::on_context_menu(const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn*, GdkEventButton* event) {
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
    std::string game = it.game, file = it.path, origin = it.origin;
    add(_("Copy game / file name"), [copy, game]   { copy(game, _("the name")); });
    add(_("Copy file path"),        [copy, file]   { copy(file, _("the file path")); });
    add(_("Copy original location"), [copy, origin] { copy(origin, _("the original location")); }, !origin.empty());
    m_context_menu.append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());
    add(_("Restore to Import"), [this, row] { row[m_cols.include] = true; m_items[(unsigned int)row[m_cols.index]].selected = true; update_action_buttons(); on_restore(false); });
    add(_("Restore to original location"), [this, row] { row[m_cols.include] = true; m_items[(unsigned int)row[m_cols.index]].selected = true; update_action_buttons(); on_restore(true); },
        it.has_record && !it.origin.empty() && it.action != RomManifest::action::Extracted);
    add(_("Delete"), [this, row] { row[m_cols.include] = true; m_items[(unsigned int)row[m_cols.index]].selected = true; update_action_buttons(); on_delete_selected(); });
    m_context_menu.show_all();
    m_context_menu.popup_at_pointer((GdkEvent*)event);
}

// ═══ Actions ════════════════════════════════════════════════════════════════

void RomQuarantineTab::on_open_folder() {
    Paths p = m_paths();
    std::error_code ec;
    if (p.quarantine.empty() || !fs::is_directory(p.quarantine, ec)) { flash(_("No quarantine folder to open.")); return; }
    try { Gio::AppInfo::launch_default_for_uri(Glib::filename_to_uri(p.quarantine)); }
    catch (const Glib::Error& e) { flash(Glib::ustring::compose(_("Could not open the folder: %1"), e.what())); }
}

// config.json est aussi ecrit par le panneau de reglages : on relit le fichier
// entier, on ne change que cette cle, et on le reecrit. Un fichier illisible
// n'est pas reecrit du tout, plutot que remplace par un fichier presque vide.
void RomQuarantineTab::save_quarantine_path(const std::string& folder) const {
    nlohmann::json j;
    const std::string path = AppContext::get_config_path();
    { std::ifstream fi(path); if (fi) { try { fi >> j; } catch (...) { return; } } }
    j["rom_manager"]["quarantine_path"] = folder;
    std::ofstream fo(path);
    if (fo) fo << j.dump(4);
}

void RomQuarantineTab::apply_quarantine_path(const std::string& folder) {
    if (folder == m_paths().quarantine) return;
    save_quarantine_path(folder);
    m_entry_folder.set_text(folder);
    m_sig_path_changed.emit(folder);
    // Sans cela l'ecran continuerait de montrer le contenu de l'ancien dossier.
    refresh();
}

void RomQuarantineTab::on_browse_folder() {
    auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
    Gtk::FileChooserDialog dlg(_("Select the quarantine folder"), Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
    if (top) dlg.set_transient_for(*top);
    dlg.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dlg.add_button(_("Select"), Gtk::RESPONSE_OK);
    const std::string current = m_entry_folder.get_text().raw();
    if (!current.empty()) dlg.set_filename(current);
    if (dlg.run() != Gtk::RESPONSE_OK) return;
    apply_quarantine_path(dlg.get_filename());
}

void RomQuarantineTab::on_restore(bool to_origin) {
    Paths p = m_paths();
    auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
    std::error_code ec;
    std::vector<Item*> chosen;
    for (auto& it : m_items) if (it.selected) chosen.push_back(&it);
    if (chosen.empty()) { flash(_("Check at least one file first.")); return; }
    if (!to_origin) {
        if (p.inbox.empty()) { if (top) ui::notice(*top, _("No import folder"), _("Set an import folder in the Import tab first.")); return; }
        fs::create_directories(p.inbox, ec);
    }

    int done = 0, skipped = 0;
    for (Item* it : chosen) {
        fs::path src(it->path), dest;
        if (to_origin) {
            if (!it->has_record || it->origin.empty() || it->action == RomManifest::action::Extracted) { ++skipped; continue; }
            dest = it->origin;
            if (fs::exists(dest, ec)) {
                m_sig_log.emit("[QUARANTINE] " + src.filename().string() + " : original location already holds a file, left in quarantine");
                ++skipped;
                continue;
            }
        } else {
            dest = fs::path(p.inbox) / src.filename();
            if (fs::exists(dest, ec)) {
                m_sig_log.emit("[QUARANTINE] " + src.filename().string() + " : the import folder already has a file of that name, left in quarantine");
                ++skipped;
                continue;
            }
        }
        std::string err;
        if (!move_file(src, dest, err)) {
            m_sig_log.emit("[QUARANTINE] could not restore " + src.filename().string() + ": " + err);
            ++skipped;
            continue;
        }
        prune_empty_dirs(src.parent_path(), fs::path(p.quarantine));
        m_manifest.remove(it->rel, RomManifest::outcome::Restored);
        m_sig_log.emit("[QUARANTINE] restored " + src.filename().string() + " → " + dest.string());
        ++done;
    }
    if (done) m_manifest.save();
    refresh();
    flash(skipped ? Glib::ustring::compose(_("Restored %1 file(s), %2 left in quarantine (see the Import log)."), done, skipped)
                  : Glib::ustring::compose(_("Restored %1 file(s)."), done));
    if (done && !to_origin) m_sig_restored.emit(done);
}

void RomQuarantineTab::on_delete_selected() {
    auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
    std::vector<Item*> chosen;
    uintmax_t bytes = 0;
    for (auto& it : m_items) if (it.selected) { chosen.push_back(&it); bytes += it.bytes; }
    if (chosen.empty()) { flash(_("Check at least one file first.")); return; }
    if (top) {
        ConfirmationDialog confirm(*top, _("Permanently delete the selected files?"),
            Glib::ustring::compose(_("%1 file(s) (%2) will be deleted from your filesystem : not moved, deleted. This cannot be undone."),
                                   (int)chosen.size(), human_size(bytes)), "bc-trash.svg");
        if (!confirm.show_and_confirm()) return;
    }
    Paths p = m_paths();
    std::error_code ec;
    int done = 0;
    for (Item* it : chosen) {
        fs::path src(it->path);
        fs::remove(src, ec);
        if (ec) { m_sig_log.emit("[QUARANTINE] could not delete " + it->path + ": " + ec.message()); ec.clear(); continue; }
        prune_empty_dirs(src.parent_path(), fs::path(p.quarantine));
        m_manifest.remove(it->rel, RomManifest::outcome::Deleted);
        m_sig_log.emit("[QUARANTINE] deleted " + it->path);
        ++done;
    }
    if (done) m_manifest.save();
    refresh();
    flash(Glib::ustring::compose(_("Deleted %1 file(s)."), done));
}

void RomQuarantineTab::on_empty() {
    Paths p = m_paths();
    std::error_code ec;
    if (m_items.empty()) { flash(_("The quarantine is already empty.")); return; }
    uintmax_t bytes = 0;
    for (const auto& it : m_items) bytes += it.bytes;
    auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (top) {
        ConfirmationDialog confirm(*top, _("Permanently delete every quarantined file?"),
            Glib::ustring::compose(_("%1 file(s) (%2) will be deleted from your filesystem : not moved, deleted. "
                                     "This cannot be undone. The record of what was here is kept."),
                                   (int)m_items.size(), human_size(bytes)), "bc-trash.svg");
        if (!confirm.show_and_confirm()) return;
    }
    // The record of what was here outlives the files: every entry is retired
    // to the manifest's history as purged, and the manifest is the one file
    // written back into the emptied folder.
    for (const auto& it : m_items) m_sig_log.emit("[QUARANTINE] purged " + it.path);
    m_manifest.retire_all(RomManifest::outcome::Purged);
    fs::remove_all(p.quarantine, ec);
    fs::create_directories(p.quarantine, ec);
    m_manifest.save();
    refresh();
    flash(_("Quarantine emptied."));
}

void RomQuarantineTab::flash(const Glib::ustring& text) {
    m_status.set_text(text);
    m_flash_timer.disconnect();
    m_flash_timer = Glib::signal_timeout().connect([this] { update_summary(); return false; }, 4000);
}
