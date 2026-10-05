// src/GameListSetup.cpp
#include "GameListSetup.h"
#include "AppContext.h"
#include "DatSource.h"
#include "GenerateDAT.h"
#include "SettingsUi.h"
#include "i18n.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>

namespace fs = std::filesystem;

namespace GameListSetup {

std::string default_fbneo_folder() {
    const char* home = std::getenv("HOME");
    return (fs::path(home ? home : ".") / "Bootcade" / "DAT" / "FBNeo").string();
}

std::string adopt_fbneo_folder() {
    std::vector<DatSource::Group> groups = DatSource::load_groups();
    DatSource::Group* fb = nullptr;
    for (auto& g : groups)
        if (g.emulator == "fbneo" && (!fb || (g.active && !fb->active))) fb = &g;

    nlohmann::json j;
    const std::string path = AppContext::get_config_path();
    { std::ifstream fi(path); if (fi) { try { fi >> j; } catch (...) { j = nlohmann::json{}; } } }
    const std::string legacy = j.value("dat_path", std::string());

    std::string folder = fb && !fb->folder.empty() ? fb->folder
                       : !legacy.empty()           ? legacy
                                                   : default_fbneo_folder();
    std::error_code ec;
    fs::create_directories(folder, ec);

    if (!fb) {
        DatSource::Group g;
        g.id   = DatSource::make_id("FinalBurn Neo", groups);
        g.name = "FinalBurn Neo";
        groups.push_back(std::move(g));
        fb = &groups.back();
    }
    if (fb->folder != folder) {
        fb->folder = folder;
        DatSource::save_groups(groups);
    }
    if (legacy != folder) {
        // Relu apres save_groups : il vient de reecrire le fichier.
        nlohmann::json k;
        { std::ifstream fi(path); if (fi) { try { fi >> k; } catch (...) { k = nlohmann::json{}; } } }
        k["dat_path"] = folder;
        std::ofstream fo(path);
        if (fo) fo << k.dump(4);
    }
    return folder;
}

bool has_dats(const std::string& folder) {
    return !folder.empty() && !DatSource::list_folder(folder).empty();
}

// Le serveur Bootcade : le manifeste, puis chaque fichier, verifie par son
// SHA256 avant de prendre sa place (DatSource::download).
static bool fetch_from_server(Gtk::Window& parent, const std::string& folder, bool& cancelled_out) {
    Gtk::Dialog dialog;
    dialog.set_transient_for(parent);
    dialog.set_modal(true);
    dialog.set_resizable(false);
    dialog.set_default_size(520, -1);
    dialog.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);
    SettingsUi::window_header(dialog, "bc-download.svg", _("Getting the game list"),
                              _("Downloading the FinalBurn Neo game list from the Bootcade server..."));

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 14);
    box->set_margin_start(22);
    box->set_margin_end(22);
    box->set_margin_top(20);
    box->set_margin_bottom(20);
    auto* bar = Gtk::make_managed<Gtk::ProgressBar>();
    bar->set_show_text(true);
    bar->set_text(_("Contacting the server..."));
    box->pack_start(*bar, Gtk::PACK_SHRINK);
    auto* cancel = SettingsUi::button(_("Cancel"), "bc-close.svg");
    cancel->set_halign(Gtk::ALIGN_END);
    box->pack_start(*cancel, Gtk::PACK_SHRINK);
    dialog.get_content_area()->set_spacing(0);
    dialog.get_content_area()->pack_start(*box, Gtk::PACK_EXPAND_WIDGET);
    dialog.show_all();

    std::atomic<bool> cancelled{false}, finished{false}, ok{false};
    std::atomic<double> fraction{0.0};
    std::mutex text_mutex;
    std::string text;

    Glib::Dispatcher on_tick, on_finished;
    on_tick.connect([&] {
        if (cancelled.load()) return;
        bar->set_fraction(fraction.load());
        std::lock_guard<std::mutex> lk(text_mutex);
        bar->set_text(text);
    });
    on_finished.connect([&dialog] { dialog.response(Gtk::RESPONSE_OK); });
    cancel->signal_clicked().connect([&] {
        cancelled = true;
        cancel->set_sensitive(false);
        bar->set_text(_("Cancelling..."));
    });

    std::thread worker([&] {
        DatSource::Manifest m;
        std::string error;
        bool good = DatSource::fetch_manifest(DatSource::kDefaultManifestUrl, m, error) && !m.files.empty();
        if (!good) std::cerr << "[DAT] server: " << error << std::endl;
        uint64_t total = 0, done = 0;
        for (const auto& f : m.files) total += f.size;
        for (size_t i = 0; good && i < m.files.size() && !cancelled.load(); ++i) {
            const auto& f = m.files[i];
            {
                std::lock_guard<std::mutex> lk(text_mutex);
                text = Glib::ustring::compose(_("%1 of %2 files"), i + 1, m.files.size());
            }
            on_tick.emit();
            good = DatSource::download(DatSource::kDefaultManifestUrl, f, folder, error,
                [&](double pct, const std::string&) {
                    if (!total) return;
                    // curl rappelle des centaines de fois par seconde : un
                    // pas visible suffit a la barre.
                    const double now = (done + f.size * pct / 100.0) / double(total);
                    if (now - fraction.load() < 0.005) return;
                    fraction = now;
                    on_tick.emit();
                },
                [&] { return cancelled.load(); });
            if (!good) std::cerr << "[DAT] " << f.name << ": " << error << std::endl;
            done += f.size;
        }
        ok = good && !cancelled.load();
        finished = true;
        on_finished.emit();
    });
    while (!finished.load()) dialog.run();
    worker.join();
    dialog.hide();
    cancelled_out = cancelled.load();
    return ok.load();
}

bool fill(Gtk::Window& parent, const std::string& folder, const std::string& fbneo_executable) {
    bool cancelled = false;
    if (fetch_from_server(parent, folder, cancelled) && has_dats(folder)) {
        auto groups = DatSource::load_groups();
        for (auto& g : groups)
            if (g.folder == folder) g.last_check = g.last_update = DatSource::now_iso();
        DatSource::save_groups(groups);
        return true;
    }
    if (cancelled) return false;
    // Pas de reseau, ou serveur muet : le FinalBurn Neo installe sait ecrire
    // la meme liste. GenerateDAT dit lui-meme pourquoi il echoue.
    if (fbneo_executable.empty()) {
        SettingsUi::notice(parent, _("The game list could not be downloaded"),
                           _("The Bootcade server did not answer. Check the network connection, "
                             "or install FinalBurn Neo: it can write the list itself."),
                           "bc-error.svg");
        return false;
    }
    if (!GenerateDAT::execute(parent, fbneo_executable, folder, nullptr, /*quiet=*/true))
        return false;   // GenerateDAT a deja dit pourquoi
    if (has_dats(folder)) return true;
    SettingsUi::notice(parent, _("The game list could not be written"),
                       _("FinalBurn Neo ran but wrote no DAT file in:") + std::string("\n") + folder,
                       "bc-error.svg");
    return false;
}

}  // namespace GameListSetup
