// src/RomDownloadTab.cpp
#include "RomDownloadTab.h"

#include "BootcadeAuth.h"
#include "ConfirmationDialog.h"
#include "FileChooser.h"
#include "LolRoms.h"
#include "CrcDb.h"
#include "RetroRoms.h"
#include "RomDownload.h"
#include "SourcesDialog.h"
#include "i18n.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <set>

namespace fs = std::filesystem;
namespace ui = SettingsUi;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}
std::string key_of(const std::string& system, const std::string& name) { return system + "/" + name; }

// Un travail hors du fil GTK, dont le resultat revient par un minuteur.
template <typename T>
void run_async(std::shared_ptr<bool> alive, std::function<T()> work, std::function<void(T)> done) {
    struct St { std::atomic<bool> ready{false}; T value{}; };
    auto st = std::make_shared<St>();
    std::thread([st, work] { st->value = work(); st->ready = true; }).detach();
    Glib::signal_timeout().connect([st, alive, done] {
        if (!*alive) return false;
        if (!st->ready) return true;
        done(st->value);
        return false;
    }, 150);
}

} // namespace

RomDownloadTab::RomDownloadTab(Env env)
    : Gtk::Box(Gtk::ORIENTATION_VERTICAL, ui::kCardSpacing), m_env(std::move(env)) {
    get_style_context()->add_class("set-page");
    build_top();
    build_table();
    build_queue();
    build_footer();
    m_check_done.connect([this] {
        // Une verification annulee peut encore signaler sa fin : seule la
        // derniere lancee compte.
        if (m_check_finished != m_check_gen) return;
        if (m_check.joinable()) m_check.join();
        m_checking = false;
        m_avail = std::move(m_check_result);
        m_size = std::move(m_check_size);
        m_crcinfo = std::move(m_check_crcinfo);
        m_avail_for = m_check_for;
        set_loading(true, _("Building the list…"));
        populate();
        set_loading(false);
    });
    m_worker_event.connect(sigc::mem_fun(*this, &RomDownloadTab::on_worker_event));
    reload_sources();
    show_all_children();
    // Le compte et le quota de la source deja choisie : le signal du menu est
    // coupe pendant son remplissage, il fallait changer de source pour les voir.
    update_quota();
    refresh();
}

RomDownloadTab::~RomDownloadTab() {
    *m_alive = false;
    m_check_cancel = true;
    m_stop_all = true;
    m_cancel_current = true;
    if (m_check.joinable()) m_check.join();
    if (m_worker.joinable()) m_worker.join();
}

// ═══ Construction ═══════════════════════════════════════════════════════════

void RomDownloadTab::build_top() {
    auto* top = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, ui::kCardSpacing);

    // ── La source ─────────────────────────────────────────────────────────
    auto src = ui::card("bc-globe.svg", _("Download source"), _("Where the missing sets are taken from."));
    auto* line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    line->set_margin_top(10);
    m_source.set_size_request(ui::kFieldWidth, -1);
    m_source.signal_changed().connect([this] {
        if (m_sources_loading) return;
        DownloadSources::set_chosen_source(m_source.get_active_id());
        update_quota();
        start_availability_check();
        populate();
    });
    line->pack_start(m_source, Gtk::PACK_SHRINK);
    auto* manage = ui::button(_("Manage…"), "bc-sliders.svg");
    manage->set_tooltip_text(_("Accounts, custom sources, and which sources are offered."));
    manage->signal_clicked().connect([this] {
        auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
        if (!top) return;
        SourcesDialog dlg(*top);
        dlg.signal_account_changed().connect([this] { m_sig_account.emit(); });
        dlg.run();
        dlg.hide();
        top->present();
        reload_sources();
        update_quota();
        start_availability_check();
        populate();
    });
    line->pack_start(*manage, Gtk::PACK_SHRINK);
    // Le compte de la source, a cote de Manage : pastille verte quand on y est connecte.
    m_conn.set_valign(Gtk::ALIGN_CENTER);
    m_conn.set_margin_start(6);
    line->pack_start(m_conn, Gtk::PACK_SHRINK);
    src.body->pack_start(*line, Gtk::PACK_SHRINK);
    m_account.set_xalign(0.0f);
    m_account.set_line_wrap(true);
    m_account.get_style_context()->add_class("set-sub");
    m_account.set_margin_top(8);
    src.body->pack_start(m_account, Gtk::PACK_SHRINK);
    m_quota_bar.set_mode(Gtk::LEVEL_BAR_MODE_CONTINUOUS);
    m_quota_bar.set_margin_top(6);
    m_quota_bar.set_size_request(-1, 8);
    m_quota_bar.set_no_show_all(true);
    src.body->pack_start(m_quota_bar, Gtk::PACK_SHRINK);
    top->pack_start(*src.frame, Gtk::PACK_EXPAND_WIDGET);

    // ── Ou ils vont ───────────────────────────────────────────────────────
    auto dst = ui::card("bc-folder.svg", _("Download to"), _("Where the downloaded sets are written."));
    m_target.append("inbox",   _("Import folder : checked against the DAT, then filed"));
    m_target.append("outbox",  _("Outbox : ready to be moved to the library"));
    m_target.append("library", _("Library : straight into your ROM folders"));
    m_target.append("folder",  _("Another folder…"));
    m_target.set_active_id(DownloadSources::download_target());
    if (m_target.get_active_row_number() < 0) m_target.set_active_id("inbox");
    m_target.set_margin_top(10);
    m_target.signal_changed().connect([this] {
        DownloadSources::set_download_target(target());
        update_target_ui();
    });
    dst.body->pack_start(m_target, Gtk::PACK_SHRINK);
    m_folder_row = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    m_folder_row->set_margin_top(8);
    m_folder.set_text(DownloadSources::download_folder());
    m_folder.set_placeholder_text(_("Folder"));
    m_folder.signal_changed().connect([this] { DownloadSources::set_download_folder(m_folder.get_text()); update_target_ui(); });
    m_folder_row->pack_start(m_folder, Gtk::PACK_EXPAND_WIDGET);
    auto* browse = ui::button(_("Browse…"), "bc-folder.svg");
    browse->signal_clicked().connect([this] {
        auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
        BcFileChooser dlg(_("Download folder"), Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
        if (top) dlg.set_transient_for(*top);
        dlg.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
        dlg.add_button(_("Select"), Gtk::RESPONSE_OK);
        if (!m_folder.get_text().empty()) dlg.set_current_folder(m_folder.get_text());
        if (dlg.run() == Gtk::RESPONSE_OK) m_folder.set_text(dlg.get_filename());
    });
    m_folder_row->pack_start(*browse, Gtk::PACK_SHRINK);
    m_folder_row->set_no_show_all(true);
    dst.body->pack_start(*m_folder_row, Gtk::PACK_SHRINK);
    m_folder_note.set_xalign(0.0f);
    m_folder_note.set_line_wrap(true);
    m_folder_note.set_line_wrap_mode(Pango::WRAP_WORD_CHAR);   // un long chemin se coupe aussi
    m_folder_note.set_max_width_chars(60);
    m_folder_note.get_style_context()->add_class("set-sub");
    m_folder_note.set_margin_top(8);
    dst.body->pack_start(m_folder_note, Gtk::PACK_SHRINK);
    top->pack_start(*dst.frame, Gtk::PACK_EXPAND_WIDGET);

    pack_start(*top, Gtk::PACK_SHRINK);
    update_target_ui();
}

void RomDownloadTab::update_target_ui() {
    const std::string t = target();
    const bool folder = t == "folder";
    // show_all() ne fait rien sur une boite « no_show_all » : ses enfants
    // restaient caches, et le champ du dossier n'apparaissait jamais.
    for (Gtk::Widget* c : m_folder_row->get_children()) c->show();
    m_folder_row->set_visible(folder);
    std::string note;
    if (t == "inbox")
        note = Glib::ustring::compose(_("%1 : Import checks every set against the DAT and files it into your library."),
                                      m_env.inbox ? m_env.inbox() : std::string()).raw();
    else if (t == "outbox")
        note = Glib::ustring::compose(_("%1, in the DAT's folder : Outbox › Move to library puts them in place. "
                                        "A 7z goes through Import, which converts it."),
                                      m_env.outbox ? m_env.outbox() : std::string()).raw();
    else if (t == "library")
        note = _("Into the first ROM folder of the emulator, in the DAT's folder, then a scan. Only sets whose CRC match "
                 "the DAT get there ; a 7z goes through Import, which converts it.");
    else
        note = m_folder.get_text().empty() ? std::string(_("Choose a folder.")) : std::string(_("Sets stay in this folder."));
    m_folder_note.set_text(note);
}

void RomDownloadTab::build_table() {
    auto* head = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    m_group.set_xalign(0.0f);
    m_group.get_style_context()->add_class("set-sub");
    head->pack_start(m_group, Gtk::PACK_SHRINK);
    m_list_status.set_xalign(0.0f);
    m_list_status.set_ellipsize(Pango::ELLIPSIZE_END);
    m_list_status.get_style_context()->add_class("set-sub");
    head->pack_start(m_list_status, Gtk::PACK_EXPAND_WIDGET);
    m_btn_audit = ui::button(_("Audit library"), "bc-chart.svg", ui::Tone::Accent);
    m_btn_audit->set_tooltip_text(_("The missing sets come from the audit of the Library tab."));
    m_btn_audit->signal_clicked().connect([this] { m_sig_audit.emit(); });
    m_btn_audit->set_no_show_all(true);
    head->pack_end(*m_btn_audit, Gtk::PACK_SHRINK);
    pack_start(*head, Gtk::PACK_SHRINK);

    m_filter = Gtk::make_managed<ui::FilterBar>(_("Search (game, system…)"));
    m_system_combo = m_filter->add_combo(_("System:"));
    m_system_combo->append(_("All"));
    m_system_combo->set_active(0);
    m_filter->signal_changed().connect(sigc::mem_fun(*this, &RomDownloadTab::refilter));
    // Par defaut : les jeux qui manquent, dont le fichier du site a les bons CRC.
    // Deux cases elargissent la liste.
    m_p_invalid = Gtk::make_managed<Gtk::CheckButton>();
    m_p_all     = Gtk::make_managed<Gtk::CheckButton>();
    set_filter_counts(0, 0);
    m_p_invalid->set_tooltip_text(_("Also list the games whose file on the source has other CRC than your DAT (the wrong "
                                    "version). They are greyed ; you can still tick one to download it anyway."));
    m_p_all->set_tooltip_text(_("List everything the source has : also the games whose CRC are not known yet, and the games "
                                "your library already has. Tick one to download it (again)."));
    for (auto* c : {m_p_invalid, m_p_all}) {
        m_pills.add(*c);
        c->signal_toggled().connect([this] { if (m_table) refilter(); });
    }
    m_pills.set_selection_mode(Gtk::SELECTION_NONE);
    m_pills.set_homogeneous(false);
    m_pills.set_column_spacing(26);
    m_pills.set_row_spacing(6);
    m_pills.set_max_children_per_line(2);
    pack_start(m_pills, Gtk::PACK_SHRINK);
    pack_start(*m_filter, Gtk::PACK_SHRINK);

    m_store = Gtk::ListStore::create(m_cols);
    m_models.sort_column = m_cols.game.index();
    m_table = Gtk::make_managed<ui::Table>(Gtk::SELECTION_MULTIPLE);
    m_models.attach(m_table->view(), m_store, sigc::mem_fun(*this, &RomDownloadTab::row_visible));
    m_table->add_check_column(m_cols.include, sigc::mem_fun(*this, &RomDownloadTab::on_toggled),
                              [this](bool on) { set_all(on); });
    std::vector<std::pair<Gtk::TreeViewColumn*, bool>> cols;
    { ui::ColumnOptions o; o.expand = true; o.min_width = 260; cols.push_back({m_table->add_text_column(_("Game"), m_cols.game, o), false}); }
    cols.push_back({m_table->add_text_column(_("System"), m_cols.system), false});
    cols.push_back({m_table->add_text_column(_("Status"), m_cols.status), false});
    { ui::ColumnOptions o; o.min_width = 190; cols.push_back({m_table->add_text_column(_("On this source"), m_cols.avail, o), false}); }
    { ui::ColumnOptions o; o.mono = true; o.min_width = 110; o.sortable = false;
      auto* c = m_table->add_text_column(_("CRC"), m_cols.crc, o);
      cols.push_back({c, true}); }
    { ui::ColumnOptions o; o.xalign = 1.0f; o.sortable = false; cols.push_back({m_table->add_text_column(_("Size"), m_cols.size, o), false}); }
    // Une ligne d'une autre version ou absente de la source s'efface ; la colonne
    // CRC dit OK en vert, KO en rouge.
    for (auto& [col, is_crc] : cols) {
        Gtk::TreeViewColumn* c = col;
        const bool crc_col = is_crc;
        c->set_cell_data_func(*c->get_first_cell(), [this, crc_col](Gtk::CellRenderer* r, const Gtk::TreeModel::iterator& it) {
            style_cell(r, it, crc_col);
        });
    }
    m_table->set_size_request(-1, 140);
    // L'indicateur de chargement, au centre de la liste.
    m_overlay.add(*m_table);
    m_loading.set_halign(Gtk::ALIGN_CENTER);
    m_loading.set_valign(Gtk::ALIGN_CENTER);
    m_loading.get_style_context()->add_class("set-card");
    m_loading.set_border_width(18);
    m_spinner.set_size_request(28, 28);
    m_loading.pack_start(m_spinner, Gtk::PACK_SHRINK);
    m_loading_text.get_style_context()->add_class("set-title");
    m_loading.pack_start(m_loading_text, Gtk::PACK_SHRINK);
    m_loading.set_no_show_all(true);
    m_overlay.add_overlay(m_loading);
    m_overlay.set_overlay_pass_through(m_loading, false);
}

void RomDownloadTab::set_loading(bool on, const Glib::ustring& text) {
    if (on) {
        m_loading_text.set_text(text);
        // show_all() ne fait rien sur un widget « no_show_all » : on montre chaque piece.
        m_spinner.show();
        m_loading_text.show();
        m_loading.show();
        m_spinner.start();
        // Peint maintenant : la suite occupe le fil de l'interface, et sans cela rien n'apparait.
        while (Gtk::Main::events_pending()) Gtk::Main::iteration(false);
    } else {
        m_spinner.stop();
        m_loading.hide();
    }
}

void RomDownloadTab::set_filter_counts(int invalid, int all) {
    m_p_invalid->set_label(Glib::ustring::compose(_("Show CRC invalid (%1)"), invalid));
    m_p_all->set_label(Glib::ustring::compose(_("Show all, even the games already in my library (%1)"), all));
}

void RomDownloadTab::ensure_colours() {
    if (m_colours.ready || !get_toplevel() || !get_toplevel()->get_realized()) return;
    m_colours.ok    = ui::probe_color(*this, "set-ok");
    m_colours.err   = ui::probe_color(*this, "set-err");
    m_colours.muted = ui::probe_color(*this, "set-sub");
    m_colours.ready = true;
}

void RomDownloadTab::style_cell(Gtk::CellRenderer* renderer, const Gtk::TreeModel::iterator& it, bool crc_column) {
    auto* text = dynamic_cast<Gtk::CellRendererText*>(renderer);
    if (!text) return;
    ensure_colours();
    if (!m_colours.ready) return;
    const int cat = (*it)[m_cols.cat];
    const int crc = (*it)[m_cols.crc_state];
    const bool faded = cat == 1 || cat == 3;   // autre version, ou absent de la source
    Gdk::RGBA c = m_colours.muted;
    bool bold = false;
    if (crc_column && crc == 0)      { c = m_colours.ok;  bold = true; }
    else if (crc_column && crc == 1) { c = m_colours.err; bold = true; }
    else if (!faded && !crc_column)  { text->property_foreground_set() = false; text->property_weight() = Pango::WEIGHT_NORMAL; return; }
    text->property_foreground_rgba() = c;
    text->property_weight() = bold ? Pango::WEIGHT_BOLD : Pango::WEIGHT_NORMAL;
}

void RomDownloadTab::build_queue() {
    auto q = ui::card("bc-download.svg", _("Downloads"), _("In progress and done. Each one can be cancelled."));
    m_queue_summary.set_xalign(1.0f);
    m_queue_summary.get_style_context()->add_class("set-sub");
    m_queue_summary.set_valign(Gtk::ALIGN_CENTER);
    q.head->pack_end(m_queue_summary, Gtk::PACK_SHRINK);
    m_queue_list.set_selection_mode(Gtk::SELECTION_NONE);
    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    scroll->set_min_content_height(80);
    scroll->set_name("no-propagate-width");
    scroll->add(m_queue_list);
    scroll->set_margin_top(8);
    q.body->pack_start(*scroll, Gtk::PACK_EXPAND_WIDGET);
    auto* empty = Gtk::make_managed<Gtk::Label>(_("Nothing downloading. Tick sets above, then Download."));
    empty->get_style_context()->add_class("set-sub");
    empty->show();
    m_queue_list.set_placeholder(*empty);
    // Tableau en haut, telechargements en bas, une poignee entre les deux.
    pack_start(*ui::splitter(m_overlay, *q.frame, 260), Gtk::PACK_EXPAND_WIDGET);
}

void RomDownloadTab::build_footer() {
    auto* footer = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    m_btn_clear = ui::button(_("Clear finished"), "bc-clear.svg");
    m_btn_clear->signal_clicked().connect([this] {
        std::lock_guard<std::mutex> lk(m_qmutex);
        m_queue.erase(std::remove_if(m_queue.begin(), m_queue.end(), [this](const auto& it) {
            if (it->state == State::Queued || it->state == State::Running) return false;
            if (it->row) ui::destroy_child(m_queue_list, it->row);
            return true;
        }), m_queue.end());
    });
    footer->pack_start(*m_btn_clear, Gtk::PACK_SHRINK);
    m_btn_cancel_all = ui::button(_("Cancel all"), "bc-close.svg", ui::Tone::Danger);
    m_btn_cancel_all->signal_clicked().connect([this] {
        {
            std::lock_guard<std::mutex> lk(m_qmutex);
            for (auto& it : m_queue) if (it->state == State::Queued) { it->state = State::Cancelled; it->note = _("cancelled"); }
        }
        m_stop_all = true;
        m_cancel_current = true;
        refresh_queue_rows();
    });
    footer->pack_start(*m_btn_cancel_all, Gtk::PACK_SHRINK);
    m_btn_download = ui::button(_("Download"), "bc-download.svg", ui::Tone::Accent);
    m_btn_download->signal_clicked().connect(sigc::mem_fun(*this, &RomDownloadTab::enqueue_selected));
    footer->pack_end(*m_btn_download, Gtk::PACK_SHRINK);
    pack_start(*footer, Gtk::PACK_SHRINK);
}

// ═══ Sources ════════════════════════════════════════════════════════════════

void RomDownloadTab::reload_sources() {
    m_sources_loading = true;
    std::string chosen = m_source.get_active_id();
    if (chosen.empty()) chosen = DownloadSources::chosen_source();
    m_sources.clear();
    m_source.remove_all();
    for (auto& s : DownloadSources::load()) {
        if (!s.enabled) continue;
        m_source.append(s.id, s.name);
        m_sources.push_back(s);
    }
    if (!m_source.set_active_id(chosen) && !m_sources.empty()) m_source.set_active(0);
    m_sources_loading = false;
}

DownloadSources::Source RomDownloadTab::current_source() const {
    const std::string id = m_source.get_active_id();
    for (const auto& s : m_sources) if (s.id == id) return s;
    return {};
}

bool RomDownloadTab::eligible(const Set& s, const DownloadSources::Source& src) const {
    if (src.id.empty()) return false;
    if (src.kind == "bootcade")  return s.emulator == "fbneo" && !s.is_chd && !s.dat_header.empty();
    if (src.kind == "lolroms")   return !LolRoms::folder_for(s.emulator, s.system).empty();
    if (src.kind == "retroroms") return !RetroRoms::folder_for(s.emulator, s.system).empty();
    return src.serves(s.emulator) && !s.is_chd;
}

std::string RomDownloadTab::destination(const Set& s) const {
    const std::string t = target();
    // Le dossier du DAT, comme la bibliotheque et l'Outbox les rangent.
    const std::string folder = !s.dat_header.empty() ? s.dat_header
                             : s.emulator == "mame" ? "MAME " + s.system : "FinalBurn Neo - " + s.system + " Games";
    if (t == "outbox") {
        const std::string o = m_env.outbox ? m_env.outbox() : std::string();
        return o.empty() ? std::string() : (fs::path(o) / folder).string();
    }
    if (t == "library") {
        const std::string r = m_env.library_root ? m_env.library_root(s.emulator) : std::string();
        return r.empty() ? std::string() : (fs::path(r) / folder).string();
    }
    if (t == "folder") return m_folder.get_text();
    return m_env.inbox ? m_env.inbox() : std::string();
}

void RomDownloadTab::show_connection(const std::string& user, bool connected) {
    ui::destroy_children(m_conn);
    if (user.empty() && !connected) return;
    auto* dot = ui::status_dot(connected ? Glib::ustring::compose(_("Connected as %1"), user).raw()
                                         : std::string(_("Not connected")),
                               connected ? ui::State::Ok : ui::State::Muted);
    m_conn.pack_start(*dot, Gtk::PACK_SHRINK);
    m_conn.show_all();
}

void RomDownloadTab::show_quota(int used, int limit) {
    if (limit <= 0) { m_quota_bar.hide(); return; }
    m_quota_bar.set_min_value(0);
    m_quota_bar.set_max_value(limit);
    m_quota_bar.set_value(std::clamp(used, 0, limit));
    m_quota_bar.show();
}

// Le compte et le quota de la source, demandes a part : le site peut tarder.
void RomDownloadTab::update_quota() {
    const auto src = current_source();
    show_connection("", false);
    show_quota(0, 0);
    if (src.id.empty()) { m_account.set_text(_("No source enabled : turn one on with Manage.")); return; }
    const int used = DownloadSources::used_last_24h(src.id);
    if (src.kind == "lolroms") {
        m_account.set_text(_("No account, no quota."));
    } else if (src.kind == "bootcade") {
        if (!BootcadeAuth::signed_in()) {
            m_account.set_text(_("Sign in with Manage to download from Bootcade."));
            show_connection(" ", false);
            return;
        }
        const std::string user = BootcadeAuth::username();
        show_connection(user, true);
        m_account.set_text(_("Asking the Bootcade server for your quota…"));
        run_async<RomDownload::Quota>(m_alive, [] { return RomDownload::fetch_quota(); }, [this](RomDownload::Quota q) {
            if (current_source().kind != "bootcade") return;
            if (!q.answered) { m_account.set_text(std::string(_("The Bootcade server did not answer : ")) + q.error); return; }
            if (!q.can_download) { m_account.set_text(RomDownload::explain(q.reason)); return; }
            m_account.set_text(Glib::ustring::compose(_("Bootcade quota : %1 of %2 downloads used today (%3 left)"),
                                                      q.quota - q.remaining, q.quota, q.remaining));
            show_quota(q.quota - q.remaining, q.quota);
        });
    } else if (src.kind == "retroroms") {
        m_account.set_text(_("Asking RetroRoms…"));
        run_async<RetroRoms::Status>(m_alive, [] { return RetroRoms::status(); }, [this, used, src](RetroRoms::Status st) {
            if (current_source().kind != "retroroms") return;
            if (!st.answered) { m_account.set_text(_("RetroRoms does not answer.")); return; }
            if (!st.signed_in) {
                show_connection(" ", false);
                m_account.set_text(_("Sign in with Manage to download from RetroRoms."));
                return;
            }
            show_connection(st.user, true);
            const int u = st.limit > 0 ? st.used : used;
            const int l = st.limit > 0 ? st.limit : src.daily_quota;
            // Le site n'affiche pas son compteur : Bootcade compte ce qu'il y a telecharge.
            m_account.set_text(st.limit > 0
                ? Glib::ustring::compose(_("RetroRoms quota : %1 of %2 used"), u, l)
                : Glib::ustring::compose(_("RetroRoms quota : %1 of %2 downloads in the last 24 h "
                                           "(the site does not show its counter : Bootcade counts the downloads it made)"), u, l));
            show_quota(u, l);
        });
    } else {
        if (!src.user.empty()) show_connection(src.user, true);
        m_account.set_text(src.daily_quota > 0
            ? Glib::ustring::compose(_("Quota : %1 of %2 downloads in the last 24 h (counted by Bootcade)"), used, src.daily_quota)
            : Glib::ustring(_("No quota.")));
        show_quota(used, src.daily_quota);
    }
}

// Ce que la source a, sans rien telecharger : une page par dossier pour les
// sites, une requete gratuite par jeu pour Bootcade (liste courte seulement).
void RomDownloadTab::start_availability_check() {
    const auto src = current_source();
    if (src.id.empty() || m_sets.empty() || m_avail_for == src.id) return;
    if (m_checking) { m_check_cancel = true; if (m_check.joinable()) m_check.join(); }
    if (m_check.joinable()) m_check.join();
    m_check_cancel = false;
    m_checking = true;
    m_avail.clear();
    m_avail_for.clear();
    m_list_status.set_text(Glib::ustring::compose(_("Asking %1 what it has…"), src.name));
    set_loading(true, Glib::ustring::compose(_("Loading what %1 holds and checking the CRC…"), src.name));
    std::vector<Set> sets;
    for (const auto& s : m_sets) if (eligible(s, src)) sets.push_back(s);
    const int gen = ++m_check_gen;
    m_check = std::thread([this, src, sets, gen] {
        std::map<std::string, Avail> out;
        std::map<std::string, std::string> sizes;
        std::map<std::string, std::pair<int, int>> crcinfo;
        RomDownload::ServerView changes;
        if (src.kind == "bootcade") {
            if (BootcadeAuth::signed_in() && std::count_if(sets.begin(), sets.end(), [](const Set& x) { return !x.owned; }) <= (long)kMaxAtOnce)
                for (const auto& s : sets) {
                    if (m_check_cancel) break;
                    if (s.owned) continue;   // une requete par jeu : seulement pour ce qui manque
                    const auto o = changes.check(s.dat_header, s.name);
                    out[key_of(s.system, s.name)] = o == RomDownload::Offer::Yes ? Avail::Yes
                                                  : o == RomDownload::Offer::Absent ? Avail::No
                                                  : o == RomDownload::Offer::Outdated ? Avail::Outdated : Avail::Unknown;
                }
        } else if (src.kind == "lolroms" || src.kind == "retroroms") {
            LolRoms::Catalog lol;
            RetroRoms::Catalog retro;
            // La base de CRC de CT 105 : ce que chaque fichier du site contient.
            std::map<std::string, CrcDb::Db> dbs;
            auto db_for = [&](const Set& s) -> const CrcDb::Db* {
                const std::string f = CrcDb::file_for(src.kind, s.emulator);
                if (f.empty()) return nullptr;
                auto it = dbs.find(f);
                if (it == dbs.end()) { CrcDb::Db d; d.load(f); it = dbs.emplace(f, std::move(d)).first; }
                return it->second.loaded() ? &it->second : nullptr;
            };
            for (const auto& s : sets) {
                if (m_check_cancel) break;
                bool known = false;
                std::string day, size;
                const bool has = src.kind == "lolroms" ? lol.has(s.emulator, s.system, s.name, s.is_chd, known, &day, &size)
                                                       : retro.has(s.emulator, s.system, s.name, s.is_chd, known, &day, &size);
                if (!known) continue;
                if (!has) { out[key_of(s.system, s.name)] = Avail::No; continue; }
                sizes[key_of(s.system, s.name)] = size;
                // Les CRC du fichier, connus : ils disent tout, sans date a deviner.
                if (const CrcDb::Db* db = db_for(s)) {
                    std::string folder;
                    if (src.kind == "lolroms") folder = LolRoms::folder_for(s.emulator, s.system);
                    else if (s.emulator == "fbneo") folder = fs::path(RetroRoms::folder_for(s.emulator, s.system)).filename().string();
                    int have = 0;
                    const auto v = db->check(CrcDb::key_for(src.kind, folder, s.name), s.crcs, &have);
                    if (v == CrcDb::Verdict::Match || v == CrcDb::Verdict::Mismatch) crcinfo[key_of(s.system, s.name)] = {have, (int)s.crcs.size()};
                    if (v == CrcDb::Verdict::Match)    { out[key_of(s.system, s.name)] = Avail::Verified; continue; }
                    if (v == CrcDb::Verdict::Mismatch) { out[key_of(s.system, s.name)] = Avail::Outdated; continue; }
                    // Rien a comparer (un CHD) : on retombe sur les dates ci-dessous.
                    if (!(v == CrcDb::Verdict::Unknown && (s.is_chd || s.crcs.empty()))) {
                        out[key_of(s.system, s.name)] = Avail::Unverified;
                        continue;
                    }
                }
                // Deja recu dans une autre version, et le fichier n'a pas change depuis.
                if (DownloadSources::known_wrong(src.id, key_of(s.system, s.name), day)) {
                    out[key_of(s.system, s.name)] = Avail::Outdated;
                    continue;
                }
                // Plus ancien que le dernier changement de ses ROMs dans le DAT :
                // meme nom, autre version.
                const std::string changed = changes.changed_on(s.name);
                out[key_of(s.system, s.name)] = !changed.empty() && !day.empty() && day < changed ? Avail::Outdated : Avail::Yes;
            }
        }
        if (!m_check_cancel) {
            m_check_result = std::move(out);
            m_check_size = std::move(sizes);
            m_check_crcinfo = std::move(crcinfo);
            m_check_for = src.id;
        }
        m_check_finished = gen;
        m_check_done();
    });
}

// ═══ Liste ══════════════════════════════════════════════════════════════════

void RomDownloadTab::refresh() {
    m_sets.clear();
    const RomAudit::Report* rep = m_env.report ? m_env.report() : nullptr;
    m_group.set_text(m_env.group_label ? m_env.group_label() : std::string());
    if (!rep) {
        m_emulator.clear();
        m_btn_audit->show();
        m_list_status.set_text(_("Run an audit (Library tab) to list the missing sets."));
        populate();
        return;
    }
    m_btn_audit->hide();
    m_emulator = rep->emulator;
    for (const auto& g : rep->games) {
        if (g.ignored) continue;
        if (!g.is_chd && !g.archive_is_own) continue;
        // Ce que la bibliotheque a deja (ou peut reconstruire elle-meme) reste
        // dans la liste : le joueur voit tout, et peut tout retelecharger.
        const bool owned = (g.status != "missing" && g.status != "incorrect") || g.repairable;
        Set s{g.dat_header, g.name, g.description, g.system, rep->emulator, g.status, g.is_chd, {}, owned};
        // Les ROMs PROPRES du jeu : celles du parent (merge=) se verifient dans le zip du parent,
        // et un site range tantot des zips split, tantot complets.
        for (const auto& r : g.roms) if (!r.is_disk && !r.inherited && !r.merge_attr && r.crc) s.crcs.push_back(r.crc);
        m_sets.push_back(std::move(s));
    }
    m_avail.clear();
    m_avail_for.clear();
    set_loading(true, _("Building the list…"));
    populate();
    set_loading(false);
    start_availability_check();
}

void RomDownloadTab::populate() {
    const auto src = current_source();
    m_models.detach(m_table->view());
    m_store->clear();
    std::set<std::string> systems;
    int n_invalid = 0, n_all = 0;
    for (size_t i = 0; i < m_sets.size(); ++i) {
        const auto& s = m_sets[i];
        const std::string key = key_of(s.system, s.name);
        const bool ok = eligible(s, src);
        Avail a = Avail::Unknown;
        if (auto it = m_avail.find(key); it != m_avail.end() && m_avail_for == src.id) a = it->second;
        // On ne parle que de ce que le site contient : ce qu'il n'a pas n'est pas liste.
        if (!ok || a == Avail::No) continue;
        auto row = *(m_store->append());
        systems.insert(s.system);

        // Une seule categorie par jeu.
        int cat = 0;
        Glib::ustring text;
        if (!ok)                          { cat = 3; text = _("not offered by this source"); }
        else if (a == Avail::No)          { cat = 3; text = _("not on this source"); }
        else if (a == Avail::Outdated)    { cat = 1; text = _("other version than the DAT"); }
        else if (a == Avail::Unverified)  { cat = 2; text = _("not verified yet"); }
        else if (a == Avail::Verified)    { cat = 0; text = _("available"); }
        else if (a == Avail::Yes)         { cat = 0; text = _("available (by date)"); }
        else if (m_checking)              { cat = 2; text = _("checking…"); }
        else                              { cat = 0; text = _("checked at download"); }

        // La colonne CRC : combien de CRC du DAT le fichier du site contient.
        Glib::ustring crc = "—";
        int crc_state = 2;
        if (auto ci = m_crcinfo.find(key); ci != m_crcinfo.end() && m_avail_for == src.id) {
            crc_state = ci->second.first == ci->second.second ? 0 : 1;
            crc = Glib::ustring::compose(crc_state == 0 ? _("OK  %1/%2") : _("KO  %1/%2"), ci->second.first, ci->second.second);
        } else if (a == Avail::Outdated) {
            crc_state = 1; crc = _("KO");
        } else if (a == Avail::Unverified) {
            crc = "?";
        }

        row[m_cols.include]    = false;
        row[m_cols.game]       = s.description.empty() ? s.name : s.name + "  (" + s.description + ")";
        row[m_cols.system]     = s.system;
        row[m_cols.status]     = s.owned ? _("In library") : s.status == "missing" ? _("Missing") : _("Incorrect");
        row[m_cols.avail]      = text;
        row[m_cols.crc]        = crc;
        {
            auto z = m_size.find(key);
            row[m_cols.size] = z != m_size.end() && m_avail_for == src.id ? z->second : std::string();
        }
        row[m_cols.avail_key]  = ok ? (int)a : -1;
        row[m_cols.index]      = (unsigned int)i;
        row[m_cols.owned]      = s.owned;
        row[m_cols.cat]        = cat;
        row[m_cols.crc_state]  = crc_state;
        // Tout ce que la source a peut etre coche, meme une autre version ou un
        // jeu qu'on possede : c'est au joueur de decider (une confirmation le
        // previent). Seul ce que la source n'a pas ne se coche pas.
        row[m_cols.selectable] = ok && cat != 3;
        row[m_cols.search]     = lower(s.name + ' ' + s.description + ' ' + s.system);
        ++n_all;
        if (cat == 1) ++n_invalid;
    }
    set_filter_counts(n_invalid, n_all);
    m_filter->set_combo_items(m_system_combo, _("All"), systems, m_system_combo->get_active_text());
    refilter();
    const int missing = n_all;
    if (!m_sets.empty() && !m_checking)
        m_list_status.set_text(Glib::ustring::compose(_("%1 game(s) of this source are in your DAT."), missing));
    else if (m_sets.empty() && m_env.report && m_env.report())
        m_list_status.set_text(_("Nothing in the audit."));
}

bool RomDownloadTab::row_visible(const Gtk::TreeModel::const_iterator& it) const {
    const Gtk::TreeModel::Row row = *it;
    const int cat = row[m_cols.cat];
    // Par defaut : manquant, CRC OK. « CRC invalide » ajoute les autres versions ;
    // « tout » ajoute aussi ce dont les CRC sont inconnus et ce qu'on possede deja.
    const bool all = m_p_all->get_active();
    if (!all) {
        if (row[m_cols.owned]) return false;
        if (cat == 2) return false;
        if (cat == 1 && !m_p_invalid->get_active()) return false;
    }
    if (!m_vis_system.empty() && row[m_cols.system] != m_vis_system) return false;
    if (!m_vis_needle.empty() && Glib::ustring(row[m_cols.search]).raw().find(m_vis_needle) == std::string::npos) return false;
    return true;
}

void RomDownloadTab::refilter() {
    m_vis_system = m_system_combo->get_active_row_number() > 0 ? m_system_combo->get_active_text() : Glib::ustring();
    m_vis_needle = lower(m_filter->search_text());
    m_models.detach(m_table->view());
    m_models.attach(m_table->view(), m_store, sigc::mem_fun(*this, &RomDownloadTab::row_visible));
    m_filter->set_summary(Glib::ustring::compose(_("%1 shown"), m_models.visible_count()));
    update_buttons();
}

void RomDownloadTab::on_toggled(const Glib::ustring& path) {
    auto child = m_models.filter->convert_path_to_child_path(m_models.sort->convert_path_to_child_path(Gtk::TreeModel::Path(path)));
    Gtk::TreeModel::Row row = *m_store->get_iter(child);
    if (!row[m_cols.selectable]) return;
    row[m_cols.include] = !row[m_cols.include];
    update_buttons();
}

void RomDownloadTab::set_all(bool on) {
    std::vector<Gtk::TreeModel::iterator> shown;
    for (const auto& frow : m_models.filter->children()) shown.push_back(m_models.filter->convert_iter_to_child_iter(frow));
    m_models.detach(m_table->view());
    const auto src = current_source();
    for (const auto& it : shown) {
        Gtk::TreeModel::Row row = *it;
        if (!row[m_cols.selectable]) continue;
        // La case de l'en-tete ne coche que ce qui est sur : disponible, qu'on ne
        // possede pas. Le reste (autre version, pas verifie, deja possede) se coche
        // a la main. Elle decoche tout.
        const int cat = row[m_cols.cat];
        const bool safe = !row[m_cols.owned] && (cat == 0 || (cat == 2 && src.kind == "lolroms"));
        if (!on || safe) row[m_cols.include] = on;
    }
    m_models.attach(m_table->view(), m_store, sigc::mem_fun(*this, &RomDownloadTab::row_visible));
    update_buttons();
}

void RomDownloadTab::update_buttons() {
    if (m_table) m_table->refresh_header_check();
    int n = 0;
    for (const auto& row : m_store->children()) if (row[m_cols.include]) ++n;
    const auto src = current_source();
    m_btn_download->set_label(n ? Glib::ustring::compose(_("Download from %1 (%2)"), src.name, n)
                                : Glib::ustring::compose(_("Download from %1"), src.name.empty() ? "…" : src.name));
    m_btn_download->set_sensitive(n > 0 && n <= kMaxAtOnce && !src.id.empty());
    m_btn_download->set_tooltip_text(n > kMaxAtOnce
        ? Glib::ustring::compose(_("%1 sets are ticked : download at most %2 at a time. Untick some first."), n, kMaxAtOnce)
        : Glib::ustring(_("Adds the ticked sets to the downloads below.")));
}

// ═══ File ═══════════════════════════════════════════════════════════════════

void RomDownloadTab::enqueue_selected() {
    const auto src = current_source();
    if (src.id.empty()) return;
    auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
    const std::string t = target();
    // Le dossier de base doit exister avant de lancer quoi que ce soit.
    const std::string base = t == "inbox" ? (m_env.inbox ? m_env.inbox() : std::string())
                           : t == "outbox" ? (m_env.outbox ? m_env.outbox() : std::string())
                           : t == "folder" ? std::string(m_folder.get_text())
                           : (m_env.library_root && !m_emulator.empty() ? m_env.library_root(m_emulator) : std::string());
    if (base.empty()) {
        if (top) ui::notice(*top, _("No download folder"),
                            t == "inbox"   ? _("Set an import folder (Import tab) first.")
                          : t == "outbox"  ? _("Set an outbox folder (Outbox tab) first.")
                          : t == "library" ? _("This emulator has no ROM folder : add one in Settings › Library.")
                                           : _("Choose a folder first."));
        return;
    }
    if (src.kind == "bootcade" && !BootcadeAuth::signed_in()) {
        if (top) ui::notice(*top, _("Download from Bootcade"), RomDownload::explain("not_signed_in"));
        return;
    }

    // Ce que le joueur force : un jeu qu'il possede, d'une autre version que son
    // DAT, ou pas encore verifie. Il le peut, mais il est prevenu de ce que ca coute.
    int risky = 0;
    for (const auto& row : m_store->children()) {
        if (!row[m_cols.include]) continue;
        const int cat = row[m_cols.cat];
        if (row[m_cols.owned] || cat == 1 || cat == 2) ++risky;
    }
    if (risky && top) {
        const bool quota = src.kind == "bootcade" || src.kind == "retroroms" || src.daily_quota > 0;
        ConfirmationDialog confirm(*top, _("Download sets that may not fit?"),
            Glib::ustring::compose(_("%1 of the ticked sets are already in your library, are another version than your DAT, "
                                     "or are not verified yet. They are downloaded as you asked%2"),
                                   risky, quota ? Glib::ustring(_(", and each one counts against the daily quota of this source."))
                                                : Glib::ustring(".")),
            "bc-download.svg");
        if (!confirm.show_and_confirm()) return;
    }

    std::vector<std::shared_ptr<Item>> added;
    {
        std::lock_guard<std::mutex> lk(m_qmutex);
        std::set<std::string> queued;
        for (const auto& it : m_queue)
            if (it->state == State::Queued || it->state == State::Running) queued.insert(key_of(it->set.system, it->set.name));
        for (auto row : m_store->children()) {
            if (!row[m_cols.include]) continue;
            row[m_cols.include] = false;
            const auto& s = m_sets[(unsigned int)row[m_cols.index]];
            if (queued.count(key_of(s.system, s.name))) continue;
            auto item = std::make_shared<Item>();
            item->set = s;
            item->source = src;
            item->dest = destination(s);
            item->target = t;
            item->force = (int)row[m_cols.cat] == 1;   // autre version choisie : on la garde
            // LolRoms ne sert que des 7z : vers l'Outbox ou la bibliotheque ils
            // passent de toute facon par Import, autant les y poser directement.
            if ((t == "outbox" || t == "library") && src.kind == "lolroms" && !s.is_chd && m_env.inbox)
                item->dest = m_env.inbox();
            item->note = _("waiting");
            m_queue.push_back(item);
            added.push_back(item);
        }
    }
    // Une ligne par jeu : nom, etat, barre, et sa croix.
    for (auto& item : added) {
        auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
        box->set_border_width(4);
        item->label = Gtk::make_managed<Gtk::Label>(item->set.name + "  ·  " + item->set.system + "  ·  " + item->source.name);
        item->label->set_xalign(0.0f);
        item->label->set_ellipsize(Pango::ELLIPSIZE_END);
        item->label->set_size_request(320, -1);
        box->pack_start(*item->label, Gtk::PACK_EXPAND_WIDGET);
        item->bar = Gtk::make_managed<Gtk::ProgressBar>();
        item->bar->set_size_request(220, -1);
        item->bar->set_valign(Gtk::ALIGN_CENTER);
        box->pack_start(*item->bar, Gtk::PACK_SHRINK);
        item->state_label = Gtk::make_managed<Gtk::Label>();
        item->state_label->set_xalign(0.0f);
        item->state_label->set_size_request(260, -1);
        item->state_label->set_ellipsize(Pango::ELLIPSIZE_END);
        item->state_label->get_style_context()->add_class("set-sub");
        box->pack_start(*item->state_label, Gtk::PACK_SHRINK);
        item->cancel = Gtk::make_managed<Gtk::Button>();
        item->cancel->set_image(*ui::image("bc-close.svg", 14));
        item->cancel->set_tooltip_text(_("Cancel this download"));
        item->cancel->set_relief(Gtk::RELIEF_NONE);
        std::weak_ptr<Item> weak = item;
        item->cancel->signal_clicked().connect([this, weak] {
            auto it = weak.lock();
            if (!it) return;
            {
                std::lock_guard<std::mutex> lk(m_qmutex);
                if (it->state == State::Queued) { it->state = State::Cancelled; it->note = _("cancelled"); }
                else if (it->state == State::Running) m_cancel_current = true;
            }
            refresh_queue_rows();
        });
        box->pack_start(*item->cancel, Gtk::PACK_SHRINK);
        row->add(*box);
        row->show_all();
        item->row = row;
        m_queue_list.append(*row);
    }
    update_buttons();
    refresh_queue_rows();
    if (!added.empty()) start_worker();
}

void RomDownloadTab::start_worker() {
    if (m_running) return;
    if (m_worker.joinable()) m_worker.join();
    m_stop_all = false;
    m_running = true;
    m_worker = std::thread(&RomDownloadTab::worker, this);
}

void RomDownloadTab::worker() {
    std::map<std::string, int> bootcade_left;   // compte -> telechargements restants
    bool bootcade_asked = false, retro_checked = false;
    for (;;) {
        std::shared_ptr<Item> item;
        {
            std::lock_guard<std::mutex> lk(m_qmutex);
            for (auto& it : m_queue) if (it->state == State::Queued) { item = it; break; }
            if (!item || m_stop_all) break;
            item->state = State::Running;
            item->note = _("connecting…");
        }
        m_cancel_current = false;
        m_worker_event();
        const auto& s = item->set;
        const auto& src = item->source;
        { std::error_code dec; fs::create_directories(item->dest, dec); }
        auto progress = [&](double f) {
            { std::lock_guard<std::mutex> lk(m_qmutex); item->progress = f; item->note = _("downloading…"); }
            static thread_local auto last = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            if (now - last > std::chrono::milliseconds(150)) { last = now; m_worker_event(); }
        };
        DownloadSources::Result r;
        bool quota_stop = false, wrong = false, via_import = false;

        // Le quota, avant de demander quoi que ce soit.
        if (src.kind == "bootcade") {
            if (!bootcade_asked) {
                const auto q = RomDownload::fetch_quota();
                bootcade_asked = true;
                bootcade_left[src.id] = q.answered && q.can_download ? q.remaining : 0;
                if (!q.answered) r.reason = std::string(_("The Bootcade server did not answer : ")) + q.error;
                else if (!q.can_download) r.reason = RomDownload::explain(q.reason);
            }
            if (bootcade_left[src.id] <= 0) { quota_stop = true; if (r.reason.empty()) r.reason = _("daily quota used up"); }
        } else if (src.daily_quota > 0 && DownloadSources::used_last_24h(src.id) >= src.daily_quota) {
            quota_stop = true;
            r.reason = _("daily quota reached");
        }

        if (!quota_stop) {
            if (src.kind == "bootcade") {
                RomDownload::ServerView view;
                const auto offer = view.check(s.dat_header, s.name);
                if (offer == RomDownload::Offer::Absent || offer == RomDownload::Offer::Outdated) {
                    r.reason = offer == RomDownload::Offer::Outdated ? _("outdated on the Bootcade server")
                                                                     : _("not available on the Bootcade server");
                } else {
                    auto res = RomDownload::download(s.dat_header, s.name, item->dest, m_cancel_current, progress);
                    r.ok = res.ok; r.path = res.path; r.reason = res.reason; r.http_status = res.http_status;
                    r.site_fault = res.server_fault;
                    if (res.ok) --bootcade_left[src.id];
                    else if (res.http_status == 401 || res.http_status == 403) {
                        const auto again = RomDownload::fetch_quota();
                        bootcade_left[src.id] = again.can_download ? again.remaining : 0;
                        if (!again.can_download) { r.reason = RomDownload::explain(again.reason); quota_stop = true; }
                    }
                }
            } else if (src.kind == "lolroms") {
                auto res = LolRoms::download(s.emulator, s.system, s.name, s.is_chd, item->dest, m_cancel_current, progress);
                r.ok = res.ok; r.path = res.path; r.reason = res.reason; r.site_fault = res.site_fault;
            } else if (src.kind == "retroroms") {
                if (!retro_checked) { retro_checked = true; RetroRoms::status(); }   // rouvre la session si elle a expire
                r = RetroRoms::download(s.emulator, s.system, s.name, s.is_chd, item->dest, m_cancel_current, progress);
                if (!r.ok && r.http_status == 403) {
                    // Pas connecte, ou quota du site atteint : les suivants auraient le meme sort.
                    const auto st = RetroRoms::status();
                    r.reason = st.signed_in ? _("refused by RetroRoms : daily quota reached")
                                            : _("not signed in to RetroRoms (Manage)");
                    quota_stop = true;
                }
            } else {
                r = DownloadSources::download_custom(src, s.system, s.name, item->dest, m_cancel_current, progress);
                if (!r.ok && (r.http_status == 401 || r.http_status == 403)) quota_stop = true;
            }
            // Le bon nom, une autre version : il n'ira pas plus loin, et la source
            // ne le proposera plus tant que son fichier ne change pas.
            if (r.ok && !item->force && !s.is_chd && src.kind != "bootcade" && !LolRoms::has_all_crcs(r.path, s.crcs)) {
                std::error_code ec;
                fs::remove(r.path, ec);
                r.ok = false;
                r.reason = _("other version than the DAT on this source (CRC differ) : will not be offered again");
                std::string day;
                bool known = false;
                if (src.kind == "lolroms") { LolRoms::Catalog c; c.has(s.emulator, s.system, s.name, s.is_chd, known, &day); }
                else if (src.kind == "retroroms") { RetroRoms::Catalog c; c.has(s.emulator, s.system, s.name, s.is_chd, known, &day); }
                DownloadSources::remember_wrong(src.id, key_of(s.system, s.name), day);
                wrong = true;
                { std::lock_guard<std::mutex> lk(m_qmutex); m_wrong_keys.push_back(src.id + "|" + key_of(s.system, s.name)); }
            }
            // Outbox et bibliotheque attendent des zip : un 7z passe par Import, qui le convertit.
            if (r.ok && (item->target == "outbox" || item->target == "library")) {
                std::error_code ec;
                if (fs::is_regular_file(r.path, ec) && fs::path(r.path).extension() != ".zip") {
                    const std::string inbox = m_env.inbox ? m_env.inbox() : std::string();
                    if (!inbox.empty()) {
                        fs::create_directories(inbox, ec);
                        const std::string moved = (fs::path(inbox) / fs::path(r.path).filename()).string();
                        fs::rename(r.path, moved, ec);
                        if (ec) { ec.clear(); fs::copy_file(r.path, moved, fs::copy_options::overwrite_existing, ec); if (!ec) fs::remove(r.path, ec); }
                        if (!ec) { r.path = moved; via_import = true; }
                    }
                }
            }
        }

        {
            std::lock_guard<std::mutex> lk(m_qmutex);
            if (r.ok) {
                item->state = State::Done;
                item->progress = 1.0;
                item->path = r.path;
                item->note = via_import ? _("done : 7z sent to Import, which converts it")
                           : item->target == "library" ? _("done : in the library")
                           : item->target == "outbox" ? _("done : in the outbox") : _("done");
                std::error_code ec;
                if (fs::is_regular_file(r.path, ec) && (via_import || item->target == "inbox")) m_received.push_back(r.path);
                if (item->target == "library" && !via_import) m_to_library.push_back(s.emulator);
                if (item->target == "outbox" && !via_import) m_outbox_got = true;
                m_log_lines.push_back(Glib::ustring::compose(_("Downloaded %1 from %2."), s.name, src.name).raw());
            } else if (m_cancel_current && !quota_stop) {
                item->state = State::Cancelled;
                item->note = _("cancelled");
            } else {
                item->state = State::Failed;
                item->note = r.reason;
                m_log_lines.push_back(s.name + " (" + src.name + ") : " + r.reason);
            }
            // Quota, compte ou site en panne : les suivants de cette source attendent.
            if (quota_stop || r.site_fault)
                for (auto& it : m_queue)
                    if (it->state == State::Queued && it->source.id == src.id) {
                        it->state = State::Failed;
                        it->note = r.reason;
                    }
        }
        // Ce que le site a envoye compte a son quota, meme refuse ensuite.
        if (r.ok || r.transferred || wrong) DownloadSources::record_download(src.id);
        m_worker_event();
    }
    m_running = false;
    m_worker_event();
}

void RomDownloadTab::on_worker_event() {
    refresh_queue_rows();
    std::vector<std::string> received, lines, library;
    bool outbox = false;
    {
        std::lock_guard<std::mutex> lk(m_qmutex);
        lines.swap(m_log_lines);
        if (!m_running) {
            received.swap(m_received);
            library.swap(m_to_library);
            std::swap(outbox, m_outbox_got);
        }
    }
    for (const auto& l : lines) m_sig_log.emit(l);
    if (!m_running) {
        if (m_worker.joinable()) m_worker.join();
        update_quota();
        // Arrives dans le dossier d'import : Import les verifie et les range.
        const std::string inbox = m_env.inbox ? m_env.inbox() : std::string();
        std::vector<std::string> to_import;
        std::error_code ec;
        for (const auto& p : received)
            if (!inbox.empty() && fs::equivalent(fs::path(p).parent_path(), inbox, ec)) to_import.push_back(p);
        if (!to_import.empty()) m_sig_import.emit(to_import);
        // Les jeux recus dans une autre version ne sont plus proposes.
        std::vector<std::string> wrong;
        { std::lock_guard<std::mutex> lk(m_qmutex); wrong.swap(m_wrong_keys); }
        bool changed = false;
        for (const auto& w : wrong) {
            const size_t bar = w.find('|');
            if (w.substr(0, bar) == m_avail_for) { m_avail[w.substr(bar + 1)] = Avail::Outdated; changed = true; }
        }
        if (changed) populate();
        std::set<std::string> emus(library.begin(), library.end());
        for (const auto& e : emus) m_sig_scan.emit(e);
        if (outbox) m_sig_outbox.emit();
    }
}

void RomDownloadTab::refresh_queue_rows() {
    int queued = 0, running = 0, done = 0, failed = 0;
    std::lock_guard<std::mutex> lk(m_qmutex);
    for (auto& it : m_queue) {
        switch (it->state) {
            case State::Queued:    ++queued; break;
            case State::Running:   ++running; break;
            case State::Done:      ++done; break;
            default:               ++failed; break;
        }
        if (!it->bar) continue;
        it->bar->set_fraction(std::clamp(it->progress, 0.0, 1.0));
        it->bar->set_show_text(it->state == State::Running || it->state == State::Done);
        it->bar->set_text(std::to_string((int)(it->progress * 100)) + "%");
        it->state_label->set_text(it->note);
        it->state_label->set_tooltip_text(it->note);
        auto sc = it->state_label->get_style_context();
        for (const char* c : {"set-ok", "set-err", "set-sub"}) sc->remove_class(c);
        sc->add_class(it->state == State::Done ? "set-ok" : it->state == State::Failed ? "set-err" : "set-sub");
        it->cancel->set_sensitive(it->state == State::Queued || it->state == State::Running);
    }
    m_queue_summary.set_text(m_queue.empty() ? Glib::ustring()
        : Glib::ustring::compose(_("%1 waiting · %2 in progress · %3 done · %4 failed or cancelled"), queued, running, done, failed));
    m_btn_cancel_all->set_sensitive(queued + running > 0);
    m_btn_clear->set_sensitive(done + failed > 0);
}
