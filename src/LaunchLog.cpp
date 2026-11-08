// src/LaunchLog.cpp
#include "LaunchLog.h"

#include "AppContext.h"
#include "SettingsUi.h"
#include "i18n.h"

#include <gtkmm.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <sys/wait.h>
#endif

namespace LaunchLog {
namespace {

// Au-dela, on ne garde que la fin : un emulateur bavard pendant deux heures
// ne doit pas remplir une boite de dialogue, et la cause d'un arret est
// toujours dans les dernieres lignes.
constexpr std::streamoff kMaxShown = 256 * 1024;

std::string read_tail(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    const std::streamoff from = size > kMaxShown ? size - kMaxShown : 0;
    f.seekg(from);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (from > 0) {
        const size_t nl = text.find('\n');
        if (nl != std::string::npos) text.erase(0, nl + 1);
    }
    // Une sortie d'emulateur n'est pas forcement de l'UTF-8 valide (noms de
    // fichiers, caracteres de console Windows) : GTK refuserait tout le texte.
    gchar* valid = g_utf8_make_valid(text.c_str(), (gssize)text.size());
    std::string out = valid ? valid : "";
    g_free(valid);
    return out;
}

// Ce que l'emulateur a ecrit, sans l'en-tete et le pied que Bootcade ajoute.
std::string emulator_output(const std::string& log) {
    std::istringstream in(log);
    std::string line, out;
    while (std::getline(in, line)) {
        if (line.rfind("# ", 0) == 0) continue;
        out += line;
        out += '\n';
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out;
}

std::string label_of(const std::string& emulator) {
    return emulator == "mame" ? "MAME" : "FinalBurn Neo";
}

std::string signal_name(int sig) {
#ifndef _WIN32
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGILL:  return "SIGILL";
        case SIGFPE:  return "SIGFPE";
        case SIGBUS:  return "SIGBUS";
        case SIGKILL: return "SIGKILL";
        case SIGTERM: return "SIGTERM";
        default: break;
    }
    return "signal " + std::to_string(sig);
#else
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08X", (unsigned)sig);
    return buf;
#endif
}

// La cause, en mots courants. MAME a des codes de sortie documentes
// (emu/main.h) ; FinalBurn Neo rend 1 pour un jeu qu'il ne connait pas.
std::string explain(const Game& g, const Exit& e) {
    const std::string emu = label_of(g.emulator);
    std::string why;
    if (e.signaled) {
        why = Glib::ustring::compose(_("%1 crashed (%2)."), emu, signal_name(e.code)).raw();
    } else if (g.emulator == "mame") {
        switch (e.code) {
            case 0: why = _("MAME closed right after starting, without reporting an error."); break;
            case 1: why = _("MAME found a problem in its own data and refused to start."); break;
            case 2: why = _("Files this game needs are missing: MAME lists them below. "
                            "A game may also need its BIOS, its parent or a disk image (CHD)."); break;
            case 3: why = _("MAME stopped on a fatal error. Its last messages below give the reason."); break;
            case 4: why = _("A device this machine needs could not start."); break;
            case 5: why = _("This version of MAME does not know this game: it may have been renamed or removed."); break;
            case 6: why = _("MAME refused one of its options or its configuration file (mame.ini)."); break;
            default:
                why = Glib::ustring::compose(_("MAME stopped with error code %1."), e.code).raw();
        }
    } else {
        switch (e.code) {
            case 0: why = _("FinalBurn Neo closed right after starting, without reporting an error. "
                            "Its messages below may say why."); break;
            case 1: why = _("FinalBurn Neo could not load this game. Its messages below say why."); break;
            default:
                why = Glib::ustring::compose(_("FinalBurn Neo stopped with error code %1."), e.code).raw();
        }
    }
    if (g.emulator == "mame" && g.driver_status == "preliminary")
        why += "\n\n" + std::string(_("MAME marks this game as not working yet: even with all its "
                                      "files, it may refuse to start, freeze or stay on a black screen."));
    return why;
}

void run_dialog(Gtk::Window& parent, const std::string& title, const std::string& message,
                const std::string& icon, const std::string& log_path) {
    const std::string log = read_tail(log_path);
    std::string shown = emulator_output(log);
    if (shown.empty()) shown = _("The emulator did not write anything.");

    Gtk::Dialog dlg;
    dlg.set_title(title);
    dlg.set_transient_for(parent);
    dlg.set_modal(true);
    dlg.set_decorated(false);
    dlg.get_style_context()->add_class("cc-window");
    dlg.get_style_context()->add_class("set-window");
    dlg.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);

    auto* content = dlg.get_content_area();
    content->set_spacing(0);

    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 12);
    body->get_style_context()->add_class("set-notice");

    auto* head = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 15);
    head->pack_start(*SettingsUi::tile(icon, SettingsUi::kIconSection, SettingsUi::kTileSection),
                     Gtk::PACK_SHRINK);
    auto* txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 4);
    txt->set_valign(Gtk::ALIGN_CENTER);
    txt->pack_start(*SettingsUi::card_title_label(title), Gtk::PACK_SHRINK);
    if (!message.empty()) {
        auto* sub = SettingsUi::sub_label(message);
        sub->set_max_width_chars(70);
        txt->pack_start(*sub, Gtk::PACK_SHRINK);
    }
    head->pack_start(*txt, Gtk::PACK_EXPAND_WIDGET);
    body->pack_start(*head, Gtk::PACK_SHRINK);

    auto* view = Gtk::make_managed<Gtk::TextView>();
    view->set_editable(false);
    view->set_cursor_visible(false);
    view->set_wrap_mode(Gtk::WRAP_WORD_CHAR);
    view->get_style_context()->add_class("set-mono");
    view->get_style_context()->add_class("set-log");
    view->get_buffer()->set_text(shown);
    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    scroll->get_style_context()->add_class("set-log-frame");
    scroll->set_size_request(720, 280);
    scroll->add(*view);
    body->pack_start(*scroll, Gtk::PACK_EXPAND_WIDGET);

    auto* where = SettingsUi::sub_label(Glib::ustring::compose(_("Full log: %1"), log_path).raw());
    where->set_selectable(true);
    where->set_line_wrap(true);
    where->set_line_wrap_mode(Pango::WRAP_CHAR);   // un chemin n'a pas d'espaces
    where->set_max_width_chars(90);
    body->pack_start(*where, Gtk::PACK_SHRINK);
    content->pack_start(*body, Gtk::PACK_EXPAND_WIDGET);

    auto* foot = SettingsUi::footer();
    auto* ok = SettingsUi::button(_("OK"), "", SettingsUi::Tone::Accent);
    ok->set_size_request(96, -1);
    ok->signal_clicked().connect([&dlg] { dlg.response(Gtk::RESPONSE_CLOSE); });
    auto* copy = SettingsUi::button(_("Copy"));
    copy->signal_clicked().connect([log] {
        Gtk::Clipboard::get()->set_text(log);
    });
    auto* open = SettingsUi::button(_("Open the log"), "bc-file.svg");
    open->signal_clicked().connect([&dlg, log_path] {
        SettingsUi::open_uri(Glib::filename_to_uri(log_path), &dlg);
    });
    foot->pack_end(*ok, Gtk::PACK_SHRINK);
    foot->pack_end(*copy, Gtk::PACK_SHRINK);
    foot->pack_end(*open, Gtk::PACK_SHRINK);
    content->pack_start(*foot, Gtk::PACK_SHRINK);

    dlg.show_all_children();
    // Le texte s'ouvre sur sa fin : c'est la que l'emulateur dit pourquoi il
    // s'arrete.
    Glib::signal_idle().connect_once([view] {
        auto buf = view->get_buffer();
        auto end = buf->end();
        view->scroll_to(end);
    });
    ok->grab_focus();
    dlg.run();
}

}  // namespace

std::string path_for(const std::string& emulator) {
    const std::filesystem::path dir =
        std::filesystem::path(AppContext::get_user_config_dir()) / "logs";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return (dir / (emulator + "-last.log")).string();
}

void begin(const std::string& path, const std::string& emulator_label,
           const std::string& command_line) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return;
    const Glib::DateTime now = Glib::DateTime::create_now_local();
    f << "# Bootcade: " << emulator_label << " launched " << now.format("%Y-%m-%d %H:%M:%S").raw() << "\n"
      << "# Command: " << command_line << "\n";
}

#ifndef _WIN32
Exit from_wait_status(int status, int seconds) {
    Exit e;
    e.seconds = seconds;
    if (WIFSIGNALED(status)) {
        e.signaled = true;
        e.code = WTERMSIG(status);
    } else if (WIFEXITED(status)) {
        e.code = WEXITSTATUS(status);
        /* Sous Flatpak, le jeu tourne derriere flatpak-spawn, qui rend
         * 128 + n quand le programme a ete tue par le signal n. Ni MAME ni
         * FinalBurn Neo n'utilisent ces codes eux-memes. */
        if (e.code > 128 && e.code < 128 + 32) {
            e.signaled = true;
            e.code -= 128;
        }
    }
    return e;
}
#else
Exit from_exit_code(int code, int seconds) {
    Exit e;
    e.seconds = seconds;
    e.code = code;
    // Les plantages Windows rendent un code d'exception : 0xC0000005, etc.
    if ((unsigned)code >= 0xC0000000u) e.signaled = true;
    return e;
}
#endif

void finish(const std::string& path, const Exit& e) {
    std::ofstream f(path, std::ios::binary | std::ios::app);
    if (f) f << "\n# Exit: " << (e.signaled ? "crash " + signal_name(e.code)
                                             : "code " + std::to_string(e.code))
             << " after " << e.seconds << " s\n";
}

bool looks_failed(const Exit& e) {
    if (e.signaled) {
#ifndef _WIN32
        // Fermer la fenetre peut se terminer par un SIGTERM ou un SIGKILL
        // venus du bureau : ce n'est pas une panne du jeu.
        if (e.code == SIGTERM || e.code == SIGKILL || e.code == SIGINT || e.code == SIGHUP)
            return false;
#endif
        return true;
    }
    if (e.code != 0) return true;
    // Personne ne joue cinq secondes : un arret aussi rapide est un refus.
    return e.seconds < 5;
}

void show_report(Gtk::Window& parent, const Game& game, const Exit& exit,
                 const std::string& log_path) {
    const std::string name = game.title.empty() ? std::string(_("This game")) : game.title;
    const std::string title = exit.seconds < 5
        ? Glib::ustring::compose(_("%1 did not start"), name).raw()
        : Glib::ustring::compose(_("%1 stopped unexpectedly"), name).raw();
    run_dialog(parent, title, explain(game, exit), "bc-error.svg", log_path);
}

void show_last(Gtk::Window& parent, const std::string& emulator) {
    const std::string path = path_for(emulator);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        SettingsUi::notice(parent, _("No launch log yet"),
                           Glib::ustring::compose(_("%1 has not been launched from Bootcade yet."),
                                                  label_of(emulator)).raw(),
                           "bc-info.svg");
        return;
    }
    run_dialog(parent,
               Glib::ustring::compose(_("Last launch of %1"), label_of(emulator)).raw(),
               _("Everything the emulator wrote while it ran, as Bootcade received it."),
               "bc-info.svg", path);
}

}  // namespace LaunchLog
