// src/Workspace.cpp
#include "Workspace.h"

#include "AppContext.h"
#include "DefaultFolders.h"
#include "FileChooser.h"
#include "i18n.h"

#include <gtkmm.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace Workspace {
namespace {

std::atomic<bool> g_relocated{false};

std::string pending_file() { return AppContext::standard_config_dir() + "/relocate.json"; }

// Separateurs et, sous Windows, casse : « C:\Roms » et « c:/roms » sont le meme dossier.
std::string key_of(std::string p) {
    for (char& c : p) {
        if (c == '\\') c = '/';
#ifdef _WIN32
        c = (char)std::tolower((unsigned char)c);
#endif
    }
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

// `path` est `dir` lui-meme ou se trouve dessous.
bool inside(const std::string& path, const std::string& dir) {
    const std::string p = key_of(path), d = key_of(dir);
    if (d.empty()) return false;
    return p == d || (p.size() > d.size() && p.compare(0, d.size(), d) == 0 && p[d.size()] == '/');
}

bool is_empty_dir(const std::string& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return true;
    if (!fs::is_directory(dir, ec)) return false;
    return fs::directory_iterator(dir, ec) == fs::directory_iterator();
}

// Remplace, dans toutes les chaines de config.json, un chemin du dossier d'avant par celui d'apres.
void rewrite_paths(nlohmann::json& j, const std::vector<std::string>& from, const std::string& to) {
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        for (std::string f : from) {
            while (f.size() > 1 && (f.back() == '/' || f.back() == '\\')) f.pop_back();
            if (f.empty() || !inside(s, f)) continue;
            // key_of ne change pas la longueur : ce qui suit le dossier d'avant est intact.
            j = to + s.substr(f.size());
            return;
        }
    } else if (j.is_array() || j.is_object()) {
        for (auto& v : j) rewrite_paths(v, from, to);
    }
}

// Un enfant de `src` vers `dst` : rename, sinon copie puis suppression. "" ou l'erreur.
std::string move_one(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::rename(src, dst, ec);
    if (!ec) return {};
    ec.clear();
    fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove_all(dst, ignored);   // pas de demi-copie
        return ec.message();
    }
    fs::remove_all(src, ec);
    return {};
}

std::string move_children(const std::string& from, const std::string& to, const Say& say) {
    std::error_code ec;
    if (from.empty() || !fs::is_directory(from, ec)) return {};
    std::vector<fs::path> children;
    for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) children.push_back(it->path());
    for (const auto& c : children) {
        const std::string name = c.filename().string();
        if (name == "workdir" || name == "relocate.json") continue;   // les reperes restent ou ils sont
        if (inside(c.string(), to)) continue;                         // le nouveau dossier, s'il est dessous
        const fs::path dst = fs::path(to) / name;
        if (fs::exists(dst, ec)) { std::cerr << "[WORKDIR] " << dst << " exists : left in place" << std::endl; continue; }
        if (say) say(name);
        const std::string err = move_one(c, dst);
        if (!err.empty()) return std::string(_("Could not move")) + " " + c.string() + " : " + err;
    }
    return {};
}

}  // namespace

bool pending() {
    std::error_code ec;
    return fs::exists(pending_file(), ec);
}

bool relocated_this_run() { return g_relocated.load(); }

std::string request(const std::string& to_in) {
    if (to_in.empty()) return _("Choose a folder.");
    std::error_code ec;
    const fs::path abs = fs::absolute(fs::path(to_in), ec);
    if (ec || !abs.is_absolute()) return _("Choose a full folder path.");
    const std::string to = abs.lexically_normal().string();

    const std::string cur_root = DefaultFolders::root();
    const std::string cur_cfg = AppContext::get_user_config_dir();
    if (key_of(to) == key_of(cur_root) && key_of(to) == key_of(cur_cfg)) return _("Bootcade already uses this folder.");
    for (const auto& cur : {cur_root, cur_cfg})
        if (inside(to, cur)) return _("Choose a folder outside the current Bootcade folder.");
    if (!is_empty_dir(to)) return _("Choose an empty folder, or a new one.");

    // Le joueur peut-il y ecrire ?
    fs::create_directories(to, ec);
    if (ec) return std::string(_("Cannot create this folder")) + " : " + ec.message();
    const fs::path probe = fs::path(to) / ".bootcade-write-test";
    { std::ofstream t(probe); if (!t) return _("Bootcade cannot write in this folder."); }
    fs::remove(probe, ec);

    std::ofstream out(pending_file(), std::ios::trunc);
    if (!out) return _("Cannot save the request.");
    nlohmann::json j; j["to"] = to;
    out << j.dump(2) << std::endl;
    return {};
}

std::string relocate(const std::string& to_in, const Say& say) {
    std::error_code ec;
    const std::string to = fs::absolute(fs::path(to_in), ec).lexically_normal().string();
    fs::create_directories(to, ec);
    if (ec) return std::string(_("Cannot create this folder")) + " : " + ec.message();

    // D'ou l'on vient : le dossier de donnees et le dossier de config actuels.
    std::vector<std::string> from;
    from.push_back(DefaultFolders::root());
    const std::string cfg = AppContext::get_user_config_dir();
    if (key_of(cfg) != key_of(from.front())) from.push_back(cfg);
    // La config d'avant, pour recopier ses chemins.
    const std::string old_config = cfg + "/config.json";

    for (const auto& f : from) {
        if (key_of(f) == key_of(to)) continue;
        const std::string err = move_children(f, to, say);
        if (!err.empty()) return err;
    }

    // Les chemins de config.json qui pointaient dans l'ancien dossier pointent dans le nouveau.
    const std::string new_config = to + "/config.json";
    nlohmann::json j;
    { std::ifstream in(fs::exists(new_config, ec) ? new_config : old_config); if (in) { try { in >> j; } catch (...) { j = nlohmann::json::object(); } } }
    if (!j.is_object()) j = nlohmann::json::object();
    std::vector<std::string> prefixes;
    for (const auto& f : from) if (key_of(f) != key_of(to)) prefixes.push_back(f);
    rewrite_paths(j, prefixes, to);
    { std::ofstream out(new_config, std::ios::trunc); if (!out) return _("Cannot write the settings in the new folder."); out << j.dump(4) << std::endl; }

    // L'ancien dossier de donnees, vide maintenant, ne traine pas : remove ne supprime qu'un dossier vide.
    // (Le dossier de config habituel reste : il porte le fichier repere.)
    for (const auto& f : from)
        if (key_of(f) != key_of(to) && key_of(f) != key_of(AppContext::standard_config_dir())) fs::remove(f, ec);

    AppContext::set_workspace_pointer(to);
    fs::remove(pending_file(), ec);
    g_relocated = true;
    std::cout << "[WORKDIR] Bootcade now lives in " << to << std::endl;
    return {};
}

std::string run_pending(const Say& say) {
    std::ifstream in(pending_file());
    if (!in) return {};
    std::string to;
    try { nlohmann::json j; in >> j; to = j.value("to", std::string()); } catch (...) {}
    in.close();
    std::error_code ec;
    if (to.empty()) { fs::remove(pending_file(), ec); return {}; }
    return relocate(to, say);
}

std::string ask_first_run() {
    Gtk::Dialog dlg(_("Welcome to Bootcade"), true);
    dlg.set_default_size(560, -1);
    dlg.set_position(Gtk::WIN_POS_CENTER);
    auto* box = dlg.get_content_area();
    box->set_spacing(12);
    box->set_margin_start(24); box->set_margin_end(24); box->set_margin_top(20); box->set_margin_bottom(8);

    auto* title = Gtk::make_managed<Gtk::Label>();
    title->set_markup(std::string("<b>") + Glib::Markup::escape_text(_("Where should Bootcade keep its files?")) + "</b>");
    title->set_xalign(0.0f);
    box->pack_start(*title, Gtk::PACK_SHRINK);
    auto* text = Gtk::make_managed<Gtk::Label>(_("Your settings, your game database, the DAT files, the artwork and the working "
                                                  "ROM folders all go there. You can change it later in Settings."));
    text->set_xalign(0.0f); text->set_line_wrap(true);
    box->pack_start(*text, Gtk::PACK_SHRINK);

    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    auto* entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_text(DefaultFolders::default_root());
    row->pack_start(*entry, Gtk::PACK_EXPAND_WIDGET);
    auto* browse = Gtk::make_managed<Gtk::Button>(_("Browse…"));
    row->pack_start(*browse, Gtk::PACK_SHRINK);
    box->pack_start(*row, Gtk::PACK_SHRINK);

    auto* warn = Gtk::make_managed<Gtk::Label>();
    warn->set_xalign(0.0f); warn->set_line_wrap(true);
    box->pack_start(*warn, Gtk::PACK_SHRINK);

    browse->signal_clicked().connect([&] {
        BcFileChooser chooser(dlg, _("Choose the Bootcade folder"), Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
        chooser.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
        chooser.add_button(_("Select"), Gtk::RESPONSE_OK);
        if (chooser.run() == Gtk::RESPONSE_OK) entry->set_text(chooser.get_filename());
    });
    auto* ok = dlg.add_button(_("Continue"), Gtk::RESPONSE_OK);
    ok->get_style_context()->add_class("suggested-action");
    dlg.set_default_response(Gtk::RESPONSE_OK);
    dlg.show_all();

    for (;;) {
        dlg.run();
        std::string chosen = entry->get_text();
        if (chosen.empty()) chosen = DefaultFolders::default_root();
        std::error_code ec;
        const fs::path abs = fs::absolute(fs::path(chosen), ec);
        if (ec || !abs.is_absolute()) { warn->set_text(_("Choose a full folder path.")); continue; }
        // Un dossier qui contient deja des fichiers : Bootcade y range les siens
        // a cote, il n'efface rien. Le seul refus : un dossier ou l'on ne peut pas ecrire.
        fs::create_directories(abs, ec);
        const fs::path probe = abs / ".bootcade-write-test";
        { std::ofstream t(probe); if (ec || !t) { warn->set_text(_("Bootcade cannot write in this folder.")); continue; } }
        fs::remove(probe, ec);
        return abs.lexically_normal().string();
    }
}

void run_pending_with_window() {
    if (!pending()) return;
    Gtk::Window win;
    win.set_title("Bootcade");
    win.set_default_size(460, -1);
    win.set_position(Gtk::WIN_POS_CENTER);
    win.set_deletable(false);
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 10);
    box->set_margin_start(24); box->set_margin_end(24); box->set_margin_top(22); box->set_margin_bottom(22);
    auto* title = Gtk::make_managed<Gtk::Label>(_("Moving Bootcade to its new folder…"));
    title->set_xalign(0.0f);
    auto* now = Gtk::make_managed<Gtk::Label>("");
    now->set_xalign(0.0f);
    auto* bar = Gtk::make_managed<Gtk::ProgressBar>();
    box->pack_start(*title, Gtk::PACK_SHRINK);
    box->pack_start(*bar, Gtk::PACK_SHRINK);
    box->pack_start(*now, Gtk::PACK_SHRINK);
    win.add(*box);
    win.show_all();

    std::atomic<bool> done{false};
    std::string error;
    std::string current;
    std::mutex m;
    std::thread worker([&] {
        error = run_pending([&](const std::string& what) { std::lock_guard<std::mutex> l(m); current = what; });
        done = true;
    });
    while (!done) {
        { std::lock_guard<std::mutex> l(m); now->set_text(current); }
        bar->pulse();
        while (Gtk::Main::events_pending()) Gtk::Main::iteration(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    worker.join();
    win.hide();
    if (!error.empty()) {
        std::cerr << "[WORKDIR] " << error << std::endl;
        Gtk::MessageDialog d(error, false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
        d.set_title(_("Bootcade could not move its files"));
        d.set_secondary_text(_("Nothing was lost : what was not moved stays where it was. Bootcade starts from its previous location."));
        d.run();
    }
}

}  // namespace Workspace
