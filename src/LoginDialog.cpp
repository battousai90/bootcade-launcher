// src/LoginDialog.cpp
#include "LoginDialog.h"
#include "i18n.h"
#include "SettingsUi.h"

#include <chrono>
#include <iomanip>
#include <ctime>

LoginDialog::LoginDialog(Gtk::Window& parent)
    : Gtk::Dialog(_("Sign in to Bootcade"), parent, true) {
    namespace ui = SettingsUi;

    /* La presentation des autres fenetres : barre de titre de Bootcade
     * (tuile, titre, sous-titre, croix), contenu en carte, pied d'actions en
     * bas a droite. Elle avait garde la barre du bureau et un bouton Cancel
     * de GTK : une autre application au milieu de Bootcade. */
    set_default_size(500, -1);
    set_resizable(false);
    set_position(Gtk::WIN_POS_CENTER_ON_PARENT);
    ui::window_header(*this, "bc-account.svg", _("Sign in to Bootcade"),
                      _("Link this launcher to your Bootcade account"),
                      [this] { response(Gtk::RESPONSE_CANCEL); });

    m_intro.set_text(_("Open the page below and confirm the code to link this "
                       "launcher to your Bootcade account."));
    m_intro.set_line_wrap(true);
    m_intro.set_max_width_chars(48);
    m_intro.set_xalign(0.0f);
    m_intro.get_style_context()->add_class("set-sub");

    // Le code est l'élément que le joueur doit lire et recopier : il est donc
    // le plus gros de la fenêtre, en chasse fixe pour qu'un 0 ne se confonde
    // pas avec un O, et sélectionnable pour être copié plutôt que ressaisi.
    m_code.set_selectable(true);
    m_code.set_xalign(0.5f);

    m_countdown.set_xalign(0.5f);

    // Le seul geste attendu : le bouton principal, en couleur.
    m_open.set_label(_("Open browser"));
    m_open.set_image(*ui::image("bc-external.svg", ui::kIconButton));
    m_open.set_always_show_image(true);
    m_open.get_style_context()->add_class("accent-button");

    m_status.set_xalign(0.0f);
    m_status.set_line_wrap(true);
    m_status.get_style_context()->add_class("set-sub");

    auto* code_box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 4);
    code_box->get_style_context()->add_class("cc-subcard");
    code_box->pack_start(m_code, Gtk::PACK_SHRINK);
    code_box->pack_start(m_countdown, Gtk::PACK_SHRINK);

    auto* waiting = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    waiting->pack_start(m_spinner, Gtk::PACK_SHRINK);
    waiting->pack_start(m_status, Gtk::PACK_SHRINK);

    m_box.set_margin_top(20);
    m_box.set_margin_bottom(20);
    m_box.set_margin_start(22);
    m_box.set_margin_end(22);
    m_box.pack_start(m_intro,  Gtk::PACK_SHRINK);
    m_box.pack_start(*code_box, Gtk::PACK_SHRINK);
    m_box.pack_start(m_open,   Gtk::PACK_SHRINK);
    m_box.pack_start(*waiting, Gtk::PACK_SHRINK);
    get_content_area()->set_spacing(0);
    get_content_area()->pack_start(m_box);

    // Pied d'actions de la charte : Cancel en bas a droite.
    auto* foot = ui::footer();
    auto* cancel = ui::button(_("Cancel"));
    cancel->signal_clicked().connect([this] { response(Gtk::RESPONSE_CANCEL); });
    foot->pack_end(*cancel, Gtk::PACK_SHRINK);
    get_content_area()->pack_end(*foot, Gtk::PACK_SHRINK);

    m_open.signal_clicked().connect([this] {
        // L'adresse COMPLÈTE, celle qui porte déjà le code : le joueur n'a
        // alors rien à saisir, il n'a qu'à confirmer. C'est tout l'intérêt
        // d'ouvrir le navigateur plutôt que de lui laisser l'adresse courte.
        if (m_dc.verification_uri_complete.empty()) return;
        // Sans navigateur, le code affiché reste la porte de sortie. L'échec
        // était avant ignoré : le bouton semblait ne rien faire.
        if (!SettingsUi::open_uri(m_dc.verification_uri_complete, this))
            m_status.set_text(_("Could not open a browser. Use the code above."));
    });

    m_done.connect(sigc::mem_fun(*this, &LoginDialog::on_poll_done));
    show_all_children();
    m_spinner.hide();
    start();
}

LoginDialog::~LoginDialog() {
    m_stop = true;
    if (m_worker.joinable()) m_worker.join();
    m_close_timer.disconnect();
    m_tick.disconnect();
}

void LoginDialog::start() {
    m_dc = BootcadeAuth::begin();
    if (!m_dc.ok) {
        m_status.set_markup("<span foreground='" + SettingsUi::tone_hex(*this, "error") + "'>"
                            + Glib::Markup::escape_text(
                                Glib::ustring::compose(_("Sign-in could not start: %1"),
                                                       m_dc.error))
                            + "</span>");
        m_open.set_sensitive(false);
        return;
    }

    m_code.set_markup("<span size='xx-large' weight='bold' font_family='monospace'>"
                      + Glib::Markup::escape_text(m_dc.user_code) + "</span>");
    m_status.set_text(_("Waiting for authorisation…"));
    m_spinner.show();
    m_spinner.start();

    // Le temps restant : sans lui, un joueur qui hesite ne sait pas s'il a
    // encore une minute ou dix, et un code qui expire sans prevenir donne
    // l'impression d'une panne.
    m_deadline = std::time(nullptr) + m_dc.expires_in;
    auto refresh_countdown = [this] {
        long left = long(m_deadline - std::time(nullptr));
        if (left < 0) left = 0;
        m_countdown.set_markup(
            "<span size='small' alpha='70%'>"
            + Glib::Markup::escape_text(Glib::ustring::compose(
                  _("Code valid for %1:%2"), long(left / 60),
                  Glib::ustring::format(std::setfill(L'0'), std::setw(2), left % 60)))
            + "</span>");
        return left > 0;
    };
    refresh_countdown();
    m_tick = Glib::signal_timeout().connect_seconds(refresh_countdown, 1);

    // L'interrogation vit sur son propre fil : la faire sur celui de l'UI
    // figerait la fenêtre entre deux requêtes, et le joueur croirait à un
    // plantage.
    m_worker = std::thread([this] {
        int wait = m_dc.interval;
        const std::time_t deadline = std::time(nullptr) + m_dc.expires_in;

        while (!m_stop) {
            for (int i = 0; i < wait * 10 && !m_stop; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (m_stop) return;

            if (std::time(nullptr) >= deadline) {
                m_result = BootcadeAuth::Poll::Expired;
                m_done.emit();
                return;
            }

            auto r = BootcadeAuth::poll(m_dc);
            if (r == BootcadeAuth::Poll::Pending) continue;
            if (r == BootcadeAuth::Poll::SlowDown) {
                // La RFC demande d'allonger l'attente, pas de réessayer au
                // même rythme : sans ça le serveur finit par refuser tout court.
                wait += 5;
                continue;
            }
            m_result = r;
            m_done.emit();
            return;
        }
    });
}

void LoginDialog::on_poll_done() {
    m_spinner.stop();
    m_spinner.hide();
    m_tick.disconnect();

    switch (m_result) {
    case BootcadeAuth::Poll::Granted:
        finish_ok();
        return;
    case BootcadeAuth::Poll::Denied:
        m_status.set_text(_("Sign-in was refused."));
        break;
    case BootcadeAuth::Poll::Expired:
        m_status.set_text(_("The code expired. Close this window and try again."));
        break;
    default:
        m_status.set_text(_("Sign-in failed. Check your connection and try again."));
        break;
    }
    m_open.set_sensitive(false);
}

void LoginDialog::finish_ok() {
    m_ok = true;
    m_code.hide();
    m_countdown.hide();
    m_open.hide();
    m_intro.hide();
    m_status.set_markup("<span size='large' foreground='" + SettingsUi::tone_hex(*this, "success") + "'>✓ "
                        + Glib::Markup::escape_text(
                            Glib::ustring::compose(_("Signed in as %1"),
                                                   BootcadeAuth::username()))
                        + "</span>");

    // Une seconde et demie : assez pour lire la confirmation, assez peu pour
    // ne pas donner l'impression que la fenêtre est restée bloquée.
    // connect_seconds_once ne rend rien : on garde la connexion pour pouvoir
    // annuler la minuterie si la fenetre est detruite avant son echeance.
    m_close_timer = Glib::signal_timeout().connect_seconds(
        [this] { response(Gtk::RESPONSE_OK); return false; }, 2);
}
