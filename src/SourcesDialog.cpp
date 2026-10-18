// src/SourcesDialog.cpp
#include "SourcesDialog.h"

#include "BootcadeAuth.h"
#include "LoginDialog.h"
#include "RetroRoms.h"
#include "SettingsUi.h"
#include "i18n.h"

#include <algorithm>
#include <atomic>
#include <thread>

namespace ui = SettingsUi;

namespace {

// Un travail hors du fil GTK, dont le resultat revient par un minuteur : la
// fenetre peut se fermer avant la fin sans rien bloquer.
template <typename T>
void run_async(std::shared_ptr<bool> alive, std::function<T()> work, std::function<void(T)> done) {
    struct State { std::atomic<bool> ready{false}; T value{}; };
    auto st = std::make_shared<State>();
    std::thread([st, work] { st->value = work(); st->ready = true; }).detach();
    Glib::signal_timeout().connect([st, alive, done] {
        if (!*alive) return false;
        if (!st->ready) return true;
        done(st->value);
        return false;
    }, 150);
}

} // namespace

SourcesDialog::SourcesDialog(Gtk::Window& parent)
    : Gtk::Dialog(_("Download sources"), parent, true), m_parent(parent) {
    get_style_context()->add_class("set-window");
    set_default_size(760, -1);
    auto* area = get_content_area();
    area->set_spacing(12);
    area->set_border_width(16);

    auto card = ui::card("bc-download.svg", _("Download sources"),
                         _("Where the Downloads tab takes the missing sets from. Disabled sources are not offered."));
    card.body->pack_start(m_list, Gtk::PACK_SHRINK);
    auto* add = ui::button(_("Add source…"), "bc-plus.svg");
    add->set_halign(Gtk::ALIGN_START);
    add->set_margin_top(10);
    add->signal_clicked().connect([this] { edit_source(""); });
    card.body->pack_start(*add, Gtk::PACK_SHRINK);
    area->pack_start(*card.frame, Gtk::PACK_EXPAND_WIDGET);

    add_button(_("Close"), Gtk::RESPONSE_CLOSE);
    rebuild();
    show_all_children();
    refresh_retroroms_status();
}

SourcesDialog::~SourcesDialog() { *m_alive = false; }

void SourcesDialog::rebuild() {
    ui::destroy_children(m_list);
    auto* rows = ui::rows();
    for (const auto& s : DownloadSources::load()) ui::add_row(rows, *source_row(s));
    m_list.pack_start(*rows, Gtk::PACK_SHRINK);
    m_list.show_all();
}

Gtk::Widget* SourcesDialog::source_row(const DownloadSources::Source& s) {
    std::string sub;
    auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    const std::string id = s.id;

    if (s.kind == "bootcade") {
        const bool in = BootcadeAuth::signed_in();
        sub = in ? Glib::ustring::compose(_("Signed in as %1 · FinalBurn Neo · daily quota of your account"),
                                          BootcadeAuth::username()).raw()
                 : std::string(_("Not signed in · FinalBurn Neo · needs a Bootcade account"));
        auto* b = ui::button(in ? _("Change account…") : _("Sign in…"), "bc-account.svg");
        b->signal_clicked().connect([this] {
            if (BootcadeAuth::signed_in()) BootcadeAuth::sign_out();
            LoginDialog dlg(*this);
            dlg.run();
            dlg.hide();
            present();
            m_sig_account.emit();
            rebuild();
        });
        actions->pack_start(*b, Gtk::PACK_SHRINK);
        if (in) {
            auto* o = ui::button(_("Sign out"), "bc-signout.svg");
            o->signal_clicked().connect([this] { BootcadeAuth::sign_out(); m_sig_account.emit(); rebuild(); });
            actions->pack_start(*o, Gtk::PACK_SHRINK);
        }
    } else if (s.kind == "lolroms") {
        sub = _("lolroms.com · FinalBurn Neo and MAME, CHDs included · no account");
    } else if (s.kind == "retroroms") {
        sub = "bda.retroroms.info · " + std::string(_("FinalBurn Neo and MAME, CHDs included · 100 sets per 24 h")) + "\n"
            + (m_retro_busy ? std::string(_("Checking the account…")) : m_retro_text);
        auto* b = ui::button(_("Sign in…"), "bc-account.svg");
        b->set_sensitive(!m_retro_busy);
        b->signal_clicked().connect([this] { retroroms_sign_in(); });
        actions->pack_start(*b, Gtk::PACK_SHRINK);
        auto* o = ui::button(_("Sign out"), "bc-signout.svg");
        o->set_sensitive(!m_retro_busy);
        o->signal_clicked().connect([this] { RetroRoms::sign_out(); refresh_retroroms_status(); });
        actions->pack_start(*o, Gtk::PACK_SHRINK);
    } else {
        sub = s.url + (s.user.empty() ? std::string() : "  ·  " + s.user);
        auto* e = ui::button(_("Edit…"));
        e->signal_clicked().connect([this, id] { edit_source(id); });
        actions->pack_start(*e, Gtk::PACK_SHRINK);
        auto* r = ui::button(_("Remove"), "bc-close.svg", ui::Tone::Danger);
        r->signal_clicked().connect([this, id] {
            auto all = DownloadSources::load();
            all.erase(std::remove_if(all.begin(), all.end(), [&](const auto& x) { return x.id == id; }), all.end());
            DownloadSources::save(all);
            DownloadSources::set_password(id, "");
            Glib::signal_idle().connect_once([this] { rebuild(); });
        });
        actions->pack_start(*r, Gtk::PACK_SHRINK);
    }

    auto* sw = Gtk::make_managed<Gtk::Switch>();
    sw->set_active(s.enabled);
    sw->set_valign(Gtk::ALIGN_CENTER);
    sw->set_tooltip_text(_("Offer this source in the Downloads tab"));
    sw->property_active().signal_changed().connect([sw, id] {
        auto all = DownloadSources::load();
        for (auto& x : all) if (x.id == id) x.enabled = sw->get_active();
        DownloadSources::save(all);
    });
    actions->pack_start(*sw, Gtk::PACK_SHRINK);
    actions->set_valign(Gtk::ALIGN_CENTER);
    return ui::row("bc-globe.svg", s.name, sub, actions);
}

void SourcesDialog::refresh_retroroms_status() {
    m_retro_busy = true;
    rebuild();
    run_async<RetroRoms::Status>(m_alive, [] { return RetroRoms::status(); }, [this](RetroRoms::Status st) {
        m_retro_busy = false;
        if (!st.answered)       m_retro_text = _("RetroRoms does not answer.");
        else if (!st.signed_in) m_retro_text = _("Not signed in : downloads need a retroroms account.");
        else {
            m_retro_text = Glib::ustring::compose(_("Signed in as %1"), st.user).raw();
            if (st.limit > 0)
                m_retro_text += "  ·  " + Glib::ustring::compose(_("%1 of %2 used in the last 24 h"), st.used, st.limit).raw();
        }
        rebuild();
    });
}

void SourcesDialog::retroroms_sign_in() {
    Gtk::Dialog dlg(_("Sign in to RetroRoms"), *this, true);
    dlg.get_style_context()->add_class("set-window");
    auto* area = dlg.get_content_area();
    area->set_spacing(10);
    area->set_border_width(16);
    auto* intro = ui::sub_label(_("Your retroroms forum account. Bootcade signs in once and keeps only the session, "
                                  "never the password."));
    intro->set_line_wrap(true);
    intro->set_max_width_chars(50);
    area->pack_start(*intro, Gtk::PACK_SHRINK);
    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(8);
    grid->set_column_spacing(12);
    Gtk::Entry user, pass;
    pass.set_visibility(false);
    pass.set_activates_default(true);
    user.set_size_request(ui::kFieldWidth, -1);
    grid->attach(*ui::title_label(_("E-mail or user name")), 0, 0);
    grid->attach(user, 1, 0);
    grid->attach(*ui::title_label(_("Password")), 0, 1);
    grid->attach(pass, 1, 1);
    area->pack_start(*grid, Gtk::PACK_SHRINK);
    Gtk::Label msg;
    msg.set_line_wrap(true);
    msg.set_xalign(0.0f);
    msg.get_style_context()->add_class("set-warn");
    area->pack_start(msg, Gtk::PACK_SHRINK);
    auto* cancel = dlg.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    auto* ok = dlg.add_button(_("Sign in"), Gtk::RESPONSE_OK);
    ok->get_style_context()->add_class("suggested-action");
    dlg.set_default_response(Gtk::RESPONSE_OK);
    dlg.show_all();
    msg.hide();

    while (dlg.run() == Gtk::RESPONSE_OK) {
        const std::string id = user.get_text(), pw = pass.get_text();
        if (id.empty() || pw.empty()) continue;
        msg.set_text(_("Signing in…"));
        msg.show();
        // Pas toute la fenetre : rendue insensible pendant l'attente, GTK ne
        // lui rendait plus les clics, et l'application paraissait figee.
        for (Gtk::Widget* w : std::initializer_list<Gtk::Widget*>{ok, cancel, &user, &pass}) w->set_sensitive(false);
        // La connexion tourne a part ; la boite attend sans figer l'ecran.
        struct R { std::atomic<bool> done{false}; bool ok = false; std::string why; };
        auto r = std::make_shared<R>();
        std::thread([r, id, pw] { r->ok = RetroRoms::sign_in(id, pw, r->why); r->done = true; }).detach();
        while (!r->done) Gtk::Main::iteration(false), Glib::usleep(20000);
        for (Gtk::Widget* w : std::initializer_list<Gtk::Widget*>{ok, cancel, &user, &pass}) w->set_sensitive(true);
        if (r->ok) break;
        msg.set_text(r->why);
    }
    dlg.hide();
    present();   // la fenetre des sources revient devant, sinon elle reste cachee et bloque tout
    refresh_retroroms_status();
}

void SourcesDialog::edit_source(const std::string& id) {
    DownloadSources::Source s;
    if (!id.empty()) s = DownloadSources::find(id);
    if (s.id.empty()) s.kind = "custom";

    Gtk::Dialog dlg(id.empty() ? _("Add a download source") : _("Edit download source"), *this, true);
    dlg.get_style_context()->add_class("set-window");
    auto* area = dlg.get_content_area();
    area->set_spacing(10);
    area->set_border_width(16);
    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(8);
    grid->set_column_spacing(12);
    Gtk::Entry name, url, user, pass;
    Gtk::ComboBoxText emus;
    Gtk::SpinButton quota;
    name.set_text(s.name);
    url.set_text(s.url);
    url.set_placeholder_text("https://example.org/roms/{system}/{name}.zip");
    url.set_size_request(420, -1);
    user.set_text(s.user);
    pass.set_visibility(false);
    if (!s.id.empty()) pass.set_text(DownloadSources::password(s.id));
    emus.append("fbneo", "FinalBurn Neo");
    emus.append("mame", "MAME");
    emus.append("fbneo,mame", "FinalBurn Neo + MAME");
    if (!emus.set_active_id(s.emulators)) emus.set_active(0);
    quota.set_range(0, 100000);
    quota.set_increments(1, 10);
    quota.set_value(s.daily_quota);
    int r = 0;
    auto line = [&](const char* label, Gtk::Widget& w) {
        grid->attach(*ui::title_label(_(label)), 0, r);
        grid->attach(w, 1, r++);
    };
    line(N_("Name"), name);
    line(N_("Address"), url);
    auto* help = ui::sub_label(_("{name} : the set, {system} : its system, {ext} : zip or 7z. Without {name}, "
                                 "/{name}.zip is added to the address."));
    help->set_line_wrap(true);
    grid->attach(*help, 1, r++);
    line(N_("Emulator"), emus);
    line(N_("User"), user);
    line(N_("Password"), pass);
    line(N_("Daily quota (0 : none)"), quota);
    area->pack_start(*grid, Gtk::PACK_SHRINK);
    dlg.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dlg.add_button(_("Save"), Gtk::RESPONSE_OK)->get_style_context()->add_class("suggested-action");
    dlg.show_all();
    const int resp = dlg.run();
    dlg.hide();
    present();
    if (resp != Gtk::RESPONSE_OK) return;
    if (url.get_text().empty()) return;

    auto all = DownloadSources::load();
    if (s.id.empty()) {
        int n = 1;
        auto taken = [&](const std::string& c) { for (auto& x : all) if (x.id == c) return true; return false; };
        while (taken("custom-" + std::to_string(n))) ++n;
        s.id = "custom-" + std::to_string(n);
        s.enabled = true;
    }
    s.name = name.get_text().empty() ? std::string(url.get_text()) : std::string(name.get_text());
    s.url = url.get_text();
    s.user = user.get_text();
    s.emulators = emus.get_active_id();
    s.daily_quota = quota.get_value_as_int();
    bool replaced = false;
    for (auto& x : all) if (x.id == s.id) { x = s; replaced = true; }
    if (!replaced) all.push_back(s);
    DownloadSources::save(all);
    DownloadSources::set_password(s.id, pass.get_text());
    rebuild();
}
