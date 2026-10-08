// src/SplashScreen.cpp
#include "SplashScreen.h"
#include "i18n.h"
#include "AppContext.h"
#include <iostream>

SplashScreen::SplashScreen() {
    setup_ui();
}

SplashScreen::~SplashScreen() = default;

void SplashScreen::setup_ui() {
    set_title("Bootcade");
    set_default_size(440, 300);
    set_position(Gtk::WIN_POS_CENTER);
    set_resizable(false);
    set_decorated(false);
    set_modal(true);
    set_type_hint(Gdk::WINDOW_TYPE_HINT_SPLASHSCREEN);

    // The panel : not the window : carries the background: a GtkWindow skips its
    // own CSS background whenever it is app-paintable or has an RGBA visual,
    // which is what made the splash look see-through.
    get_style_context()->add_class("splash-window");
    m_root.get_style_context()->add_class("splash-screen");

    m_main_box.set_halign(Gtk::ALIGN_CENTER);
    m_main_box.set_valign(Gtk::ALIGN_CENTER);

    // Logo (vector; falls back to a system icon if missing).
    try {
        auto pixbuf = Gdk::Pixbuf::create_from_file(AppContext::get_asset_path("logo.svg"), 96, 96);
        m_logo.set(pixbuf);
    } catch (...) {
        m_logo.set_from_icon_name("applications-games", Gtk::ICON_SIZE_DIALOG);
    }
    m_logo.set_halign(Gtk::ALIGN_CENTER);
    m_logo.set_margin_bottom(4);

    m_title_label.set_markup("<span size='xx-large' weight='bold'>Bootcade</span>");
    m_title_label.set_halign(Gtk::ALIGN_CENTER);
    m_title_label.get_style_context()->add_class("title");

    m_subtitle_label.set_text(_("Arcade library"));
    m_subtitle_label.set_halign(Gtk::ALIGN_CENTER);
    m_subtitle_label.get_style_context()->add_class("subtitle");
    m_subtitle_label.set_margin_bottom(6);

    m_content_box.set_halign(Gtk::ALIGN_CENTER);

    m_status_label.set_text(_("Initializing…"));
    m_status_label.set_halign(Gtk::ALIGN_CENTER);
    m_status_label.get_style_context()->add_class("status");

    m_progress_bar.set_size_request(320, 6);
    m_progress_bar.set_halign(Gtk::ALIGN_CENTER);
    m_progress_bar.set_fraction(0.0);
    m_progress_bar.set_show_text(false);
    m_progress_bar.get_style_context()->add_class("splash-progress");

    m_content_box.pack_start(m_progress_bar, Gtk::PACK_SHRINK);
    m_content_box.pack_start(m_status_label, Gtk::PACK_SHRINK);

    m_main_box.pack_start(m_logo, Gtk::PACK_SHRINK);
    m_main_box.pack_start(m_title_label, Gtk::PACK_SHRINK);
    m_main_box.pack_start(m_subtitle_label, Gtk::PACK_SHRINK);
    m_main_box.pack_start(m_content_box, Gtk::PACK_SHRINK);

    m_root.pack_start(m_main_box, Gtk::PACK_EXPAND_WIDGET);
    add(m_root);

    // Dark "arcade graphite" splash, matching the app theme.
    auto css = Gtk::CssProvider::create();
    css->load_from_data(R"(
        .splash-window { background-color: transparent; }
        .splash-screen {
            padding: 28px;
            background-color: #101219;
            background-image: radial-gradient(120% 100% at 50% 0%, #1c1f2e 0%, #101219 62%);
            border-radius: 16px;
            border: 1px solid #2a2f3d;
        }
        .splash-screen .title    { color: #eef0f6; }
        .splash-screen .subtitle { color: #7c6bff; font-size: 12px; letter-spacing: 2px; }
        .splash-screen .status   { color: #939aab; font-size: 12px; }
        .splash-screen progressbar.splash-progress trough {
            min-height: 6px; border-radius: 999px; background-color: #0c0e15; border: none;
        }
        .splash-screen progressbar.splash-progress progress {
            min-height: 6px; border-radius: 999px;
            background-image: linear-gradient(to right, #7c6bff, #9a8cff);
        }
    )");

    auto screen = Gdk::Screen::get_default();
    Gtk::StyleContext::add_provider_for_screen(screen, css, GTK_STYLE_PROVIDER_PRIORITY_USER + 10);

    // Rounded corners need an RGBA visual (compositing) to avoid black corners.
    if (auto vis = screen->get_rgba_visual()) gtk_widget_set_visual(GTK_WIDGET(gobj()), vis->gobj());

    show_all();
}

void SplashScreen::set_progress(double progress, const std::string& message) {
    // Assure-toi que progress est entre 0.0 et 1.0
    progress = std::max(0.0, std::min(1.0, progress));
    
    m_progress_bar.set_fraction(progress);
    m_progress_bar.set_text(std::to_string(static_cast<int>(progress * 100)) + "%");
    m_status_label.set_text(message);
    
    // Force la mise à jour de l'affichage
    while (Gtk::Main::events_pending()) {
        Gtk::Main::iteration();
    }
    
    std::cout << "[SPLASH] Progress: " << static_cast<int>(progress * 100) << "% - " << message << std::endl;
}

void SplashScreen::show_splash() {
    show();
    present();
    
    // Force l'affichage immédiat
    while (Gtk::Main::events_pending()) {
        Gtk::Main::iteration();
    }
    
    std::cout << "[SPLASH] Splash screen shown" << std::endl;
}

void SplashScreen::hide_splash() {
    hide();
    std::cout << "[SPLASH] Splash screen hidden" << std::endl;
}