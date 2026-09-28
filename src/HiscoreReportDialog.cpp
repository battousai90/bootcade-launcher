// src/HiscoreReportDialog.cpp
#include "HiscoreReportDialog.h"
#include "i18n.h"
#include "SettingsUi.h"

#include <filesystem>
#include <iostream>

namespace {

constexpr int kMaxMessage = 1000;                  // caracteres, comme le serveur
constexpr std::uintmax_t kMaxShot = 3 * 1024 * 1024;

// Une vignette qui tient dans 200 x 150 sans deformer la capture.
Glib::RefPtr<Gdk::Pixbuf> thumbnail(const std::string& path) {
    try {
        return Gdk::Pixbuf::create_from_file(path, 200, 150, true);
    } catch (const Glib::Error&) {
        return {};
    }
}

} // namespace

// ═══ ReportDialog ═══════════════════════════════════════════════════════════

ReportDialog::ReportDialog(Gtk::Window& parent, const HiscoreClient::ReportForm& target,
                           const std::string& title, const std::vector<std::string>& screenshots)
    : Gtk::Dialog(_("Report a problem"), parent, true), m_form(target) {
    set_default_size(480, -1);
    set_border_width(20);

    m_intro.set_markup("<b>" + Glib::Markup::escape_text(title) + "</b>\n"
                       + Glib::Markup::escape_text(
                           _("Tell us what you saw : the score on screen, a few words, a "
                             "screenshot. We read every report and answer you in the launcher.")));
    m_intro.set_line_wrap(true);
    m_intro.set_xalign(0.0f);

    auto* score_label = Gtk::make_managed<Gtk::Label>(_("Score shown on screen (optional)"));
    score_label->set_xalign(0.0f);
    m_score.set_input_purpose(Gtk::INPUT_PURPOSE_DIGITS);
    // Chiffres seuls : le serveur attend un entier, et « 18 500 » ou
    // « 18,500 » tels que le jeu les affiche doivent passer quand meme.
    m_score.signal_changed().connect([this] {
        const Glib::ustring text = m_score.get_text();
        Glib::ustring digits;
        for (auto c : text) if (c >= '0' && c <= '9') digits += c;
        if (digits != text) { m_score.set_text(digits); return; }
        update_send_state();
    });

    auto* msg_label = Gtk::make_managed<Gtk::Label>(_("What happened? (optional)"));
    msg_label->set_xalign(0.0f);
    m_message.set_wrap_mode(Gtk::WRAP_WORD_CHAR);
    m_message.set_left_margin(8);
    m_message.set_right_margin(8);
    m_message.set_top_margin(6);
    m_message.set_bottom_margin(6);
    auto* msg_scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    msg_scroll->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    msg_scroll->set_shadow_type(Gtk::SHADOW_IN);
    msg_scroll->set_size_request(-1, 110);
    msg_scroll->add(m_message);
    m_count.set_xalign(1.0f);
    m_count.get_style_context()->add_class("dim-label");
    m_message.get_buffer()->signal_changed().connect([this] {
        auto buffer = m_message.get_buffer();
        Glib::ustring text = buffer->get_text();
        if ((int)text.size() > kMaxMessage) {
            buffer->set_text(text.substr(0, kMaxMessage));
            return;
        }
        m_count.set_text(Glib::ustring::compose("%1 / %2", (int)text.size(), kMaxMessage));
        update_send_state();
    });
    m_count.set_text(Glib::ustring::compose("0 / %1", kMaxMessage));

    // La derniere capture de la partie, jointe d'office : c'est souvent la
    // preuve du score. Le joueur peut la retirer ou en choisir une autre.
    if (!screenshots.empty()) m_screenshot = screenshots.back();
    m_attach.set_label(_("Attach this screenshot"));
    m_attach.set_active(!m_screenshot.empty());
    m_attach.signal_toggled().connect([this] { m_thumb.set_sensitive(m_attach.get_active()); });
    if (!m_screenshot.empty()) m_thumb.set(thumbnail(m_screenshot));

    auto filter = Gtk::FileFilter::create();
    filter->set_name(_("Images (PNG, JPEG, WebP)"));
    for (const char* p : {"*.png", "*.PNG", "*.jpg", "*.JPG", "*.jpeg", "*.JPEG", "*.webp", "*.WEBP"})
        filter->add_pattern(p);
    m_pick.add_filter(filter);
    m_pick.set_title(_("Choose a screenshot"));
    m_pick.signal_file_set().connect([this] {
        const std::string path = m_pick.get_filename();
        if (path.empty()) return;
        m_screenshot = path;
        m_thumb.set(thumbnail(path));
        m_attach.set_active(true);
        m_attach.show();
        m_thumb.show();
    });
    auto* shot_label = Gtk::make_managed<Gtk::Label>(
        m_screenshot.empty() ? _("Screenshot (optional)") : _("Or choose another image"));
    shot_label->set_xalign(0.0f);

    m_status.set_xalign(0.0f);
    m_status.set_line_wrap(true);
    auto* status_row = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    status_row->pack_start(m_spinner, Gtk::PACK_SHRINK);
    status_row->pack_start(m_status, Gtk::PACK_EXPAND_WIDGET);

    m_box.pack_start(m_intro, Gtk::PACK_SHRINK);
    m_box.pack_start(*score_label, Gtk::PACK_SHRINK);
    m_box.pack_start(m_score, Gtk::PACK_SHRINK);
    m_box.pack_start(*msg_label, Gtk::PACK_SHRINK);
    m_box.pack_start(*msg_scroll, Gtk::PACK_SHRINK);
    m_box.pack_start(m_count, Gtk::PACK_SHRINK);
    m_box.pack_start(m_attach, Gtk::PACK_SHRINK);
    m_thumb.set_halign(Gtk::ALIGN_START);
    m_box.pack_start(m_thumb, Gtk::PACK_SHRINK);
    m_box.pack_start(*shot_label, Gtk::PACK_SHRINK);
    m_box.pack_start(m_pick, Gtk::PACK_SHRINK);
    m_box.pack_start(*status_row, Gtk::PACK_SHRINK);
    get_content_area()->pack_start(m_box);

    add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    m_send = add_button(_("Send"), Gtk::RESPONSE_NONE);
    m_send->get_style_context()->add_class("suggested-action");
    m_send->signal_clicked().connect(sigc::mem_fun(*this, &ReportDialog::on_send));

    m_done.connect(sigc::mem_fun(*this, &ReportDialog::on_sent));
    show_all_children();
    m_spinner.hide();
    if (m_screenshot.empty()) { m_attach.hide(); m_thumb.hide(); }
    update_send_state();
}

ReportDialog::~ReportDialog() {
    if (m_worker.joinable()) m_worker.join();
    m_close_timer.disconnect();
}

void ReportDialog::update_send_state() {
    // Le serveur exige au moins le score ou un message : le bouton le dit
    // avant qu'une requete ne revienne le dire.
    const bool something = !m_score.get_text().empty()
                        || !m_message.get_buffer()->get_text().empty();
    if (m_send) m_send->set_sensitive(something && !m_worker.joinable());
}

void ReportDialog::show_status(const Glib::ustring& text, const char* tone) {
    if (!tone) { m_status.set_text(text); return; }
    m_status.set_markup("<span foreground='" + SettingsUi::tone_hex(*this, tone) + "'>"
                        + Glib::Markup::escape_text(text) + "</span>");
}

void ReportDialog::on_send() {
    if (m_worker.joinable()) return;
    HiscoreClient::ReportForm form = m_form;
    const std::string score = m_score.get_text();
    if (!score.empty()) {
        try { form.score = std::stoll(score); form.has_score = true; }
        catch (const std::exception&) {}
    }
    form.message = m_message.get_buffer()->get_text();
    if (m_attach.get_active() && !m_screenshot.empty()) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(m_screenshot, ec);
        if (ec) { show_status(_("This image can no longer be read : choose another one."), "error"); return; }
        if (size > kMaxShot) { show_status(_("This image is larger than 3 MB : choose a smaller one."), "error"); return; }
        form.screenshot_path = m_screenshot;
    }

    m_send->set_sensitive(false);
    m_spinner.show();
    m_spinner.start();
    show_status(_("Sending…"), nullptr);
    m_worker = std::thread([this, form] {
        m_reply = HiscoreClient::send_report(form);
        m_done.emit();
    });
}

void ReportDialog::on_sent() {
    if (m_worker.joinable()) m_worker.join();
    m_spinner.stop();
    m_spinner.hide();
    const auto& r = m_reply;
    std::cerr << "[HISCORE] signalement : "
              << (r.answered ? "HTTP " + std::to_string(r.http_status) : "sans reponse")
              << (r.code.empty() ? "" : ", " + r.code) << std::endl;

    if (r.ok) {
        show_status(_("Thanks! We will look into it and answer you here."), "success");
        m_close_timer = Glib::signal_timeout().connect_seconds(
            [this] { response(Gtk::RESPONSE_OK); return false; }, 3);
        return;
    }
    Glib::ustring why;
    if (!r.answered)                          why = _("The server cannot be reached : try again later.");
    else if (r.http_status == 401)            why = _("Sign in again to send a report.");
    else if (r.code == "too_many_reports")    why = _("You already have reports waiting : we will answer those first.");
    else if (r.code == "empty_report")        why = _("Give the score you saw or a few words.");
    else if (r.code == "bad_screenshot")      why = _("This image cannot be used : choose a PNG, JPEG or WebP under 3 MB.");
    else if (r.code == "unknown_submission")  why = _("This game can no longer be reported : games are kept for 14 days.");
    else if (r.code == "missing_game")        why = _("Incomplete report : update the launcher.");
    else if (!r.detail.empty())               why = r.detail;
    else why = Glib::ustring::compose(_("The report could not be sent (%1)."), r.error);
    show_status(why, "error");
    update_send_state();
}

// ═══ MyReportsDialog ════════════════════════════════════════════════════════

MyReportsDialog::MyReportsDialog(Gtk::Window& parent, TitleOf title_of, Format format)
    : Gtk::Dialog(_("My reports"), parent, true),
      m_title_of(std::move(title_of)), m_format(std::move(format)) {
    set_default_size(540, 480);
    set_border_width(20);

    m_scroll.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    m_scroll.add(m_list);
    m_status.set_xalign(0.0f);
    m_status.set_line_wrap(true);
    auto* status_row = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    status_row->pack_start(m_spinner, Gtk::PACK_SHRINK);
    status_row->pack_start(m_status, Gtk::PACK_EXPAND_WIDGET);

    m_box.pack_start(*status_row, Gtk::PACK_SHRINK);
    m_box.pack_start(m_scroll, Gtk::PACK_EXPAND_WIDGET);
    get_content_area()->pack_start(m_box);
    add_button(_("Close"), Gtk::RESPONSE_CLOSE);

    m_done.connect(sigc::mem_fun(*this, &MyReportsDialog::on_loaded));
    show_all_children();
    m_status.set_text(_("Loading…"));
    m_spinner.start();
    m_worker = std::thread([this] {
        m_result = HiscoreClient::fetch_my_reports();
        m_done.emit();
    });
}

MyReportsDialog::~MyReportsDialog() {
    if (m_worker.joinable()) m_worker.join();
}

void MyReportsDialog::on_loaded() {
    if (m_worker.joinable()) m_worker.join();
    m_spinner.stop();
    m_spinner.hide();
    if (!m_result.answered) { m_status.set_text(_("The server cannot be reached : try again later.")); return; }
    if (m_result.http_status == 401) { m_status.set_text(_("Sign in again to see your reports.")); return; }
    if (!m_result.ok) {
        m_status.set_text(Glib::ustring::compose(_("Your reports could not be loaded (%1)."), m_result.error));
        return;
    }
    if (m_result.value.empty()) { m_status.set_text(_("You have not reported anything yet.")); return; }
    m_status.hide();
    fill(m_result.value);

    // Tout ce qui est affiche est lu : le bandeau du demarrage n'en reparlera pas.
    std::string latest;
    for (const auto& r : m_result.value) latest = std::max(latest, r.replied_at);
    HiscoreClient::set_reports_seen_at(latest);
}

void MyReportsDialog::fill(const std::vector<HiscoreClient::PlayerReport>& reports) {
    for (const auto& r : reports) {
        auto* frame = Gtk::make_managed<Gtk::Frame>();
        auto* card = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 4);
        card->set_border_width(10);
        frame->add(*card);

        // Ce qu'il est advenu, en mots de joueur, avec la couleur de l'etat.
        const char* tone = "muted";
        Glib::ustring state;
        if (r.status == "published")     { state = _("Score published");       tone = "success"; }
        else if (r.status == "answered") { state = _("Answered");              tone = "info"; }
        else if (r.status == "closed")   { state = _("Closed");                tone = "muted"; }
        else                             { state = _("Waiting for our answer"); tone = "warning"; }

        const std::string title = m_title_of(r.system, r.game);
        const std::string day = r.created_at.substr(0, 10);
        auto* head = Gtk::make_managed<Gtk::Label>();
        head->set_xalign(0.0f);
        head->set_line_wrap(true);
        head->set_markup("<b>" + Glib::Markup::escape_text(title) + "</b>  ·  "
                         + Glib::Markup::escape_text(day) + "  ·  <span foreground='"
                         + SettingsUi::tone_hex(*this, tone) + "'>"
                         + Glib::Markup::escape_text(state) + "</span>");
        card->pack_start(*head, Gtk::PACK_SHRINK);

        std::vector<Glib::ustring> facts;
        if (r.has_claimed)
            facts.push_back(Glib::ustring::compose(_("Score you reported : %1"),
                                                   m_format(r.system, r.game, r.score_claimed)));
        if (r.has_published && r.status == "published")
            facts.push_back(Glib::ustring::compose(_("Published on the leaderboard : %1"),
                                                   m_format(r.system, r.game, r.published)));
        for (const auto& f : facts) {
            auto* l = Gtk::make_managed<Gtk::Label>(f);
            l->set_xalign(0.0f);
            l->get_style_context()->add_class("dim-label");
            card->pack_start(*l, Gtk::PACK_SHRINK);
        }
        if (!r.reply.empty()) {
            auto* reply = Gtk::make_managed<Gtk::Label>();
            reply->set_xalign(0.0f);
            reply->set_line_wrap(true);
            reply->set_selectable(true);
            reply->set_markup("<i>" + Glib::Markup::escape_text(_("Our answer :")) + "</i> "
                              + Glib::Markup::escape_text(r.reply));
            card->pack_start(*reply, Gtk::PACK_SHRINK);
        }
        m_list.pack_start(*frame, Gtk::PACK_SHRINK);
    }
    m_list.show_all();
}
