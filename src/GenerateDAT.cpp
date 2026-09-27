// src/GenerateDAT.cpp
#include "GenerateDAT.h"
#include "i18n.h"
#include "SettingsUi.h"
#include "IconManager.h"
#include "AppContext.h"
#include "MameCatalog.h"
#include <atomic>
#include <thread>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <iostream>
#include <cstdlib>

// fork/exec, then wait for the child WITHOUT blocking the GTK thread : the
// wait runs the progress dialog's own loop, a child watch ends it. The old
// waitpid() froze the window for the whole `fbneo -dat` run (ten seconds and
// more) : the progress bar never moved and the desktop offered to kill the
// application. Returns the child exit code, or -1 on error.
static int spawn_and_wait(const std::vector<std::string>& args, Gtk::Dialog& dialog, Gtk::ProgressBar& bar) {
    if (args.empty()) return -1;
    // Runs on the host when sandboxed : see AppContext::host_command.
    const std::vector<std::string> cmd = AppContext::host_command(args);
    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "[ERROR] fork() failed for: " << args[0] << std::endl;
        return -1;
    }
    if (pid == 0) {
        std::vector<char*> argv;
        argv.reserve(cmd.size() + 1);
        for (const auto& a : cmd)
            argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        std::cerr << "[ERROR] execvp failed for: " << cmd[0] << std::endl;
        _exit(1);
    }
    constexpr int kStillRunning = -2;
    int result = kStillRunning;
    auto watch = Glib::signal_child_watch().connect([&result, &dialog](GPid p, int status) {
        result = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        Glib::spawn_close_pid(p);
        dialog.response(Gtk::RESPONSE_OK);
    }, pid);
    auto pulse = Glib::signal_timeout().connect([&bar] { bar.pulse(); return true; }, 120);
    // Closing the dialog does not stop the emulator : keep waiting for it.
    while (result == kStillRunning) dialog.run();
    pulse.disconnect();
    watch.disconnect();
    return result;
}

// Pins szAppDatListsPath in fbneo.ini to `path` so FBNeo is *told* where to
// write instead of the launcher guessing where it might write on its own // the guess broke the moment FBNeo's own default location changed upstream
// (see MainWindow::update_fbneo_config for the equivalent pattern already
// used for szAppRomPaths). Silently does nothing if the ini can't be read;
// -dat still runs against whatever it already had.
static void patch_fbneo_ini_dat_path(const std::string& path) {
    const char* home_env = std::getenv("HOME");
    if (!home_env) return;
    std::string config_file = std::string(home_env) + "/.local/share/fbneo/config/fbneo.ini";

    std::string value = path;
    if (!value.empty() && value.back() != '/') value += "/";

    std::vector<std::string> lines;
    {
        std::ifstream in(config_file);
        if (!in.is_open()) return; // fbneo has never run yet : nothing to patch
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
    }

    bool found = false;
    for (auto& line : lines) {
        if (line.rfind("szAppDatListsPath", 0) == 0) {
            line = "szAppDatListsPath " + value;
            found = true;
            break;
        }
    }
    if (!found) lines.push_back("szAppDatListsPath " + value);

    std::ofstream out(config_file);
    for (const auto& line : lines) out << line << "\n";
}

void GenerateDAT::execute(Gtk::Window& parent, const std::string& fbneo_executable,
                           const std::string& dat_path, Gtk::Entry* dat_entry) {
    if (fbneo_executable.empty()) {
        SettingsUi::notice(parent, _("FBNeo executable missing"),
                           _("Please configure the FBNeo executable path first."),
                           "bc-error.svg");
        return;
    }

    // The configured DAT path is the single source of truth: tell FBNeo to
    // write there (patch_fbneo_ini_dat_path, below) instead of guessing where
    // it might decide to write on its own : that guess broke outright the
    // moment FBNeo's own default changed upstream. Only fall back to FBNeo's
    // documented default when nothing is configured yet (first-ever run).
    std::string dat_output_dir = dat_path;
    if (dat_output_dir.empty()) {
        const char* home_env = std::getenv("HOME");
        dat_output_dir = std::string(home_env ? home_env : ".") + "/.local/share/fbneo/support/lists/dat";
    }
    try {
        std::filesystem::create_directories(dat_output_dir);
    } catch (const std::exception& e) {
        SettingsUi::notice(parent, _("Directory creation failed"),
                           _("Failed to create directory: ") + dat_output_dir + "\n\n" + std::string(e.what()),
                           "bc-error.svg");
        return;
    }
    
    // La fenetre de progression, dans le langage visuel de l'application :
    // en-tete a tuile, et la barre seule dans le corps. Elle ne se ferme pas
    // a la croix : le traitement tourne dans l'emulateur, pas ici.
    auto progress_dialog = Gtk::Dialog();
    progress_dialog.set_transient_for(parent);
    progress_dialog.set_modal(true);
    progress_dialog.set_resizable(false);
    progress_dialog.set_default_size(520, -1);
    progress_dialog.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);
    SettingsUi::window_header(progress_dialog, "bc-generate-dat.svg", _("Generating DAT Files"),
                              _("Reading the game list from FBNeo..."));

    auto content_area = progress_dialog.get_content_area();
    auto main_box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 14);
    main_box->set_margin_start(22);
    main_box->set_margin_end(22);
    main_box->set_margin_top(20);
    main_box->set_margin_bottom(20);

    auto progress_bar = Gtk::make_managed<Gtk::ProgressBar>();
    progress_bar->pulse();
    main_box->pack_start(*progress_bar, Gtk::PACK_SHRINK);
    
    content_area->set_spacing(0);
    content_area->pack_start(*main_box, Gtk::PACK_EXPAND_WIDGET);
    progress_dialog.show_all();
    
    patch_fbneo_ini_dat_path(dat_output_dir);

    // Execute fbneo -dat command (no shell : safe for paths with spaces)
    int result = spawn_and_wait({fbneo_executable, "-dat"}, progress_dialog, *progress_bar);
    
    progress_dialog.hide();
    
    if (result == 0) {
        show_success_dialog(parent, dat_output_dir, dat_entry);
    } else {
        SettingsUi::notice(parent, _("DAT generation failed"),
                           _("Failed to generate DAT files.\n\nMake sure the FBNeo executable is valid and accessible."),
                           "bc-error.svg");
    }
}

void GenerateDAT::execute_mame(Gtk::Window& parent, const std::string& mame_executable,
                               const std::string& dat_path, Gtk::Entry* dat_entry) {
    if (mame_executable.empty()) {
        SettingsUi::notice(parent, _("MAME executable missing"),
                           _("MAME was not found on this system. Install it, then try again."),
                           "bc-error.svg");
        return;
    }
    // MAME n'ecrit pas de DAT et n'a donc pas de dossier a lui ou retomber :
    // c'est le dossier du groupe qui decide, et il doit etre choisi.
    if (dat_path.empty()) {
        SettingsUi::notice(parent, _("No DAT folder"),
                           _("Choose the folder where the DAT files should be written first."),
                           "bc-error.svg");
        return;
    }
    try {
        std::filesystem::create_directories(dat_path);
    } catch (const std::exception& e) {
        SettingsUi::notice(parent, _("Directory creation failed"),
                           _("Failed to create directory: ") + dat_path + "\n\n" + std::string(e.what()),
                           "bc-error.svg");
        return;
    }

    // Meme fenetre, meme langage visuel que la generation depuis FBNeo : le
    // geste est le meme pour l'utilisateur, seul le producteur change.
    auto progress_dialog = Gtk::Dialog();
    progress_dialog.set_transient_for(parent);
    progress_dialog.set_modal(true);
    progress_dialog.set_resizable(false);
    progress_dialog.set_default_size(520, -1);
    progress_dialog.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);
    SettingsUi::window_header(progress_dialog, "bc-generate-dat.svg", _("Generating DAT Files"),
                              _("Reading the machine list from MAME..."));

    auto content_area = progress_dialog.get_content_area();
    auto main_box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 14);
    main_box->set_margin_start(22);
    main_box->set_margin_end(22);
    main_box->set_margin_top(20);
    main_box->set_margin_bottom(20);

    auto progress_bar = Gtk::make_managed<Gtk::ProgressBar>();
    progress_bar->set_show_text(true);
    progress_bar->set_text(_("Starting MAME..."));
    progress_bar->pulse();
    main_box->pack_start(*progress_bar, Gtk::PACK_SHRINK);

    auto* cancel_button = SettingsUi::button(_("Cancel"), "bc-close.svg");
    cancel_button->set_halign(Gtk::ALIGN_END);
    main_box->pack_start(*cancel_button, Gtk::PACK_SHRINK);

    content_area->set_spacing(0);
    content_area->pack_start(*main_box, Gtk::PACK_EXPAND_WIDGET);
    progress_dialog.show_all();

    // La conversion lit 320 Mo de -listxml : sur le fil principal, la barre
    // ne bougerait plus et le bureau proposerait de tuer l'application. Elle
    // tourne donc a cote, et les deux Dispatcher sont le seul chemin de
    // retour vers l'interface.
    std::atomic<bool> cancelled{false};
    std::atomic<bool> finished{false};
    std::atomic<int>  machines{0};
    int result = -1;

    Glib::Dispatcher on_tick;
    Glib::Dispatcher on_finished;
    on_tick.connect([progress_bar, &machines, &cancelled] {
        if (cancelled.load()) return;   // le libelle « Annulation » ne doit plus bouger
        progress_bar->set_text(Glib::ustring::compose(_("%1 machines read..."), machines.load()));
    });
    on_finished.connect([&progress_dialog] { progress_dialog.response(Gtk::RESPONSE_OK); });

    cancel_button->signal_clicked().connect([cancel_button, progress_bar, &cancelled] {
        cancelled = true;
        cancel_button->set_sensitive(false);
        progress_bar->set_text(_("Cancelling..."));
    });

    std::thread worker([&] {
        result = MameCatalog::generate_dats(mame_executable, dat_path, [&](int count) {
            machines = count;
            on_tick.emit();
            return !cancelled.load();
        });
        finished = true;
        on_finished.emit();
    });

    auto pulse = Glib::signal_timeout().connect([progress_bar] { progress_bar->pulse(); return true; }, 120);
    // La croix de la fenetre ne doit pas laisser le fil derriere elle : on
    // revient dans la boucle tant que la conversion n'a pas rendu la main.
    while (!finished.load()) progress_dialog.run();
    pulse.disconnect();
    worker.join();
    progress_dialog.hide();

    if (result > 0) {
        show_success_dialog(parent, dat_path, dat_entry);
    } else if (cancelled.load()) {
        SettingsUi::notice(parent, _("DAT generation cancelled"),
                           _("The DAT files were not written."),
                           "bc-info.svg");
    } else {
        SettingsUi::notice(parent, _("DAT generation failed"),
                           _("Failed to generate DAT files from MAME.\n\nMake sure the MAME executable is valid and accessible."),
                           "bc-error.svg");
    }
}

void GenerateDAT::show_success_dialog(Gtk::Window& parent, const std::string& dat_path, Gtk::Entry* dat_entry) {
    // La boite de la charte, avec un seul geste propose a cote de « OK ».
    const bool set_path = SettingsUi::offer(parent, _("DAT generation complete"),
        _("DAT files have been generated successfully."),
        _("Set as DAT Path"), "bc-check.svg", "bc-folder.svg");

    if (set_path && dat_entry) {
        // Update DAT entry
        dat_entry->set_text(dat_path);

        // Save to config. Read-modify-write on the real config path: writing a
        // bare "config.json" targeted the current working directory and replaced
        // the whole file with this single key.
        const std::string config_path = AppContext::get_config_path();
        nlohmann::json j;
        {
            std::ifstream in(config_path);
            if (in) { try { in >> j; } catch (...) { j = nlohmann::json{}; } }
        }
        j["dat_path"] = dat_path;
        std::ofstream config(config_path);
        if (config.is_open()) {
            config << j.dump(4);
            config.close();
        }

        SettingsUi::notice(parent, _("Path updated"),
                           _("The DAT folder is now:") + std::string("\n") + dat_path,
                           "bc-check.svg");
    }
}