// src/RomFolderDialog.cpp
#include "RomFolderDialog.h"

#include "i18n.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;
namespace ui = SettingsUi;

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string human_size(uintmax_t b) {
    const char* unit[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)b;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    char buf[32];
    std::snprintf(buf, sizeof buf, i == 0 ? "%.0f %s" : "%.1f %s", v, unit[i]);
    return buf;
}

}  // namespace

RomFolderDialog::RomFolderDialog(Gtk::Window& parent, const std::string& root)
    : m_root(root), m_cur(root) {
    set_transient_for(parent);
    set_modal(true);
    set_position(Gtk::WIN_POS_CENTER_ON_PARENT);
    set_default_size(760, 520);
    ui::window_header(*this, "bc-folder.svg", _("Your ROM folder"),
                      _("Drop your ROM archives here, or create folders to sort them."),
                      [this] { response(Gtk::RESPONSE_CLOSE); });

    auto* column = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);

    // La barre du dossier : monter, ou l'on est, nouveau dossier.
    auto* bar = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    bar->set_margin_start(22); bar->set_margin_end(22); bar->set_margin_top(16); bar->set_margin_bottom(10);
    m_up = ui::button(_("Up"), "");
    m_up->signal_clicked().connect(sigc::mem_fun(*this, &RomFolderDialog::on_up));
    bar->pack_start(*m_up, Gtk::PACK_SHRINK);
    m_where = Gtk::make_managed<Gtk::Label>();
    m_where->set_xalign(0.0f);
    m_where->set_ellipsize(Pango::ELLIPSIZE_START);
    m_where->get_style_context()->add_class("set-row-title");
    bar->pack_start(*m_where, Gtk::PACK_EXPAND_WIDGET);
    m_new = ui::button(_("New folder"), "bc-folder.svg");
    m_new->signal_clicked().connect(sigc::mem_fun(*this, &RomFolderDialog::on_new_folder));
    bar->pack_end(*m_new, Gtk::PACK_SHRINK);
    column->pack_start(*bar, Gtk::PACK_SHRINK);

    // La liste.
    m_store = Gtk::ListStore::create(m_cols);
    m_table = Gtk::make_managed<ui::Table>(Gtk::SELECTION_SINGLE);
    m_table->view().set_model(m_store);
    ui::ColumnOptions name_opts; name_opts.expand = true; name_opts.min_width = 300; name_opts.sortable = false;
    m_table->add_text_column(_("Name"), m_cols.name, name_opts);
    ui::ColumnOptions kind_opts; kind_opts.sortable = false; kind_opts.min_width = 130;
    m_table->add_text_column(_("Type"), m_cols.kind, kind_opts);
    ui::ColumnOptions size_opts; size_opts.sortable = false; size_opts.xalign = 1.0f; size_opts.min_width = 90;
    m_table->add_text_column(_("Size"), m_cols.size, size_opts);
    m_table->view().signal_row_activated().connect(sigc::mem_fun(*this, &RomFolderDialog::on_row_activated));
    m_table->set_margin_start(22); m_table->set_margin_end(22);
    m_table->set_vexpand(true);
    column->pack_start(*m_table, Gtk::PACK_EXPAND_WIDGET);

    m_hint = Gtk::make_managed<Gtk::Label>();
    m_hint->set_xalign(0.0f);
    m_hint->set_margin_start(22); m_hint->set_margin_end(22); m_hint->set_margin_top(10); m_hint->set_margin_bottom(14);
    m_hint->set_line_wrap(true);
    m_hint->get_style_context()->add_class("set-sub");
    column->pack_start(*m_hint, Gtk::PACK_SHRINK);

    // Les actions : le dossier dans l'explorateur a gauche, fermer et scanner a droite.
    auto* foot = ui::footer();
    auto* manager = ui::button(_("Open in file manager"), "bc-external.svg");
    manager->signal_clicked().connect(sigc::mem_fun(*this, &RomFolderDialog::on_open_in_manager));
    foot->pack_start(*manager, Gtk::PACK_SHRINK);
    m_scan = ui::button(_("Scan now"), "bc-search.svg", ui::Tone::Accent);
    m_scan->signal_clicked().connect([this] { response(kScan); });
    auto* close = ui::button(_("Close"));
    close->signal_clicked().connect([this] { response(Gtk::RESPONSE_CLOSE); });
    foot->pack_end(*m_scan, Gtk::PACK_SHRINK);
    foot->pack_end(*close, Gtk::PACK_SHRINK);
    column->pack_start(*foot, Gtk::PACK_SHRINK);

    get_content_area()->set_spacing(0);
    get_content_area()->pack_start(*column, Gtk::PACK_EXPAND_WIDGET);

    // On depose ses fichiers n'importe ou dans la fenetre.
    std::vector<Gtk::TargetEntry> targets{Gtk::TargetEntry("text/uri-list", Gtk::TargetFlags(0), 0)};
    for (Gtk::Widget* w : std::initializer_list<Gtk::Widget*>{this, &m_table->view(), column}) {
        w->drag_dest_set(targets, Gtk::DEST_DEFAULT_ALL, Gdk::ACTION_COPY);
        w->signal_drag_data_received().connect(sigc::mem_fun(*this, &RomFolderDialog::on_drop));
    }
    m_progress_dispatcher.connect(sigc::mem_fun(*this, &RomFolderDialog::on_copy_progress));
    m_done_dispatcher.connect(sigc::mem_fun(*this, &RomFolderDialog::on_copy_done));

    show_all();
    refresh();
}

RomFolderDialog::~RomFolderDialog() {
    if (m_worker.joinable()) m_worker.join();
}

void RomFolderDialog::navigate(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    m_cur = dir;
    refresh();
}

void RomFolderDialog::refresh() {
    m_store->clear();
    std::vector<fs::directory_entry> entries;
    std::error_code ec;
    for (fs::directory_iterator it(m_cur, ec), end; !ec && it != end; it.increment(ec)) entries.push_back(*it);
    // Les dossiers d'abord, puis par nom.
    std::sort(entries.begin(), entries.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
        std::error_code e;
        const bool da = a.is_directory(e), db = b.is_directory(e);
        if (da != db) return da;
        return lower(a.path().filename().string()) < lower(b.path().filename().string());
    });
    int files = 0, folders = 0;
    for (const auto& e : entries) {
        std::error_code e2;
        const std::string name = e.path().filename().string();
        if (!name.empty() && name[0] == '.') continue;   // fichiers caches
        auto row = *m_store->append();
        row[m_cols.name] = name;
        if (e.is_directory(e2)) {
            row[m_cols.kind] = _("Folder");
            row[m_cols.size] = "";
            row[m_cols.is_dir] = true;
            ++folders;
        } else {
            const std::string ext = lower(e.path().extension().string());
            row[m_cols.kind] = ext == ".zip" ? Glib::ustring(_("ROM archive")) : Glib::ustring(_("File"));
            row[m_cols.size] = human_size(e.file_size(e2));
            row[m_cols.is_dir] = false;
            ++files;
        }
    }
    // Ou l'on est, depuis la racine du dossier de ROMs.
    std::string rel = m_cur == m_root ? std::string() : fs::relative(m_cur, m_root, ec).string();
    m_where->set_text(m_cur.string());
    m_where->set_tooltip_text(m_cur.string());
    m_up->set_sensitive(m_cur != m_root);
    if (files + folders == 0)
        m_hint->set_text(_("This folder is empty. Drag your ROM archives (.zip) from your file manager into this window."));
    else
        m_hint->set_text(Glib::ustring::compose(_("%1 file(s), %2 folder(s). Drag more archives into this window to add them."),
                                                files, folders));
    (void)rel;
}

void RomFolderDialog::on_row_activated(const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn*) {
    auto it = m_store->get_iter(path);
    if (!it || !(*it)[m_cols.is_dir]) return;
    const Glib::ustring name = (*it)[m_cols.name];
    navigate(m_cur / name.raw());
}

void RomFolderDialog::on_up() {
    if (m_cur == m_root) return;
    navigate(m_cur.parent_path());
}

void RomFolderDialog::on_new_folder() {
    Gtk::Dialog ask;
    ask.set_transient_for(*this);
    ask.set_modal(true);
    ask.set_resizable(false);
    ask.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);
    ask.set_default_size(440, -1);
    ui::window_header(ask, "bc-folder.svg", _("New folder"), _("Create a folder inside this one."),
                      [&ask] { ask.response(Gtk::RESPONSE_CANCEL); });
    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 10);
    body->set_margin_start(22); body->set_margin_end(22); body->set_margin_top(18); body->set_margin_bottom(14);
    auto* entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_placeholder_text(_("Folder name"));
    entry->set_activates_default(true);
    body->pack_start(*entry, Gtk::PACK_SHRINK);
    auto* problem = Gtk::make_managed<Gtk::Label>();
    problem->set_xalign(0.0f);
    problem->get_style_context()->add_class("set-sub");
    body->pack_start(*problem, Gtk::PACK_SHRINK);
    auto* foot = ui::footer();
    auto* create = ui::button(_("Create"), "", ui::Tone::Accent);
    auto* cancel = ui::button(_("Cancel"));
    create->set_can_default(true);
    create->signal_clicked().connect([&] {
        const std::string name = entry->get_text();
        if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:*?\"<>|") != std::string::npos) {
            problem->set_text(_("This name cannot be used for a folder."));
            return;
        }
        std::error_code ec;
        if (fs::exists(m_cur / name, ec)) { problem->set_text(_("This name already exists.")); return; }
        fs::create_directories(m_cur / name, ec);
        if (ec) { problem->set_text(Glib::ustring(_("Cannot create this folder")) + " : " + ec.message()); return; }
        ask.response(Gtk::RESPONSE_OK);
    });
    cancel->signal_clicked().connect([&ask] { ask.response(Gtk::RESPONSE_CANCEL); });
    foot->pack_end(*create, Gtk::PACK_SHRINK);
    foot->pack_end(*cancel, Gtk::PACK_SHRINK);
    auto* col = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
    col->pack_start(*body, Gtk::PACK_EXPAND_WIDGET);
    col->pack_start(*foot, Gtk::PACK_SHRINK);
    ask.get_content_area()->set_spacing(0);
    ask.get_content_area()->pack_start(*col, Gtk::PACK_EXPAND_WIDGET);
    ask.set_default_response(Gtk::RESPONSE_OK);
    ask.show_all();
    create->grab_default();
    entry->grab_focus();
    const int r = ask.run();
    ask.hide();
    if (r == Gtk::RESPONSE_OK) refresh();
}

void RomFolderDialog::on_open_in_manager() {
    ui::open_uri(Glib::filename_to_uri(m_cur.string()), this);
}

void RomFolderDialog::on_drop(const Glib::RefPtr<Gdk::DragContext>& ctx, int, int, const Gtk::SelectionData& data,
                              guint, guint time) {
    std::vector<fs::path> sources;
    for (const auto& uri : data.get_uris()) {
        try { sources.emplace_back(Glib::filename_from_uri(uri)); }
        catch (const Glib::Error&) {}
    }
    ctx->drag_finish(!sources.empty(), false, time);
    if (!sources.empty() && !m_busy) start_copy(std::move(sources));
}

void RomFolderDialog::set_busy(bool busy) {
    m_busy = busy;
    m_new->set_sensitive(!busy);
    m_scan->set_sensitive(!busy);
    m_up->set_sensitive(!busy && m_cur != m_root);
}

void RomFolderDialog::start_copy(std::vector<fs::path> sources) {
    if (m_worker.joinable()) m_worker.join();
    set_busy(true);
    m_copied = m_skipped = m_failed = 0;
    const fs::path dest = m_cur;
    m_worker = std::thread([this, sources = std::move(sources), dest] {
        for (const auto& src : sources) {
            {
                std::lock_guard<std::mutex> l(m_mutex);
                m_status = src.filename().string();
            }
            m_progress_dispatcher.emit();
            std::error_code ec;
            const fs::path target = dest / src.filename();
            // Pas de copie dans soi-meme, et rien n'est ecrase.
            if (fs::exists(target, ec)) { ++m_skipped; continue; }
            if (fs::is_directory(src, ec)) fs::copy(src, target, fs::copy_options::recursive, ec);
            else                           fs::copy_file(src, target, ec);
            if (ec) { ++m_failed; std::error_code ignored; fs::remove_all(target, ignored); }
            else    ++m_copied;
        }
        m_done_dispatcher.emit();
    });
}

void RomFolderDialog::on_copy_progress() {
    std::lock_guard<std::mutex> l(m_mutex);
    m_hint->set_text(Glib::ustring(_("Copying")) + " " + m_status + " …");
}

void RomFolderDialog::on_copy_done() {
    if (m_worker.joinable()) m_worker.join();
    set_busy(false);
    refresh();
    Glib::ustring summary = Glib::ustring::compose(_("%1 added"), m_copied);
    if (m_skipped) summary += " · " + Glib::ustring::compose(_("%1 already there"), m_skipped);
    if (m_failed)  summary += " · " + Glib::ustring::compose(_("%1 could not be copied"), m_failed);
    m_hint->set_text(summary + ".  " + m_hint->get_text());
}
