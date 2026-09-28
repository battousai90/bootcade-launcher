// src/MainWindow.cpp
#include "MainWindow.h"
#include "EmulatorRegistry.h"
#include "MameCatalog.h"
#include "BootcadeAuth.h"
#include "LoginDialog.h"
#include "IconManager.h"
#include "i18n.h"
#include <iostream>
#include "DatParser.h"
#include "DatSource.h"
#include "SettingsPanel.h"
#include "SettingsUi.h"
#include "DownloadDialog.h"
#include "GenerateDAT.h"
#include "FbneoUpdateCheck.h"
#include "ScreenshotAssignDialog.h"
#include "HiscoreReportDialog.h"
#include "SystemPrefix.h"
#include "Game.h"
#include "ModelColumns.h"
#include "RomScanner.h"
#include "AppContext.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <set>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <optional>
#include <cstdlib>
#include <thread>
#include <random>
#include "IconManager.h"
#include "ControllerDialog.h"
#include "MameControls.h"
#include <memory>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <zip.h>
#include <sstream>
#include <locale>
#include <ctime>

/* Largeurs des colonnes de la liste.
 *
 * Partagees par l'en-tete et par chaque ligne : c'est la seule facon de
 * garantir qu'ils restent alignes.
 */
static constexpr int kColThumb  = 52;
static constexpr int kColSystem = 104;
// Colonne de l'emulateur : juste la place du nom le plus long du registre.
static constexpr int kColEmu    = 104;
static constexpr int kColYear   = 46;
static constexpr int kColStatus = 54;
static constexpr int kColHs     = 38;

/* La marque d'un emulateur, posee dans les vues.
 *
 * brand_pixbuf() est definie plus bas, avec le reste du selecteur : les trois
 * vues en ont besoin bien avant, d'ou cette declaration.
 *
 * Le cache n'est pas une optimisation de confort : la liste aligne des
 * dizaines de milliers de lignes pour deux images en tout, et sans lui chaque
 * ligne redecoderait un SVG. Une reference vide est mise en cache elle aussi,
 * sans quoi un fichier absent serait redemande a chaque ligne.
 */
static Glib::RefPtr<Gdk::Pixbuf> brand_pixbuf(const std::string& rel, int w, int h);

static Glib::RefPtr<Gdk::Pixbuf> emulator_badge(const std::string& emu_id, int w, int h) {
    static std::unordered_map<std::string, Glib::RefPtr<Gdk::Pixbuf>> cache;
    const std::string key = emu_id + ':' + std::to_string(w) + 'x' + std::to_string(h);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    const EmulatorInfo* e = EmulatorRegistry::find(emu_id);
    auto pb = brand_pixbuf(e ? e->logo : std::string(), w, h);
    cache.emplace(key, pb);
    return pb;
}

/* Taille des cartes, en PIXELS, et non en nombre de colonnes.
 *
 * La grille imposait exactement N colonnes
 * (min_children_per_line == max_children_per_line == N). Avec des cartes de
 * largeur fixe, elle reclamait donc N x 176 px de large quoi qu'il reste :
 * quand la fenetre ne les avait pas, elle ne se repliait pas, elle poussait,
 * et le volet de details sortait de la fenetre. C'etait la cause du bug de
 * mise en page, pas un reglage a ajuster.
 *
 * Desormais le curseur fixe la LARGEUR D'UNE CARTE et la grille place
 * autant de cartes que la place disponible en permet. Elle peut toujours se
 * replier jusqu'a une seule colonne, donc elle ne pousse plus jamais rien.
 * Le nombre de colonnes devient une consequence, ce qui est aussi ce que le
 * joueur attend d'un reglage nomme « taille des cartes ».
 */
static constexpr int kMinCardWidth = 120;
static constexpr int kMaxCardWidth = 300;


// Launch an external process without invoking a shell.
// args[0] must be the executable path; remaining entries are its arguments.
// Returns the child PID on success, -1 on failure.
//
// FinalBurn Neo lives on the host and is linked against the host's libraries
// (SDL2 and friends), none of which exist inside our sandbox : running it
// directly from a Flatpak fails with "error while loading shared libraries".
// So when sandboxed, the command is handed to flatpak-spawn, which executes it
// on the host. That is what the --talk-name=org.freedesktop.Flatpak permission
// in the manifest is for.
// std::filesystem::last_write_time()'s usual duration_cast-to-seconds idiom
// does NOT yield a Unix timestamp on this toolchain under strict C++17 // std::filesystem::file_clock's epoch here is not 1970 (confirmed empirically:
// off by billions of seconds), and file_clock::to_sys() needs C++20. stat()
// gives a real time_t directly, no epoch ambiguity.
static std::time_t get_file_mtime(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return -1;
    return st.st_mtime;
}

// Le dossier cfg ou Bootcade fait ecrire MAME : celui qu'il utilise deja
// quand on l'appelle a la main.
static std::string mame_cfg_dir() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.mame/cfg";
}

// `env` : variables « NOM=valeur » pour ce seul processus. Sous Flatpak elles
// doivent traverser flatpak-spawn, qui ne transmet pas notre environnement.
static pid_t spawn_process(const std::vector<std::string>& args,
                           const std::vector<std::string>& env = {}) {
    if (args.empty()) return -1;

    std::vector<std::string> cmd = AppContext::host_command(args);
    const bool via_host = cmd.size() > args.size();
    if (via_host)
        for (auto it = env.rbegin(); it != env.rend(); ++it)
            cmd.insert(cmd.begin() + 2, "--env=" + *it);

    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "[ERROR] fork() failed for: " << args[0] << std::endl;
        return -1;
    }
    if (pid == 0) {
        if (!via_host)
            for (const auto& e : env) putenv(const_cast<char*>(e.c_str()));
        std::vector<char*> argv;
        argv.reserve(cmd.size() + 1);
        for (const auto& a : cmd)
            argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        std::cerr << "[ERROR] execvp failed for: " << cmd[0] << std::endl;
        _exit(1);
    }
    return pid; // parent gets child PID
}

// Watch a child process and record its playtime in the database when it exits.
/* `record` porte le reglage « Keep play history ».
 *
 * L'attente du processus, elle, a lieu dans tous les cas : c'est elle qui dit
 * quand la partie s'est terminee, donc quand relire le fichier de scores et
 * chercher les captures. Couper l'historique doit arreter l'ECRITURE, pas la
 * fin de partie. Lu sur le fil graphique et passe ici, comme le reste : ce fil
 * ne touche pas au panneau de reglages.
 */
static void watch_playtime(pid_t pid,
                            std::shared_ptr<DatabaseManager> db,
                            const std::string& game_name,
                            const std::string& system,
                            const std::string& emulator,
                            bool record)
{
    auto start = std::chrono::steady_clock::now();
    int status = 0;
    waitpid(pid, &status, 0); // blocking wait
    auto end = std::chrono::steady_clock::now();
    int elapsed = (int)std::chrono::duration_cast<std::chrono::seconds>(end - start).count();
    if (record && elapsed > 0)
        db->addPlayTime(game_name, system, elapsed, emulator);
}

// Where FBNeo keeps the raw RAM dump it writes when a game with hiscore
// support exits. Named after the FBNeo ROM name, which carries the console
// prefix : not after the catalogue name used to identify the game online.
static std::string fbneo_hiscores_dir() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "") + "/.local/share/fbneo/support/hiscores";
}

static std::string fbneo_hiscore_path(const std::string& fbneo_rom_name) {
    return fbneo_hiscores_dir() + "/" + fbneo_rom_name + ".hi";
}

// Neo Geo keeps its score table in the cartridge SRAM, not in a hiscore.dat
// memory range, so no .hi is ever written for Metal Slug or King of Fighters.
// FBNeo saves that SRAM here instead, with a 96 byte header the server strips.
static std::string fbneo_saveram_path(const std::string& fbneo_rom_name) {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "") +
           "/.local/share/fbneo/config/games/" + fbneo_rom_name + ".fs";
}

// The one file that holds this game's score table. The choice stays the same
// before and after the session : comparing a .hi against a .fs would compare
// two unrelated blobs and could publish a score nobody played.
//
// Preferring whichever file exists is not enough. Metal Slug 2 and X have
// both, and only their SRAM decodes correctly : their .hi yields the tenth
// row instead of the first. The service knows, from the definitions, which
// file each game is read from, and says so in /api/supported.
static std::string fbneo_score_state_path(const std::string& system,
                                          const std::string& game,
                                          const std::string& fbneo_rom_name) {
    if (HiscoreClient::is_saveram(system, game))
        return fbneo_saveram_path(fbneo_rom_name);
    const std::string hi = fbneo_hiscore_path(fbneo_rom_name);
    if (std::filesystem::exists(hi)) return hi;
    const std::string fs = fbneo_saveram_path(fbneo_rom_name);
    if (std::filesystem::exists(fs)) return fs;
    return hi;
}

// Returns "" when the file is absent, which is the normal state before a
// game has ever been played to a score worth keeping.
static std::string read_file_bytes(const std::string& path) {
    std::ifstream fi(path, std::ios::binary);
    if (!fi) return {};
    return std::string(std::istreambuf_iterator<char>(fi),
                       std::istreambuf_iterator<char>());
}

// "FR" -> the French flag. Built from Unicode regional indicators, which is
// what makes the fallback graceful: a system with no flag glyph draws the two
// letters instead of a blank or a placeholder box.
static std::string country_flag(const std::string& iso) {
    if (iso.size() != 2) return {};
    std::string out;
    for (char c : iso) {
        if (c < 'A' || c > 'Z') return {};       // lower-case or junk: skip it
        gunichar cp = 0x1F1E6 + (c - 'A');
        char buf[8] = {0};
        out.append(buf, g_unichar_to_utf8(cp, buf));
    }
    return out;
}

// "2026-08-29T09:11:57Z" -> "2026-08-29". The time of day says nothing a
// player wants; the day is what places a record in the life of a leaderboard.
static std::string short_date(const std::string& iso8601) {
    return iso8601.size() >= 10 ? iso8601.substr(0, 10) : std::string();
}

// "2 h 14", "37 min", "45 s" : the coarsest unit that still says something.
// A session is read at a glance, so seconds past the first minute are noise.
static std::string format_duration(int seconds) {
    if (seconds <= 0) return "";
    if (seconds < 60)   return std::to_string(seconds) + " s";
    int minutes = seconds / 60;
    if (minutes < 60)   return std::to_string(minutes) + " min";
    return std::to_string(minutes / 60) + " h " + std::to_string(minutes % 60);
}

// Thin-space grouping: 1234567 -> "1 234 567". Scores are the one figure a
// player compares to someone else's, and ungrouped digits make that slow.
// Un chrono se range en trois octets : minutes, secondes, centiemes. La valeur
// brute croit comme le temps, ce qui permet de la classer telle quelle, mais
// elle ne se lit pas : 917504 est en realite 14'00"00.
static std::string format_time(long long value) {
    const long long minutes = (value >> 16) & 0xFF;
    const long long seconds = (value >> 8) & 0xFF;
    const long long hundredths = value & 0xFF;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%lld'%02lld\"%02lld",
                  minutes, seconds, hundredths);
    return buffer;
}

// Au golf, le resultat est un ecart au par : moins trois se lit "-3", zero se
// lit "EVEN", plus un se lit "+1". Le signe fait tout le sens de la valeur.
static std::string format_par(long long value) {
    if (value == 0) return "EVEN";
    return (value > 0 ? "+" : "-") + std::to_string(value < 0 ? -value : value);
}

/* Groupement des milliers.
 *
 * La version precedente inserait U+202F, une espace fine insecable que
 * beaucoup de polices ne dessinent pas : le separateur existait dans la
 * chaine mais restait INVISIBLE a l'ecran, et « 790119 » s'affichait colle
 * alors que le code croyait l'avoir espace.
 *
 * On demande donc son separateur a la locale, virgule en anglais, espace en
 * francais, et on retombe sur une espace insecable ordinaire si la locale
 * du systeme n'est pas installee, cas frequent dans un conteneur.
 */
/* « Aujourd'hui », « Hier », sinon la date.
 *
 * Une date brute oblige a la comparer mentalement au jour courant. Les deux
 * cas frequents portent leur nom, le reste garde sa date, qui est la seule
 * reponse honnete au-dela de la veille. */
static std::string relative_day(const std::string& iso8601) {
    if (iso8601.empty()) return "";
    std::tm tm{};
    if (!strptime(iso8601.c_str(), "%Y-%m-%d", &tm)) return iso8601.substr(0, 10);
    tm.tm_hour = 12;                       // midi : a l'abri des fuseaux
    const std::time_t then = std::mktime(&tm);
    const std::time_t now  = std::time(nullptr);
    const double days = std::difftime(now, then) / 86400.0;
    if (days < 1.0) return _("Today");
    if (days < 2.0) return _("Yesterday");
    return iso8601.substr(0, 10);
}

static std::string format_score(long long value) {
    try {
        std::ostringstream os;
        os.imbue(std::locale(""));
        os << std::fixed << value;
        std::string out = os.str();
        // Une locale sans groupement rend la chaine inchangee : on repasse
        // alors par le repli plutot que d'afficher un nombre nu.
        if (out.size() != std::to_string(value).size()) return out;
    } catch (...) {
        // locale absente : repli ci-dessous
    }
    std::string digits = std::to_string(value < 0 ? -value : value);
    std::string out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += "\u00a0";
        out += digits[i];
    }
    return (value < 0 ? "-" : "") + out;
}

static std::string format_by_metric(long long value, const std::string& metric) {
    if (metric == "time") return format_time(value);
    if (metric == "par")  return format_par(value);
    return format_score(value);
}

MainWindow::MainWindow(std::shared_ptr<DatabaseManager> database,
                       std::function<void(double, const std::string&)> progress_callback,
                       std::vector<Game> preloaded_games) {
    // Widgets carry English literals in the header as a fallback; the
    // translated text can only be applied once the catalogue is loaded.
    m_button_scan.set_label(_("ROM Manager"));
    m_download_cancel_button.set_label(_("Cancel"));

    std::cout << "[DEBUG] MainWindow constructor started" << std::endl;

    if (progress_callback) progress_callback(0.75, "Setting up interface...");
    set_title("Bootcade");
    /* Grande par defaut.
     *
     * 1400 x 800 laissait une fenetre modeste au milieu d'un ecran 1440p,
     * alors que Bootcade affiche trois colonnes et un tableau de 29 000
     * lignes : c'est une application de bureau, pas un utilitaire. La taille
     * reelle est reprise des preferences juste apres, quand elles existent.
     */
    set_default_size(1760, 1000);
    set_border_width(8);
    try { set_icon(Gdk::Pixbuf::create_from_file(AppContext::get_asset_path("logo.svg"), 64, 64)); } catch (...) {}

    // === Modern theme ===
    // Providers are created and applied by apply_theme(); the actual mode is set
    // once settings are loaded (see below). Default to dark until then.
    apply_theme("dark");
    // Tag widgets so the stylesheet can target them.
    m_toolbar_play.get_style_context()->add_class("accent-button");
    m_button_play.get_style_context()->add_class("accent-button");
    m_search_entry.get_style_context()->add_class("search-entry");
    m_toolbar_container.get_style_context()->add_class("app-toolbar");
    m_scrolled_filters.get_style_context()->add_class("sidebar");
    m_status_box.get_style_context()->add_class("statusbar");

    // === Database ===
    // Reuse the connection opened in main() : opening a second sqlite3 handle on
    // the same file caused write contention and double-init noise in the log.
    m_database = database;
    // La carte de profil des reglages compte les favoris et les jeux joues :
    // elle emprunte la base, elle ne la possede pas.
    m_settings_panel.set_database(database);
    if (!m_database) {
        std::cerr << "[ERROR] No database handle passed to MainWindow" << std::endl;
        m_status_label.set_text(_("Error: Failed to initialize database"));
        m_status_label.show();
    }

    // La session du compte est restauree AVANT le reste : le nom du joueur en
    // depend, et le lire trop tard afficherait « non connecte » une seconde
    // avant de se corriger. Le rafraichissement part sur son propre fil : il
    // touche le reseau, et bloquer le demarrage dessus rendrait le launcher
    // injoignable quand le serveur d'identite est lent.
    m_online_state_changed.connect([this] { apply_online_state(); });
    /* Les signaux des classements sont branches ICI, avant que le fil de
     * restauration ne parte, et non a la fin du constructeur ou ils etaient.
     *
     * L'ecran de demarrage fait tourner la boucle GTK pendant toute la
     * construction (plusieurs secondes pour 29 000 jeux). La session, elle,
     * est restauree en une seconde : le rafraichissement qu'elle declenche
     * vidait la file des scores et emettait << 1 score publie >> alors que
     * personne n'ecoutait encore. Le score partait bien, mais le joueur ne
     * voyait rien, et rien ne le lui redisait ensuite. Un Glib::Dispatcher
     * emis sans slot n'est pas retarde : il est perdu. */
    m_hiscore_supported_dispatcher.connect(sigc::mem_fun(*this, &MainWindow::on_hiscore_supported_ready));
    m_hiscore_top_dispatcher.connect(sigc::mem_fun(*this, &MainWindow::on_hiscore_top_ready));
    m_hiscore_result_dispatcher.connect(sigc::mem_fun(*this, &MainWindow::on_hiscore_result_ready));
    m_hiscore_refresh_dispatcher.connect(sigc::mem_fun(*this, &MainWindow::on_hiscore_refresh_done));

    /* Perdre sa session sans le savoir, c'est jouer pour rien.
     *
     * Les scores sont gares et repartiront a la reconnexion, mais le joueur
     * doit l'apprendre au moment ou ca arrive, pas le decouvrir plus tard.
     * On redessine donc le compte, qui affichait encore un nom et un avatar,
     * et on le dit en clair dans le bandeau. */
    m_session_lost.connect([this] {
        refresh_account_button();
        apply_online_state();
        std::lock_guard<std::mutex> lock(m_hiscore_result_mutex);
        m_hiscore_results.push_back(
            {_("You have been signed out. Your scores are saved and will be "
               "sent as soon as you sign in again."), true});
        m_hiscore_result_dispatcher.emit();
    });
    /* Le gestionnaire vit aussi longtemps que la couche auth, c'est-a-dire
     * tout le programme : il ne fait qu'emettre, et le destructeur le retire
     * avant que cette fenetre disparaisse. */
    BootcadeAuth::set_session_lost_handler([this] { m_session_lost.emit(); });
    m_account_restored.connect([this] {
        // Les deux endroits qui montrent le compte : la barre du haut et le
        // panneau de reglages. Ne rafraichir que l'un des deux affichait deux
        // etats contradictoires en meme temps, ce qui s'est produit.
        refresh_account_button();
        m_settings_panel.refresh_account();
        /* Et la file des scores, MAINTENANT que le jeton est la.
         *
         * La synchronisation lancee par le constructeur court en parallele de
         * la restauration de session : quand elle atteignait la file avant
         * que la session soit relue, elle n'avait pas de jeton et passait son
         * tour sans un mot. Rien ne la relancait avant le minuteur de quinze
         * minutes ou un clic sur Refresh highscores : un score gare hors
         * ligne restait donc en attente alors que le joueur etait connecte
         * et regardait l'ecran. La connexion depuis le menu fait deja cet
         * appel ; la restauration au demarrage est une connexion comme une
         * autre. */
        refresh_hiscore_data_async(false);
    });
    /* Une session sur le disque n'est pas encore une session valide : le
     * serveur a le dernier mot, et il repond apres un aller-retour reseau.
     * Le drapeau se pose AVANT le fil, sur le fil graphique, sinon le bandeau
     * peut se calculer entre le demarrage du fil et sa premiere instruction.
     */
    m_account_pending =
        std::filesystem::exists(BootcadeAuth::session_path());
    m_account_settled.connect([this] {
        m_account_pending = false;
        if (m_account_question_waiting) {
            m_account_question_waiting = false;
            ask_hiscore_account_again();
        }
        // Le seul affichage que la restauration laissait derriere elle. Le
        // bouton du compte et le panneau de reglages sont deja traites par
        // m_account_restored ; le bandeau, lui, n'etait prevenu par personne
        // et gardait a l'ecran un « il te faut un compte » dementi par le
        // reste de la fenetre.
        refresh_hiscore_nudge();
    });
    std::thread([this, alive = m_alive_token] {
        /* Sonde de joignabilite, en arriere-plan et hors du chemin critique.
         *
         * Sans elle, « en ligne mais pas connecte » etait indistinguable de
         * « hors ligne » : rien ne prouvait la joignabilite tant qu'aucune
         * session ni aucune synchronisation n'avait abouti, et l'indicateur
         * annoncait Offline sur une machine parfaitement connectee.
         * Elle ne transporte aucune donnee et se lance quel que soit le
         * reglage des classements, parce que l'ETAT doit etre juste meme
         * quand la fonction est desactivee.
         */
        /* La sonde decide de NET, seule.
         *
         * Un 401 ou un 403 prouvent que le serveur repond : c'est ONLINE
         * avec un probleme d'authentification, ce n'est pas OFFLINE.
         * probe_reachable rend donc vrai des qu'une reponse HTTP arrive,
         * quel qu'en soit le code. */
        /* L'adresse est posee ICI, avant de sonder.
         *
         * set_base_url n'etait appele que par la synchronisation des
         * classements, qui ne tourne pas quand ils sont desactives et qui,
         * de toute facon, demarre apres. La sonde interrogeait donc une
         * adresse vide et repondait toujours « hors ligne » : les cinq
         * scenarios affichaient Offline, machine connectee comprise.
         */
        {
            nlohmann::json j;
            std::ifstream fi(AppContext::get_config_path());
            if (fi) { try { fi >> j; } catch (...) {} }
            const std::string url = j.value(
                "hiscore_url", std::string("https://scores.bootcade.duckdns.org"));
            HiscoreClient::set_base_url(url);
            HiscoreClient::set_store_dir(
                std::filesystem::path(AppContext::get_config_path()).parent_path().string());
        }
        m_net.store(HiscoreClient::probe_reachable() ? NetState::Online
                                                     : NetState::Offline);
        {
            std::lock_guard<std::mutex> live(alive->mutex);
            if (alive->alive) m_online_state_changed.emit();
        }

        const bool ok = BootcadeAuth::restore();
        // La session ne dit RIEN du reseau : ne pas en avoir est un cas
        // normal et durable, pas un signe de deconnexion. NET vient de la
        // sonde, et d'elle seule.
        std::lock_guard<std::mutex> live(alive->mutex);
        if (!alive->alive) return;
        if (ok) m_account_restored.emit();
        // Un echec se signale AUSSI : c'est lui qui autorise le bandeau a
        // s'afficher, et sans ce signal une session invalide resterait
        // silencieuse pour toujours.
        m_account_settled.emit();
    }).detach();

    // === Load config ===
    m_settings_panel.load_from_file(AppContext::get_config_path());
    // Called whether or not a config was found : a first launch has no file at
    // all, and that is exactly the case that needs a name generated for it.
    m_settings_panel.ensure_hiscore_identity();
    load_launch_prefs();
    // Deferred: a modal dialog raised from here would be parented to a window
    // that is not on screen yet. Idle runs it once the main loop is up, which
    // is after main.cpp has shown us.
    if (!m_settings_panel.was_hiscore_asked())
        Glib::signal_idle().connect_once(
            sigc::mem_fun(*this, &MainWindow::ask_hiscore_optin));
    // Deja repondu, mais a la question d'avant les comptes : voir
    // ask_hiscore_account_again. Le meme report, pour la meme raison.
    else if (!m_settings_panel.was_account_asked_this_version())
        Glib::signal_idle().connect_once(
            sigc::mem_fun(*this, &MainWindow::ask_hiscore_account_again));
    else
        Glib::signal_idle().connect_once(
            sigc::mem_fun(*this, &MainWindow::refresh_hiscore_nudge));
    // Same deferral, same reason. Checked after the hiscore question so the
    // two never stack on a first launch of a new version.
    if (m_database && m_database->needsDatResync())
        Glib::signal_idle().connect_once(
            sigc::mem_fun(*this, &MainWindow::ask_dat_resync));
    // The full reload above covers MAME too : only one question at a time.
    else if (m_database && m_database->needsMameDatResync())
        Glib::signal_idle().connect_once(
            sigc::mem_fun(*this, &MainWindow::ask_mame_dat_resync));

    // Apply the saved theme, and react to theme/language changes from Settings.
    apply_theme(m_settings_panel.get_theme());
    m_settings_panel.signal_theme_changed().connect([this](Glib::ustring mode) {
        apply_theme(mode);
        m_settings_panel.save_to_file(AppContext::get_config_path());
    });
    // Effet immediat : un interrupteur qui exige un redemarrage n'est pas un
    // interrupteur. Eteint, on vide la liste et on repeint pour que les
    // pastilles disparaissent tout de suite ; allume, on recharge.
    // Une connexion ou une deconnexion depuis les reglages doit se voir tout de
    // suite dans la barre : sans ca le joueur verrait deux etats contradictoires
    // a l'ecran en meme temps.
    // "Manage DATs in ROM Management" : the ROM window, on its DAT tab.
    m_settings_panel.signal_open_rom_manager().connect([this] {
        on_rom_manager();
        if (m_rom_manager) m_rom_manager->show_tab("dat");
    });

    m_settings_panel.signal_account_changed().connect([this] {
        refresh_account_button();
        refresh_hiscore_data_async(false);
    });

    m_settings_panel.signal_hiscore_toggled().connect([this](bool on) {
        if (on) {
            refresh_hiscore_data_async(true);
        } else {
            /* Eteindre coupe les ENVOIS, pas la connaissance.
             *
             * La liste des jeux classables etait videe ici : la pastille
             * Highscore, le filtre de la colonne de gauche et la colonne HS
             * disparaissaient donc avec elle. Le joueur se retrouvait devant
             * un panneau qui l'avertit que rien n'est enregistre, sans plus
             * aucun moyen de voir DE QUOI on lui parle ni quels jeux sont
             * concernes. On garde donc la liste, et seul l'envoi s'arrete.
             */
            m_hiscore_box.hide();
            on_hiscore_supported_ready();
            m_status_label.set_text(_("Online highscores turned off."));
            if (auto it = m_treeview_games.get_selection()->get_selected())
                show_game_details(*it);
        }
    });

    m_settings_panel.signal_language_changed().connect([this](Glib::ustring code) {
        on_language_selected(code);
    });

    /* Le plein ecran et la mise a l'echelle entiere ont deux commandes : le
     * menu « Launch » et la fiche de l'emulateur dans les reglages. Elles
     * doivent afficher la meme chose a tout instant, donc chacune previent
     * l'autre. Sans cela, le prochain save_launch_prefs() de la fenetre
     * principale reecrirait par-dessus le choix fait dans les reglages. */
    m_settings_panel.signal_launch_options_changed().connect([this] {
        m_launch_fullscreen   = m_settings_panel.launches_fullscreen();
        m_launch_integerscale = m_settings_panel.launches_integerscale();
        m_syncing_launch_menu = true;
        m_menu_item_fullscreen_mode.set_active(m_launch_fullscreen);
        m_menu_item_integerscale_mode.set_active(m_launch_integerscale);
        m_syncing_launch_menu = false;
    });

    // === Menu Bar ===
    // File Menu
    m_menu_file.set_label(_("File"));
    m_menu_file.set_submenu(m_submenu_file);
    m_app_menu.append(m_menu_file);
    
    m_menu_item_settings.set_label(_("Launcher Settings..."));
    m_menu_item_settings.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_settings_clicked));
    m_submenu_file.append(m_menu_item_settings);
    
    m_menu_item_export_game_list.set_label(_("Export Game List..."));
    m_menu_item_export_game_list.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_export_game_list));
    m_submenu_file.append(m_menu_item_export_game_list);

    // Rafraîchissement à la demande. Les classements se mettent à jour seuls
    // au démarrage et toutes les quinze minutes ; ceci est pour le joueur qui
    // vient de battre un ami et ne veut pas attendre le prochain cycle. Il
    // demande, donc il accepte l'attente : mais la barre d'état doit le lui
    // dire, sinon il recommence en croyant qu'il ne s'est rien passé.
    m_menu_item_refresh_hiscores.set_label(_("Refresh highscores"));
    m_menu_item_refresh_hiscores.signal_activate().connect(
        [this]() { refresh_hiscore_data_async(true); });
    m_submenu_file.append(m_menu_item_refresh_hiscores);
    
    m_submenu_file.append(*Gtk::manage(new Gtk::SeparatorMenuItem()));
    
    m_menu_item_quit.set_label(_("Quit"));
    m_menu_item_quit.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_quit));
    m_submenu_file.append(m_menu_item_quit);
    
    // Emulator Menu
    m_menu_emulator.set_label(_("Emulator"));
    m_menu_emulator.set_submenu(m_submenu_emulator);
    m_app_menu.append(m_menu_emulator);
    
    m_menu_item_fbneo_menu.set_label(_("Open FBNeo Menu"));
    m_menu_item_fbneo_menu.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_fbneo_menu));
    m_submenu_emulator.append(m_menu_item_fbneo_menu);

    m_menu_item_input_settings.set_label(_("Controller Settings..."));
    m_menu_item_input_settings.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_input_settings));
    m_submenu_emulator.append(m_menu_item_input_settings);

    m_submenu_emulator.append(*Gtk::manage(new Gtk::SeparatorMenuItem()));
    
    m_menu_item_fullscreen_mode.set_label(_("Launch in Fullscreen  (-fullscreen)"));
    m_menu_item_fullscreen_mode.signal_toggled().connect(sigc::mem_fun(*this, &MainWindow::on_fullscreen_mode));
    m_submenu_emulator.append(m_menu_item_fullscreen_mode);

    m_menu_item_integerscale_mode.set_label(_("Use Integer Scale  (-integerscale)"));
    m_menu_item_integerscale_mode.signal_toggled().connect(sigc::mem_fun(*this, &MainWindow::on_integerscale_mode));
    m_submenu_emulator.append(m_menu_item_integerscale_mode);
    
    m_submenu_emulator.append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());
    
    m_menu_item_download_latest_fbneo.set_label(_("Download Latest FBNeo Release"));
    m_menu_item_download_latest_fbneo.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_download_latest_fbneo));
    m_submenu_emulator.append(m_menu_item_download_latest_fbneo);
    
    m_menu_item_generate_dat_files.set_label(_("Generate DAT Files"));
    m_menu_item_generate_dat_files.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_generate_dat_files));
    m_submenu_emulator.append(m_menu_item_generate_dat_files);
    
    // Filter Menu
    // Le sous-menu « Filter » a ete retire : ses cinq entrees (tous les
    // jeux, arcade, console, disponibles, manquants) sont toutes doublees
    // par la colonne de gauche, qui EST le systeme de filtres. Les
    // gestionnaires (on_all_systems, on_arcade_mode...) restent en place,
    // appeles depuis la colonne.
    
    // ROMs Menu
    m_menu_roms.set_label(_("ROMs"));
    m_menu_roms.set_submenu(m_submenu_roms);
    m_app_menu.append(m_menu_roms);
    
    m_menu_item_rescan_roms.set_label(_("Rescan ROMs"));
    m_menu_item_rescan_roms.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_rescan_roms));
    m_submenu_roms.append(m_menu_item_rescan_roms);
    
    m_menu_item_update_dat.set_label(_("Update DAT"));
    m_menu_item_update_dat.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_update_dat_clicked));
    m_submenu_roms.append(m_menu_item_update_dat);

    m_menu_item_find_duplicates.set_label(_("Find Duplicate ROMs..."));
    m_menu_item_find_duplicates.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_find_duplicate_roms));
    m_submenu_roms.append(m_menu_item_find_duplicates);

    m_submenu_roms.append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());

    m_menu_item_rom_manager.set_label(_("ROM Management…"));
    m_menu_item_rom_manager.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_rom_manager));
    m_submenu_roms.append(m_menu_item_rom_manager);

    // Help Menu
    m_menu_help.set_label(_("Help"));
    m_menu_help.set_submenu(m_submenu_help);
    m_app_menu.append(m_menu_help);
    
    m_menu_item_about_fbneo.set_label(_("About FinalBurn Neo"));
    m_menu_item_about_fbneo.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_about_fbneo));
    m_submenu_help.append(m_menu_item_about_fbneo);
    
    m_menu_item_about_launcher.set_label(_("About Launcher"));
    m_menu_item_about_launcher.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_about_launcher));
    m_submenu_help.append(m_menu_item_about_launcher);

    // === Toolbar ===
    m_toolbar_container.set_spacing(2);
    m_toolbar_row1.set_spacing(5);
    m_toolbar_row1.set_margin_start(5);
    m_toolbar_row1.set_margin_end(5);
    m_toolbar_row2.set_spacing(5);
    m_toolbar_row2.set_margin_start(5);
    m_toolbar_row2.set_margin_end(5);
    
    // First row: Action buttons and search
    // Load custom play icon
    std::string play_icon_path = AppContext::get_asset_path("icons/play-icon.svg");
    try {
        auto pixbuf = Gdk::Pixbuf::create_from_file(play_icon_path, 24, 24);
        auto image = Gtk::manage(new Gtk::Image(pixbuf));
        m_toolbar_play.set_image(*image);
    } catch (...) {
        // Fallback to system icon if custom icon fails
        m_toolbar_play.set_image_from_icon_name("media-playback-start", Gtk::ICON_SIZE_BUTTON);
    }
    m_toolbar_play.set_always_show_image(true);
    m_toolbar_play.set_sensitive(false); // Disabled until a game is selected
    m_toolbar_play.set_size_request(80, 32); // Force minimum size
    // Scan becomes an icon-only action on the right of the header (see header block).
    // Une base de donnees, pas une loupe : le bouton ouvre le gestionnaire de
    // ROMs, il ne lance pas une recherche. L'icone precedente annoncait
    // exactement le contraire de ce que fait le bouton.
    // Meme chargeur que les autres pictogrammes : la lecture directe du
    // fichier echouait silencieusement et le bouton se retrouvait sans icone,
    // ce qui donnait exactement l'impression que je l'avais retiree.
    m_button_scan.set_image(*SettingsUi::image("database.svg", 19));
    m_button_scan.set_always_show_image(true);
    m_button_scan.set_label(_("ROM Manager"));
    m_button_scan.set_tooltip_text(_("ROM Manager"));

    // View toggle: list <-> cover grid (segmented). Packed on the right below.
    // Le mot a cote du pictogramme, comme le mockup : « ≡ » et « ▦ » seuls
    // se devinent mal, et la barre a la place de les nommer.
    // De vraies icones du theme, pas des caracteres : « ≡ » et « ▦ » rendent
    // differemment d'une police a l'autre et n'ont pas le poids des autres
    // pictogrammes de la barre.
    /* Nos propres pictogrammes, pas ceux du theme.
     *
     * Les icones symboliques du systeme changent d'un theme a l'autre : trait
     * plus ou moins epais, taille optique differente, et rien ne garantit
     * qu'elles forment une famille. Celles-ci sont dessinees ensemble, meme
     * epaisseur de trait, meme grille, et en blanc franc.
     */
    m_btn_view_list.set_image(*SettingsUi::image("view-list.svg", 18));
    m_btn_view_grid.set_image(*SettingsUi::image("view-grid.svg", 18));
    m_btn_view_list.set_label(_("List"));
    m_btn_view_grid.set_label(_("Grid"));
    m_btn_view_list.set_always_show_image(true);
    m_btn_view_grid.set_always_show_image(true);
    m_btn_view_list.set_tooltip_text(_("List view"));
    m_btn_view_grid.set_tooltip_text(_("Grid view"));
    m_btn_view_list.set_active(true);
    m_btn_view_list.signal_toggled().connect([this] {
        if (!m_suppress_view_toggle && m_btn_view_list.get_active()) set_view_mode(false);
    });
    m_btn_view_grid.signal_toggled().connect([this] {
        if (!m_suppress_view_toggle && m_btn_view_grid.get_active()) set_view_mode(true);
    });

    m_search_entry.set_placeholder_text(_("Search game..."));
    m_search_entry.signal_changed().connect(sigc::mem_fun(*this, &MainWindow::filter_games_async));
    m_search_entry.set_size_request(430, 38);
    // Le raccourci est ANNONCE dans le champ : un raccourci qu'il faut
    // deviner n'existe pas. Le texte suit la langue, comme le reste.
    m_search_entry.set_icon_from_icon_name("edit-find-symbolic",
                                           Gtk::ENTRY_ICON_PRIMARY);
    m_search_entry.set_tooltip_text(_("Search"));

    // Le badge par-dessus le champ, cale a droite. « input-shortcut » lui
    // donne l'aspect d'une touche : encadre et attenue, il se lit comme un
    // raccourci et non comme du texte deja saisi.
    m_search_hint.set_text("Ctrl+K");
    m_search_hint.get_style_context()->add_class("kbd-hint");
    m_search_hint.set_halign(Gtk::ALIGN_END);
    m_search_hint.set_valign(Gtk::ALIGN_CENTER);
    m_search_hint.set_margin_end(10);
    m_search_overlay.add(m_search_entry);
    m_search_overlay.add_overlay(m_search_hint);
    m_search_overlay.set_overlay_pass_through(m_search_hint, true);
    m_headerbar.set_custom_title(m_search_overlay); // centered search

    // Ctrl+K place le curseur dans la recherche et selectionne ce qui s'y
    // trouve deja : reappuyer relance donc une recherche au lieu d'ajouter
    // a la precedente, ce qu'on attend d'une palette de recherche.
    add_events(Gdk::KEY_PRESS_MASK);
    signal_key_press_event().connect([this](GdkEventKey* ev) {
        if ((ev->state & GDK_CONTROL_MASK) &&
            (ev->keyval == GDK_KEY_k || ev->keyval == GDK_KEY_K)) {
            m_search_entry.grab_focus();
            m_search_entry.select_region(0, -1);
            return true;
        }
        return false;
    }, false);

    // Labels for the buttons that live in the detail dock.
    m_button_play.set_image(*SettingsUi::image("play.svg", 20));
    m_button_play.set_always_show_image(true);
    m_button_play.set_label(_("Play"));
    m_button_download_art.set_image(*SettingsUi::image("bc-image.svg", SettingsUi::kIconButton));
    m_button_download_art.set_always_show_image(true);
    m_button_download_art.set_label(_("Download Art"));
    
    // Second row: Keep empty for now - filters will be in left panel
    // m_toolbar_row2 kept for future use
    
    // Add spacer to push filters to the left
    auto spacer = Gtk::make_managed<Gtk::Label>("");
    m_toolbar_row2.pack_start(*spacer, Gtk::PACK_EXPAND_WIDGET);
    
    // Pack rows into container
    m_toolbar_container.pack_start(m_toolbar_row1, Gtk::PACK_SHRINK);
    m_toolbar_container.pack_start(m_toolbar_row2, Gtk::PACK_SHRINK);

    // === TreeView setup ===
    m_model_games = Gtk::ListStore::create(m_columns);
    m_treeview_games.set_model(m_model_games);

    m_treeview_games.append_column(" ", m_columns.m_col_icon);
    m_treeview_games.append_column_editable("★", m_columns.m_col_favorite);
    m_treeview_games.append_column("HI", m_columns.m_col_hiscore);
    m_treeview_games.append_column("Name", m_columns.m_col_name);
    m_treeview_games.append_column("Title", m_columns.m_col_title);
    m_treeview_games.append_column("Year", m_columns.m_col_year);
    m_treeview_games.append_column("Manufacturer", m_columns.m_col_manufacturer);
    m_treeview_games.append_column("System", m_columns.m_col_system);
    m_treeview_games.append_column("Type", m_columns.m_col_video_type);
    m_treeview_games.append_column("Orientation", m_columns.m_col_orientation);
    m_treeview_games.append_column("Width", m_columns.m_col_width);
    m_treeview_games.append_column("Height", m_columns.m_col_height);
    m_treeview_games.append_column("Aspect", m_columns.m_col_aspect);
    m_treeview_games.append_column("Driver", m_columns.m_col_driver_status);
    m_treeview_games.append_column("Comment", m_columns.m_col_comment);
    m_treeview_games.append_column("Clone", m_columns.m_col_cloneof);
    m_treeview_games.append_column("Source", m_columns.m_col_sourcefile);

    /* Colonne « Emulator ».
     *
     * En portee « tous », mslug existe dans les deux catalogues, en systeme
     * « Arcade » des deux cotes : sans elle, rien dans la ligne ne dit lequel
     * on s'apprete a lancer. La marque accompagne le nom lisible, elle se
     * reconnait plus vite qu'un mot.
     *
     * Rendue a la demande depuis la colonne d'identifiant deja presente dans
     * le modele : une colonne de noms lisibles en plus, c'est une chaine de
     * plus par jeu sur 57 000 entrees pour deux valeurs distinctes.
     */
    {
        auto* emu_col = Gtk::manage(new Gtk::TreeView::Column(_("Emulator")));
        auto* emu_pix = Gtk::manage(new Gtk::CellRendererPixbuf());
        auto* emu_txt = Gtk::manage(new Gtk::CellRendererText());
        emu_pix->property_xpad() = 4;
        emu_col->pack_start(*emu_pix, false);
        emu_col->pack_start(*emu_txt, true);
        emu_col->set_cell_data_func(*emu_pix,
            [this, emu_pix](Gtk::CellRenderer*, const Gtk::TreeModel::iterator& it) {
                if (!it) return;
                emu_pix->property_pixbuf() = emulator_badge(
                    Glib::ustring((*it)[m_columns.m_col_emulator]).raw(), 34, 16);
            });
        emu_col->set_cell_data_func(*emu_txt,
            [this, emu_txt](Gtk::CellRenderer*, const Gtk::TreeModel::iterator& it) {
                if (!it) return;
                emu_txt->property_text() = EmulatorRegistry::display_name(
                    Glib::ustring((*it)[m_columns.m_col_emulator]).raw());
            });
        m_treeview_games.insert_column(*emu_col, 5);   // juste apres Title
        m_emulator_column = emu_col;
    }

    // Configure column properties for better user experience
    configure_columns();

    // Connect favourite toggle: clicking the ★ checkbox toggles favourite in DB
    {
        auto* fav_col = m_treeview_games.get_column(1); // col index 1 = ★
        if (fav_col) {
            auto* cell = dynamic_cast<Gtk::CellRendererToggle*>(fav_col->get_first_cell());
            if (cell) {
                cell->signal_toggled().connect([this](const Glib::ustring& path_str) {
                    auto iter = m_model_games->get_iter(path_str);
                    if (!iter) return;
                    Gtk::TreeModel::Row row = *iter;
                    std::string name   = Glib::ustring(row[m_columns.m_col_name]).raw();
                    std::string system = Glib::ustring(row[m_columns.m_col_system]).raw();
                    std::string emu    = Glib::ustring(row[m_columns.m_col_emulator]).raw();
                    if (emu.empty()) emu = "fbneo";
                    m_database->toggleFavorite(name, system, emu);
                    bool now_fav = m_database->isFavorite(name, system, emu);
                    row[m_columns.m_col_favorite] = now_fav;
                    // Update cached game too
                    for (auto& g : m_cached_games)
                        if (g.name == name && g.system == system && g.emulator == emu) { g.is_favorite = now_fav; break; }
                });
            }
        }
    }

    m_treeview_games.get_selection()->signal_changed().connect(sigc::mem_fun(*this, &MainWindow::on_game_selected));
    m_scrolled_games.add(m_treeview_games);
    m_scrolled_games.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);

    // === Details ===
    m_label_title.set_markup("<b>" + Glib::Markup::escape_text(_("Select a game to play")) + "</b>");
    m_button_play.set_sensitive(false);
    m_button_play.set_size_request(120, 32); // Force minimum size
    m_button_download_art.set_sensitive(false);
    m_button_download_art.set_size_request(140, 32); // Slightly wider for "Download Art"

    // Detail dock. Two artworks (Title on top, Preview below) sit in an image
    // column; the info column carries the big title, the full metadata block and
    // the action buttons. m_details_box reflows between horizontal (bottom dock)
    // and vertical (right dock) : see set_dock_position().
    m_details_box.get_style_context()->add_class("detail-dock");
    // Le volet respire : le mockup laisse une vraie marge autour de son
    // contenu, sans quoi les cartes touchent le bord et paraissent serrees.
    m_details_box.set_margin_start(18);
    m_details_box.set_margin_end(18);
    m_details_box.set_margin_top(16);
    m_details_box.set_margin_bottom(18);

    // Group A : the two artworks (Title + Preview). Orientation flips per dock.
    m_title_image.get_style_context()->add_class("dock-thumb");
    m_preview_image.get_style_context()->add_class("dock-thumb");
    // Centre : cale a gauche dans un volet large, l'artwork laissait un vide
    // a droite et donnait un panneau bancal. Les deux images partagent le meme
    // cadre pour qu'elles s'alignent au lieu d'avoir chacune sa largeur.
    m_detail_image_wrap.set_valign(Gtk::ALIGN_START);
    m_title_image.get_style_context()->add_class("dock-art");
    m_preview_image.get_style_context()->add_class("dock-art");
    /* Seul le title reste en haut.
     *
     * Le preview descend a cote de la fiche technique, comme le mockup :
     * empiles tous les deux en tete, ils repoussaient le classement et la
     * carte « ta meilleure place » sous la ligne de flottaison, alors que
     * ce sont eux qu'on vient voir. En bas, le preview illustre la fiche au
     * lieu de disputer la place a ce qui compte.
     */
    m_detail_image_wrap.pack_start(m_title_image, Gtk::PACK_SHRINK);
    m_detail_image_wrap.set_hexpand(false);
    m_details_box.pack_start(m_detail_image_wrap, Gtk::PACK_SHRINK);

    // Group B : big title + full metadata block.
    m_label_title.set_xalign(0.5f);
    m_label_title.set_justify(Gtk::JUSTIFY_CENTER);
    m_label_title.set_line_wrap(true);
    m_label_title.get_style_context()->add_class("dock-title");
    // Editeur, annee, systeme sur une ligne sous le titre : les trois choses
    // qu'on veut savoir avant tout le reste, sans avoir a lire le tableau.
    m_label_meta.set_xalign(0.5f);
    m_label_meta.set_justify(Gtk::JUSTIFY_CENTER);
    m_label_meta.get_style_context()->add_class("dock-meta");
    m_label_info.set_xalign(0.0f);
    m_label_info.set_line_wrap(true);
    m_label_info.set_valign(Gtk::ALIGN_START);
    m_label_info.get_style_context()->add_class("dock-sub");

    // Deux colonnes : intitule en retrait, valeur en avant. C'est ce qui fait
    // qu'on trouve une information d'un coup d'oeil au lieu de lire une liste.
    m_specs_grid.set_row_spacing(5);
    m_specs_grid.set_column_spacing(18);
    m_specs_grid.set_hexpand(true);

    m_detail_text_col.set_valign(Gtk::ALIGN_START);
    // Une colonne de largeur fixe, centree. Sans borne, le volet s'etirait a
    // la largeur de la fenetre : les cartes traversaient l'ecran, le titre
    // flottait au milieu d'un vide et les pastilles restaient collees a
    // gauche. Ce n'est pas un centrage qui manquait, c'est une colonne.
    m_detail_text_col.pack_start(m_label_title, Gtk::PACK_SHRINK);
    m_detail_text_col.pack_start(m_label_meta, Gtk::PACK_SHRINK);
    // Les pastilles d'etat remontent sous le titre : elles decrivent le jeu,
    // leur place n'est pas en bas a cote des boutons d'action.
    m_detail_text_col.pack_start(m_dock_pills, Gtk::PACK_SHRINK);
    m_detail_text_col.pack_start(m_label_info, Gtk::PACK_SHRINK);
    // Les caracteristiques dans une carte, comme le tableau des scores : deux
    // blocs identifiables valent mieux qu'un ruissellement de lignes.
    m_specs_grid.get_style_context()->add_class("spec-card");
    /* Deux blocs, deux titres, la meme presentation.
     *
     * La fiche technique portait un cadre et aucun titre, l'activite un
     * titre et aucun cadre : les deux se ressemblaient assez pour se
     * confondre et differaient assez pour paraitre bacles. Ils recoivent
     * desormais le meme traitement, seul leur intitule les distingue.
     */
    m_specs_title.set_text(_("Game information"));
    m_specs_title.set_xalign(0.0f);
    m_specs_title.get_style_context()->add_class("dock-section");

    m_activity_title.set_text(_("Your activity"));
    m_activity_title.set_xalign(0.0f);
    m_activity_title.get_style_context()->add_class("dock-section");
    m_activity_grid.set_column_spacing(18);
    m_activity_grid.set_row_spacing(5);
    m_activity_grid.get_style_context()->add_class("spec-card");
    // Avant la fiche technique : ce que le joueur a fait l'interesse plus
    // que la resolution du jeu.
    /* Ordre du panneau, et il compte.
     *
     * Ce qui concerne le joueur vient d'abord : sa place, puis le
     * classement. La fiche technique ferme la marche, parce qu'on la
     * consulte rarement et jamais dans l'urgence. La resolution et le
     * driver etaient au-dessus du classement, ce qui obligeait a faire
     * defiler pour savoir ou l'on se situe.
     *
     * m_hiscore_box est empile ici, avant l'activite et la fiche, alors que
     * sa construction se poursuit plus bas : pack_start fixe la position,
     * pas le moment ou l'on remplit.
     */
    m_detail_text_col.pack_start(m_hiscore_box,  Gtk::PACK_SHRINK);
    build_dock_section(m_activity_exp, m_activity_sum, _("Your activity"), m_activity_grid);
    build_dock_section(m_specs_exp,    m_specs_sum,    _("Game information"), m_specs_row);
    m_detail_text_col.pack_start(m_activity_exp, Gtk::PACK_SHRINK);
    m_detail_text_col.pack_start(m_specs_exp,    Gtk::PACK_SHRINK);
    // Fiche a gauche, capture a droite : la seconde donne une idee du jeu
    // que dix lignes de caracteristiques ne donnent pas.
    m_specs_row.pack_start(m_specs_grid,     Gtk::PACK_EXPAND_WIDGET);
    m_preview_image.set_valign(Gtk::ALIGN_START);
    m_specs_row.pack_start(m_preview_image,  Gtk::PACK_SHRINK);
    // Calage en haut : sans lui, la grille occupait toute la hauteur
    // disponible et « Game information » devenait absurde.
    m_specs_row.set_valign(Gtk::ALIGN_START);
    m_specs_grid.set_valign(Gtk::ALIGN_START);
    m_activity_box.set_valign(Gtk::ALIGN_START);
    m_detail_text_col.set_valign(Gtk::ALIGN_START);

    m_hiscore_title.set_xalign(0.0f);
    m_hiscore_title.get_style_context()->add_class("hi-heading");
    m_hiscore_note.set_xalign(0.0f);
    m_hiscore_note.get_style_context()->add_class("hi-note");
    m_btn_hiscore_refresh.set_tooltip_text(
        _("Fetch the latest scores now, without waiting for the next automatic refresh."));
    m_btn_hiscore_refresh.get_style_context()->add_class("hi-refresh");
    m_btn_hiscore_refresh.set_relief(Gtk::RELIEF_NONE);
    m_btn_hiscore_refresh.set_image_from_icon_name("view-refresh-symbolic", Gtk::ICON_SIZE_BUTTON);
    m_btn_hiscore_refresh.signal_clicked().connect(
        [this]() { refresh_hiscore_data_async(true); });
    m_hiscore_head.pack_start(m_hiscore_title, Gtk::PACK_SHRINK);
    m_hiscore_head.pack_end(m_btn_hiscore_refresh, Gtk::PACK_SHRINK);

    m_hiscore_grid.set_row_spacing(1);
    m_hiscore_grid.set_column_spacing(12);
    m_hiscore_grid.get_style_context()->add_class("hi-board");
    m_hiscore_grid.set_hexpand(true);

    m_label_hiscore.set_xalign(0.0f);
    m_label_hiscore.get_style_context()->add_class("hi-note");

    /* Carte « ta meilleure place », au-dessus de la table. */
    /* La carte, batie comme le mockup : un titre, puis une ligne portant la
     * pastille de rang, le score, le nom, et le lien a droite.
     *
     * Le rang est une PASTILLE et non un texte : c'est le seul element que
     * l'oeil doit trouver sans lire, et un « #2 » noye dans une ligne de
     * chiffres se confond avec le score qui le suit.
     */
    m_best_title.set_text(_("Your best"));
    m_best_title.set_xalign(0.0f);
    m_best_title.get_style_context()->add_class("best-title");

    m_best_rank.get_style_context()->add_class("best-rank");
    m_best_rank.set_valign(Gtk::ALIGN_CENTER);
    m_best_score.get_style_context()->add_class("best-score");
    m_best_score.set_valign(Gtk::ALIGN_CENTER);
    m_best_who.set_xalign(0.0f);
    m_best_who.get_style_context()->add_class("best-who");
    m_best_hint.set_xalign(0.0f);
    m_best_hint.get_style_context()->add_class("hi-note");

    auto* best_txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
    best_txt->pack_start(m_best_score, Gtk::PACK_SHRINK);
    best_txt->pack_start(m_best_who,   Gtk::PACK_SHRINK);
    best_txt->set_valign(Gtk::ALIGN_CENTER);

    // Un LinkButton porte par defaut un soulignement et la couleur des liens
    // de navigateur : dans une application, ca fait corps etranger.
    m_best_link.set_label(_("View your scores"));
    m_best_link.set_uri("https://bootcade.netlify.app/profile/?sso=1");
    m_best_link.set_relief(Gtk::RELIEF_NONE);
    m_best_link.get_style_context()->add_class("dock-link");
    m_best_link.set_valign(Gtk::ALIGN_CENTER);
    m_board_link.set_label(_("View all"));
    m_board_link.set_uri("https://bootcade.netlify.app/leaderboard/?sso=1");
    m_board_link.set_relief(Gtk::RELIEF_NONE);
    m_board_link.get_style_context()->add_class("dock-link");

    m_best_row.pack_start(m_best_rank, Gtk::PACK_SHRINK);
    m_best_row.pack_start(*best_txt,   Gtk::PACK_SHRINK);
    m_best_row.pack_start(m_best_hint, Gtk::PACK_EXPAND_WIDGET);
    m_best_row.pack_end(m_best_link,   Gtk::PACK_SHRINK);
    // Le titre est DANS la carte, pas au-dessus : pose dehors il flottait
    // entre le classement precedent et le cadre, sans appartenir a l'un ni
    // a l'autre.
    m_best_box.get_style_context()->add_class("spec-card");
    m_best_box.pack_start(m_best_title, Gtk::PACK_SHRINK);
    m_best_box.pack_start(m_best_row,   Gtk::PACK_SHRINK);
    m_best_box.set_margin_bottom(10);
    m_best_box.set_valign(Gtk::ALIGN_START);
    m_best_box.set_no_show_all(true);

    /* L'encart d'alerte vient AVANT la carte « ta meilleure place » : ce
     * qu'il annonce conditionne tout ce qui suit, et lu apres un classement
     * il n'aurait servi qu'a expliquer apres coup. */
    /* Un vrai panneau d'avertissement : pictogramme, mot ATTENTION, phrase,
     * et le bouton qui resout. Une simple phrase teintee se lisait comme une
     * note de bas de page et se traversait sans la voir ; ce qui empeche un
     * score d'exister merite d'arreter l'oeil. */
    m_hs_warn_title.set_xalign(0.0f);
    m_hs_warn_title.get_style_context()->add_class("warn-title");
    m_hs_warn_label.set_xalign(0.0f);
    m_hs_warn_label.set_line_wrap(true);
    m_hs_warn_label.set_max_width_chars(34);
    m_hs_warn_btn.set_halign(Gtk::ALIGN_START);
    m_hs_warn_btn.get_style_context()->add_class("warn-btn");
    m_hs_warn_btn.signal_clicked().connect([this] {
        if (m_hs_warn_signin) {
            LoginDialog dlg(*this);
            dlg.run();
            refresh_account_button();
        } else {
            // Le joueur vient de dire oui en cliquant : on ne lui repose pas
            // la question, on allume. S'il n'a pas de compte, le panneau
            // basculera de lui-meme sur « connecte-toi », ce qui est la
            // suite honnete et non une deuxieme demande.
            m_settings_panel.record_hiscore_answer(true);
            m_settings_panel.save_to_file(AppContext::get_config_path());
        }
        refresh_hiscore_data_async(false);
        refresh_hiscore_nudge();
        // Redessiner la fiche affichee : le panneau qu'on vient de traiter en
        // fait partie, et il doit disparaitre sans attendre un autre clic.
        if (auto it = m_treeview_games.get_selection()->get_selected())
            show_game_details(*it);
    });

    auto* warn_txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 4);
    warn_txt->pack_start(m_hs_warn_title, Gtk::PACK_SHRINK);
    warn_txt->pack_start(m_hs_warn_label, Gtk::PACK_SHRINK);
    warn_txt->pack_start(m_hs_warn_btn,   Gtk::PACK_SHRINK);

    auto* warn_icon = Gtk::make_managed<SettingsUi::Icon>("bc-warning.svg", 26);
    warn_icon->set_valign(Gtk::ALIGN_START);
    m_hs_warn.get_style_context()->add_class("warn-card");
    m_hs_warn.pack_start(*warn_icon, Gtk::PACK_SHRINK);
    m_hs_warn.pack_start(*warn_txt, Gtk::PACK_EXPAND_WIDGET);
    m_hs_warn.set_margin_bottom(10);
    /* Les enfants sont montres ICI, une fois pour toutes.
     *
     * set_no_show_all(true) empeche le panneau de paraitre avec la zone qui
     * le porte : c'est voulu, il ne doit paraitre que quand il a quelque
     * chose a dire. Mais gtk_widget_show_all() SORT IMMEDIATEMENT sur un
     * widget qui porte ce drapeau : l'appeler plus tard ne montrait donc ni
     * le panneau ni son contenu. Le conteneur se pilote au show()/hide(),
     * ses enfants sont visibles depuis le depart. */
    warn_icon->show();
    warn_txt->show_all();
    m_hs_warn.set_no_show_all(true);
    m_hiscore_box.pack_start(m_hs_warn, Gtk::PACK_SHRINK);

    m_hiscore_box.pack_start(m_best_box, Gtk::PACK_SHRINK);

    m_hiscore_head.pack_end(m_board_link, Gtk::PACK_SHRINK);
    m_hiscore_box.pack_start(m_hiscore_head, Gtk::PACK_SHRINK);
    m_hiscore_box.pack_start(m_hiscore_grid, Gtk::PACK_SHRINK);
    m_hiscore_box.pack_start(m_label_hiscore, Gtk::PACK_SHRINK);
    m_hiscore_box.set_margin_top(14);
    m_hiscore_box.set_no_show_all(true);
    // PACK_SHRINK et non EXPAND : la colonne prend la hauteur de son
    // contenu. En EXPAND, chaque carte grandissait avec la fenetre.
    m_details_box.pack_start(m_detail_text_col, Gtk::PACK_SHRINK);

    // Group C : status pills above the action buttons.
    /* Etoile et « ... » : de vraies icones, blanches et grandes.
     *
     * C'etaient des caracteres « ★ » et « ⋯ » rendus dans la couleur de
     * texte du theme : minuscules, gris, et perdus a cote d'un bouton Play
     * plein. Ils ont maintenant le meme poids visuel que lui.
     */
    /* L'etoile est PLEINE et BLANCHE, toujours.
     *
     * En contour, elle se lisait comme un bouton inactif a cote d'un Play
     * plein : c'est le meme malentendu que le gris precedent. L'etat favori
     * se dit par la COULEUR, or plutot que blanc, pas par le remplissage. */
    m_button_favorite.set_image(*SettingsUi::image("star.svg", 24));
    m_button_favorite.set_always_show_image(true);
    m_button_favorite.set_label("");
    m_button_favorite.set_tooltip_text(_("Toggle favorite"));
    m_button_favorite.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_dock_favorite_clicked));
    // « Download Art » quitte la barre d'actions pour le menu « ⋯ ». Le
    // bouton lui-meme subsiste : c'est lui qui porte le gestionnaire et son
    // etat d'activation, l'entree de menu ne fait que le declencher.
    m_mi_download_art.set_label(_("Download artwork"));
    m_mi_download_art.signal_activate().connect([this] {
        if (m_button_download_art.get_sensitive()) m_button_download_art.clicked();
    });
    m_mi_game_page.set_label(_("Open game page"));
    m_mi_game_page.signal_activate().connect([this] {
        // Vers le classement tant que la page par jeu du site n'existe pas :
        // un lien qui mene a une page absente est pire que pas de lien.
        open_web("/leaderboard/");
    });
    /* « Reset game settings » : ecarter le <rom>.ini que FBNeo ecrit par jeu.
     *
     * Ce fichier fige les entrees, les DIP et la video du jeu au premier
     * lancement, et il n'y a AUCUN moyen de le voir depuis le lanceur. Quand
     * il est faux (une manette enumeree dans un autre ordre, et le joueur 1
     * lie a un pad qui n'existe plus), le jeu ne repond plus a rien et rien
     * ne dit pourquoi. On n'efface pas : on renomme en .bak, et FBNeo repart
     * des defauts (p1defaults.ini) au prochain lancement. La SRAM et les
     * scores (.fs, .hi) ne sont pas concernes. */
    m_mi_reset_settings.set_label(_("Reset game settings"));
    m_mi_reset_settings.signal_activate().connect(
        sigc::mem_fun(*this, &MainWindow::on_reset_game_settings));
    // « Game controls » : le meme ecran que Emulator > Controller Settings,
    // mais dont Save ne vaut que pour ce jeu. Voir ControllerDialog::set_game_scope.
    m_mi_game_controls.set_label(_("Game controls…"));
    m_mi_game_controls.signal_activate().connect(
        sigc::mem_fun(*this, &MainWindow::on_game_controls));
    m_detail_menu.append(m_mi_download_art);
    m_detail_menu.append(m_mi_game_page);
    m_detail_menu.append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());
    m_detail_menu.append(m_mi_game_controls);
    m_detail_menu.append(m_mi_reset_settings);
    m_detail_menu.show_all();
    m_btn_detail_more.set_image(*SettingsUi::image("more.svg", 24));
    m_btn_detail_more.set_tooltip_text(_("More actions"));
    m_btn_detail_more.set_popup(m_detail_menu);

    // Play domine, les deux autres sont carres et discrets : c'est le
    // rapport de taille du mockup, et il dit lequel des trois on vient
    // chercher.
    /* Play scinde : l'action principale a gauche, ses variantes derriere le
     * chevron. Elles existaient deja dans le menu Emulator, ou personne ne
     * va les chercher au moment de lancer une partie. */
    m_mi_play_fullscreen.set_label(_("Play fullscreen"));
    m_mi_play_fullscreen.signal_activate().connect([this] {
        m_menu_item_fullscreen_mode.set_active(true);
        m_button_play.clicked();
    });
    m_mi_play_integer.set_label(_("Play with integer scale"));
    m_mi_play_integer.signal_activate().connect([this] {
        m_menu_item_integerscale_mode.set_active(true);
        m_button_play.clicked();
    });
    m_mi_play_fbneo.set_label(_("Open FBNeo menu"));
    m_mi_play_fbneo.signal_activate().connect([this] {
        m_menu_item_fbneo_menu.activate();
    });
    m_play_menu.append(m_mi_play_fullscreen);
    m_play_menu.append(m_mi_play_integer);
    m_play_menu.append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());
    m_play_menu.append(m_mi_play_fbneo);
    m_play_menu.show_all();
    m_btn_play_more.set_popup(m_play_menu);
    m_btn_play_more.set_size_request(44, 52);
    m_btn_play_more.get_style_context()->add_class("accent-button");
    m_btn_play_more.set_tooltip_text(_("Launch options"));

    m_button_play.set_size_request(250, 52);
    m_button_favorite.set_size_request(52, 52);   // carre, comme le mockup
    m_button_favorite.get_style_context()->add_class("dock-action");
    m_btn_detail_more.set_size_request(52, 52);
    m_btn_detail_more.get_style_context()->add_class("dock-action");
    // Le chevron colle a Play : ensemble ils forment UN bouton.
    m_play_split.get_style_context()->add_class("play-split");
    m_play_split.pack_start(m_button_play,  Gtk::PACK_SHRINK);
    m_play_split.pack_start(m_btn_play_more, Gtk::PACK_SHRINK);
    m_detail_actions.set_spacing(10);
    m_detail_actions_col.get_style_context()->add_class("dock-actions");
    m_detail_actions.pack_start(m_play_split, Gtk::PACK_SHRINK);
    m_detail_actions.pack_start(m_button_favorite, Gtk::PACK_SHRINK);
    m_detail_actions.pack_start(m_btn_detail_more, Gtk::PACK_SHRINK);
    // Les actions ne sont plus empilees dans le contenu : elles vivent hors
    // de la zone defilante, ancrees en bas du volet. Voir m_dock_root.
    m_detail_actions_col.set_valign(Gtk::ALIGN_END);
    m_detail_actions_col.pack_start(m_detail_actions, Gtk::PACK_SHRINK);

    // === Filter TreeView Setup ===
    m_model_filters = Gtk::TreeStore::create(m_filter_columns);
    m_treeview_filters.set_model(m_model_filters);
    
    // Create a single column with both icon and text
    auto combined_column = Gtk::manage(new Gtk::TreeView::Column("Filters"));
    
    // Add icon renderer
    auto icon_renderer = Gtk::manage(new Gtk::CellRendererPixbuf());
    combined_column->pack_start(*icon_renderer, false);
    combined_column->add_attribute(icon_renderer->property_pixbuf(), m_filter_columns.m_col_icon);
    icon_renderer->property_xpad() = 6;
    icon_renderer->property_ypad() = 7;

    // Add text renderer
    auto text_renderer = Gtk::manage(new Gtk::CellRendererText());
    combined_column->pack_start(*text_renderer, true);
    combined_column->add_attribute(text_renderer->property_text(), m_filter_columns.m_col_name);
    text_renderer->property_ypad() = 7; // taller, airier rows

    // Design-only: render group headers (roots/categories) in bold, like the
    // mockup's LIBRARY / SYSTEMS section labels. Filter logic is untouched.
    combined_column->set_cell_data_func(*text_renderer,
        [this, text_renderer](Gtk::CellRenderer*, const Gtk::TreeModel::iterator& it) {
            if (!it) return;
            const auto& row = *it;
            std::string type = Glib::ustring(row[m_filter_columns.m_col_type]).raw();
            const bool header  = (type == "root" || type == "category");
            const bool section = (type == "section");
            text_renderer->property_weight() =
                (header || section) ? Pango::WEIGHT_BOLD : Pango::WEIGHT_NORMAL;
            // Le titre de section est plus petit et attenue : il classe, il
            // ne se clique pas. Lui donner le meme poids qu'une entree
            // aurait ajoute deux fausses lignes cliquables.
            // Les titres de section etaient trop petits et trop pales pour
            // se lire ; ils restent plus discrets qu'une entree, mais
            // lisibles. Et ils prennent de la marge au-dessus, sinon les
            // trois blocs de la colonne se touchent.
            text_renderer->property_scale()     = section ? 0.86 : 1.0;
            text_renderer->property_sensitive() = !section;
            text_renderer->property_ypad()      = section ? 13 : 7;
        });

    m_treeview_filters.append_column(*combined_column);
    combined_column->set_expand(true);

    /* Les compteurs dans leur propre colonne, alignes a droite.
     *
     * Ils etaient colles au libelle, « All Games (29461) », ce qui rendait
     * la colonne illisible : les nombres n'etaient pas alignes entre eux et
     * la parenthese se lisait avant le chiffre. A droite et en chiffres
     * tabulaires, ils se comparent d'un coup d'oeil. */
    auto count_column = Gtk::manage(new Gtk::TreeView::Column(""));
    auto count_renderer = Gtk::manage(new Gtk::CellRendererText());
    count_renderer->property_xalign() = 1.0f;
    count_renderer->property_xpad() = 8;
    count_column->pack_start(*count_renderer, false);
    count_column->set_cell_data_func(*count_renderer,
        [this, count_renderer](Gtk::CellRenderer*, const Gtk::TreeModel::iterator& it) {
            if (!it) return;
            const auto& row = *it;
            const std::string type = Glib::ustring(row[m_filter_columns.m_col_type]).raw();
            const int n = row[m_filter_columns.m_col_count];
            // Une categorie ou une section n'a pas de total propre : y
            // afficher 0 ferait croire a une bibliotheque vide.
            const bool none = (type == "category" || type == "section");
            count_renderer->property_text() = none ? "" : std::to_string(n);
            count_renderer->property_sensitive() = n > 0;
        });
    m_treeview_filters.append_column(*count_column);

    // Une section n'est qu'un titre : la selectionner ne filtrerait rien et
    // laisserait la liste inchangee sans que rien ne l'explique.
    m_treeview_filters.get_selection()->set_select_function(
        [this](const Glib::RefPtr<Gtk::TreeModel>& model,
               const Gtk::TreeModel::Path& path, bool) {
            auto it = model->get_iter(path);
            if (!it) return true;
            return Glib::ustring((*it)[m_filter_columns.m_col_type]).raw() != "section";
        });
    
    // Configure filter TreeView - clean look without lines
    m_treeview_filters.set_headers_visible(false);
    m_treeview_filters.set_enable_tree_lines(false);
    m_treeview_filters.set_show_expanders(true);
    // Les chevrons flottaient au-dessus de la ligne : un peu d'indentation
    // les cale sur l'icone et le libelle qu'ils commandent.
    m_treeview_filters.set_level_indentation(4);
    m_treeview_filters.get_selection()->signal_changed().connect(sigc::mem_fun(*this, &MainWindow::on_filter_selection_changed));
    
    m_scrolled_filters.add(m_treeview_filters);
    m_scrolled_filters.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    m_scrolled_filters.set_size_request(262, -1); // Fixed width for filter panel
    
    // === Cover-art grid view (alternative to the list) ===
    m_flowbox.set_valign(Gtk::ALIGN_START);
    m_flowbox.set_selection_mode(Gtk::SELECTION_SINGLE);
    // Single click only selects (updates the detail dock); launching is on
    // double-click / Enter (child-activated).
    m_flowbox.set_activate_on_single_click(false);
    m_flowbox.set_homogeneous(true);
    m_flowbox.set_row_spacing(14);
    m_flowbox.set_column_spacing(14);
    // Exact column count, driven by the 3/4/5 selector (see set_grid_columns).
    // Voir set_grid_columns : la grille se replie jusqu'a une colonne.
    m_flowbox.set_min_children_per_line(1);
    m_flowbox.set_max_children_per_line(30);
    m_flowbox.set_homogeneous(false);
    m_flowbox.set_margin_top(12);
    m_flowbox.set_margin_bottom(12);
    m_flowbox.set_margin_start(12);
    m_flowbox.set_margin_end(12);
    m_flowbox.get_style_context()->add_class("game-grid");
    m_flowbox.signal_selected_children_changed().connect(
        sigc::mem_fun(*this, &MainWindow::on_grid_selection_changed));
    m_flowbox.signal_child_activated().connect(
        sigc::mem_fun(*this, &MainWindow::on_grid_child_activated));
    m_scrolled_grid.add(m_flowbox);
    // POLICY_NEVER en horizontal : si la grille debordait, il faudrait
    // corriger son calcul, pas offrir une barre pour aller chercher ce
    // qui depasse.
    m_scrolled_grid.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    m_scrolled_grid.set_propagate_natural_width(false);
    // value_changed -> the user scrolled; changed -> content or viewport resized,
    // which also covers a first batch too short to fill the window.
    if (auto adj = m_scrolled_grid.get_vadjustment()) {
        adj->signal_value_changed().connect(sigc::mem_fun(*this, &MainWindow::maybe_extend_grid));
        adj->signal_changed().connect(sigc::mem_fun(*this, &MainWindow::maybe_extend_grid));
    }

    // Modern list view (styled ListBox of rows).
    m_mlist.set_selection_mode(Gtk::SELECTION_SINGLE);
    m_mlist.set_activate_on_single_click(false); // single click selects; dbl/Enter launches
    m_mlist.get_style_context()->add_class("mlist");
    m_mlist.signal_row_selected().connect(
        sigc::mem_fun(*this, &MainWindow::on_mlist_row_selected));
    m_mlist.signal_row_activated().connect(
        sigc::mem_fun(*this, &MainWindow::on_mlist_row_activated));
    build_mlist_header();
    m_mlist_wrap.pack_start(m_mlist_head, Gtk::PACK_SHRINK);
    m_mlist_wrap.pack_start(m_mlist,      Gtk::PACK_SHRINK);
    m_scrolled_mlist.add(m_mlist_wrap);
    m_scrolled_mlist.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    if (auto adj = m_scrolled_mlist.get_vadjustment()) {
        adj->signal_value_changed().connect(sigc::mem_fun(*this, &MainWindow::maybe_extend_mlist));
        adj->signal_changed().connect(sigc::mem_fun(*this, &MainWindow::maybe_extend_mlist));
    }

    // Stack children: the hidden TreeView ("table") backs data/selection; the
    // visible views are the modern list and the cover grid.
    m_view_stack.add(m_scrolled_games, "table");
    m_view_stack.add(m_scrolled_mlist, "list");
    m_view_stack.add(m_scrolled_grid, "grid");
    m_view_stack.set_visible_child("list");

    // === Layout: sidebar | (chips, views, detail dock) ===
    m_chips_box.set_margin_start(12);
    m_chips_box.set_margin_end(12);
    m_chips_box.set_margin_top(8);
    m_chips_box.get_style_context()->add_class("chips-bar");
    m_right_box.pack_start(m_chips_box, Gtk::PACK_SHRINK);
    // A resizable split between the game views and the detail dock. The view
    // stack takes the extra space (resize=true); the dock keeps its requested
    // size (resize=false) but can be dragged. set_dock_position() flips the
    // paned orientation to move the dock between bottom and right.
    /* Trois zones. Le contenu occupe la zone defilante, qui prend toute la
     * hauteur restante : l'espace vide y vit donc naturellement, sous les
     * cartes. La barre d'actions est empilee APRES, hors du defilement, donc
     * toujours visible. Un ascenseur ne sert plus qu'aux fenetres vraiment
     * petites, ce qu'il devrait toujours avoir ete. */
    m_details_scroll.add(m_details_box);
    m_details_scroll.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    /* La zone centrale porte son propre pied : le compteur de jeux affiches
     * a gauche, le curseur de taille a droite. Le curseur descend ici depuis
     * la barre d'etat generale, ou il flottait a cote d'un bilan de scan qui
     * ne parle pas de la meme chose. */
    m_center_count.set_xalign(0.0f);
    m_center_count.get_style_context()->add_class("center-count");
    m_center_foot.pack_start(m_center_count, Gtk::PACK_SHRINK);
    m_center_foot.pack_end(m_zoom_box,       Gtk::PACK_SHRINK);
    m_center_foot.set_margin_start(12);
    m_center_foot.set_margin_end(12);
    m_center_foot.set_margin_top(4);
    m_center_foot.set_margin_bottom(4);
    // La vue defile, le pied non : le curseur de taille reste atteignable
    // quelle que soit la taille des cartes et la position du defilement.
    m_center_box.pack_start(m_view_stack,  Gtk::PACK_EXPAND_WIDGET);
    m_center_box.pack_start(m_center_foot, Gtk::PACK_SHRINK);
    // Le volet de droite recoit plus de largeur : dans le mockup il a une
    // vraie presence, alors qu'ici la colonne centrale mangeait tout.
    /* shrink = true, et c'est le point capital.
     *
     * Avec shrink = false, la zone centrale refusait de descendre sous sa
     * largeur minimale : le Paned n'avait alors d'autre choix que de
     * repousser le volet de details hors de la fenetre. C'est la zone
     * centrale qui est flexible, elle doit donc ceder, jamais imposer.
     */
    m_content_paned.pack1(m_center_box, true, true);
    // Recalculee a chaque redimensionnement de la fenetre.
    signal_size_allocate().connect([this](Gtk::Allocation&) {
        update_dock_width();
    });
    /* Le volet de details ne se redimensionne pas avec la fenetre et ne se
     * laisse pas ecraser : sa largeur minimale est garantie plus bas par
     * set_min_content_width. Sur un ultra-large, l'espace supplementaire va
     * donc a la liste, mais jamais au prix du volet. */
    m_dock_root.pack_start(m_details_scroll,     Gtk::PACK_EXPAND_WIDGET);
    m_dock_root.pack_start(m_detail_actions_col, Gtk::PACK_SHRINK);
    m_content_paned.pack2(m_dock_root, false, false);
    m_right_box.pack_start(m_content_paned, Gtk::PACK_EXPAND_WIDGET);

    // La colonne devient une boite : l'arbre s'etire, le pied reste colle
    // en bas quelle que soit la hauteur de la fenetre.
    m_lbl_app_version.set_xalign(0.0f);
    m_lbl_app_version.get_style_context()->add_class("dim-label");
    /* « 1.0.19.dirty » est une information de build, pas une version de
     * produit : le suffixe reste dans les journaux et la boite « A propos »,
     * il ne s'affiche pas en permanence sous les yeux du joueur. */
    {
        std::string v = BOOTCADE_VERSION;
        for (const char* suffix : {".dirty", "-dirty", "+dirty"}) {
            const auto at = v.find(suffix);
            if (at != std::string::npos) { v.erase(at); break; }
        }
        m_lbl_app_version.set_text("v" + v);
    }
    m_lbl_emu_state.set_xalign(0.0f);
    m_lbl_emu_state.get_style_context()->add_class("dim-label");
    refresh_emu_state();
    m_sidebar_foot.pack_start(m_lbl_app_version, Gtk::PACK_SHRINK);
    m_sidebar_foot.pack_start(m_lbl_emu_state,   Gtk::PACK_SHRINK);
    m_sidebar_foot.set_margin_start(14);
    m_sidebar_foot.set_margin_end(10);
    m_sidebar_foot.set_margin_top(6);
    m_sidebar_foot.set_margin_bottom(12);
    build_emulator_picker();
    m_sidebar_box.pack_start(m_btn_emu_picker,   Gtk::PACK_SHRINK);
    m_sidebar_box.pack_start(m_scrolled_filters, Gtk::PACK_EXPAND_WIDGET);
    m_sidebar_box.pack_start(m_sidebar_foot,     Gtk::PACK_SHRINK);
    m_paned_main.pack1(m_sidebar_box, false, false);
    m_paned_main.pack2(m_right_box, true, true);
    // Suit la largeur demandee par la colonne : les laisser diverger faisait
    // apparaitre une bande vide entre la colonne et la liste.
    m_paned_main.set_position(262);

    // === Status Bar ===
    m_status_label.set_margin_end(10);
    m_status_label.set_halign(Gtk::ALIGN_END);
    m_status_label.set_size_request(400, -1);  // Largeur fixe pour le texte de scan

    // Configure thumbnail download progress widgets
    m_download_status_label.set_size_request(200, -1);  // Increased width for more characters
    m_download_status_label.set_ellipsize(Pango::ELLIPSIZE_END);  // Add ellipsis for long names
    m_download_status_label.set_halign(Gtk::ALIGN_START);
    
    m_download_progress_bar.set_size_request(200, 20);
    m_download_progress_bar.set_show_text(true);
    
    // Configure cancel button
    m_download_cancel_button.set_size_request(60, 25);
    
    m_download_progress_box.pack_start(m_download_status_label, Gtk::PACK_SHRINK);
    m_download_progress_box.pack_start(m_download_progress_bar, Gtk::PACK_SHRINK);
    m_download_progress_box.pack_start(m_download_cancel_button, Gtk::PACK_SHRINK);
    m_download_progress_box.set_spacing(3);  // Tighter spacing within download box
    m_download_progress_box.set_size_request(470, -1);  // Fixed total width: 200 + 200 + 60 + spacing
    m_download_progress_box.set_no_show_all(true);  // Prevent showing on parent show_all()
    m_download_progress_box.hide();  // Hidden by default
    
    // Scan progress widgets : hidden until a scan is running
    m_scan_progress_bar.set_size_request(160, 18);
    m_scan_progress_bar.set_show_text(false);
    m_scan_progress_label.set_size_request(220, -1);
    m_scan_progress_label.set_ellipsize(Pango::ELLIPSIZE_END);
    m_scan_progress_label.set_halign(Gtk::ALIGN_START);
    m_scan_details_button.set_image(*SettingsUi::image("bc-chart.svg", SettingsUi::kIconButton));
    m_scan_details_button.set_always_show_image(true);
    m_scan_details_button.set_label(_("Details"));
    m_scan_details_button.set_size_request(90, 24);
    m_scan_details_button.signal_clicked().connect([this]() {
        if (m_scan_dialog) {
            m_scan_dialog->show();
            m_scan_dialog->present();
        }
    });
    m_scan_status_dot.get_style_context()->add_class("status-dot");
    m_scan_progress_label.get_style_context()->add_class("status-text");
    m_scan_status_dot.set_valign(Gtk::ALIGN_CENTER);
    m_scan_status_box.pack_start(m_scan_status_dot,     Gtk::PACK_SHRINK);
    m_scan_status_box.pack_start(m_scan_progress_label, Gtk::PACK_SHRINK);
    m_scan_status_box.pack_start(m_scan_progress_bar,   Gtk::PACK_SHRINK);
    m_scan_status_box.pack_start(m_scan_details_button, Gtk::PACK_SHRINK);
    m_scan_status_box.set_no_show_all(true);
    m_scan_status_box.hide();

    m_stats_box.set_halign(Gtk::ALIGN_START);
    m_stats_box.set_spacing(4);
    m_summary_label.get_style_context()->add_class("dim-label");
    m_status_box.pack_start(m_stats_box,              Gtk::PACK_EXPAND_WIDGET);
    m_status_box.pack_start(m_scan_status_box,        Gtk::PACK_SHRINK);
    m_status_box.pack_start(m_status_label,           Gtk::PACK_SHRINK);
    m_status_box.pack_start(m_download_progress_box,  Gtk::PACK_SHRINK);
    m_status_box.pack_end(m_summary_label,            Gtk::PACK_SHRINK);

    // Reglages d'affichage, a droite de la barre du bas. pack_end : le
    // bouton d'ancrage est le plus a droite, le curseur juste avant.
    m_zoom_label.set_text(_("Card size"));
    m_zoom_label.get_style_context()->add_class("dim-label");
    m_zoom_scale.set_range(kMinCardWidth, kMaxCardWidth);
    m_zoom_scale.set_increments(10, 30);
    m_zoom_scale.set_digits(0);
    m_zoom_scale.set_draw_value(false);
    m_zoom_scale.set_size_request(150, -1);
    m_zoom_scale.set_tooltip_text(_("Card size"));
    /* Curseur -> nombre de colonnes, en sens INVERSE.
     *
     * La plage vaut 3 a 5 comme le nombre de colonnes, mais les deux vont
     * en sens contraire : pousser a droite doit AGRANDIR les cartes, donc
     * en afficher MOINS. La transformation est donc 8 - valeur, qui envoie
     * 3 sur 5 colonnes et 5 sur 3 colonnes.
     *
     * Une premiere version utilisait 6 - valeur, qui donne 3, 2 puis 1
     * colonne : set_grid_columns bornant a 3, les trois positions
     * demandaient toutes la meme chose et le curseur paraissait inerte.
     */
    m_zoom_scale.signal_value_changed().connect([this] {
        if (!m_suppress_zoom)
            set_grid_columns(static_cast<int>(m_zoom_scale.get_value()));
    });
    m_zoom_box.pack_start(m_zoom_label, Gtk::PACK_SHRINK);
    m_zoom_box.pack_start(m_zoom_scale, Gtk::PACK_SHRINK);
    m_status_box.pack_end(m_btn_dock_toggle, Gtk::PACK_SHRINK);

    m_status_box.set_margin_start(6);
    m_status_box.set_margin_end(6);
    m_status_box.set_spacing(5);

    // === Header bar (modern client-side titlebar) ===
    // Brand on the left, centered search, and view/scan/settings/menu/language
    // actions on the right : matching the design mockup.
    m_headerbar.set_show_close_button(true);

    auto* brand = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    Gtk::Widget* logo = nullptr;
    try {
        auto pix = Gdk::Pixbuf::create_from_file(AppContext::get_asset_path("logo.svg"), 30, 30);
        auto* img = Gtk::make_managed<Gtk::Image>(pix);
        logo = img;
    } catch (...) {
        auto* box = Gtk::make_managed<Gtk::Box>();
        box->get_style_context()->add_class("brand-logo");
        box->set_size_request(30, 30);
        logo = box;
    }
    logo->set_valign(Gtk::ALIGN_CENTER);
    auto* names = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
    names->set_valign(Gtk::ALIGN_CENTER);
    auto* nm = Gtk::make_managed<Gtk::Label>();
    nm->set_markup("<b>Bootcade</b>");
    nm->set_xalign(0.0f);
    auto* subn = Gtk::make_managed<Gtk::Label>(_("Arcade library"));
    subn->set_xalign(0.0f);
    subn->get_style_context()->add_class("brand-sub");
    names->pack_start(*nm, Gtk::PACK_SHRINK);
    names->pack_start(*subn, Gtk::PACK_SHRINK);
    brand->pack_start(*logo, Gtk::PACK_SHRINK);
    brand->pack_start(*names, Gtk::PACK_SHRINK);
    m_headerbar.pack_start(*brand);

    m_menu_button.set_image(*SettingsUi::image("menu.svg", 18));
    m_menu_button.set_tooltip_text(_("Menu"));
    m_app_menu.show_all();
    m_menu_button.set_popup(m_app_menu);

    // « emblem-system-symbolic » est la roue dentee epaisse du theme : elle
    // jure avec les autres pictogrammes de la barre, tous fins.
    m_btn_settings.set_image(*SettingsUi::image("gear.svg", 18));
    m_btn_settings.set_tooltip_text(_("Settings"));
    m_btn_settings.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_settings_clicked));

    m_btn_favorites.set_label("★");
    m_btn_favorites.set_tooltip_text(_("Show favourites only"));
    m_btn_favorites.get_style_context()->add_class("fav-toggle");
    m_btn_favorites.signal_toggled().connect([this] {
        if (!m_suppress_fav_toggle) set_favorites_only(m_btn_favorites.get_active());
    });

    auto* view_seg = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL);
    view_seg->get_style_context()->add_class("linked");
    view_seg->pack_start(m_btn_view_list);
    view_seg->pack_start(m_btn_view_grid);

    // Cards-per-row segmented control (shown only in grid view).
    m_btn_cols3.set_label("3");
    m_btn_cols4.set_label("4");
    m_btn_cols5.set_label("5");
    m_btn_cols3.set_tooltip_text(_("3 cards per row"));
    m_btn_cols4.set_tooltip_text(_("4 cards per row"));
    m_btn_cols5.set_tooltip_text(_("5 cards per row"));
    m_btn_cols3.signal_toggled().connect([this] {
        if (!m_suppress_cols_toggle && m_btn_cols3.get_active()) set_grid_columns(3);
    });
    m_btn_cols4.signal_toggled().connect([this] {
        if (!m_suppress_cols_toggle && m_btn_cols4.get_active()) set_grid_columns(4);
    });
    m_btn_cols5.signal_toggled().connect([this] {
        if (!m_suppress_cols_toggle && m_btn_cols5.get_active()) set_grid_columns(5);
    });
    // m_btn_cols3/4/5 ne sont plus empiles : le curseur de la barre du bas
    // les remplace. Les objets subsistent et restent synchronises par
    // set_grid_columns, ce qui evite de disperser le changement.
    // Visibility is driven by the view mode (set_view_mode), applied after the
    // header's show_all so the children are realised before we may hide the box.

    // Detail-dock position toggle: released = bottom, pressed = right.
    m_btn_dock_toggle.set_label("▐");
    m_btn_dock_toggle.set_tooltip_text(_("Dock details to the right"));
    m_btn_dock_toggle.signal_toggled().connect([this] {
        set_dock_position(m_btn_dock_toggle.get_active() ? "right" : "bottom");
    });

    // Packed end -> rightmost first: compte, menu, settings, scan, favourites…
    // Le compte passe en tete : c'est l'element que le joueur cherche du
    // regard, et sa place habituelle dans une application est le coin.
    // pack_end empile de la droite vers la gauche. Ordre voulu, lu de
    // gauche a droite : recherche (titre centre), vue, tri, ROM Manager,
    // reglages, menu avance, compte.
    //
    // Deux boutons quittent la barre :
    //  - « ★ favoris » est deja une entree de la colonne de gauche, au meme
    //    titre que « Filter » du menu avance : le meme filtre a deux
    //    endroits obligeait a se demander lequel fait foi ;
    //  - le bouton d'ancrage du panneau descend dans la barre du bas, avec
    //    les autres reglages d'affichage. Il n'est double nulle part, il
    //    n'est donc pas supprime mais deplace.
    build_account_button();
    m_headerbar.pack_end(m_btn_account);
    m_headerbar.pack_end(m_menu_button);
    m_headerbar.pack_end(m_btn_settings);
    m_headerbar.pack_end(m_button_scan);
    // Le de, a gauche de ROM Manager : une action de jeu, pas de gestion.
    m_btn_random.set_image(*SettingsUi::image("bc-dice.svg", 19));
    m_btn_random.set_always_show_image(true);
    m_btn_random.set_tooltip_text(_("Pick a random game"));
    m_btn_random.get_style_context()->add_class("hb-button");
    m_btn_random.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_random_game_clicked));
    m_headerbar.pack_end(m_btn_random);
    m_combo_sort.append("default",  _("By system"));
    m_combo_sort.append("name",     _("Name (A–Z)"));
    m_combo_sort.append("year",     _("Newest first"));
    m_combo_sort.append("yearAsc",  _("Oldest first"));
    m_combo_sort.append("played",   _("Recently played"));
    m_combo_sort.append("hiscore",  _("Highscore first"));
    m_combo_sort.set_active_id("default");
    m_combo_sort.set_tooltip_text(_("Sort"));
    m_combo_sort.set_size_request(150, -1);
    m_combo_sort.signal_changed().connect([this] {
        std::string id = m_combo_sort.get_active_id();
        m_sort_mode = id == "name"    ? SortMode::Name
                    : id == "year"    ? SortMode::Year
                    : id == "yearAsc" ? SortMode::YearAsc
                    : id == "played"  ? SortMode::RecentlyPlayed
                    : id == "hiscore" ? SortMode::Highscore
                                      : SortMode::Default;
        refresh_mlist_header();   // le chevron suit le tri, d'ou qu'il vienne
        filter_games();
    });
    m_headerbar.pack_end(m_combo_sort);
    m_headerbar.pack_end(*view_seg);

    // L'espacement des groupes de la barre est pose en CSS : Gtk::HeaderBar
    // n'expose pas set_spacing, contrairement a une Box.
    set_titlebar(m_headerbar);
    m_headerbar.show_all();

    // === FBNeo update banner (hidden until the startup check finds one) ===
    m_fbneo_update_infobar.set_message_type(Gtk::MESSAGE_INFO);
    m_fbneo_update_infobar.set_show_close_button(true);
    m_fbneo_update_infobar.set_no_show_all(true);
    dynamic_cast<Gtk::Container*>(m_fbneo_update_infobar.get_content_area())->add(m_fbneo_update_label);
    m_fbneo_update_label.show();
    m_fbneo_update_infobar.add_button(_("Download"), Gtk::RESPONSE_OK);
    m_fbneo_update_infobar.signal_response().connect(
        sigc::mem_fun(*this, &MainWindow::on_fbneo_update_infobar_response));
    m_fbneo_update_infobar.hide();

    // === Packing ===
    m_main_box.pack_start(m_fbneo_update_infobar, Gtk::PACK_SHRINK);

    m_app_update_infobar.set_message_type(Gtk::MESSAGE_INFO);
    m_app_update_infobar.set_show_close_button(true);
    m_app_update_infobar.set_no_show_all(true);
    dynamic_cast<Gtk::Container*>(m_app_update_infobar.get_content_area())
        ->add(m_app_update_label);
    m_app_update_label.show();
    m_app_update_infobar.add_button(_("Download"), Gtk::RESPONSE_OK);
    m_app_update_infobar.signal_response().connect([this](int id) {
        if (id == Gtk::RESPONSE_OK) {
            try {
                Gio::AppInfo::launch_default_for_uri(
                    "https://github.com/battousai90/bootcade-launcher/releases/latest");
            } catch (const Glib::Error&) { /* pas de navigateur : rien a faire */ }
        }
        m_app_update_infobar.hide();
    });
    m_app_update_infobar.hide();
    m_main_box.pack_start(m_app_update_infobar, Gtk::PACK_SHRINK);

    // Result of a score submission. An infobar rather than a dialog: the
    // player has just quit a game and is on their way somewhere else, so the
    // news must be readable without being dismissed first.
    m_hiscore_infobar.set_message_type(Gtk::MESSAGE_INFO);
    // La couleur vient de la charte, pas du message-type : voir .bc-infobar
    // dans style-common.css.
    m_hiscore_infobar.get_style_context()->add_class("bc-infobar");
    m_hiscore_infobar.set_show_close_button(true);
    m_hiscore_infobar.set_no_show_all(true);
    dynamic_cast<Gtk::Container*>(m_hiscore_infobar.get_content_area())
        ->add(m_hiscore_infobar_label);
    m_hiscore_infobar_label.show();
    // Un bandeau qui annonce un probleme sans porter le geste qui le resout
    // renvoie le joueur chercher dans les reglages. Le bouton est donc dedans,
    // et il n'apparait que quand il sert a quelque chose.
    m_hiscore_signin_button = m_hiscore_infobar.add_button(_("Sign in"),
                                                           kHiscoreSignIn);
    m_hiscore_signin_button->get_style_context()->add_class("suggested-action");
    // Le bandeau entier est en no_show_all : sans cela, show() sur le bandeau
    // ferait reapparaitre le bouton que le message precedent avait cache.
    m_hiscore_signin_button->set_no_show_all(true);
    m_hiscore_signin_button->hide();
    // Signaler : la partie n'a pas compte comme le joueur l'attendait, et
    // seul lui a vu l'ecran. Propose quand le serveur le dit, pas autrement.
    m_hiscore_report_button = m_hiscore_infobar.add_button(_("Report a problem"), kHiscoreReport);
    m_hiscore_report_button->set_no_show_all(true);
    m_hiscore_report_button->hide();
    m_hiscore_replies_button = m_hiscore_infobar.add_button(_("See the answer"), kHiscoreReplies);
    m_hiscore_replies_button->get_style_context()->add_class("suggested-action");
    m_hiscore_replies_button->set_no_show_all(true);
    m_hiscore_replies_button->hide();
    m_hiscore_infobar.signal_response().connect([this](int id) {
        if (id == kHiscoreReport) {
            // Copie : la fenetre est modale, et un autre message peut arriver
            // dans le bandeau pendant qu'elle est ouverte.
            const HiscoreNotice notice = m_hiscore_shown;
            m_hiscore_infobar.hide();
            open_report_dialog(notice);
            return;
        }
        if (id == kHiscoreReplies) {
            m_hiscore_infobar.hide();
            open_my_reports();
            return;
        }
        if (id == kHiscoreSignIn) {
            LoginDialog dlg(*this);
            dlg.run();
            refresh_account_button();
            refresh_hiscore_data_async(false);
            refresh_hiscore_nudge();
            return;
        }
        m_hiscore_infobar.hide();
        m_hiscore_nudge_shown = false;
    });
    m_hiscore_infobar.hide();
    m_main_box.pack_start(m_hiscore_infobar, Gtk::PACK_SHRINK);
    m_main_box.pack_start(m_paned_main, Gtk::PACK_EXPAND_WIDGET);
    m_main_box.pack_start(m_status_box, Gtk::PACK_SHRINK);

    add(m_main_box);

    // === Load Database ===
    if (progress_callback) progress_callback(0.8, "Setting up game list...");
    
    m_model_games->clear();
    m_cached_games.clear();

    // Use preloaded games if provided, otherwise load from database
    std::vector<Game> db_games;
    if (!preloaded_games.empty()) {
        db_games = std::move(preloaded_games);
        std::cout << "[INFO] Using preloaded games - " << db_games.size()
                  << " games available" << std::endl;
    } else {
        if (progress_callback) progress_callback(0.85, "Loading database...");
        db_games = m_database->getAllGamesLight();
        
        if (db_games.empty()) {
            std::cout << "[INFO] Database is empty - use 'Update DAT' button to load games" << std::endl;
            m_status_label.set_text(_("Database empty - use 'Update DAT' button to load games"));
            m_status_label.show();
        } else {
            std::cout << "[INFO] Database loaded - " << db_games.size() << " games available" << std::endl;
        }
    }
    
    // Keep compatibility with legacy code - load games into cache
    m_cached_games = std::move(db_games);

    {
        std::set<std::string> ids;
        for (const auto& g : m_cached_games) ids.insert(g.emulator);
        m_multi_emulator = ids.size() > 1;
    }
    refresh_emu_state();
    m_search_blobs.clear();   // the haystacks describe the old vector
    refresh_emulator_picker();
    // Et m_filtered_games pointait dedans : le laisser en place, c'est garder
    // des pointeurs vers un vecteur qui vient d'etre remplace.
    { std::lock_guard<std::mutex> lk(m_filter_mutex); m_filtered_games.clear(); }
    {
        // Les systemes connus, pour la carte « Random game » des reglages.
        std::set<std::pair<std::string, std::string>> systems;
        for (const auto& g : m_cached_games)
            if (!g.system.empty()) systems.emplace(g.emulator, g.system);
        m_settings_panel.set_random_systems({systems.begin(), systems.end()});
    }
    
    // Populate system filter and display games
    if (!m_cached_games.empty()) {
        if (progress_callback) progress_callback(0.9, "Loading filter cache...");
        
        // Load filter cache (will be empty if no DAT update has been done yet)
        load_filter_cache();
        
        if (progress_callback) progress_callback(0.92, "Populating filters...");
        
        // Removed all ComboBox filter initialization - will implement MAMEUI-style panel
        
        if (progress_callback) progress_callback(0.95, "Finalizing interface...");
        
        // Populate filter tree and defer game filtering
        Glib::signal_idle().connect_once([this]() {
            populate_filter_tree();
            filter_games();
            update_status_bar_stats();
        });
    }

    // === Signals ===
    // Les deux boutons portent deja une icone de lecture : le libelle ne
    // reprend donc pas le triangle, sous peine de l'afficher deux fois.
    m_toolbar_play.set_label(_("Play"));
    m_toolbar_play.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_play_clicked));
    m_button_play.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_play_clicked));
    m_button_download_art.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_download_art_clicked));
    m_button_scan.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_rom_manager));
    m_button_update_dat.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_update_dat_clicked));
    m_download_cancel_button.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::on_download_cancel_clicked));
    
    // Connect preview and titles download buttons from settings panel
    m_settings_panel.get_download_previews_button().signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_download_previews_clicked));
    m_settings_panel.get_download_titles_button().signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_download_titles_clicked));
    
    // Connect thumbnail download dispatchers
    m_download_progress_dispatcher.connect([this]() {
        // Snapshot under the lock : the worker keeps writing while we draw.
        std::string file; int index, total; double pct;
        {
            std::lock_guard<std::mutex> lk(m_download_progress_mutex);
            file = m_current_download_file; index = m_current_download_index;
            total = m_total_download_count; pct = m_download_percentage;
        }
        show_download_progress(file, index, total, pct);
    });
    
    m_download_finished_dispatcher.connect([this]() {
        hide_download_progress();
        std::cout << "[INFO] All artwork downloaded!" << std::endl;
        
        // Update status bar with completion message instead of popup
        m_status_label.set_text(_("Download completed successfully!"));
        
        // Hide the completion message after 5 seconds
        Glib::signal_timeout().connect([this]() {
            m_status_label.set_text("");
            return false; // Don't repeat the timeout
        }, 5000);
    });
    
    // Connect ROM scan dispatchers
    m_scan_progress_dispatcher.connect(sigc::mem_fun(*this, &MainWindow::on_scan_progress));
    m_scan_finished_dispatcher.connect(sigc::mem_fun(*this, &MainWindow::on_scan_finished));

    m_fbneo_update_dispatcher.connect(sigc::mem_fun(*this, &MainWindow::on_fbneo_update_check_result));
    m_app_update_dispatcher.connect(sigc::mem_fun(*this, &MainWindow::on_app_update_result));
    check_app_update_async();
    // Rafraîchissement de fond : sans lui, le score d'un autre joueur
    // n'apparaîtrait qu'au prochain démarrage du lanceur.
    Glib::signal_timeout().connect_seconds([this]() {
        refresh_hiscore_data_async(false);
        return true;
    }, 15 * 60);
    refresh_hiscore_data_async(false);
    m_screenshot_found_dispatcher.connect(sigc::mem_fun(*this, &MainWindow::on_screenshot_batch_found));
    
    // Removed filter population dispatcher

    // === Final setup ===
    if (progress_callback) progress_callback(0.95, "Finalizing interface...");
    show_all_children();

    // Activate the modern list view. This must run AFTER show_all_children():
    // Gtk::Stack::set_visible_child() is ignored while the target child is not yet
    // shown, so an earlier call would leave the hidden TreeView ("table") on screen
    // until the user toggled the view.
    set_view_mode(false);

    // Apply the persisted detail-dock position now that the widget tree exists.
    set_dock_position(m_dock_position);
    // Sync the cards-per-row selector to the persisted value.
    set_grid_columns(m_grid_columns);
    // Start the background cover-art loader (disk I/O + PNG decode off the UI thread).
    start_art_thread();

    // One GitHub API call, off-thread : never blocks startup, and stays silent
    // unless it actually finds something newer than what was downloaded here.
    check_fbneo_update_async();

    if (progress_callback) progress_callback(1.0, "Ready!");
    /* Selection de demarrage : UN seul appel, ici, en priorite basse.
     *
     * Les deux points d'accroche precedents ne servaient a rien : le premier
     * etait dans apply_filters(), une fonction heritee que plus rien
     * n'appelle ; le second dans apply_tree_filters(), qui n'est pas non plus
     * traversee au demarrage. Le modele est en realite rempli ICI, dans le
     * constructeur. La fonction n'etait donc jamais executee, et plusieurs
     * corrections de son contenu n'y pouvaient rien.
     *
     * Un DELAI et non une tache d'inactivite : le chargement des vignettes
     * occupe la file d'inactivite en continu, si bien qu'une tache de
     * priorite basse n'obtenait jamais son tour et ne s'executait pas du
     * tout. Constate au lancement, la trace ne sortait jamais.
     *
     * 400 ms laissent l'arbre de filtres se construire et les vues se
     * peupler : a ce moment le modele est rempli, la selection GTK existe et
     * le volet peut etre mis a jour.
     */
    // Appel DIRECT, ici, a la fin du constructeur : le modele est rempli
    // juste au-dessus et les vues viennent d'etre construites. Aucun
    // mecanisme differe, aucune temporisation.
    /* Selection de demarrage, declenchee a l'AFFICHAGE de la fenetre.
     *
     * Pas dans le constructeur : il s'execute avant app->run(), donc avant
     * que la boucle principale ne tourne. Rien de differe ne s'y execute, et
     * surtout la fenetre n'a pas encore de geometrie : un defilement demande
     * a ce moment porte sur des hauteurs nulles et ne va nulle part. Mesure
     * au lancement : la ligne etait bien selectionnee, la liste restait en
     * haut.
     *
     * signal_map_event est le premier moment ou la fenetre existe vraiment a
     * l'ecran. Le connect est a usage unique : on rend false apres le premier
     * passage pour ne pas resélectionner a chaque reaffichage.
     */
    select_startup_game();

    std::cout << "[DEBUG] MainWindow constructor completed" << std::endl;
}

MainWindow::~MainWindow() {
    // Se desabonner AVANT tout le reste : la couche auth garde le
    // gestionnaire, et un ouvrier qui echouerait a renouveler son jeton
    // pendant la fermeture emettrait sur un objet detruit.
    BootcadeAuth::set_session_lost_handler(nullptr);

    // Avant tout démontage. Le verrou garantit qu'aucun ouvrier n'est en
    // train de tester le drapeau puis d'émettre : il attend ici, voit le
    // drapeau tombé, et renonce.
    {
        std::lock_guard<std::mutex> lock(m_alive_token->mutex);
        m_alive_token->alive = false;
    }

    // Clean up scan thread
    if (m_scan_thread.joinable()) {
        m_scan_cancelled = true;
        m_scan_thread.join();
    }
    
    // Disconnect timeout connection
    if (m_search_timeout_connection.connected()) {
        m_search_timeout_connection.disconnect();
    }

    // Stop the background art loader and wait for it to finish.
    m_art_thread_stop.store(true);
    m_art_req_cv.notify_all();
    if (m_art_thread.joinable()) {
        m_art_thread.join();
    }
}

void MainWindow::on_game_selected() {
    auto iter = m_treeview_games.get_selection()->get_selected();
    if (!iter) return;
    show_game_details(*iter);
}

void MainWindow::show_game_details(const Gtk::TreeModel::Row& row) {
    std::string name = Glib::ustring(row[m_columns.m_col_name]).raw();
    std::string title = Glib::ustring(row[m_columns.m_col_title]).raw();
    std::string system = Glib::ustring(row[m_columns.m_col_system]).raw();

    // Nom ET systeme : « 1941 » existe en Arcade et sur d'autres machines, le
    // nom seul aurait pu designer le mauvais jeu au redemarrage. Pose APRES
    // la declaration de `system` : avant, ce nom designe la fonction system()
    // de la bibliotheque C, et le compilateur l'a signale.
    m_last_selected_rom    = name;
    m_last_selected_system = system;
    m_last_selected_emulator = Glib::ustring(row[m_columns.m_col_emulator]).raw();
    // « Game controls » vaut pour les deux emulateurs : FinalBurn Neo recoit
    // son .ini, MAME un fichier controleur regenere au lancement.
    m_mi_game_controls.set_sensitive(true);
    
    // Get system prefix for file lookup
    std::string system_prefix = get_fbneo_system_prefix(system);
    
    // Load both artworks (Title screen and in-game Preview) at a legible size,
    // preserving aspect ratio inside a bounding box.
    std::string filename_with_prefix = system_prefix + name;
    /* Les deux conventions de nommage, essayees dans l'ordre.
     *
     * Les dossiers de visuels sont remplis par des sources differentes : les
     * unes nomment par nom de ROM (« spf2t.png »), les autres par titre
     * complet (« Super Puzzle Fighter II Turbo (Europe 960529).png »). Ne
     * chercher que la premiere laissait la moitie d'une collection sans
     * image, sans que rien ne l'explique a l'ecran.
     *
     * Le conteneur est montre explicitement : montrer l'image ne suffit pas
     * si la boite qui la porte est restee cachee.
     */
    auto load_art = [&](Gtk::Image& img, const std::string& dir, int max_w, int max_h) {
        if (dir.empty()) { img.hide(); return; }
        const std::vector<std::string> candidates = {
            dir + "/" + filename_with_prefix + ".png",
            dir + "/" + name + ".png",
            dir + "/" + title + ".png",
        };
        for (const auto& path : candidates) {
            try {
                if (!std::filesystem::exists(path)) continue;
                auto pix = Gdk::Pixbuf::create_from_file(path, max_w, max_h, true);
                if (pix) {
                    img.set(pix);
                    img.show();
                    if (auto* parent = img.get_parent()) parent->show();
                    return;
                }
            } catch (...) {}
        }
        img.hide();
    };
    // 360 de large : la banniere occupe la colonne du volet, comme le
    // mockup, au lieu d'une vignette perdue au milieu.
    load_art(m_title_image,   m_settings_panel.get_titles_path(m_last_selected_emulator),   430, 215);
    // 200 x 150 et non 320 x 240 : la capture accompagne desormais la fiche
    // technique au lieu de trôner en tete, et a l'ancienne taille elle
    // ecrasait le tableau qu'elle est censee illustrer.
    load_art(m_preview_image, m_settings_panel.get_previews_path(m_last_selected_emulator), 240, 180);

    std::string manufacturer = Glib::ustring(row[m_columns.m_col_manufacturer]).raw();
    std::string year         = Glib::ustring(row[m_columns.m_col_year]).raw();
    std::string status       = Glib::ustring(row[m_columns.m_col_status]).raw();
    bool        fav          = row[m_columns.m_col_favorite];

    // Extra metadata straight from the DAT (already in the model).
    std::string video_type    = Glib::ustring(row[m_columns.m_col_video_type]).raw();
    std::string orientation   = Glib::ustring(row[m_columns.m_col_orientation]).raw();
    std::string width         = Glib::ustring(row[m_columns.m_col_width]).raw();
    std::string height        = Glib::ustring(row[m_columns.m_col_height]).raw();
    std::string aspect        = Glib::ustring(row[m_columns.m_col_aspect]).raw();
    std::string driver_status = Glib::ustring(row[m_columns.m_col_driver_status]).raw();
    std::string cloneof       = Glib::ustring(row[m_columns.m_col_cloneof]).raw();
    std::string comment       = Glib::ustring(row[m_columns.m_col_comment]).raw();

    m_label_title.set_text(title.empty() ? name : title);
    {
        std::vector<std::string> bits;
        if (!manufacturer.empty()) bits.push_back(manufacturer);
        if (!year.empty())         bits.push_back(year);
        if (!system.empty())       bits.push_back(system);
        std::string line;
        for (size_t i = 0; i < bits.size(); ++i)
            line += (i ? "  \u00b7  " : "") + bits[i];
        m_label_meta.set_text(line);
    }

    // Full metadata block: one "Label: value" line per known field. Empty fields
    // are skipped so the block stays tight. Genre / players / synopsis will slot
    // in here once the offline metadata import (history.dat, catver.ini…) lands.
    // Les caracteristiques vont dans la grille ; l'etiquette ne garde que le
    // commentaire du DAT, qui est une phrase et non un couple intitule-valeur.
    SettingsUi::destroy_children(m_specs_grid);
    int spec_row = 0;
    auto add_spec = [&](const std::string& label, const std::string& value) {
        if (value.empty()) return;
        auto* k = Gtk::make_managed<Gtk::Label>(label);
        k->set_xalign(0.0f);
        k->get_style_context()->add_class("spec-key");
        auto* v = Gtk::make_managed<Gtk::Label>(value);
        v->set_xalign(0.0f);
        /* Ni ellipse, ni retour a la ligne.
         *
         * L'ellipse donnait « Arc... », le retour a la ligne au caractere
         * donnait « Arc- / ade » : les deux repondent a un manque de place
         * en abimant la valeur. La valeur s'ecrit en entier sur une ligne,
         * et c'est au volet de garantir la largeur, ce que fait sa largeur
         * minimale.
         */
        v->set_line_wrap(false);
        v->set_hexpand(true);
        v->get_style_context()->add_class("spec-val");
        m_specs_grid.attach(*k, 0, spec_row, 1, 1);
        m_specs_grid.attach(*v, 1, spec_row, 1, 1);
        spec_row++;
    };
    add_spec(_("System"),       system);
    add_spec(_("Manufacturer"), manufacturer);
    add_spec(_("Year"),         year);
    add_spec(_("ROM name"),     name);
    if (!cloneof.empty()) add_spec(_("Clone of"), cloneof);
    if (!width.empty() && !height.empty())
        add_spec(_("Resolution"), width + " x " + height);
    add_spec(_("Orientation"),  orientation);
    add_spec(_("Video"),        video_type);
    add_spec(_("Aspect"),       aspect);
    add_spec(_("Driver"),       driver_status);

    m_specs_grid.show_all();
    // Resume de l'en-tete : ce qu'on veut savoir d'un coup d'oeil quand la
    // section est repliee.
    {
        std::vector<std::string> bits;
        for (const std::string& b : {system, year, manufacturer})
            if (!b.empty()) bits.push_back(b);
        std::string line;
        for (size_t i = 0; i < bits.size(); ++i)
            line += (i ? "  \u00b7  " : "") + bits[i];
        m_specs_sum.set_text(line);
    }

    // Activite personnelle, dans son propre bloc et seulement si elle existe.
    SettingsUi::destroy_children(m_activity_grid);
    // Relu avec la cle qui a servi a l'ecrire, sans quoi la fiche d'une
    // machine MAME afficherait le temps de jeu du set homonyme de FinalBurn
    // Neo : chacun a sa ligne dans player_stats.
    Game stats = m_database->getPlayerStats(
        name, system, Glib::ustring(row[m_columns.m_col_emulator]).raw());
    const bool played = stats.play_time_secs > 0 || stats.play_count > 0;
    if (played) {
        int arow = 0;
        auto add_act = [&](const std::string& label, const std::string& value) {
            if (value.empty()) return;
            auto* k = Gtk::make_managed<Gtk::Label>(label);
            k->set_xalign(0.0f);
            k->get_style_context()->add_class("spec-key");
            auto* v = Gtk::make_managed<Gtk::Label>(value);
            v->set_xalign(0.0f);
            v->get_style_context()->add_class("spec-val");
            m_activity_grid.attach(*k, 0, arow, 1, 1);
            m_activity_grid.attach(*v, 1, arow, 1, 1);
            arow++;
        };
        if (stats.play_count > 0)
            add_act(_("Played"), std::to_string(stats.play_count) + " " + _("times"));
        add_act(_("Total time"),  format_duration(stats.play_time_secs));
        add_act(_("Longest run"), format_duration(stats.longest_session_secs));
        // QUAND, pas combien de temps : « Last session 15 min » repetait la
        // duree deja donnee deux lignes plus haut et ne disait jamais si
        // c'etait hier ou l'an dernier.
        add_act(_("Last played"), relative_day(stats.last_played));
        m_activity_sum.set_text(
            std::to_string(stats.play_count) + " " + _("plays") + "  \u00b7  "
            + format_duration(stats.play_time_secs));
        m_activity_exp.show_all();
    } else {
        m_activity_exp.hide();
    }

    std::string info;
    if (!comment.empty()) info = escape_markup(comment);
    m_label_info.set_markup(info);

    /* Le bloc des classements ne parait que s'il a quelque chose a dire.
     *
     * Trois cas ou il se tait completement, plutot que d'occuper le volet
     * avec un grand cadre vide :
     *   - le joueur a DESACTIVE les classements : c'est un choix delibere,
     *     on le respecte jusque dans l'affichage ;
     *   - le jeu n'est pas classe par le service ;
     *   - rien n'est joignable ET aucun classement n'est en cache pour ce
     *     jeu, donc il n'y a litteralement rien a montrer.
     *
     * Si un classement est en cache, il s'affiche meme hors ligne : c'est
     * une donnee utile, et le bloc porte deja sa date sous la forme
     * « offline, last seen ... ».
     */
    const bool has_cached_board =
        !HiscoreClient::cached_top(system, name, nullptr).empty();
    /* La condition porte sur le CONTENU, pas sur la joignabilite.
     *
     * Une premiere version affichait le bloc des qu'on etait joignable :
     * verifie a l'ecran, on obtenait un cadre « World leaderboard » vide
     * tant que la synchronisation n'avait pas repondu, ce qui est
     * exactement le grand bloc vide a eviter. Le bloc parait donc quand il
     * a des lignes, et le rafraichissement redessine le volet quand elles
     * arrivent.
     */
    const bool hs_on = m_settings_panel.is_hiscore_enabled();
    const bool signed_in = BootcadeAuth::signed_in();
    // Coupe, la liste des jeux classables est videe : on interroge alors la
    // copie qui survit, sinon la fiche d'un jeu classe serait muette
    // exactement dans le cas ou elle a quelque chose a dire.
    // Connu dans les deux cas : la liste n'est plus videe quand on eteint.
    const bool ranked = game_ranks_online(Glib::ustring(row[m_columns.m_col_emulator]).raw(), system, name);
    const bool show_board = hs_on && ranked && has_cached_board;
    // Un jeu qui n'est pas classe ne recoit aucun avertissement : il n'y a
    // rien a rater dessus, et le dire partout ferait du bruit.
    const bool warn = ranked && (!hs_on || !signed_in);

    refresh_hiscore_dock_warning(hs_on, signed_in);
    if (warn && !show_board) {
        // Rien a montrer que l'alerte : la table et son titre resteraient
        // deux cadres vides.
        m_hiscore_box.show();
        m_hiscore_head.hide();
        m_hiscore_grid.hide();
        m_best_box.hide();
        m_label_hiscore.hide();
    } else if (show_board) {
        // Lecture du cache, sans requête. Le lot complet est chargé au
        // démarrage : une requête par jeu cliqué rendait la navigation
        // poussive, chacune pouvant caler le temps du délai de connexion.
        m_hiscore_box.show();
        m_hiscore_head.show_all();
        m_hiscore_grid.show_all();
        show_cached_board(system, name);
    } else {
        m_hiscore_box.hide();
    }

    // Pills: status / zip / CRC (matches the design mockup).
    SettingsUi::destroy_children(m_dock_pills);
    auto add_pill = [this](const std::string& text, const char* cls) {
        auto* l = Gtk::make_managed<Gtk::Label>(text);
        l->get_style_context()->add_class("pill");
        if (cls) l->get_style_context()->add_class(cls);
        m_dock_pills.pack_start(*l, Gtk::PACK_SHRINK);
    };
    if (status == "available") {
        // Le nom du zip a quitte les pastilles : il figure deja dans la
        // fiche technique, et sur la premiere ligne il prenait la place de
        // ce qui distingue vraiment ce jeu.
        add_pill("● " + _("Available"), "pill-ok");
        add_pill("\u2713 " + _("Verified (CRC)"), nullptr);
    } else if (status == "incorrect") {
        add_pill("● " + _("Incorrect"), "pill-warn");
        add_pill(_("CRC mismatch"), nullptr);
    } else {
        add_pill("● " + _("Missing"), "pill-muted");
    }
    if (game_ranks_online(Glib::ustring(row[m_columns.m_col_emulator]).raw(), system, name))
        add_pill("◆ " + _("Highscore"), "pill-hiscore");
    m_dock_pills.show_all();

    m_button_favorite.set_image(*SettingsUi::image(fav ? "star-gold.svg" : "star.svg", 24));
    m_button_favorite.set_sensitive(true);
    m_button_play.set_sensitive(true); // Details panel button
    m_button_download_art.set_sensitive(true); // Download Art button
    m_toolbar_play.set_sensitive(true); // Toolbar button
    refresh_reset_settings_item();
}

// Un jeu jamais lance n'a pas de <rom>.ini : rien a remettre a zero, et une
// entree active qui ne ferait rien laisserait croire que ca n'a pas marche.
void MainWindow::refresh_reset_settings_item() {
    auto iter = m_treeview_games.get_selection()->get_selected();
    if (!iter) { m_mi_reset_settings.set_sensitive(false); return; }
    Gtk::TreeModel::Row row = *iter;
    std::string name   = Glib::ustring(row[m_columns.m_col_name]).raw();
    std::string system = Glib::ustring(row[m_columns.m_col_system]).raw();
    // Le sf2 de MAME trouverait le sf2.ini de FinalBurn Neo : ce fichier
    // n'est pas le sien.
    const std::string emu = Glib::ustring(row[m_columns.m_col_emulator]).raw();
    // Pour MAME, ce qui peut deregler la manette est une commande redefinie
    // dans son propre menu : rien a remettre a zero s'il n'y en a pas.
    if (emu == "mame") {
        m_mi_reset_settings.set_sensitive(
            MameControls::input_overrides(mame_cfg_dir() + "/" + name + ".cfg") > 0);
        return;
    }
    if (!emu.empty() && emu != "fbneo") { m_mi_reset_settings.set_sensitive(false); return; }
    const std::string ini = ControllerManager::get_fbneo_config_dir() + "/games/"
                          + get_fbneo_system_prefix(system) + name + ".ini";
    std::error_code ec;
    m_mi_reset_settings.set_sensitive(std::filesystem::is_regular_file(ini, ec));
}

void MainWindow::on_reset_game_settings() {
    auto iter = m_treeview_games.get_selection()->get_selected();
    if (!iter) return;
    Gtk::TreeModel::Row row = *iter;
    std::string name   = Glib::ustring(row[m_columns.m_col_name]).raw();
    std::string system = Glib::ustring(row[m_columns.m_col_system]).raw();
    std::string title  = Glib::ustring(row[m_columns.m_col_title]).raw();

    /* MAME : on retire les commandes redefinies dans son menu, et elles
     * seules. Elles passent devant le fichier controleur de Bootcade, donc
     * devant le profil choisi ici. Le reste du .cfg (compteurs, reglages
     * video, avertissements deja vus) ne bouge pas ; une copie .bak reste. */
    if (Glib::ustring(row[m_columns.m_col_emulator]).raw() == "mame") {
        const std::string cfg = mame_cfg_dir() + "/" + name + ".cfg";
        ConfirmationDialog confirm(*this,
            _("Reset game settings?"),
            Glib::Markup::escape_text(title) + "\n\n" +
            _("The controls and DIP switches changed in MAME's own menu for this "
              "machine will be removed, so your Bootcade controller profile "
              "applies again.\n\n"
              "A backup is kept next to the file. Saved games and high scores "
              "are kept."),
            "bc-controller.svg");
        if (!confirm.show_and_confirm()) return;
        if (!MameControls::reset_input(cfg)) {
            SettingsUi::notice(*this, _("Could not reset the game settings."),
                               cfg, "bc-error.svg");
            return;
        }
        std::cout << "[Settings] " << name << ": MAME input settings removed from "
                  << cfg << " (backup: " << cfg << ".bak)\n";
        refresh_reset_settings_item();
        return;
    }

    const std::string rom = get_fbneo_system_prefix(system) + name;
    const std::string ini = ControllerManager::get_fbneo_config_dir() + "/games/" + rom + ".ini";
    const std::string bak = ini + ".bak";

    ConfirmationDialog confirm(*this,
        _("Reset game settings?"),
        Glib::Markup::escape_text(title) + "\n\n" +
        _("FBNeo's per-game settings for this title (controls, DIP switches, "
          "video) will be discarded and rebuilt from your defaults at the next "
          "launch.\n\n"
          "Saved games, high scores and save states are kept."),
        "bc-controller.svg");
    if (!confirm.show_and_confirm()) return;

    // Un seul niveau de secours : le .bak precedent est ecrase. Garder un
    // historique n'aiderait personne, le fichier se regenere a chaque
    // lancement.
    std::error_code ec;
    std::filesystem::rename(ini, bak, ec);
    if (ec) {
        std::cerr << "[Settings] Cannot reset " << ini << ": " << ec.message() << "\n";
        SettingsUi::notice(*this, _("Could not reset the game settings."),
                           ec.message(),
                           "bc-error.svg");
        return;
    }
    std::cout << "[Settings] " << rom << ": per-game settings moved to " << bak << "\n";
    refresh_reset_settings_item();
}

void MainWindow::on_dock_favorite_clicked() {
    auto iter = m_treeview_games.get_selection()->get_selected();
    if (!iter) return;
    Gtk::TreeModel::Row row = *iter;
    std::string name   = Glib::ustring(row[m_columns.m_col_name]).raw();
    std::string system = Glib::ustring(row[m_columns.m_col_system]).raw();
    // `mslug` existe chez les deux emulateurs : l'etoile va a celui de la ligne.
    std::string emu    = Glib::ustring(row[m_columns.m_col_emulator]).raw();
    if (emu.empty()) emu = "fbneo";
    m_database->toggleFavorite(name, system, emu);
    bool now_fav = m_database->isFavorite(name, system, emu);
    row[m_columns.m_col_favorite] = now_fav;
    m_button_favorite.set_image(*SettingsUi::image(now_fav ? "star-gold.svg" : "star.svg", 24));
    for (auto& g : m_cached_games)
        if (g.name == name && g.system == system && g.emulator == emu) { g.is_favorite = now_fav; break; }
}

void MainWindow::set_dock_position(const std::string& pos) {
    const bool right = (pos == "right");
    m_dock_position = right ? "right" : "bottom";

    // The outer split runs horizontally when the dock is on the right, vertically
    // when it is at the bottom. The dock's own A|B|C layout is the opposite so it
    // fills the available axis: stacked (fill height) on the right, in a row (fill
    // width) at the bottom. The two artworks stack on the right, sit side-by-side
    // at the bottom.
    m_content_paned.set_orientation(right ? Gtk::ORIENTATION_HORIZONTAL
                                          : Gtk::ORIENTATION_VERTICAL);
    m_details_box.set_orientation(right ? Gtk::ORIENTATION_VERTICAL
                                        : Gtk::ORIENTATION_HORIZONTAL);
    m_detail_image_wrap.set_orientation(right ? Gtk::ORIENTATION_VERTICAL
                                              : Gtk::ORIENTATION_HORIZONTAL);
    m_details_box.set_spacing(right ? 20 : 24);

    // TOUS les alignements du volet sont decides ici, et nulle part ailleurs.
    // Ils etaient auparavant poses a la construction puis reecrits par cette
    // fonction, si bien qu'une retouche visible restait sans effet : l'artwork
    // repassait a gauche pendant que le reste se centrait.
    //
    // Dock lateral : tout est empile, donc tout partage une colonne de 360 px
    // centree. Sans cette borne, le volet s'etire a la largeur de la fenetre
    // et le contenu se disperse.
    // Dock bas : les trois groupes sont cote a cote, chacun garde sa largeur
    // naturelle et s'aligne a gauche.
    const auto lead = right ? Gtk::ALIGN_CENTER : Gtk::ALIGN_START;
    m_detail_image_wrap.set_halign(lead);
    m_detail_text_col.set_halign(lead);
    m_detail_text_col.set_hexpand(false);
    m_detail_text_col.set_size_request(right ? 420 : -1, -1);
    m_dock_pills.set_halign(lead);
    m_dock_pills.set_spacing(10);
    m_dock_pills.set_margin_top(6);
    m_dock_pills.set_margin_bottom(4);
    m_specs_grid.set_halign(Gtk::ALIGN_FILL);
    m_detail_actions.set_halign(lead);
    m_detail_actions_col.set_halign(lead);
    m_label_title.set_xalign(right ? 0.5f : 0.0f);
    m_label_meta.set_xalign(right ? 0.5f : 0.0f);
    m_label_title.set_justify(right ? Gtk::JUSTIFY_CENTER : Gtk::JUSTIFY_LEFT);
    m_label_meta.set_justify(right ? Gtk::JUSTIFY_CENTER : Gtk::JUSTIFY_LEFT);

    // Give the dock a sensible floor so the paned doesn't collapse it: a column
    // on the right, a short band at the bottom.
    if (right) {
        // 460 : la largeur qu'il faut pour que « Arcade », « horizontal » et
        // « 384 x 224 » s'ecrivent en entier. Tronquer ces valeurs revenait a
        // masquer le probleme de place au lieu de le resoudre.
        m_details_scroll.set_min_content_width(460);
        m_details_scroll.set_min_content_height(-1);
    } else {
        m_details_scroll.set_min_content_width(-1);
        m_details_scroll.set_min_content_height(270); // fits the side-by-side artworks
    }

    m_btn_dock_toggle.set_tooltip_text(right ? _("Dock details at the bottom")
                                             : _("Dock details to the right"));
    if (m_btn_dock_toggle.get_active() != right) {
        m_btn_dock_toggle.set_active(right); // reflect programmatic changes (e.g. load)
    }

    save_launch_prefs();
}

void MainWindow::on_play_clicked() {
    /* Le jeu a lancer est celui que le volet de details MONTRE.
     *
     * Se fier a la seule selection de la liste laissait le bouton sans effet
     * au demarrage. Le jeu choisi a l'ouverture s'affiche bien dans le volet,
     * mais la liste est remplie juste apres et set_model efface la selection
     * du treeview : on cliquait Play sur un jeu parfaitement visible et il ne
     * se passait rien, sans le moindre message. La selection reste la source
     * principale, le volet prend le relais quand elle est vide.
     */
    std::string rom_name, game_system, emulator_id;
    if (auto selection = m_treeview_games.get_selection()) {
        if (auto iter = selection->get_selected()) {
            Gtk::TreeModel::Row row = *iter;
            rom_name    = Glib::ustring(row[m_columns.m_col_name]).raw();
            game_system = Glib::ustring(row[m_columns.m_col_system]).raw();
            emulator_id = Glib::ustring(row[m_columns.m_col_emulator]).raw();
        }
    }
    if (rom_name.empty()) {
        rom_name    = m_last_selected_rom;
        game_system = m_last_selected_system;
        emulator_id = m_last_selected_emulator;
    }
    if (rom_name.empty()) return;   // vraiment aucun jeu a lancer
    
    // === Jeu MAME : autre emulateur, autre chemin ===
    //
    // Rien de ce qui suit ne s'applique : pas de prefixe de systeme, pas de
    // fichier de configuration a reecrire avant de lancer, et MAME est en
    // plein ecran par DEFAUT — c'est l'inverse de FinalBurn Neo, ou l'on
    // ajoute une option pour l'obtenir. Les dossiers de ROMs sont passes en
    // clair plutot que laisses a mame.ini, qui peut tres bien designer des
    // chemins disparus.
    if (emulator_id == "mame") {
        // Le reglage fait foi et retombe de lui-meme sur la detection
        // automatique quand le champ est vide.
        const std::string mame = m_settings_panel.mame_executable();
        if (mame.empty()) {
            SettingsUi::notice(*this, _("MAME not found"),
                               _("MAME does not seem to be installed on this system."),
                               "bc-error.svg");
            return;
        }

        std::vector<std::string> args{mame};
        // Le reglage fait foi ; mame.ini ne sert que de secours, et seulement
        // pour les dossiers qui existent encore.
        std::vector<std::string> paths;
        {
            const std::string configured = m_settings_panel.mame_rompaths();
            std::istringstream ss(configured);
            std::string one;
            while (std::getline(ss, one, ';'))
                if (!one.empty()) paths.push_back(one);
            // Rien de specifique : on reprend les dossiers de ROMs que
            // l'utilisateur a deja declares. Lui demander de saisir une
            // seconde liste pour MAME serait lui faire redire ce qu'il a dit,
            // et c'est exactement la ou ca coincait. MAME ignore sans bruit
            // les dossiers ou il ne reconnait rien.
            if (paths.empty()) paths = m_settings_panel.get_roms_paths();

            if (paths.empty()) {
                for (const auto& q : MameCatalog::rompaths_from_mame_ini()) {
                    std::error_code ec;
                    if (std::filesystem::is_directory(q, ec)) paths.push_back(q);
                }
            }

            // MAME ne cherche pas en profondeur, mais designer le dossier qui
            // contient « ROMs (split) », « bios-devices » et les CHD est le
            // geste naturel. On descend donc d'un cran, mais seulement dans ce
            // qui n'est pas deja un dossier de ROMs : sans cela, un dossier de
            // CHD — ou chaque machine a son propre sous-dossier — ajoutait ses
            // centaines d'entrees a la ligne de commande sans rien apporter.
            //
            // Un dossier sert a MAME s'il porte des archives, ou des
            // sous-dossiers de CHD : c'est ainsi qu'un set merge se presente.
            auto useful_for_mame = [](const std::string& dir) {
                std::error_code ec;
                for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
                    if (e.is_regular_file(ec)) {
                        const auto x = e.path().extension().string();
                        if (x == ".zip" || x == ".7z" || x == ".chd") return true;
                    } else if (e.is_directory(ec)) {
                        std::error_code ec2;
                        for (const auto& f : std::filesystem::directory_iterator(e.path(), ec2))
                            if (f.is_regular_file(ec2) &&
                                f.path().extension().string() == ".chd") return true;
                    }
                }
                return false;
            };

            std::vector<std::string> expanded;
            std::error_code ec;
            for (const auto& q : paths) {
                if (!std::filesystem::is_directory(q, ec)) continue;
                if (useful_for_mame(q)) { expanded.push_back(q); continue; }
                // Dossier de rangement : ce sont ses enfants qui parlent a MAME.
                expanded.push_back(q);
                for (const auto& e : std::filesystem::directory_iterator(q, ec))
                    if (e.is_directory(ec) && useful_for_mame(e.path().string()))
                        expanded.push_back(e.path().string());
            }
            paths.swap(expanded);
        }
        if (!paths.empty()) {
            std::string joined;
            for (const auto& q : paths) { if (!joined.empty()) joined += ';'; joined += q; }
            args.push_back("-rompath");
            args.push_back(joined);
        }
        // Sans dossier utilisable, MAME demarre, ne trouve pas la machine et
        // se referme aussitot : de l'exterieur on croit a un plantage. Mieux
        // vaut ne pas lancer et dire ou se trouve le reglage.
        if (paths.empty()) {
            SettingsUi::notice(*this, _("No MAME ROM folder set"),
                               _("Bootcade does not know where your MAME ROMs are.\n\n"
                                 "Set the folders in Settings, Emulator tab. The ones "
                                 "declared in mame.ini could not be used."),
                               "bc-error.svg");
            return;
        }

        // MAME resout cfg, nvram, sta et diff relativement au repertoire
        // courant, qui est celui du launcher : lance depuis le depot, il y
        // deposait ses fichiers de configuration et ses captures. On les
        // ramene donc dans son dossier a lui, celui qu'il utilise deja quand
        // on l'appelle a la main.
        {
            const std::string mame_home = std::filesystem::path(mame_cfg_dir()).parent_path().string();
            std::error_code ec;
            for (const char* sub : {"cfg", "nvram", "sta", "diff"})
                std::filesystem::create_directories(mame_home + "/" + sub, ec);
            args.push_back("-cfg_directory");   args.push_back(mame_home + "/cfg");
            args.push_back("-nvram_directory"); args.push_back(mame_home + "/nvram");
            args.push_back("-state_directory"); args.push_back(mame_home + "/sta");
            args.push_back("-diff_directory");  args.push_back(mame_home + "/diff");
        }

        /* Les manettes de Bootcade, traduites en fichier controleur MAME.
         *
         * Regenere a chaque lancement depuis le profil qui vaut pour CETTE
         * machine (le sien s'il en a un, sinon le profil par defaut), et
         * passe AVANT les options du joueur, qui gardent le dernier mot.
         * Sans fichier ecrit, pas de -ctrlr : MAME s'arrete net sur un
         * fichier controleur introuvable. */
        std::vector<std::string> mame_env;
        // Les profils ne sont charges qu'a l'ouverture de l'ecran des
        // manettes : sans cette lecture, un premier lancement n'en trouvait
        // aucun et partait sans fichier controleur.
        ControllerManager::load_profiles(m_controller_profiles, m_active_controller_profile,
                                         AppContext::get_config_path());
        if (const ControllerConfig* prof = controller_profile_for(MameControls::profile_key(rom_name))) {
            const std::string dir = MameControls::ctrlr_dir();
            if (MameControls::write_ctrlr(*prof, dir)) {
                /* Le pilote « joystick » brut de SDL, et lui seul, numerote
                 * boutons et axes comme /dev/input/js*, donc comme les
                 * liaisons enregistrees. Celui par defaut (sdlgame) les
                 * renumerote a sa facon. HIDAPI est coupe pour la meme
                 * raison : il court-circuite le noyau et change l'ordre des
                 * boutons des manettes Sony, Nintendo et Xbox. */
                if (MameControls::uses_pad(*prof)) {
                    args.push_back("-joystickprovider"); args.push_back("sdljoy");
                    mame_env.push_back("SDL_JOYSTICK_HIDAPI=0");
                }
                args.push_back("-ctrlrpath"); args.push_back(dir);
                args.push_back("-ctrlr");     args.push_back(MameControls::CTRLR_NAME);
            }
        }
        // Une commande redefinie dans le menu de MAME passe devant le fichier
        // controleur : on le signale, c'est ce qui expliquera une manette
        // qui semble ignorer Bootcade.
        for (const std::string& cfg : {mame_cfg_dir() + "/default.cfg",
                                       mame_cfg_dir() + "/" + rom_name + ".cfg"}) {
            const int n = MameControls::input_overrides(cfg);
            if (n > 0)
                std::cout << "[MameControls] " << cfg << " redefines " << n
                          << " control(s); MAME applies them over Bootcade's\n";
        }

        // Les options viennent desormais de l'ecran des reglages, ou elles
        // sont ecrites dans les deux sens : MAME lit d'abord son propre
        // mame.ini, qu'on ne controle pas, et un reglage qui n'ajouterait rien
        // quand il est eteint laisserait ce fichier decider a notre place.
        /* Les extensions (hiscore, autofire) : MAME s'arrete net sur
         * « Unknown plugin » quand -plugin ou -noplugin cite une extension
         * qu'il ne trouve pas. Or le mame.ini des paquets met pluginspath a
         * « plugins », relatif au dossier courant : lance depuis Bootcade,
         * MAME ne voit rien. On lui donne le vrai dossier, et on ne cite que
         * les extensions qui y sont. */
        {
            namespace fs = std::filesystem;
            std::string plugins_dir;
            const fs::path exe_dir = fs::path(mame).parent_path();
            const char* home = getenv("HOME");
            for (const fs::path& cand : {exe_dir / "plugins",
                                         fs::path("/usr/share/games/mame/plugins"),
                                         fs::path("/usr/share/mame/plugins"),
                                         fs::path(home ? home : "") / ".mame" / "plugins"}) {
                std::error_code ec;
                if (fs::is_directory(cand / "hiscore", ec) || fs::is_directory(cand / "autofire", ec)) {
                    plugins_dir = cand.string();
                    break;
                }
            }
            if (!plugins_dir.empty()) {
                args.push_back("-pluginspath");
                args.push_back(plugins_dir);
            }
            auto available = [&](const std::string& name) {
                std::error_code ec;
                return !plugins_dir.empty() && fs::is_directory(fs::path(plugins_dir) / name, ec);
            };
            const std::vector<std::string> launch = m_settings_panel.mame_launch_args();
            for (size_t i = 0; i < launch.size(); ++i) {
                if ((launch[i] == "-plugin" || launch[i] == "-noplugin") && i + 1 < launch.size()) {
                    std::string kept, one;
                    std::istringstream names(launch[i + 1]);
                    while (std::getline(names, one, ','))
                        if (available(one)) kept += (kept.empty() ? "" : ",") + one;
                    if (!kept.empty()) { args.push_back(launch[i]); args.push_back(kept); }
                    else std::cout << "[INFO] MAME plugins not found, not passed: " << launch[i + 1] << std::endl;
                    ++i;
                    continue;
                }
                args.push_back(launch[i]);
            }
        }

        // Puis ce que le joueur a ajoute lui-meme, decoupe sur les espaces et
        // jamais passe a un shell : un champ de reglages ne doit pas pouvoir
        // devenir une execution de commande arbitraire.
        {
            std::istringstream words(m_settings_panel.mame_extra_args());
            std::string one;
            while (words >> one) args.push_back(one);
        }

        args.push_back(rom_name);

        std::cout << "Launching MAME machine:";
        for (const auto& q : args) std::cout << " " << q;
        std::cout << std::endl;

        // Sous l'emulateur "mame" : une partie de mslug sous MAME ne doit pas
        // grossir le compteur du mslug de FinalBurn Neo.
        if (m_settings_panel.keeps_play_history())
            m_database->recordLaunch(rom_name, game_system, "mame");

        const pid_t pid = spawn_process(args, mame_env);
        if (pid <= 0) {
            SettingsUi::notice(*this, _("Could not start MAME"),
                               _("The emulator could not be started."), "bc-error.svg");
            return;
        }
        std::thread(watch_playtime, pid, m_database, rom_name, game_system,
                    std::string("mame"), m_settings_panel.keeps_play_history()).detach();
        return;
    }

    std::string fbneo_executable = m_settings_panel.get_fbneo_executable();
    std::vector<std::string> roms_paths = m_settings_panel.get_roms_paths();
    
    if (fbneo_executable.empty()) {
        SettingsUi::notice(*this, _("FBNeo not configured"),
                           _("Please set the FBNeo executable path in Settings."),
                           "bc-error.svg");
        return;
    }

    // Verify the executable exists and is actually executable
    {
        std::error_code ec;
        auto status_fs = std::filesystem::status(fbneo_executable, ec);
        if (ec || !std::filesystem::exists(status_fs)) {
            SettingsUi::notice(*this, _("FBNeo executable not found"),
                               _("The file does not exist:\n") + fbneo_executable
                                   + "\n\nPlease update the path in Settings.",
                               "bc-error.svg");
            return;
        }
        if (access(fbneo_executable.c_str(), X_OK) != 0) {
            SettingsUi::notice(*this, _("FBNeo not executable"),
                               _("The file exists but is not executable:\n") + fbneo_executable
                                   + "\n\nRun: chmod +x \"" + fbneo_executable + "\"",
                               "bc-error.svg");
            return;
        }
    }

    if (roms_paths.empty()) {
        SettingsUi::notice(*this, _("No ROM directories configured"),
                           _("Please add at least one ROM directory in Settings."),
                           "bc-error.svg");
        return;
    }
    
    // FBNeo needs ROM paths configured in its config file
    // We'll update the FBNeo config to include all our ROM paths, then launch
    update_fbneo_config(roms_paths);
    
    
    // Set the correct system in FBNeo config before launching
    set_fbneo_system(game_system);
    
    // Adjust ROM name for console systems (they use prefixes in FBNeo)
    std::string fbneo_rom_name = get_fbneo_system_prefix(game_system) + rom_name;
    
    // === Verify ZIP integrity before launching ===
    {
        std::string zip_path = find_rom_zip_path(rom_name);
        if (!zip_path.empty() && !verify_zip_integrity(zip_path)) {
            ConfirmationDialog dlg(*this, _("Corrupt ROM archive"),
                                   _("Launch anyway?"), "bc-warning.svg", /*destructive=*/false,
                                   _("The ZIP file appears corrupt:\n") + zip_path);
            if (!dlg.show_and_confirm()) return;
        }
    }

    // === Build launch args ===
    std::vector<std::string> launch_args;
    launch_args.push_back(fbneo_executable);
    // No "-joy": that flag makes FBNeo map the pad onto EVERY player, so a
    // single controller ends up driving both P1 and P2 coin/start : one press
    // inserts two credits and starts a two-player game. Verified by running
    // the emulator both ways: without it, player 1 gets the pad in full
    // (D-pad, buttons, coin, start) and player 2 stays on the keyboard, which
    // is what a one-pad setup should do. The pad works fine without the flag.
    if (m_launch_fullscreen)   launch_args.push_back("-fullscreen");
    if (m_launch_integerscale) launch_args.push_back("-integerscale");
    /* Ce que le joueur a ajoute lui-meme dans Settings > Emulator.
     *
     * Decoupe sur les espaces, sans passer par un shell : l'interpretation
     * d'une ligne de commande par /bin/sh est exactement ce qui transforme un
     * champ de reglages en execution de commande arbitraire. Les arguments
     * arrivent donc a FBNeo tels quels, un par mot, et rien d'autre.
     * Places AVANT le nom de la ROM, que FBNeo attend en dernier.
     */
    {
        const std::string extra = m_settings_panel.get_emulator_extra_args();
        std::istringstream words(extra);
        std::string one;
        while (words >> one) launch_args.push_back(one);
    }
    launch_args.push_back(fbneo_rom_name);

    std::cout << "Launching " << game_system << " game:";
    for (const auto& a : launch_args) std::cout << " " << a;
    std::cout << std::endl;

    // Record launch (last_played + play_count)
    if (m_settings_panel.keeps_play_history())
        m_database->recordLaunch(rom_name, game_system);

    std::time_t launch_time = std::time(nullptr);
    std::string previews_dir = m_settings_panel.get_previews_path(emulator_id);
    std::string titles_dir = m_settings_panel.get_titles_path(emulator_id);

    // Repair a config left conflicting by a previous session before the game
    // starts, so the fix takes effect from this launch rather than the next.
    ControllerManager::fix_player2_input_conflicts(fbneo_rom_name);
    // Wheels, paddles, dials and pointers: FBNeo leaves them on the keyboard,
    // and no per-player default can reach them (see apply_analog_bindings).
    // Un jeu qui a son propre profil recoit aussi ses boutons : FBNeo a pu
    // reecrire le .ini depuis, ou un « Reset game settings » l'a efface.
    const bool own_profile = ControllerManager::load_game_profiles(
        AppContext::get_config_path()).count(fbneo_rom_name) > 0;
    if (const ControllerConfig* launch_profile = controller_profile_for(fbneo_rom_name)) {
        if (own_profile) ControllerManager::write_game_config(*launch_profile, fbneo_rom_name);
        ControllerManager::apply_analog_bindings(fbneo_rom_name, *launch_profile);
    }

    // Snapshot of the score table BEFORE play. Without it the server cannot
    // tell what this session achieved from what the table already held : a
    // fresh table ships with factory scores that belong to nobody.
    // The file is chosen once, here, and read again after the session : a
    // game can have both a .hi and a .fs, and comparing one before with the
    // other after would compare two unrelated tables.
    const std::string score_path = fbneo_score_state_path(game_system, rom_name, fbneo_rom_name);
    std::string hi_before = read_file_bytes(score_path);
    // Read here, on the GTK thread, and carried into the watcher: the panel
    // must not be touched from there. Empty means "do not send".
    // Le nom vient du COMPTE, plus d'un champ de reglages : c'est le seul du
    // systeme, et le serveur le reprend de toute facon dans le jeton. Vide
    // quand personne n'est connecte, ce qui coupe l'envoi en amont plutot que
    // de laisser le serveur repondre 401 apres coup.
    // Le reglage part a cote du nom : vide parce que DESACTIVE, on se tait ;
    // vide parce que DECONNECTE, le score est gare pour la prochaine connexion.
    const bool hiscore_enabled = m_settings_panel.is_hiscore_enabled();
    std::string hiscore_player = hiscore_enabled
                               ? BootcadeAuth::username() : std::string();
    std::string hiscore_country = m_settings_panel.get_hiscore_country();
    // Lus ici, sur le fil graphique : le fil d'observation ne doit toucher ni
    // au panneau ni a aucun widget.
    const bool keep_history = m_settings_panel.keeps_play_history();
    const bool share_playtime = m_settings_panel.shares_play_statistics();

    pid_t pid = spawn_process(launch_args);
    if (pid > 0) {
        // Detached watcher thread: waits for process exit, records playtime,
        // then checks whether FBNeo's own F6 screenshot hotkey was used during
        // the session : if so, offer to use the capture(s) as artwork.
        // The active profile is copied here, on the GTK thread : the worker
        // must not read m_controller_profiles while Controller Configuration
        // may be rewriting it.
        std::optional<ControllerConfig> profile;
        if (const ControllerConfig* p = controller_profile_for(fbneo_rom_name))
            profile = *p;
        std::thread([this, pid, rom_name, game_system, fbneo_rom_name, previews_dir, titles_dir, launch_time, score_path, hi_before, hiscore_player, hiscore_country,
                     hiscore_enabled, keep_history, share_playtime, profile = std::move(profile), own_profile,
                     alive = m_alive_token]() {
            watch_playtime(pid, m_database, rom_name, game_system, "fbneo", keep_history);
            // La fenêtre a pu être fermée pendant la partie. Le verrou reste
            // pris pendant l'envoi : ~MainWindow attend ici plutot que de
            // detruire l'objet sous les pieds de l'envoi (quelques secondes
            // au plus, borne par les delais du client).
            std::lock_guard<std::mutex> live(alive->mutex);
            if (!alive->alive) return;
            // FBNeo writes the .hi on exit, so this must come after the wait.
            submit_session_score(game_system, rom_name, fbneo_rom_name, score_path, hi_before, hiscore_player, hiscore_country, hiscore_enabled, share_playtime, launch_time);
            // FBNeo has just written config/games/<rom>.ini on exit : this is
            // the only moment a complete file exists to repair.
            ControllerManager::fix_player2_input_conflicts(fbneo_rom_name);
            // Same reason for the analog inputs. On a game's very first run the
            // file did not exist before launch, so there was nothing to bind
            // and the wheel stayed on the keyboard; repairing it here means it
            // is right from the second run on, without waiting for the next
            // launch to notice. The first run of a new analog game is
            // unavoidably on the keyboard: FBNeo only reveals a game's input
            // list by writing this file, and it does that on exit.
            if (profile) {
                // Premier lancement d'un jeu a profil propre : le .ini vient
                // seulement de naitre, c'est maintenant qu'il recoit ses boutons.
                if (own_profile) ControllerManager::write_game_config(*profile, fbneo_rom_name);
                ControllerManager::apply_analog_bindings(fbneo_rom_name, *profile);
            }
            std::cout << "[SCREENSHOT] session ended for " << fbneo_rom_name
                      << " previews_dir=" << previews_dir << " titles_dir=" << titles_dir
                      << " launch_time=" << launch_time << std::endl;
            if (previews_dir.empty() && titles_dir.empty()) {
                std::cout << "[SCREENSHOT] both dirs empty, skipping scan" << std::endl;
                return;
            }
            auto shots = find_session_screenshots(fbneo_rom_name, launch_time);
            std::cout << "[SCREENSHOT] scan dir=" << get_fbneo_screenshots_dir()
                      << " found " << shots.size() << " fresh capture(s)" << std::endl;
            if (shots.empty()) return;
            std::lock_guard<std::mutex> lock(m_screenshot_queue_mutex);
            m_screenshot_queue.push_back({fbneo_rom_name, shots, previews_dir, titles_dir});
            std::cout << "[SCREENSHOT] emitting dispatcher, queue size now "
                      << m_screenshot_queue.size() << std::endl;
            m_screenshot_found_dispatcher.emit();
        }).detach();
    }
}

bool MainWindow::normalize_artwork_file(const std::string& path, int target_w, int target_h) {
    try {
        auto src = Gdk::Pixbuf::create_from_file(path);
        int w = src->get_width();
        int h = src->get_height();
        if (w <= 0 || h <= 0) return false;

        // Fit the whole capture inside the target box : never crop it. Most
        // systems' screens are landscape (~4:3) while the title slot is a
        // portrait 384x512 frame: cropping to fill would cut off a large
        // chunk of the actual title screen instead of just trimming margins.
        // Pad with black bars to reach the exact target size instead.
        double scale = std::min(target_w / (double)w, target_h / (double)h);
        int scaled_w = std::max(1, (int)std::lround(w * scale));
        int scaled_h = std::max(1, (int)std::lround(h * scale));
        auto scaled = src->scale_simple(scaled_w, scaled_h, Gdk::INTERP_BILINEAR);

        auto canvas = Gdk::Pixbuf::create(scaled->get_colorspace(), scaled->get_has_alpha(),
                                           scaled->get_bits_per_sample(), target_w, target_h);
        canvas->fill(0x000000ff);
        int x = (target_w - scaled_w) / 2;
        int y = (target_h - scaled_h) / 2;
        scaled->copy_area(0, 0, scaled_w, scaled_h, canvas, x, y);
        canvas->save(path, "png");
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] normalize_artwork_file failed for " << path << ": " << e.what() << std::endl;
        return false;
    }
}

std::string MainWindow::get_fbneo_screenshots_dir() {
    // Matches FBNeo's own SDL_GetPrefPath("fbneo", "screenshots") on Linux :
    // untouched, upstream behavior behind the existing F6 hotkey.
    //
    // Pas de XDG_DATA_HOME ici. FBNeo tourne sur l'hote (flatpak-spawn --host)
    // et ecrit dans ~/.local/share/fbneo, alors que dans le Flatpak cette
    // variable designe le dossier prive du bac a sable (~/.var/app/.../data) :
    // le launcher y cherchait des captures qui n'y arrivent jamais, et la
    // question « utiliser cette capture comme artwork ? » ne venait plus.
    // Meme construction que les scores et les .ini des manettes plus haut.
    const char* home = getenv("HOME");
    return std::string(home ? home : "") + "/.local/share/fbneo/screenshots";
}

std::vector<std::string> MainWindow::find_session_screenshots(const std::string& fbneo_rom_name, std::time_t launch_time) {
    std::vector<std::string> result;
    std::string dir = get_fbneo_screenshots_dir();
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || ec) return result;

    std::string prefix = fbneo_rom_name + "-";
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) != 0) continue;
        if (entry.path().extension() != ".png") continue;
        std::time_t mtime = get_file_mtime(entry.path().string());
        if (mtime < 0 || mtime < launch_time) continue; // pre-existing capture from an earlier session
        result.push_back(entry.path().string());
    }
    std::sort(result.begin(), result.end());
    return result;
}

void MainWindow::on_screenshot_batch_found() {
    std::cout << "[SCREENSHOT] on_screenshot_batch_found() dispatched on UI thread" << std::endl;
    for (;;) {
        PendingScreenshotBatch batch;
        {
            std::lock_guard<std::mutex> lock(m_screenshot_queue_mutex);
            if (m_screenshot_queue.empty()) {
                std::cout << "[SCREENSHOT] queue empty, done" << std::endl;
                return;
            }
            batch = m_screenshot_queue.front();
            m_screenshot_queue.pop_front();
        }
        std::cout << "[SCREENSHOT] showing dialog for " << batch.fbneo_rom_name
                  << " with " << batch.screenshot_paths.size() << " capture(s)" << std::endl;

        int result;
        try {
            ScreenshotAssignDialog dialog(*this, batch.screenshot_paths);
            result = dialog.run();
            std::cout << "[SCREENSHOT] dialog closed, response=" << result << std::endl;
            if (result != Gtk::RESPONSE_OK) continue;

            std::error_code ec;
            std::string title_choice = dialog.get_title_choice();
            if (!title_choice.empty() && !batch.titles_dir.empty()) {
                std::filesystem::create_directories(batch.titles_dir, ec);
                std::string dest = batch.titles_dir + "/" + batch.fbneo_rom_name + ".png";
                std::filesystem::copy_file(title_choice, dest, std::filesystem::copy_options::overwrite_existing, ec);
                if (!ec) normalize_artwork_file(dest, 384, 512);
                else std::cerr << "[SCREENSHOT] copy_file(title) failed: " << ec.message() << std::endl;
            }
            std::string preview_choice = dialog.get_preview_choice();
            if (!preview_choice.empty() && !batch.previews_dir.empty()) {
                std::filesystem::create_directories(batch.previews_dir, ec);
                std::string dest = batch.previews_dir + "/" + batch.fbneo_rom_name + ".png";
                std::filesystem::copy_file(preview_choice, dest, std::filesystem::copy_options::overwrite_existing, ec);
                if (!ec) normalize_artwork_file(dest, 512, 384);
                else std::cerr << "[SCREENSHOT] copy_file(preview) failed: " << ec.message() << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "[SCREENSHOT] EXCEPTION in on_screenshot_batch_found: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[SCREENSHOT] UNKNOWN EXCEPTION in on_screenshot_batch_found" << std::endl;
        }
    }
}

void MainWindow::on_download_art_clicked() {
    auto selection = m_treeview_games.get_selection();
    auto iter = selection->get_selected();
    if (!iter) {
        return;
    }

    // Récupérer les informations du jeu sélectionné
    Gtk::TreeModel::Row row = *iter;
    std::string game_name = Glib::ustring(row[m_columns.m_col_name]).raw();
    std::string game_title = Glib::ustring(row[m_columns.m_col_title]).raw();
    std::string game_system = Glib::ustring(row[m_columns.m_col_system]).raw();
    std::string emulator = Glib::ustring(row[m_columns.m_col_emulator]).raw();
    if (emulator.empty()) emulator = "fbneo";
    
    // Vérifier que les répertoires sont configurés
    std::string previews_dir = m_settings_panel.get_previews_path(emulator);
    std::string titles_dir = m_settings_panel.get_titles_path(emulator);
    
    if (previews_dir.empty() && titles_dir.empty()) {
        SettingsUi::notice(*this, _("Artwork Directories Not Set"),
                           _("Please set the previews and/or titles directories in Settings before downloading."),
                           "bc-warning.svg");
        return;
    }
    
    // Vérifier si un téléchargement est déjà en cours
    if (m_thumbnail_downloader.is_downloading()) {
        SettingsUi::notice(*this, _("Download In Progress"),
                           _("Artwork download is already in progress."),
                           "bc-info.svg");
        return;
    }
    
    std::cout << "[INFO] Downloading artwork for: " << game_title << std::endl;
    
    // Create callback to update status label for single downloads
    auto single_download_callback = [this](const std::string& status, int current, int total, double percentage) {
        m_status_label.set_text(status);
    };
    
    // Download preview first if directory is configured - use ROM name and system
    if (!previews_dir.empty()) {
        std::cout << "[INFO] Downloading preview for: " << game_title << " (ROM: " << game_name << ", System: " << game_system << ")" << std::endl;
        m_status_label.set_text(_("Downloading preview for ") + game_title + "...");
        m_thumbnail_downloader.download_single_artwork(game_name, game_system, previews_dir, ThumbnailDownloader::ArtworkType::Previews, single_download_callback, emulator);
        
        // Wait a moment before downloading title
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    // Download title if directory is configured - use ROM name and system
    if (!titles_dir.empty() && !m_thumbnail_downloader.is_downloading()) {
        std::cout << "[INFO] Downloading title for: " << game_title << " (ROM: " << game_name << ", System: " << game_system << ")" << std::endl;
        m_status_label.set_text(_("Downloading title for ") + game_title + "...");
        m_thumbnail_downloader.download_single_artwork(game_name, game_system, titles_dir, ThumbnailDownloader::ArtworkType::Titles, single_download_callback, emulator);
    }
}

void MainWindow::update_fbneo_config(const std::vector<std::string>& roms_paths) {
    const char* home = getenv("HOME");
    std::string config_file = std::string(home ? home : "") + "/.local/share/fbneo/config/fbneo.ini";
    
    // Prepare paths with trailing slashes
    std::vector<std::string> normalized_paths;
    for (const auto& path : roms_paths) {
        std::string path_with_slash = path;
        if (!path_with_slash.empty() && path_with_slash.back() != '/') {
            path_with_slash += "/";
        }
        normalized_paths.push_back(path_with_slash);
    }
    
    // Read the current config to check if paths are already correctly set
    std::ifstream file(config_file);
    if (!file.is_open()) {
        std::cout << "Warning: Could not open FBNeo config file: " << config_file << std::endl;
        return;
    }
    
    std::vector<std::string> lines;
    std::string line;
    std::vector<std::string> current_paths(20); // FBNeo supports up to 20 ROM paths
    bool config_needs_update = false;

    // Read current configuration
    while (std::getline(file, line)) {
        // Check for existing ROM path slots
        for (int i = 0; i < 20; ++i) {
            std::string slot_pattern = "szAppRomPaths[" + std::to_string(i) + "]";
            if (line.find(slot_pattern) != std::string::npos) {
                // Extract the path from the line
                size_t space_pos = line.find(' ');
                if (space_pos != std::string::npos && space_pos + 1 < line.length()) {
                    current_paths[i] = line.substr(space_pos + 1);
                }
                break;
            }
        }
        lines.push_back(line);
    }
    file.close();

    // Check if any path needs to be updated
    for (size_t i = 0; i < normalized_paths.size() && i < 20; ++i) {
        if (current_paths[i] != normalized_paths[i]) {
            config_needs_update = true;
            break;
        }
    }

    // Check if we need to clear paths beyond our count
    for (size_t i = normalized_paths.size(); i < 20; ++i) {
        if (!current_paths[i].empty()) {
            config_needs_update = true;
            break;
        }
    }

    if (!config_needs_update) {
        // Configuration is already up to date
        return;
    }

    // Update the configuration
    std::set<int> updated_slots;
    for (auto& line : lines) {
        // Update existing ROM path slots
        for (int i = 0; i < static_cast<int>(normalized_paths.size()) && i < 20; ++i) {
            std::string slot_pattern = "szAppRomPaths[" + std::to_string(i) + "]";
            if (line.find(slot_pattern) != std::string::npos) {
                line = slot_pattern + " " + normalized_paths[i];
                updated_slots.insert(i);
                break;
            }
        }

        // Clear paths beyond our count
        for (int i = static_cast<int>(normalized_paths.size()); i < 20; ++i) {
            std::string slot_pattern = "szAppRomPaths[" + std::to_string(i) + "]";
            if (line.find(slot_pattern) != std::string::npos) {
                line = slot_pattern + " ";  // Clear the path
                break;
            }
        }
    }

    // Add missing ROM path slots that weren't found in the config
    for (int i = 0; i < static_cast<int>(normalized_paths.size()) && i < 20; ++i) {
        if (updated_slots.find(i) == updated_slots.end()) {
            std::string new_line = "szAppRomPaths[" + std::to_string(i) + "] " + normalized_paths[i];
            lines.push_back(new_line);
        }
    }

    // Write back the updated config
    std::ofstream outfile(config_file);
    for (const auto& l : lines) {
        outfile << l << std::endl;
    }
    outfile.close();

    std::cout << "Updated FBNeo config with " << normalized_paths.size() << " ROM paths" << std::endl;
}

void MainWindow::set_fbneo_system(const std::string& system) {
    const char* home = getenv("HOME");
    std::string config_file = std::string(home ? home : "") + "/.local/share/fbneo/config/fbneo.ini";
    
    // System-specific filter values from FBNeo
    int filter_value = 0; // Default
    
    if (system == "IGS PGM") {
        filter_value = 134217728;
    } else if (system == "Fairchild Channel F") {
        filter_value = 553648128;
    } else if (system == "Taito") {
        filter_value = 184549376;
    } else if (system == "Psykyo") {
        filter_value = 218103808;
    } else if (system == "Kaneko") {
        filter_value = 234881024;
    } else if (system == "IREM") {
        filter_value = 285212672;
    } else if (system == "Data East") {
        filter_value = 318767104;
    } else if (system == "Seta") {
        filter_value = 352321536;
    } else if (system == "Technos") {
        filter_value = 369098752;
    } else if (system == "Megadrive" || system == "Sega Megadrive Genesis") {
        filter_value = 201326592;
    } else if (system == "PC-Engine" || system == "NEC PC Engine") {
        filter_value = 385941504;
    } else if (system == "TurboGrafx 16" || system == "NEC TurboGraphX 16") {
        filter_value = 386007040;
    } else if (system == "SuprGrafx" || system == "NEC SGX") {
        filter_value = 386072576;
    } else if (system == "Sega SG-1000" || system == "SG-1000") {
        filter_value = 419430400;
    } else if (system == "ColecoVision") {
        filter_value = 436207616;
    } else if (system == "Master System" || system == "Sega MasterSystem") {
        filter_value = 402653184;
    } else if (system == "Game Gear" || system == "Sega GameGear") {
        filter_value = 301989888;
    } else if (system == "MSX 1") {
        filter_value = 469762048;
    } else if (system == "ZX Spectrum" || system == "Sinclar Spectrum") {
        filter_value = 486539264;
    } else if (system == "NES") {
        filter_value = 503316480;
    } else if (system == "FDS" || system == "Nintendo FDS") {
        filter_value = 520093696;
    } else if (system == "Capcom CPS 1 2 3") {
        filter_value = 16777216;
    } else if (system == "Cave") {
        filter_value = 100663296;
    } else if (system == "Pre 1990") {
        filter_value = 0;
    } else if (system == "Post 1990") {
        filter_value = 167772160;
    } else if (system == "Midway") {
        filter_value = 452984832;
    } else if (system == "SEGA") {
        filter_value = 33554432;
    } else if (system == "Konami") {
        filter_value = 50331648;
    } else if (system == "Toaplan") {
        filter_value = 67108864;
    } else if (system == "Neogeo") {
        filter_value = 83951616;
    } else if (system == "NeoGeo Pocket") {
        filter_value = 536870912;
    } else {
        filter_value = 2147418112; // Everything as fallback
    }
    
    // Read current config
    std::ifstream file(config_file);
    if (!file.is_open()) {
        std::cout << "Warning: Could not open FBNeo config file: " << config_file << std::endl;
        return;
    }
    
    std::vector<std::string> lines;
    std::string line;
    bool updated = false;
    
    while (std::getline(file, line)) {
        if (line.find("nFilterSelect ") != std::string::npos) {
            line = "nFilterSelect " + std::to_string(filter_value);
            updated = true;
        }
        lines.push_back(line);
    }
    file.close();
    
    if (updated) {
        // Write back the updated config
        std::ofstream outfile(config_file);
        for (const auto& l : lines) {
            outfile << l << std::endl;
        }
        outfile.close();
        std::cout << "Set FBNeo system to: " << system << " (filter: " << filter_value << ")" << std::endl;
    }
}

void MainWindow::on_start_scan_clicked() {
    // En portee MAME, verifier la collection c'est interroger MAME : la regle
    // de RomResolve ne s'applique pas a ses sets splits, et lui la connait.
    if (m_active_emulator == "mame") { run_mame_audit(); return; }
    scan_fbneo_library();
}

void MainWindow::on_library_scan_requested(const std::string& emulator) {
    if (emulator == "fbneo" || emulator.empty()) { scan_fbneo_library(); return; }

    // Any other emulator : its ROM directories are read into the cache and
    // its sets of the ROM manager's DATs resolved from there. The main
    // window's own MAME check (mame -verifyroms, run_mame_audit) is a
    // different verdict, on a different table, and stays where it is.
    const std::vector<std::string> roms_paths = m_settings_panel.get_roms_paths(emulator);
    if (roms_paths.empty()) {
        SettingsUi::notice(*this, _("No ROM directories configured"),
                           _("Please add at least one ROM directory in Settings."),
                           "bc-info.svg");
        return;
    }
    ConfirmationDialog confirm_dialog(*this,
        _("Scan ROMs"),
        _("This will rescan all ROM directories to update game status.\n\nAre you sure you want to continue?"),
        "bc-search.svg", /*destructive=*/false,
        _("This process can take several minutes depending on your ROM collection."));
    if (!confirm_dialog.show_and_confirm()) return;
    start_scan_thread(roms_paths, emulator);
}

void MainWindow::scan_fbneo_library() {
    std::cout << "[INFO] Starting ROM scan using database" << std::endl;
    
    // Confirmation dialog with custom styling
    ConfirmationDialog confirm_dialog(*this,
        _("Scan ROMs"),
        _("This will rescan all ROM directories to update game status.\n\nAre you sure you want to continue?"),
        "bc-search.svg", /*destructive=*/false,
        _("This process can take several minutes depending on your ROM collection."));
    
    if (!confirm_dialog.show_and_confirm()) {
        std::cout << "[INFO] ROM scan cancelled by user" << std::endl;
        return;
    }
    
    // Prevent multiple scans from running
    if (m_scan_in_progress) {
        std::cout << "[WARNING] Scan already in progress" << std::endl;
        return;
    }
    
    // Get ROM paths from settings panel
    std::vector<std::string> roms_paths = m_settings_panel.get_roms_paths();
    std::cout << "[DEBUG] Scanning " << roms_paths.size() << " ROM paths:" << std::endl;
    for (const auto& path : roms_paths) {
        std::cout << "[DEBUG]   - " << path << std::endl;
    }
    
    if (roms_paths.empty()) {
        m_status_label.set_text(_("Error: No ROM directories defined"));
        m_status_label.show();
        return;
    }
    
    // Ensure database is loaded with DAT data (cheap count query, no full load)
    // The whole table, both emulators : what is asked is "was any DAT ever
    // loaded", and reloading them on top of MAME rows would only collide.
    if (m_database->getGameCount(/*every emulator*/ "") == 0) {
        std::string dat_path = m_settings_panel.get_dat_path();
        if (dat_path.empty()) {
            m_status_label.set_text(_("Error: No DAT path defined"));
            m_status_label.show();
            return;
        }
        
        std::cout << "[INFO] Reloading DAT files to database..." << std::endl;
        // Same corpus as Update DAT : what the active DAT groups select.
        bool loaded_any = false;
        for (const auto& f : DatSource::files_to_load(DatSource::load_groups()))
            if (DatParser::parseToDatabase(f, m_database) > 0) loaded_any = true;
        if (!loaded_any) {
            m_status_label.set_text(_("Error: Failed to load DAT files"));
            m_status_label.show();
            return;
        }
    }
    
    // Start threaded scan
    start_scan_thread(roms_paths);
}

void MainWindow::on_update_dat_clicked() {
    confirm_update_dat("");
}

void MainWindow::confirm_update_dat(const std::string& emulator) {
    std::cout << "[INFO] Update DAT requested" << std::endl;

    // Confirmation dialog with custom styling
    ConfirmationDialog confirm_dialog(*this,
        _("Update DAT"),
        _("The game database will be reloaded from the DAT files.\nGames whose ROM definition is unchanged keep their status \nonly new or changed games are re-checked on the next scan.\n\nDo you want to continue?"),
        "bc-sync.svg");

    if (!confirm_dialog.show_and_confirm()) {
        std::cout << "[INFO] Update DAT cancelled by user" << std::endl;
        return;
    }

    do_update_dat(emulator);
}

void MainWindow::ask_mame_dat_resync() {
    // Asked once, like the question below.
    m_database->clearMameDatResyncFlag();

    ConfirmationDialog confirm(*this,
        _("Reload the MAME DAT files?"),
        _("This version of Bootcade reads more from MAME's DAT files: the devices "
          "each machine uses (a sound chip, a protection chip…), whose ROMs a "
          "non-merged collection keeps in each set's archive.\n\n"
          "MAME's DAT files need one reload to pick that up. FinalBurn Neo is "
          "not touched, and favourites and play history are kept.\n\n"
          "Reload now? (You can also do it later from the ROM Manager.)"),
        "bc-sync.svg");
    if (confirm.show_and_confirm()) do_update_dat("mame");
}

void MainWindow::ask_dat_resync() {
    // Asked once, whatever the answer : a refusal is not a reason to nag at
    // every launch, and the registrations the migration forgot mean a later
    // "Update DAT" from the menu re-reads everything anyway.
    m_database->clearDatResyncFlag();

    ConfirmationDialog confirm(*this,
        _("Reload the DAT files?"),
        _("This version of Bootcade reads more from the DAT files than before: "
          "which ROMs a set shares with its parent or its BIOS.\n\n"
          "The game database needs one full reload to pick that up. Favourites "
          "and play history are kept, and sets whose ROM list is unchanged keep "
          "their scan status.\n\n"
          "Reload now? (You can also do it later from ROMs > Update DAT.)"),
        "bc-sync.svg");
    if (confirm.show_and_confirm()) do_update_dat();
}

void MainWindow::do_update_dat(const std::string& emulator) {
    // The dialog runs a nested loop : a second request arriving meanwhile
    // (the DAT tab's deferred reload, a download finishing) waits its turn
    // rather than opening a dialog over the dialog.
    if (m_dat_update_running) {
        // Two requests for different emulators : the queued reload covers both.
        if (!m_dat_update_again) m_dat_update_again_emulator = emulator;
        else if (m_dat_update_again_emulator != emulator) m_dat_update_again_emulator.clear();
        m_dat_update_again = true;
        return;
    }
    m_dat_update_running = true;
    std::string scope = emulator;
    do {
        m_dat_update_again = false;
        run_update_dat_once(scope);
        scope = m_dat_update_again_emulator;
    } while (m_dat_update_again);
    m_dat_update_running = false;
}

void MainWindow::run_update_dat_once(const std::string& emulator) {
    std::string dat_path = m_settings_panel.get_dat_path();
    if (dat_path.empty()) {
        SettingsUi::notice(*this, _("Error"),
                           _("No DAT path configured. Please configure the path in settings."),
                           "bc-error.svg");
        return;
    }
    
    // The database is the union of what the active DAT groups select : a
    // file no group wants stays out, a file two groups share loads once.
    std::vector<std::string> conflicts;
    std::vector<std::string> files = DATUpdateDialog::files_for_update(emulator, &conflicts);
    for (const auto& c : conflicts) std::cerr << "[DAT] conflict: " << c << std::endl;
    DATUpdateDialog dialog(*this, m_database, dat_path, files, emulator);
    dialog.start_update();
    
    int result = dialog.run();
    
    if (dialog.database_changed()) {
        // Reload games from database and refresh interface
        std::cout << "[INFO] Reloading games after DAT update..." << std::endl;
        m_cached_games = load_all_catalogs();
        m_search_blobs.clear();   // the haystacks describe the old vector
    // Et m_filtered_games pointait dedans : le laisser en place, c'est garder
    // des pointeurs vers un vecteur qui vient d'etre remplace.
    { std::lock_guard<std::mutex> lk(m_filter_mutex); m_filtered_games.clear(); }
        
        // Regenerate filter cache from updated games
        std::cout << "[INFO] Regenerating filter cache after DAT update..." << std::endl;
        m_filter_cache = FilterCache::generate_from_games(m_cached_games);
        save_filter_cache();
        m_filter_cache_loaded = true; // Mark as loaded with new data
        
        // Filter cache updated - will be used by future MAMEUI-style filter panel
        std::cout << "[INFO] Filter cache updated with new DAT data" << std::endl;
        
        // Refresh display
        filter_games();
        update_status_bar_stats();
        
        std::cout << "[INFO] Interface updated with " << m_cached_games.size() << " games" << std::endl;
        // The DAT tab shows per-file counts read from the database.
        if (m_rom_manager) m_rom_manager->refresh_after_scan();
        AppContext::trim_heap();
    }
}


void MainWindow::on_settings_clicked() {
    // Deja ouverte : on la ramene devant plutot que d'en ouvrir une seconde.
    if (m_settings_win) { m_settings_win->present(); return; }

    /* Une fenetre a part entiere, decoree par le panneau lui-meme.
     *
     * L'ecran etait un Gtk::Dialog dont GTK dessinait la barre de titre et
     * les boutons : le resultat portait la barre du bureau au-dessus d'un
     * contenu Bootcade, et deux langages visuels dans la meme fenetre. Le
     * panneau fournit desormais sa HeaderBar (marque, titre, sous-titre,
     * croix) et son pied d'actions, exactement comme Controller Configuration.
     */
    auto* dialog = new Gtk::Dialog();          // sans parent : fenetre a part
    m_settings_win = dialog;
    dialog->set_title(_("Settings"));
    dialog->set_modal(false);
    /* La hauteur est laissee a -1 : « pas de defaut ».
     *
     * GTK prend alors la hauteur NATURELLE du contenu, ce qui est exactement
     * ce qu'on veut depuis que les pages ne defilent plus : la fenetre fait la
     * taille de ce qu'elle a a montrer, ni plus, ni moins. Une hauteur fixe
     * rognait la derniere carte ou laissait une bande morte selon l'onglet.
     * La largeur, elle, est imposee : la maquette tient sur deux colonnes de
     * cartes, et plus etroit les pages Emulator et Online se replient. */
    dialog->set_default_size(1180, -1);
    dialog->set_titlebar(m_settings_panel.header_bar());
    dialog->get_content_area()->set_spacing(0);
    dialog->get_content_area()->pack_start(m_settings_panel);
    m_settings_panel.show();
    m_settings_panel.on_window_shown();
    dialog->set_default_response(Gtk::RESPONSE_OK);

    auto close_conn = std::make_shared<sigc::connection>(
        m_close_settings_signal.connect([dialog]() { dialog->response(Gtk::RESPONSE_OK); }));
    // Le pied du panneau ne ferme rien lui-meme : seule la fenetre sait le
    // faire, et c'est elle qui decide d'enregistrer ou non.
    auto save_conn = std::make_shared<sigc::connection>(
        m_settings_panel.signal_save_requested().connect(
            [dialog]() { dialog->response(Gtk::RESPONSE_OK); }));
    auto cancel_conn = std::make_shared<sigc::connection>(
        m_settings_panel.signal_close_requested().connect(
            [dialog]() { dialog->response(Gtk::RESPONSE_CANCEL); }));

    dialog->signal_response().connect([this, dialog, close_conn, save_conn, cancel_conn](int result) {
        close_conn->disconnect();
        save_conn->disconnect();
        cancel_conn->disconnect();
        if (result == Gtk::RESPONSE_OK) {
            m_settings_panel.save_to_file(AppContext::get_config_path());
            // Les options de lancement vivent dans les MEMES cles que le menu
            // « Launch » : les relire ici garde le menu en phase avec ce que
            // l'ecran vient d'enregistrer, au lieu de le laisser reecrire
            // l'ancienne valeur a la fermeture de l'application.
            load_launch_prefs();
        }
        // Le panneau est un membre reutilise : il doit quitter la fenetre
        // avant qu'elle ne soit detruite, sinon elle l'emporte avec elle. Sa
        // barre de titre aussi, pour la meme raison.
        dialog->get_content_area()->remove(m_settings_panel);
        /* La barre de titre appartient au PANNEAU, qui doit la retrouver a la
         * prochaine ouverture : il faut donc la detacher avant que la fenetre
         * ne detruise ses enfants. GTK refuse set_titlebar sur une fenetre
         * deja realisee : on la cache et on la derealise d'abord, faute de
         * quoi l'appel ne fait qu'emettre un avertissement, la barre part avec
         * la fenetre, et le deuxieme « Settings » s'ouvre sans en-tete. */
        dialog->hide();
        gtk_widget_unrealize(GTK_WIDGET(dialog->gobj()));
        gtk_window_set_titlebar(GTK_WINDOW(dialog->gobj()), nullptr);
        refresh_emu_state();
        m_settings_win = nullptr;
        delete dialog;
    });
    dialog->show_all();
    dialog->present();
}

void MainWindow::on_hide() {
    m_settings_panel.save_to_file(AppContext::get_config_path());
    save_launch_prefs();
    Gtk::Window::on_hide();
}

void MainWindow::on_quit() {
    // Gtk::Main::quit() has no Gtk::Main behind it under Gtk::Application :
    // it dereferenced a null instance and the menu's Quit ended in SIGSEGV.
    // Hiding the window is what ends app->run(window); on_hide() saves the
    // settings on the way out.
    hide();
}

void MainWindow::update_status_bar_stats() {
    int total = 0, available = 0, incorrect = 0, missing = 0, error = 0;

    // Count stats from filtered games (much faster than re-filtering)
    std::lock_guard<std::mutex> lock(m_filter_mutex);
    for (const auto* game : m_filtered_games) {
        total++;
        if (game->status == "available") available++;
        else if (game->status == "incorrect") incorrect++;
        else if (game->status == "missing") missing++;
        else error++;
    }

    // Clear previous stats
    SettingsUi::destroy_children(m_stats_box);

    // Legend with coloured dots, matching the detail-dock / grid status colours.
    auto add_stat = [&](const char* color, int count, const std::string& label) {
        auto* l = Gtk::make_managed<Gtk::Label>();
        l->set_markup("<span foreground=\"" + std::string(color) + "\">●</span> " +
                      Glib::Markup::escape_text(label) + " <b>" + std::to_string(count) + "</b>");
        l->set_margin_end(14);
        m_stats_box.pack_start(*l, Gtk::PACK_SHRINK);
    };
    add_stat(SettingsUi::tone_hex(*this, "success").c_str(), available, _("Available"));
    add_stat(SettingsUi::tone_hex(*this, "warning").c_str(), incorrect, _("Incorrect"));
    add_stat(SettingsUi::tone_hex(*this, "muted").c_str(),   missing,   _("Missing"));
    if (error > 0) add_stat(SettingsUi::tone_hex(*this, "error").c_str(), error, _("Error"));

    // Explicit total ROM count.
    auto* tot = Gtk::make_managed<Gtk::Label>();
    tot->set_markup("· " + Glib::Markup::escape_text(_("Total")) + " <b>" + std::to_string(total) + "</b>");
    m_stats_box.pack_start(*tot, Gtk::PACK_SHRINK);
    m_stats_box.show_all();

    // Right-aligned summary: "N available / Total".
    /* Le compteur central annonce ce que la liste montre APRES filtrage :
     * c'est la question qu'on se pose en la regardant, alors que le bilan de
     * la barre du bas decrit la collection entiere. */
    {
        const int shown = static_cast<int>(m_model_games->children().size());
        char buf[64];
        std::snprintf(buf, sizeof buf, "%'d", shown);
        std::string n = buf;
        if (n.find('\'') != std::string::npos) n = std::to_string(shown);
        m_center_count.set_text(n + " " + _("games (filtered)"));
    }

    m_summary_label.set_markup("<b>" + std::to_string(available) + "</b> " +
                               Glib::Markup::escape_text(_("available")) +
                               " / <b>" + std::to_string(total) + "</b>");
    m_summary_label.show();
}

// Removed all ComboBox filter handlers - will implement MAMEUI-style filtering

void MainWindow::filter_games() {
    filter_games_simple();
}

void MainWindow::filter_games_async() {
    // For search, use debouncing to avoid excessive filtering
    if (m_search_timeout_connection.connected()) {
        m_search_timeout_connection.disconnect();
    }
    
    m_search_timeout_connection = Glib::signal_timeout().connect([this]() {
        filter_games_simple();
        return false;
    }, 200); // 200ms debounce for search
}

void MainWindow::filter_games_simple() {
    // Use the new tree-based filtering system
    apply_tree_filters();
}

// One gtk_list_store_insert_with_valuesv per game, all nineteen columns at
// once. The row-by-row `row[col] = value` form costs one gtk_list_store_set
// per column, each re-finding the row and re-emitting row-changed : on
// 29 000 games that was ~390 ms of the freeze after every filter, this is
// about a third of it. The model is detached from its view by the callers,
// so nothing repaints in between.
void MainWindow::append_game_rows(const std::vector<const Game*>& games) {
    GtkListStore* store = m_model_games->gobj();
    const int n = m_columns.size();
    std::vector<gint> cols(n);
    std::vector<GValue> vals(n, GValue{});
    for (int i = 0; i < n; ++i) {
        cols[i] = i;
        g_value_init(&vals[i], m_columns.types()[i]);
    }
    auto set_str = [&](const Gtk::TreeModelColumnBase& col, const std::string& v) {
        g_value_set_static_string(&vals[col.index()], v.c_str());   // copied by the store
    };
    // Every string handed to set_str must outlive the insert below : the
    // GValues only borrow the pointer.
    std::string aspect;
    const std::string ranked("\u25cf"), unranked;
    for (const auto* pgame : games) {
        const Game& game = *pgame;
        auto icon = IconManager::get_status_icon(game.status);
        g_value_set_object(&vals[m_columns.m_col_icon.index()], icon ? G_OBJECT(icon->gobj()) : nullptr);
        set_str(m_columns.m_col_emulator, game.emulator);
        g_value_set_boolean(&vals[m_columns.m_col_favorite.index()], game.is_favorite);
        set_str(m_columns.m_col_hiscore, game_ranks_online(game.emulator, game.system, game.name) ? ranked : unranked);
        set_str(m_columns.m_col_last_played, game.last_played);
        set_str(m_columns.m_col_name, game.name);
        set_str(m_columns.m_col_title, game.description);
        set_str(m_columns.m_col_year, game.year);
        set_str(m_columns.m_col_manufacturer, game.manufacturer);
        set_str(m_columns.m_col_system, game.system);
        set_str(m_columns.m_col_status, game.status);   // needed by the detail dock pills
        set_str(m_columns.m_col_video_type, game.video_type);
        set_str(m_columns.m_col_orientation, game.orientation);
        set_str(m_columns.m_col_width, game.width);
        set_str(m_columns.m_col_height, game.height);
        aspect = game.aspect_x + ":" + game.aspect_y;
        set_str(m_columns.m_col_aspect, aspect);
        set_str(m_columns.m_col_driver_status, game.driver_status);
        set_str(m_columns.m_col_comment, game.comment);
        set_str(m_columns.m_col_cloneof, game.cloneof);
        set_str(m_columns.m_col_sourcefile, game.sourcefile);
        gtk_list_store_insert_with_valuesv(store, nullptr, -1, cols.data(), vals.data(), n);
    }
    for (int i = 0; i < n; ++i) g_value_unset(&vals[i]);
}

void MainWindow::apply_filters() {
    // Apply filtered results to TreeView in main thread.
    // Take a snapshot under the lock, then release before touching the TreeView
    // or calling update_status_bar_stats() (which also acquires m_filter_mutex).
    std::vector<const Game*> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_filter_mutex);
        snapshot = m_filtered_games;
    }

    // Detach model + disable sort to avoid one redraw and one re-sort per insert.
    m_treeview_games.unset_model();
    int prev_sort_col = Gtk::TreeSortable::DEFAULT_SORT_COLUMN_ID;
    Gtk::SortType prev_sort_order = Gtk::SORT_ASCENDING;
    bool had_sort = m_model_games->get_sort_column_id(prev_sort_col, prev_sort_order);
    m_model_games->set_sort_column(Gtk::TreeSortable::DEFAULT_UNSORTED_COLUMN_ID, Gtk::SORT_ASCENDING);
    m_model_games->clear();

    append_game_rows(snapshot);

    if (had_sort &&
        prev_sort_col != Gtk::TreeSortable::DEFAULT_SORT_COLUMN_ID &&
        prev_sort_col != Gtk::TreeSortable::DEFAULT_UNSORTED_COLUMN_ID) {
        m_model_games->set_sort_column(prev_sort_col, prev_sort_order);
    }
    m_treeview_games.set_model(m_model_games);

    // Keep the active custom view (list/grid) in sync with the model.
    refresh_active_view();

    update_status_bar_stats();
}

void MainWindow::set_view_mode(bool grid) {
    m_suppress_view_toggle = true;
    m_btn_view_grid.set_active(grid);
    m_btn_view_list.set_active(!grid);
    m_suppress_view_toggle = false;
    // The cards-per-row selector only makes sense over the grid.
    m_zoom_box.set_visible(grid);
    if (grid) rebuild_grid(); else rebuild_mlist(); // build lazily, only when shown
    m_view_stack.set_visible_child(grid ? "grid" : "list");
}

void MainWindow::set_grid_columns(int px) {
    if (px < kMinCardWidth) px = kMinCardWidth;
    else if (px > kMaxCardWidth) px = kMaxCardWidth;
    m_grid_columns = px;               // porte desormais une largeur, en px

    m_suppress_zoom = true;
    m_zoom_scale.set_value(px);
    m_suppress_zoom = false;

    /* La grille ne reclame plus une largeur minimale proportionnelle au
     * nombre de colonnes : elle accepte de descendre a une seule, donc elle
     * ne peut plus repousser le volet de details hors de la fenetre. Le
     * plafond n'est qu'un plafond, atteint seulement si la place le permet.
     */
    m_flowbox.set_min_children_per_line(1);
    m_flowbox.set_max_children_per_line(30);

    save_launch_prefs();
    if (m_view_stack.get_visible_child_name() == "grid") rebuild_grid();
}

void MainWindow::refresh_active_view() {
    const auto v = m_view_stack.get_visible_child_name();
    if (v == "grid")      rebuild_grid();
    else if (v == "list") rebuild_mlist();
}

// Pure, thread-safe: no GTK, no settings access : safe to call from the art
// worker thread. Takes the artwork directories explicitly (captured on the main
// thread when the request is queued).
static std::string resolve_art_path(const std::string& name, const std::string& system,
                                    const std::string& previews_dir,
                                    const std::string& titles_dir) {
    std::string pfx = get_fbneo_system_prefix(system);

    for (const std::string& dir : {previews_dir, titles_dir}) {
        if (dir.empty()) continue;
        std::string path = dir + "/" + pfx + name + ".png";
        std::error_code ec;
        if (std::filesystem::exists(path, ec) && !ec) return path;
    }
    return "";
}

// Drop parenthesised qualifiers such as "(World 910522)" or "(set 1)" so cards
// stay compact; the untouched title is still shown in the detail dock.
static std::string strip_parentheticals(const std::string& s) {
    std::string out;
    int depth = 0;
    for (char c : s) {
        if (c == '(') { ++depth; continue; }
        if (c == ')') { if (depth > 0) --depth; continue; }
        if (depth == 0) out += c;
    }
    // Collapse the whitespace the removed groups left behind.
    std::string clean;
    bool prev_space = false;
    for (char c : out) {
        bool is_space = (c == ' ' || c == '\t');
        if (is_space && (prev_space || clean.empty())) continue;
        clean += c;
        prev_space = is_space;
    }
    while (!clean.empty() && (clean.back() == ' ' || clean.back() == '\t' || clean.back() == '-'))
        clean.pop_back();
    return clean.empty() ? s : clean;
}

Gtk::Widget* MainWindow::make_game_card(const Gtk::TreeModel::Row& row) {
    std::string name   = Glib::ustring(row[m_columns.m_col_name]).raw();
    std::string title  = Glib::ustring(row[m_columns.m_col_title]).raw();
    std::string system = Glib::ustring(row[m_columns.m_col_system]).raw();
    std::string status = Glib::ustring(row[m_columns.m_col_status]).raw();
    bool fav = row[m_columns.m_col_favorite];
    // Show the human title (e.g. "Metal Slug"), not the ROM name ("mslug"),
    // shortened to its base form to keep the card narrow.
    std::string display = strip_parentheticals(title.empty() ? name : title);

    const std::string dot = SettingsUi::tone_hex(*this,
                              status == "available" ? "success"
                            : status == "incorrect" ? "warning"
                            : status == "missing"   ? "disabled" : "muted");

    auto* card = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
    card->get_style_context()->add_class("game-card");

    // Art holder: shows a titled placeholder until the background worker resolves
    // and decodes the PNG and swaps it in (see queue_art / on_art_ready). No disk
    // access happens here on the UI thread.
    auto* art_holder = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
    art_holder->get_style_context()->add_class("card-art");
    art_holder->get_style_context()->add_class("card-art-empty");
    const int card_w = m_grid_columns;              // largeur demandee, en px
    const int card_h = card_w * 3 / 4;              // ratio conserve
    art_holder->set_size_request(card_w, card_h);
    art_holder->set_halign(Gtk::ALIGN_CENTER);
    auto* ph_lbl = Gtk::make_managed<Gtk::Label>(display);
    ph_lbl->set_line_wrap(true);
    ph_lbl->set_justify(Gtk::JUSTIFY_CENTER);
    ph_lbl->set_max_width_chars(16);
    ph_lbl->set_lines(3);
    ph_lbl->set_ellipsize(Pango::ELLIPSIZE_END);
    ph_lbl->set_valign(Gtk::ALIGN_CENTER);
    ph_lbl->set_vexpand(true);
    ph_lbl->get_style_context()->add_class("card-art-title");
    art_holder->pack_start(*ph_lbl, true, true);
    card->pack_start(*art_holder, Gtk::PACK_SHRINK);
    queue_art(art_holder, name, system, Glib::ustring(row[m_columns.m_col_emulator]).raw(),
              card_w, card_h);

    auto* meta = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 2);
    meta->get_style_context()->add_class("card-meta");
    auto* nlbl = Gtk::make_managed<Gtk::Label>();
    nlbl->set_text((fav ? "★ " : "") + display);
    // Wrap long titles onto a second line (then ellipsize) instead of letting the
    // label's natural width stretch the card and collapse the row to two columns.
    nlbl->set_line_wrap(true);
    nlbl->set_line_wrap_mode(Pango::WRAP_WORD_CHAR);
    nlbl->set_lines(2);
    nlbl->set_ellipsize(Pango::ELLIPSIZE_END);
    nlbl->set_max_width_chars(1); // don't request width from text; the cell governs it
    nlbl->set_xalign(0.0f);
    nlbl->get_style_context()->add_class("card-name");
    meta->pack_start(*nlbl, Gtk::PACK_SHRINK);
    auto* slbl = Gtk::make_managed<Gtk::Label>();
    // The ◆ rides on the system line rather than getting a row of its own: a
    // card is 176 px wide, and a second line would push the title out.
    slbl->set_markup("<span foreground=\"" + std::string(dot) + "\">●</span> " +
                     Glib::Markup::escape_text(system) +
                     (game_ranks_online(Glib::ustring(row[m_columns.m_col_emulator]).raw(), system, name)
                        ? std::string("  <span foreground=\"" + SettingsUi::tone_hex(*this, "info") + "\">◆</span>") : ""));
    slbl->set_ellipsize(Pango::ELLIPSIZE_END);
    slbl->set_max_width_chars(1); // let the cell govern width, not the text
    slbl->set_xalign(0.0f);
    slbl->get_style_context()->add_class("card-sys");

    // La marque monte sur la ligne du systeme, comme le ◆ : une ligne de
    // plus pousserait le titre hors de la carte.
    if (show_emulator_marks()) {
        auto* sysrow = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 5);
        if (auto pb = emulator_badge(
                Glib::ustring(row[m_columns.m_col_emulator]).raw(), 26, 12)) {
            auto* badge = Gtk::make_managed<Gtk::Image>(pb);
            badge->set_valign(Gtk::ALIGN_CENTER);
            sysrow->pack_start(*badge, Gtk::PACK_SHRINK);
        }
        sysrow->pack_start(*slbl, Gtk::PACK_EXPAND_WIDGET);
        meta->pack_start(*sysrow, Gtk::PACK_SHRINK);
    } else {
        meta->pack_start(*slbl, Gtk::PACK_SHRINK);
    }
    card->pack_start(*meta, Gtk::PACK_SHRINK);

    return card;
}

// ---- Background cover-art loading (disk I/O + decode never on the UI thread) ----

void MainWindow::start_art_thread() {
    // The dispatcher is created/connected on the main thread; the worker only
    // emit()s it. Start the single worker that services the request queue.
    m_art_dispatcher.connect(sigc::mem_fun(*this, &MainWindow::on_art_ready));
    m_art_thread = std::thread(&MainWindow::art_worker, this);
}

void MainWindow::queue_art(Gtk::Box* holder, const std::string& name,
                           const std::string& system, const std::string& emulator,
                           int w, int h) {
    // Capture the artwork directories here, on the main thread, so the worker
    // never touches the settings panel. Chaque emulateur a ses dossiers : un
    // jeu MAME ne cherche pas ses images chez FinalBurn Neo.
    std::string pdir = m_settings_panel.get_previews_path(emulator);
    std::string tdir = m_settings_panel.get_titles_path(emulator);
    if (pdir.empty() && tdir.empty()) return; // nowhere to look -> keep placeholder

    {
        std::lock_guard<std::mutex> lk(m_art_req_mutex);
        m_art_requests.push_back({m_art_generation.load(), holder, name, system,
                                  std::move(pdir), std::move(tdir), w, h});
    }
    m_art_req_cv.notify_one();
}

void MainWindow::art_worker() {
    for (;;) {
        ArtRequest req;
        {
            std::unique_lock<std::mutex> lk(m_art_req_mutex);
            m_art_req_cv.wait(lk, [this] {
                return m_art_thread_stop.load() || !m_art_requests.empty();
            });
            if (m_art_thread_stop.load()) return;
            req = std::move(m_art_requests.back()); // newest first: what's on screen
            m_art_requests.pop_back();
        }
        // Skip work for requests whose view has already been rebuilt.
        if (req.gen != m_art_generation.load()) continue;

        std::string path = resolve_art_path(req.name, req.system,
                                            req.previews_dir, req.titles_dir);
        if (path.empty()) continue; // no art -> the placeholder stays

        Glib::RefPtr<Gdk::Pixbuf> pix;
        try {
            pix = Gdk::Pixbuf::create_from_file(path, req.w, req.h, true);
        } catch (...) { pix.reset(); }
        if (!pix) continue;

        {
            std::lock_guard<std::mutex> lk(m_art_res_mutex);
            m_art_results.push_back({req.gen, req.holder, std::move(pix)});
        }
        m_art_dispatcher.emit(); // wake the main thread to install the pixbuf
    }
}

void MainWindow::on_art_ready() {
    // Runs on the main thread (serialised with rebuilds), so a result whose gen
    // still matches is guaranteed to reference a live holder widget.
    std::deque<ArtResult> batch;
    {
        std::lock_guard<std::mutex> lk(m_art_res_mutex);
        batch.swap(m_art_results);
    }
    const std::uint64_t gen = m_art_generation.load();
    for (auto& r : batch) {
        if (r.gen != gen || !r.holder) continue; // stale: holder already destroyed
        SettingsUi::destroy_children(*r.holder);
        auto* img = Gtk::make_managed<Gtk::Image>(r.pix);
        img->set_halign(Gtk::ALIGN_CENTER);
        img->set_valign(Gtk::ALIGN_CENTER);
        r.holder->get_style_context()->remove_class("card-art-empty");
        r.holder->add(*img);
        r.holder->show_all();
    }
}

void MainWindow::clear_art_queue() {
    // A rebuild is about to destroy the holder widgets: bump the generation so any
    // in-flight request/result is ignored, and drop everything already queued.
    m_art_generation.fetch_add(1);
    { std::lock_guard<std::mutex> lk(m_art_req_mutex); m_art_requests.clear(); }
    { std::lock_guard<std::mutex> lk(m_art_res_mutex); m_art_results.clear(); }
}

void MainWindow::rebuild_grid() {
    clear_art_queue();
    SettingsUi::destroy_children(m_flowbox);
    m_grid_refs.clear();
    m_grid_built = 0;
    append_grid_batch();
}

void MainWindow::append_grid_batch() {
    if (m_batch_lock) return; // adding widgets re-emits the adjustment signals
    auto children = m_model_games->children();
    const int total = static_cast<int>(children.size());
    if (m_grid_built >= total) return;

    m_batch_lock = true;
    const int end = std::min(m_grid_built + kViewBatch, total);
    auto it = children.begin();
    std::advance(it, m_grid_built);
    for (int i = m_grid_built; i < end; ++i, ++it) {
        m_grid_refs.push_back(Gtk::TreeRowReference(m_model_games, m_model_games->get_path(it)));
        m_flowbox.add(*make_game_card(*it));
    }
    m_grid_built = end;
    m_flowbox.show_all_children();
    m_batch_lock = false;
}

// Build the next batch once the viewport is within ~1.5 pages of the bottom.
// The condition also holds when the content is shorter than the viewport, so a
// short first batch keeps filling until the window is full.
void MainWindow::maybe_extend_grid() {
    auto adj = m_scrolled_grid.get_vadjustment();
    if (!adj) return;
    if (adj->get_value() < adj->get_upper() - adj->get_page_size() * 1.5) return;
    append_grid_batch();
}

void MainWindow::on_grid_selection_changed() {
    auto sel = m_flowbox.get_selected_children();
    if (sel.empty()) return;
    int idx = sel[0]->get_index();
    if (idx < 0 || idx >= static_cast<int>(m_grid_refs.size())) return;
    auto& ref = m_grid_refs[idx];
    if (!ref.is_valid()) return;
    auto iter = m_model_games->get_iter(ref.get_path());
    if (!iter) return;
    // Sync the (hidden) treeview selection for Play, and populate the dock directly
    // so it never depends on the treeview's selection-changed signal firing.
    m_treeview_games.get_selection()->select(iter);
    show_game_details(*iter);
}

void MainWindow::on_grid_child_activated(Gtk::FlowBoxChild* child) {
    if (!child) return;
    int idx = child->get_index();
    if (idx < 0 || idx >= static_cast<int>(m_grid_refs.size())) return;
    auto& ref = m_grid_refs[idx];
    if (!ref.is_valid()) return;
    m_treeview_games.get_selection()->select(ref.get_path());
    on_play_clicked();
}

// ---- Modern list view (styled ListBox rows) ----
Gtk::Widget* MainWindow::make_list_row(const Gtk::TreeModel::Row& row) {
    std::string name   = Glib::ustring(row[m_columns.m_col_name]).raw();
    std::string title  = Glib::ustring(row[m_columns.m_col_title]).raw();
    std::string system = Glib::ustring(row[m_columns.m_col_system]).raw();
    std::string year   = Glib::ustring(row[m_columns.m_col_year]).raw();
    std::string status = Glib::ustring(row[m_columns.m_col_status]).raw();
    bool fav = row[m_columns.m_col_favorite];

    const std::string dot = SettingsUi::tone_hex(*this,
                              status == "available" ? "success"
                            : status == "incorrect" ? "warning"
                            : status == "missing"   ? "disabled" : "muted");
    const char* pill_cls = status == "available" ? "pill-ok"
                         : status == "incorrect" ? "pill-warn" : "pill-muted";
    std::string status_txt = status == "available" ? _("Available")
                           : status == "incorrect" ? _("Incorrect")
                           : status == "missing"   ? _("Missing") : status;

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    box->get_style_context()->add_class("mlist-row");

    // Thumbnail holder: tinted placeholder until the worker swaps in the PNG.
    auto* thumb = Gtk::make_managed<Gtk::Box>();
    thumb->get_style_context()->add_class("mlist-thumb");
    thumb->get_style_context()->add_class("card-art-empty");
    thumb->set_size_request(kColThumb, 39);
    thumb->set_valign(Gtk::ALIGN_CENTER);
    thumb->set_margin_end(6);   // de l'air entre l'image et le titre
    box->pack_start(*thumb, Gtk::PACK_SHRINK);
    queue_art(thumb, name, system, Glib::ustring(row[m_columns.m_col_emulator]).raw(), 52, 39);

    // Name + subtitle.
    auto* nb = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
    nb->set_valign(Gtk::ALIGN_CENTER);
    std::string display = title.empty() ? name : title; // human title, not ROM name
    auto* nl = Gtk::make_managed<Gtk::Label>();
    nl->set_markup(std::string(fav ? "★ " : "") + "<b>" + escape_markup(display) + "</b>");
    nl->set_xalign(0.0f);
    nl->set_ellipsize(Pango::ELLIPSIZE_END);
    auto* sl = Gtk::make_managed<Gtk::Label>(name); // ROM name as the discreet subtitle
    sl->set_xalign(0.0f);
    sl->set_ellipsize(Pango::ELLIPSIZE_END);
    sl->get_style_context()->add_class("mlist-sub");
    nb->pack_start(*nl, Gtk::PACK_SHRINK);

    /* La marque va sur la ligne du nom de ROM, pas dans une colonne a elle :
     * l'en-tete de la liste est bati a la main sur des largeurs fixes, et une
     * colonne de plus l'aurait desaligne de toutes les lignes. */
    if (show_emulator_marks()) {
        auto* subrow = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 5);
        if (auto pb = emulator_badge(
                Glib::ustring(row[m_columns.m_col_emulator]).raw(), 28, 13)) {
            auto* badge = Gtk::make_managed<Gtk::Image>(pb);
            badge->set_valign(Gtk::ALIGN_CENTER);
            subrow->pack_start(*badge, Gtk::PACK_SHRINK);
        }
        subrow->pack_start(*sl, Gtk::PACK_EXPAND_WIDGET);
        nb->pack_start(*subrow, Gtk::PACK_SHRINK);
    } else {
        nb->pack_start(*sl, Gtk::PACK_SHRINK);
    }
    box->pack_start(*nb, Gtk::PACK_EXPAND_WIDGET);

    // Emulateur. Seulement quand les deux catalogues sont melanges : sinon la
    // colonne repeterait la meme valeur sur toutes les lignes.
    if (show_emulator_marks()) {
        auto* el = Gtk::make_managed<Gtk::Label>(
            EmulatorRegistry::display_name(
                Glib::ustring(row[m_columns.m_col_emulator]).raw()));
        el->set_xalign(0.0f);
        el->set_size_request(kColEmu, -1);
        el->set_ellipsize(Pango::ELLIPSIZE_END);
        el->get_style_context()->add_class("mlist-sys");
        el->set_valign(Gtk::ALIGN_CENTER);
        box->pack_start(*el, Gtk::PACK_SHRINK);
    }

    // System.
    auto* syl = Gtk::make_managed<Gtk::Label>(system);
    syl->set_xalign(0.0f);
    syl->set_size_request(kColSystem, -1);
    syl->set_ellipsize(Pango::ELLIPSIZE_END);
    syl->get_style_context()->add_class("mlist-sys");
    syl->set_valign(Gtk::ALIGN_CENTER);
    box->pack_start(*syl, Gtk::PACK_SHRINK);

    // Year.
    auto* yl = Gtk::make_managed<Gtk::Label>(year);
    yl->set_xalign(0.0f);
    yl->set_size_request(kColYear, -1);
    yl->get_style_context()->add_class("mlist-year");
    yl->set_valign(Gtk::ALIGN_CENTER);
    box->pack_start(*yl, Gtk::PACK_SHRINK);

    /* Etat de la ROM : une pastille, jamais le mot.
     *
     * « Available » repete sur 29 461 lignes n'apprend rien et mangeait la
     * largeur du titre, qui est ce qu'on lit. La couleur suffit, la colonne
     * s'appelle STATUS, et l'infobulle donne le mot pour qui en doute.
     */
    auto* pill = Gtk::make_managed<Gtk::Label>();
    pill->set_markup("<span foreground=\"" + dot + "\">\u25CF</span>");
    pill->set_size_request(kColStatus, -1);
    pill->set_valign(Gtk::ALIGN_CENTER);
    pill->set_tooltip_text(status_txt);
    box->pack_start(*pill, Gtk::PACK_SHRINK);
    (void)pill_cls;

    // Colonne HS : un trophee, ou un tiret. Toujours presente, donc alignee.
    auto* hs = Gtk::make_managed<Gtk::Label>();
    hs->set_size_request(kColHs, -1);
    hs->set_valign(Gtk::ALIGN_CENTER);
    if (game_ranks_online(Glib::ustring(row[m_columns.m_col_emulator]).raw(), system, name)) {
        hs->set_text("\U0001F3C6");
        hs->set_tooltip_text(_("This game's scores can be ranked online."));
    } else {
        hs->set_text("\u2014");
        hs->get_style_context()->add_class("hs-none");
    }
    box->pack_start(*hs, Gtk::PACK_SHRINK);

    return box;
}


void MainWindow::rebuild_mlist() {
    clear_art_queue();
    SettingsUi::destroy_children(m_mlist);
    m_mlist_refs.clear();
    m_mlist_built = 0;
    append_mlist_batch();
}

void MainWindow::append_mlist_batch() {
    if (m_batch_lock) return; // adding widgets re-emits the adjustment signals
    auto children = m_model_games->children();
    const int total = static_cast<int>(children.size());
    if (m_mlist_built >= total) return;

    m_batch_lock = true;
    const int end = std::min(m_mlist_built + kViewBatch, total);
    auto it = children.begin();
    std::advance(it, m_mlist_built);
    for (int i = m_mlist_built; i < end; ++i, ++it) {
        m_mlist.add(*make_list_row(*it));
        m_mlist_refs.push_back(Gtk::TreeRowReference(m_model_games, m_model_games->get_path(it)));
    }
    m_mlist_built = end;
    m_mlist.show_all_children();
    m_batch_lock = false;
}

void MainWindow::maybe_extend_mlist() {
    auto adj = m_scrolled_mlist.get_vadjustment();
    if (!adj) return;
    if (adj->get_value() < adj->get_upper() - adj->get_page_size() * 1.5) return;
    append_mlist_batch();
}

void MainWindow::on_mlist_row_selected(Gtk::ListBoxRow* row) {
    if (!row) return;
    int idx = row->get_index();
    if (idx < 0 || idx >= static_cast<int>(m_mlist_refs.size())) return;
    auto& ref = m_mlist_refs[idx];
    if (!ref.is_valid()) return;
    auto iter = m_model_games->get_iter(ref.get_path());
    if (!iter) return;
    m_treeview_games.get_selection()->select(iter);
    show_game_details(*iter); // populate the dock directly (deterministic)
}

void MainWindow::on_mlist_row_activated(Gtk::ListBoxRow* row) {
    if (!row) return;
    int idx = row->get_index();
    if (idx < 0 || idx >= static_cast<int>(m_mlist_refs.size())) return;
    auto& ref = m_mlist_refs[idx];
    if (!ref.is_valid()) return;
    m_treeview_games.get_selection()->select(ref.get_path());
    on_play_clicked();
}

void MainWindow::configure_columns() {
    // Get all columns to configure them
    auto columns = m_treeview_games.get_columns();

    // Map each VIEW column (in append order) to its backing MODEL column. The view
    // omits the model's `status` column, so a view index is NOT the same as its
    // model column id : deriving the sort id from the loop index made "Type" and
    // every column after it sort by the wrong data (status, etc.).
    //
    // La table se lit donc dans l'ordre exact des append_column/insert_column
    // ci-dessus, en-tete par en-tete. Toute colonne inseree ailleurs qu'a la
    // fin decale tout ce qui suit : c'est arrive avec « HI », ajoutee en
    // troisieme position sans que la table bouge, et « Name » triait alors
    // par titre, « Title » par annee, jusqu'a « Source » qui ne triait plus
    // du tout.
    const std::vector<const Gtk::TreeModelColumnBase*> sort_cols = {
        nullptr,                        // 0  icon (not sortable)
        &m_columns.m_col_favorite,      // 1  ★
        &m_columns.m_col_hiscore,       // 2  HI
        &m_columns.m_col_name,          // 3  Name
        &m_columns.m_col_title,         // 4  Title
        &m_columns.m_col_emulator,      // 5  Emulator
        &m_columns.m_col_year,          // 6  Year
        &m_columns.m_col_manufacturer,  // 7  Manufacturer
        &m_columns.m_col_system,        // 8  System
        &m_columns.m_col_video_type,    // 9  Type
        &m_columns.m_col_orientation,   // 10 Orientation
        &m_columns.m_col_width,         // 11 Width
        &m_columns.m_col_height,        // 12 Height
        &m_columns.m_col_aspect,        // 13 Aspect
        &m_columns.m_col_driver_status, // 14 Driver
        &m_columns.m_col_comment,       // 15 Comment
        &m_columns.m_col_cloneof,       // 16 Clone
        &m_columns.m_col_sourcefile,    // 17 Source
    };

    // Configure each column with proper sorting and sizing
    for (size_t i = 0; i < columns.size(); ++i) {
        auto column = columns[i];
        
        // Disable reordering to prevent GTK layout issues
        column->set_reorderable(false);
        
        // Enable resizing
        column->set_resizable(true);
        
        // Enable sorting (clickable headers)
        column->set_clickable(true);

        // Bind the header to the CORRECT model column so it sorts its own data.
        if (i < sort_cols.size() && sort_cols[i] != nullptr) {
            column->set_sort_column(*sort_cols[i]);
        }
        
        // Set minimum width and default sizing for better readability
        if (column->get_title() == " ") {
            // Icon column - fixed smaller width, no sorting
            column->set_min_width(40);
            column->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
            column->set_fixed_width(40);
            column->set_resizable(false);
            column->set_clickable(false);  // Icons shouldn't be sortable
        } else if (column->get_title() == "Name") {
            column->set_min_width(80);
            column->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
            column->set_fixed_width(100);
        } else if (column->get_title() == "Title") {
            column->set_min_width(50);
            column->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
            column->set_fixed_width(120);
        } else if (column->get_title() == "System") {
            column->set_min_width(60);
            column->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
            column->set_fixed_width(80);
        } else if (column->get_title() == "Year") {
            column->set_min_width(45);
            column->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
            column->set_fixed_width(50);
        } else if (column->get_title() == _("Emulator")) {
            // Assez large pour la marque ET le nom : « FinalBurn Neo » tronque
            // en « FinalBu... » n'aurait rien dit de plus que la marque seule.
            column->set_min_width(90);
            column->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
            column->set_fixed_width(150);
        } else if (column->get_title() == "Manufacturer") {
            column->set_min_width(80);
            column->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
            column->set_fixed_width(100);
        } else if (column->get_title() == "Orientation") {
            column->set_min_width(60);
            column->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
            column->set_fixed_width(70);
        } else if (column->get_title() == "Width" || column->get_title() == "Height") {
            column->set_min_width(40);
            column->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
            column->set_fixed_width(50);
        } else if (column->get_title() == "Driver") {
            column->set_min_width(50);
            column->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
            column->set_fixed_width(60);
        } else if (column->get_title() == "Comment") {
            column->set_min_width(80);
            column->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
            column->set_fixed_width(100);
        } else if (column->get_title() == "Source") {
            column->set_min_width(100);
            column->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
            column->set_fixed_width(120);
        } else {
            // Other columns (Type, Aspect, Clone)
            column->set_min_width(50);
            column->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
            column->set_fixed_width(60);
        }
        
        // Enable sorting indicators only for sortable columns
        if (column->get_clickable()) {
            column->set_sort_indicator(true);
        }
    }
    
    // Set TreeView properties for better interaction
    m_treeview_games.set_reorderable(false);  // Disable row reordering (causes issues)
    m_treeview_games.set_headers_clickable(true);  // Make headers clickable for sorting
    m_treeview_games.set_headers_visible(true);   // Ensure headers are visible
    m_treeview_games.set_enable_search(true);     // Enable Ctrl+F search
    m_treeview_games.set_search_column(m_columns.m_col_name);  // Search by name by default
    
    // Enable grid lines for better readability
    m_treeview_games.set_grid_lines(Gtk::TREE_VIEW_GRID_LINES_BOTH);
}

std::string MainWindow::escape_markup(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size() + 16);
    for (char c : text) {
        switch (c) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;";  break;
            case '>': escaped += "&gt;";  break;
            default:  escaped += c;       break;
        }
    }
    return escaped;
}

// === Menu Handlers ===

void MainWindow::on_export_game_list() {
    // Export game list to CSV/JSON
    Gtk::FileChooserDialog dialog(*this, "Export Game List", Gtk::FILE_CHOOSER_ACTION_SAVE);
    dialog.set_transient_for(*this);
    dialog.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dialog.add_button(_("Save"), Gtk::RESPONSE_OK);
    
    auto filter_csv = Gtk::FileFilter::create();
    filter_csv->set_name("CSV files");
    filter_csv->add_pattern("*.csv");
    dialog.add_filter(filter_csv);
    
    auto filter_json = Gtk::FileFilter::create();
    filter_json->set_name("JSON files");
    filter_json->add_pattern("*.json");
    dialog.add_filter(filter_json);
    
    if (dialog.run() == Gtk::RESPONSE_OK) {
        std::string filename = dialog.get_filename();
        // TODO: Implement actual export functionality
        std::cout << "Exporting game list to: " << filename << std::endl;
    }
}

void MainWindow::on_fbneo_menu() {
    // Launch FBNeo with menu
    std::string fbneo_executable = m_settings_panel.get_fbneo_executable();
    std::cout << "Opening FBNeo menu: " << fbneo_executable << std::endl;
    spawn_process({fbneo_executable, "-menu"});
}

void MainWindow::on_video_settings() {
    // Open video settings dialog
    SettingsUi::notice(*this, _("Video Settings"),
                       _("Video settings are configured within FBNeo.\nUse 'Emulator > Open FBNeo Menu' to access them."),
                       "bc-info.svg");
}

void MainWindow::on_audio_settings() {
    // Open audio settings dialog
    SettingsUi::notice(*this, _("Audio Settings"),
                       _("Audio settings are configured within FBNeo.\nUse 'Emulator > Open FBNeo Menu' to access them."),
                       "bc-info.svg");
}

void MainWindow::on_input_settings() {
    std::string cfg_path = AppContext::get_config_path();
    // Always reload from file so dialog reflects any external changes
    ControllerManager::load_profiles(m_controller_profiles, m_active_controller_profile, cfg_path);

    if (m_controller_win) { m_controller_win->present(); return; }

    // Fenetre independante, non modale : on doit pouvoir la poser sur un
    // second ecran et continuer a parcourir la bibliotheque a cote.
    auto* dlg = new ControllerDialog(m_controller_profiles, m_active_controller_profile, cfg_path);
    present_controller_dialog(dlg, cfg_path);
}

void MainWindow::on_game_controls() {
    auto iter = m_treeview_games.get_selection()->get_selected();
    if (!iter) return;
    Gtk::TreeModel::Row row = *iter;
    std::string name   = Glib::ustring(row[m_columns.m_col_name]).raw();
    std::string system = Glib::ustring(row[m_columns.m_col_system]).raw();
    std::string title  = Glib::ustring(row[m_columns.m_col_title]).raw();
    // Un jeu MAME a sa propre cle : son sf2 n'est pas celui de FinalBurn Neo.
    const bool mame = Glib::ustring(row[m_columns.m_col_emulator]).raw() == "mame";
    const std::string rom = mame ? MameControls::profile_key(name)
                                 : get_fbneo_system_prefix(system) + name;

    std::string cfg_path = AppContext::get_config_path();
    ControllerManager::load_profiles(m_controller_profiles, m_active_controller_profile, cfg_path);
    // Une seule fenetre de manettes a la fois : deux ecrans qui editent les
    // memes profils s'ecraseraient l'un l'autre au premier Save.
    if (m_controller_win) { m_controller_win->present(); return; }

    auto assigned = ControllerManager::load_game_profiles(cfg_path);
    auto it = assigned.find(rom);
    auto* dlg = new ControllerDialog(m_controller_profiles, m_active_controller_profile, cfg_path);
    dlg->set_game_scope(rom, title, it == assigned.end() ? "" : it->second, mame);
    present_controller_dialog(dlg, cfg_path);
}

const ControllerConfig* MainWindow::controller_profile_for(const std::string& fbneo_rom_name) const {
    auto assigned = ControllerManager::load_game_profiles(AppContext::get_config_path());
    auto it = assigned.find(fbneo_rom_name);
    // Un profil assigne puis supprime ne doit pas laisser le jeu sans rien :
    // on retombe sur le defaut, comme s'il n'avait jamais ete assigne.
    if (it != assigned.end() && m_controller_profiles.count(it->second))
        return &m_controller_profiles.at(it->second);
    if (m_controller_profiles.count(m_active_controller_profile))
        return &m_controller_profiles.at(m_active_controller_profile);
    return nullptr;
}

void MainWindow::present_controller_dialog(ControllerDialog* dlg, const std::string& cfg_path) {
    m_controller_win = dlg;
    dlg->set_modal(false);
    /* La destruction attend la fin du traitement de l'evenement.
     *
     * La fenetre porte sa propre barre de titre, donc son bouton de
     * fermeture est un widget QU'ELLE CONTIENT. Detruire le dialogue depuis
     * son gestionnaire de fermeture liberait le widget dont GTK etait en
     * train de traiter le clic, et l'application tombait avec lui. On relit
     * les profils tout de suite, mais on rend la memoire au tour de boucle
     * suivant, quand plus personne ne tient la fenetre.
     */
    auto released = std::make_shared<bool>(false);
    dlg->signal_hide().connect([this, dlg, cfg_path, released]() {
        if (*released) return;   // hide peut etre emis plusieurs fois
        *released = true;
        // Les profils peuvent avoir change : on relit avant de lacher.
        ControllerManager::load_profiles(m_controller_profiles,
                                         m_active_controller_profile, cfg_path);
        m_controller_win = nullptr;
        Glib::signal_idle().connect_once([dlg] { delete dlg; });
    });
    dlg->show_all();
    dlg->present();
}

/* Le menu et l'ecran des reglages commandent le MEME reglage.
 *
 * Ils ne peuvent donc pas garder chacun sa valeur : celui qui bouge previent
 * l'autre tout de suite. Sans cela, cocher « plein ecran » dans le menu
 * laissait l'interrupteur des reglages eteint, et le premier enregistrement
 * depuis les reglages annulait le choix fait au menu.
 */
void MainWindow::on_fullscreen_mode() {
    m_launch_fullscreen = m_menu_item_fullscreen_mode.get_active();
    // Vient des reglages : eux decideront quoi ecrire, et quand.
    if (m_syncing_launch_menu) return;
    m_settings_panel.set_launch_flags(m_launch_fullscreen, m_launch_integerscale);
    save_launch_prefs();
}

void MainWindow::on_integerscale_mode() {
    m_launch_integerscale = m_menu_item_integerscale_mode.get_active();
    if (m_syncing_launch_menu) return;
    m_settings_panel.set_launch_flags(m_launch_fullscreen, m_launch_integerscale);
    save_launch_prefs();
}

void MainWindow::on_arcade_mode() {
    // Filter to show only arcade systems
    m_active_filters.clear();
    m_active_filters["system"] = "Arcade";
    apply_tree_filters();
    std::cout << "Filtering to arcade systems only" << std::endl;
}

void MainWindow::on_console_mode() {
    // Filter to show only console systems (all non-arcade systems)
    m_active_filters.clear();
    // Set a special filter for console mode - will exclude arcade
    m_active_filters["mode"] = "console";
    apply_tree_filters();
    std::cout << "Filtering to console systems only" << std::endl;
}

/* Un jeu au hasard.
 *
 * Deux reservoirs : ce qui est affiche (les filtres et la recherche font
 * foi), ou les systemes coches dans les reglages. Dans les deux cas on ne
 * tire que du jouable : une ROM absente n'est pas une proposition. Le jeu
 * tire est SELECTIONNE dans la liste, pas seulement affiche : Play, les
 * favoris et le menu « ... » lisent la selection, et c'est elle qui fait
 * qu'un tirage se joue comme n'importe quel autre choix. */
void MainWindow::on_random_game_clicked() {
    const auto opt = m_settings_panel.random_pick();
    auto eligible = [&](const Game& g) {
        if (g.status != "available" || g.is_bios) return false;
        if (opt.hiscore_only   && !game_ranks_online(g.emulator, g.system, g.name)) return false;
        if (opt.originals_only && !g.cloneof.empty()) return false;
        if (opt.unplayed_only  && g.play_count > 0) return false;
        if (!opt.from_shown && !opt.systems.empty() &&
            !opt.systems.count(g.emulator + "/" + g.system)) return false;
        return true;
    };

    static std::mt19937 rng{std::random_device{}()};
    // L'emulateur fait partie de l'identite : '88games existe chez FinalBurn
    // Neo et chez MAME, tous deux en « Arcade ».
    std::string pick_name, pick_system, pick_emulator;
    int pick_index = -1;   // ligne du modele quand on tire dans l'affiche
    if (opt.from_shown) {
        std::vector<int> idx;
        {
            std::lock_guard<std::mutex> lock(m_filter_mutex);
            for (size_t i = 0; i < m_filtered_games.size(); ++i)
                if (eligible(*m_filtered_games[i])) idx.push_back((int)i);
            if (!idx.empty()) {
                pick_index  = idx[std::uniform_int_distribution<size_t>(0, idx.size() - 1)(rng)];
                pick_name   = m_filtered_games[pick_index]->name;
                pick_system = m_filtered_games[pick_index]->system;
                pick_emulator = m_filtered_games[pick_index]->emulator;
            }
        }
    } else {
        std::vector<const Game*> pool;
        for (const auto& g : m_cached_games) if (eligible(g)) pool.push_back(&g);
        if (!pool.empty()) {
            const Game* g = pool[std::uniform_int_distribution<size_t>(0, pool.size() - 1)(rng)];
            pick_name = g->name; pick_system = g->system; pick_emulator = g->emulator;
        }
    }
    if (pick_name.empty()) {
        std::lock_guard<std::mutex> lock(m_hiscore_result_mutex);
        m_hiscore_results.push_back(_("No game matches the random pick settings."));
        m_hiscore_result_dispatcher.emit();
        return;
    }

    // Tire hors de l'affiche : la liste est ramenee a « tout » pour que la
    // ligne existe, sinon Play n'aurait rien a lire.
    if (pick_index < 0) {
        std::lock_guard<std::mutex> lock(m_filter_mutex);
        for (size_t i = 0; i < m_filtered_games.size(); ++i)
            if (m_filtered_games[i]->name == pick_name && m_filtered_games[i]->system == pick_system &&
                m_filtered_games[i]->emulator == pick_emulator) { pick_index = (int)i; break; }
    }
    if (pick_index < 0) {
        m_search_entry.set_text("");
        if (m_search_timeout_connection.connected()) m_search_timeout_connection.disconnect();
        m_active_filters.clear();
        apply_tree_filters();
        std::lock_guard<std::mutex> lock(m_filter_mutex);
        for (size_t i = 0; i < m_filtered_games.size(); ++i)
            if (m_filtered_games[i]->name == pick_name && m_filtered_games[i]->system == pick_system &&
                m_filtered_games[i]->emulator == pick_emulator) { pick_index = (int)i; break; }
    }
    if (pick_index < 0) return;

    Gtk::TreeModel::Path path; path.push_back(pick_index);
    auto it = m_model_games->get_iter(path);
    if (!it) return;
    reveal_model_row(it);
    std::cout << "[RANDOM] " << pick_emulator << "/" << pick_system << "/" << pick_name << (opt.launch ? " (launch)" : "") << std::endl;
    if (opt.launch) on_play_clicked();
}

void MainWindow::reveal_model_row(const Gtk::TreeModel::iterator& it) {
    if (auto sel = m_treeview_games.get_selection()) sel->select(it);
    show_game_details(*it);
    const auto path = m_model_games->get_path(it);
    m_treeview_games.scroll_to_row(path, 0.5f);
    for (size_t i = 0; i < m_mlist_refs.size(); ++i) {
        if (!m_mlist_refs[i] || m_mlist_refs[i].get_path() != path) continue;
        if (auto* r = m_mlist.get_row_at_index(static_cast<int>(i))) {
            m_mlist.select_row(*r);
            r->grab_focus();
        }
        break;
    }
}

void MainWindow::on_all_systems() {
    // Show all systems - clear all filters
    m_active_filters.clear();
    apply_tree_filters();
    std::cout << "Showing all systems" << std::endl;
}

void MainWindow::on_rescan_roms() {
    // Trigger ROM rescan
    on_start_scan_clicked();
}

void MainWindow::on_verify_roms() {
    // Verify ROM integrity
    SettingsUi::notice(*this, _("ROM Verification"),
                       _("ROM verification will be implemented in a future version.\nCurrently, the scan process validates ROM CRC checksums."),
                       "bc-info.svg");
}

void MainWindow::on_show_available_only() {
    // Filter to show only available ROMs
    m_active_filters.clear();
    m_active_filters["status"] = "available";
    apply_tree_filters();
    std::cout << "Filtering to show available ROMs only" << std::endl;
}

void MainWindow::on_show_missing_roms() {
    // Filter to show missing ROMs
    m_active_filters.clear();
    m_active_filters["status"] = "missing";
    apply_tree_filters();
    std::cout << "Filtering to show missing ROMs only" << std::endl;
}

void MainWindow::on_rom_info() {
    // Show ROM information for selected game
    auto selection = m_treeview_games.get_selection();
    if (selection) {
        auto row = selection->get_selected();
        if (row) {
            Glib::ustring name = row->get_value(m_columns.m_col_name);
            Glib::ustring system = row->get_value(m_columns.m_col_system);
            Glib::ustring status = row->get_value(m_columns.m_col_status);
            
            std::string info = _("Game: ") + name.raw() + "\n";
            info += _("System: ") + system.raw() + "\n";
            info += _("Status: ") + status.raw();
            
            SettingsUi::notice(*this, _("ROM Information"),
                               info,
                               "bc-info.svg");
        }
    }
}

void MainWindow::on_about_fbneo() {
    // Create a custom dialog with buttons for documentation
    Gtk::Dialog dialog;
    dialog.set_transient_for(*this);
    dialog.set_modal(true);
    dialog.set_default_size(560, 420);
    dialog.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);
    SettingsUi::window_header(dialog, "bc-emu-fbneo.svg", _("About FinalBurn Neo"),
                              _("A powerful arcade and console emulator"),
                              [&dialog] { dialog.response(Gtk::RESPONSE_CLOSE); });

    // Get FBNeo directory path
    std::string fbneo_executable = m_settings_panel.get_fbneo_executable();
    std::filesystem::path fbneo_path(fbneo_executable);
    std::string fbneo_dir = fbneo_path.parent_path().string();

    // Create content. Le corps porte les marges ; le pied, lui, touche les
    // bords de la fenetre, comme dans tous les autres ecrans.
    auto dialog_content = dialog.get_content_area();
    dialog_content->set_spacing(0);
    auto content_area = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 8));
    content_area->set_margin_start(22);
    content_area->set_margin_end(22);
    content_area->set_margin_top(18);
    content_area->set_margin_bottom(18);
    dialog_content->pack_start(*content_area, Gtk::PACK_EXPAND_WIDGET);

    auto label = Gtk::manage(new Gtk::Label());
    std::string info_text;

    info_text += Glib::Markup::escape_text(_(
        "The launcher doesn't run the official Linux build: FinalBurn Neo's own team no "
        "longer maintains the SDL2/Linux port, which had quietly stopped generating game "
        "lists for some systems (Game Boy Advance, Bally Astrocade). We track their "
        "upstream repository daily and publish our own build with that fixed, so every "
        "system is covered."));
    info_text += "\n\n";

    info_text += "<b>" + Glib::Markup::escape_text(_("Configured executable:")) + "</b> "
               + Glib::Markup::escape_text(fbneo_executable.empty() ? _("(not set)") : fbneo_executable) + "\n";

    if (!fbneo_executable.empty()) {
        std::time_t mtime = get_file_mtime(fbneo_executable);
        if (mtime >= 0) {
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d", std::localtime(&mtime));
            info_text += "<b>" + Glib::Markup::escape_text(_("Installed build date:")) + "</b> " + buf + "\n";
        }
    }

    if (!m_fbneo_update_tag.empty()) {
        info_text += "<b>" + Glib::Markup::escape_text(_("Latest available fork build:")) + "</b> "
                   + Glib::Markup::escape_text(m_fbneo_update_tag);
        if (!m_fbneo_update_sha.empty())
            info_text += " (" + m_fbneo_update_sha.substr(0, 8) + ")";
        info_text += "\n";
    }

    label->set_markup(info_text);
    label->set_line_wrap(true);
    label->set_justify(Gtk::JUSTIFY_LEFT);
    content_area->pack_start(*label, Gtk::PACK_SHRINK);

    // Add buttons for documentation
    auto button_box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 5));
    button_box->set_margin_top(14);

    // Our fork : the build the launcher actually downloads/runs.
    auto fork_button = SettingsUi::button(_("Our Linux fork (code)"), "bc-external.svg");
    fork_button->signal_clicked().connect([this]() {
        spawn_process({"xdg-open", "https://github.com/battousai90/FBNeo"});
    });
    button_box->pack_start(*fork_button);

    // Official upstream project : for general documentation/credits.
    auto github_button = SettingsUi::button(_("Official FinalBurn Neo (upstream)"), "bc-globe.svg");
    github_button->signal_clicked().connect([this]() {
        spawn_process({"xdg-open", "https://github.com/finalburnneo/FBNeo"});
    });
    button_box->pack_start(*github_button);

    // Download the latest fork build directly from this dialog.
    auto download_button = SettingsUi::button(_("Download latest build"), "bc-download.svg", SettingsUi::Tone::Accent);
    download_button->signal_clicked().connect([this]() {
        on_download_latest_fbneo();
    });
    button_box->pack_start(*download_button);

    // License button
    auto license_button = SettingsUi::button(_("View License"), "bc-file.svg");
    license_button->signal_clicked().connect([fbneo_dir]() {
        std::string license_path = fbneo_dir + "/license.txt";
        if (std::filesystem::exists(license_path)) {
            spawn_process({"xdg-open", license_path});
        }
    });
    button_box->pack_start(*license_button);
    
    // What's New button
    auto whatsnew_button = SettingsUi::button(_("What's New"), "bc-info.svg");
    whatsnew_button->signal_clicked().connect([fbneo_dir]() {
        std::string whatsnew_path = fbneo_dir + "/whatsnew.html";
        if (std::filesystem::exists(whatsnew_path)) {
            spawn_process({"xdg-open", whatsnew_path});
        }
    });
    button_box->pack_start(*whatsnew_button);
    
    // Help file button
    auto help_button = SettingsUi::button(_("Help Documentation"), "bc-info.svg");
    help_button->signal_clicked().connect([fbneo_dir]() {
        std::string help_path = fbneo_dir + "/fbneo.chm";
        if (std::filesystem::exists(help_path)) {
            spawn_process({"xdg-open", help_path});
        }
    });
    button_box->pack_start(*help_button);
    
    content_area->pack_start(*button_box);

    // Le pied de la charte : la sortie en bas a droite.
    auto* foot = SettingsUi::footer();
    auto* close_btn = SettingsUi::button(_("Close"), "", SettingsUi::Tone::Accent);
    close_btn->set_size_request(96, -1);
    close_btn->signal_clicked().connect([&dialog] { dialog.response(Gtk::RESPONSE_CLOSE); });
    foot->pack_end(*close_btn, Gtk::PACK_SHRINK);

    dialog_content->pack_start(*foot, Gtk::PACK_SHRINK);

    dialog.show_all();
    dialog.run();
}

void MainWindow::on_controls_help() {
    // Show game controls help
    std::string help_text = _("Common Game Controls:") + std::string("\n\n");
    help_text += _("Arrow Keys: Movement") + std::string("\n");
    help_text += _("Z, X, C, V: Action buttons") + std::string("\n");
    help_text += _("1, 2: Start Player 1/2") + std::string("\n");
    help_text += _("5, 6: Insert Coin") + std::string("\n");
    help_text += _("F3: Reset Game") + std::string("\n");
    help_text += _("F5: Configure Controls") + std::string("\n");
    help_text += _("ESC: Exit Game") + std::string("\n\n");
    help_text += _("For system-specific controls, refer to the game's documentation.");
    
    SettingsUi::notice(*this, _("Game Controls"), help_text, "bc-controller.svg");
}

void MainWindow::on_about_launcher() {
    // Show launcher information
#ifdef BOOTCADE_VERSION
    std::string about_text = "Bootcade " BOOTCADE_VERSION "\n\n";
#else
    std::string about_text = "Bootcade\n\n";
#endif
    about_text += _("A modern launcher for FinalBurn Neo emulator") + std::string("\n");
    about_text += _("Supporting multiple arcade and console systems") + std::string("\n\n");
    about_text += _("Features:") + std::string("\n");
    about_text += _("• Multi-system ROM management") + std::string("\n");
    about_text += _("• Advanced filtering and search") + std::string("\n");
    about_text += _("• ROM status tracking") + std::string("\n");
    about_text += _("• Game thumbnails and details") + std::string("\n");
    about_text += _("• FBNeo integration");
    
    SettingsUi::notice(*this, _("About Bootcade"), about_text, "bc-logo-pad.svg");
}

void MainWindow::check_fbneo_update_async() {
    std::thread([this]() {
        auto r = FbneoUpdateCheck::fetch_latest();
        if (r.ok) {
            m_fbneo_update_sha = r.sha;
            m_fbneo_update_tag = r.tag;
            m_fbneo_update_published_at = r.published_at;
        }
        m_fbneo_update_dispatcher.emit();
    }).detach();
}

// ── Online scores ──────────────────────────────────────────────────────
bool MainWindow::game_ranks_online(const std::string& emulator, const std::string& system, const std::string& game) {
    // The score service reads FinalBurn Neo's score tables : a MAME set of
    // the same name and system ('Arcade/1941') is not ranked.
    if (emulator != "fbneo") return false;
    std::lock_guard<std::mutex> lock(m_hiscore_supported_mutex);
    return m_hiscore_supported.count(HiscoreClient::key(system, game)) > 0;
}

// Asked once, at the first launch that has no recorded answer. Everything it
// needs is already decided by then : the name is generated and the country
// comes from the locale, so the question is a single yes or no rather than a
// form, and both values stay editable in Settings afterwards.
void MainWindow::ask_hiscore_optin() {
    /* La question disait « No account is needed », ce qui etait vrai avant
     * que les scores ne soient rattaches a un compte. Depuis, le service
     * refuse tout envoi sans jeton : le joueur repondait donc oui, jouait, et
     * ses records s'empilaient dans une file locale en attendant une
     * connexion que rien ne lui demandait jamais. C'est ce mensonge, et pas
     * une panne, qui expliquait un classement vide alors que le service
     * recevait du trafic tous les jours.
     *
     * La question mene donc au compte au lieu de se contenter d'un
     * interrupteur : le bouton d'accord OUVRE la connexion. Refuser reste un
     * choix entier, et n'est pas represente.
     */
    const bool publish = SettingsUi::offer(*this, _("Publish your scores?"),
        _("Your highscores and play time can appear on the public "
          "leaderboard, under your Bootcade name. It takes a free account, "
          "which is also what proves a record is yours. You can turn this "
          "off again at any time in Settings."),
        _("Sign in"), "bc-trophy.svg", "bc-account.svg");
    // Recorded either way: a no is an answer, and asking again at every launch
    // would turn a question into nagging.
    m_settings_panel.record_hiscore_answer(publish);
    // La reponse a ete donnee a la question QUI PARLE DU COMPTE : c'est ce
    // que retient ce drapeau, pour ne pas reposer la question a ceux qui ont
    // deja repondu a celle-ci (voir ask_hiscore_account_again).
    m_settings_panel.record_account_answer();
    m_settings_panel.save_to_file(AppContext::get_config_path());

    if (publish && !BootcadeAuth::signed_in()) {
        LoginDialog dlg2(*this);
        dlg2.run();
        refresh_account_button();
        refresh_hiscore_data_async(false);
    }
    // Connecte ou non, l'etat est desormais visible en permanence.
    refresh_hiscore_nudge();
}


void MainWindow::refresh_hiscore_nudge() {
    /* L'etat « scores actives, personne de connecte » etait invisible : rien
     * ne le disait avant la fin d'une partie, et seulement a ce moment-la.
     * Un joueur pouvait donc lancer le launcher dix fois sans jamais
     * apprendre qu'il lui manquait un compte.
     *
     * Le bandeau le dit a chaque demarrage, avec le bouton qui resout le
     * probleme a cote du texte. Il porte le nombre de scores en attente quand
     * il y en a : c'est l'argument le plus concret possible, et il rend
     * visible une file qui, jusqu'ici, ne se consultait qu'en ouvrant un
     * dossier depuis les reglages.
     */
    if (!m_settings_panel.is_hiscore_enabled() || BootcadeAuth::signed_in()) {
        if (m_hiscore_nudge_shown) { m_hiscore_infobar.hide(); m_hiscore_nudge_shown = false; }
        return;
    }
    /* Restauration en cours : se taire plutot que deviner.
     *
     * Le bandeau se calculait des l'ouverture de la fenetre, pendant que le
     * fil de restauration attendait encore la reponse du serveur. Il lisait
     * donc un signed_in() qui n'avait pas fini de repondre, annoncait « il te
     * faut un compte » a un joueur connecte, et restait a l'ecran ensuite.
     * m_account_settled le rappelle des que la reponse est connue.
     */
    if (m_account_pending) return;
    const int queued = HiscoreClient::outbox_size();
    m_hiscore_infobar_label.set_text(
        queued > 0
            ? Glib::ustring::compose(
                  _("%1 score(s) are waiting: sign in to publish them."), queued)
            : Glib::ustring(_("Your scores are not published yet: publishing "
                              "needs a free Bootcade account.")));
    auto style = m_hiscore_infobar.get_style_context();
    style->remove_class("bc-info");
    style->add_class("bc-warn");
    m_hiscore_signin_button->show();
    m_hiscore_report_button->hide();
    m_hiscore_replies_button->hide();
    m_hiscore_infobar.show();
    m_hiscore_nudge_shown = true;
}


void MainWindow::ask_hiscore_account_again() {
    /* Ceux qui ont repondu a l'ANCIENNE question ont dit oui ou non a
     * « aucun compte n'est necessaire ». Leur reponse ne vaut pas pour la
     * nouvelle : elle a ete donnee sur une promesse qui n'est plus tenue. La
     * question est donc reposee une fois, et une seule, a eux seuls.
     */
    /* Reposee a CHAQUE nouvelle version tant que le probleme dure : un
     * joueur qui a repondu « plus tard » il y a trois versions, ou qui arrive
     * d'une version qui promettait encore qu'aucun compte n'etait necessaire,
     * ne verrait jamais rien autrement.
     *
     * Jamais reposee a un joueur connecte : pour lui tout fonctionne, et une
     * question sans objet a chaque mise a jour serait du harcelement.
     */
    /* Posee depuis le premier tour de boucle, elle passait AVANT que le fil
     * de restauration ait relu la session : signed_in() repondait non, et un
     * joueur connecte se voyait demander de se connecter a chaque nouvelle
     * version. Comme le bandeau, elle attend que la reponse soit connue ;
     * m_account_settled la rappelle. */
    if (m_account_pending) {
        m_account_question_waiting = true;
        return;
    }
    if (BootcadeAuth::signed_in()) {
        // Sa reponse vaut pour cette version : inutile de la lui redemander
        // s'il se deconnecte plus tard dans la meme.
        m_settings_panel.record_account_answer();
        m_settings_panel.save_to_file(AppContext::get_config_path());
        return;
    }
    if (m_settings_panel.was_account_asked_this_version()) return;
    ask_hiscore_optin();
}

void MainWindow::refresh_hiscore_dock_warning(bool enabled, bool signed_in) {
    /* Deux etats, deux gestes. L'ordre compte : sans l'interrupteur, se
     * connecter ne publierait toujours rien, donc c'est lui qu'on propose
     * d'abord. */
    if (!enabled) {
        m_hs_warn_signin = false;
        m_hs_warn_title.set_text(_("WARNING"));
        m_hs_warn_label.set_text(
            _("Online highscores are off: nothing you play is recorded here."));
        m_hs_warn_btn.set_label(_("Turn on"));
        m_hs_warn.show();
        return;
    }
    if (!signed_in) {
        m_hs_warn_signin = true;
        m_hs_warn_title.set_text(_("WARNING"));
        m_hs_warn_label.set_text(
            _("You are not signed in: your scores are kept on this computer "
              "and published once you sign in."));
        m_hs_warn_btn.set_label(_("Sign in"));
        m_hs_warn.show();
        return;
    }
    m_hs_warn.hide();
}


void MainWindow::refresh_hiscore_data_async(bool announce) {
    // L'interrupteur maître coupe tout : aucune pastille, aucune requête.
    /* La liste des jeux CLASSABLES est relue meme quand tout est coupe.
     *
     * Elle ne sert alors a rien d'autre qu'a l'encart d'alerte d'une fiche de
     * jeu : sans elle, le lanceur ne saurait plus distinguer un jeu classe
     * d'un autre des que l'interrupteur est eteint, et c'est precisement le
     * moment ou il doit pouvoir dire au joueur ce qu'il rate. Lecture d'un
     * fichier deja ecrit : aucune requete, rien d'allume.
     */
    HiscoreClient::set_store_dir(
        std::filesystem::path(AppContext::get_config_path()).parent_path().string());

    if (!m_settings_panel.is_hiscore_enabled()) {
        /* Tout coupe, la derniere liste connue est quand meme relue.
         *
         * Elle ne declenche aucune requete : c'est un fichier deja ecrit. Ce
         * qu'elle permet, c'est de continuer a montrer QUELS jeux sont
         * classables : pastille, filtre et colonne HS restent en place, et le
         * panneau d'avertissement de la fiche a enfin un sens. Sans elle, le
         * joueur voyait « rien n'est enregistre » sans savoir sur quoi.
         */
        auto cached = HiscoreClient::cached_supported();
        if (!cached.empty()) {
            std::lock_guard<std::mutex> lock(m_hiscore_supported_mutex);
            m_hiscore_supported = std::move(cached);
        }
        return;
    }
    /* « Automatic sync » ne coupe QUE les rafraichissements automatiques.
     *
     * `announce` distingue les deux : un rafraichissement demande par le
     * joueur l'annonce, un rafraichissement de fond ne dit rien. Eteint, le
     * lanceur cesse donc d'aller chercher les classements tout seul au
     * demarrage, mais un geste explicite marche toujours : sinon le reglage
     * ne serait pas « automatique ou manuel », il serait un second
     * interrupteur general, et il en existe deja un juste au-dessus.
     */
    if (!announce && !m_settings_panel.syncs_automatically()) return;
    // Lu sur le fil graphique, comme tout ce que l'ouvrier emporte.
    const bool community = m_settings_panel.community_enabled();
    // Un seul rafraîchissement à la fois : un joueur qui clique trois fois ne
    // doit pas déclencher trois chargements complets.
    if (m_hiscore_refreshing.exchange(true)) return;

    // The service address lives in config.json rather than being compiled in:
    // the same build has to serve a player pointed at the homelab, one pointed
    // at a public instance, and one pointed at nothing at all.
    std::string url;
    {
        nlohmann::json j;
        std::ifstream fi(AppContext::get_config_path());
        if (fi) { try { fi >> j; } catch (...) {} }
        // Defaults to the public service so a fresh install shows the
        // leaderboards without anyone having to configure anything.
        url = j.value("hiscore_url", std::string("https://scores.bootcade.duckdns.org"));
    }
    if (url.empty()) { m_hiscore_refreshing = false; return; }
    HiscoreClient::set_base_url(url);
    HiscoreClient::set_store_dir(
        std::filesystem::path(AppContext::get_config_path()).parent_path().string());

    // Dernière liste connue d'abord, pour que les pastilles soient justes dès
    // la première image et le restent sans réseau.
    {
        auto cached = HiscoreClient::cached_supported();
        if (!cached.empty()) {
            std::lock_guard<std::mutex> lock(m_hiscore_supported_mutex);
            m_hiscore_supported = std::move(cached);
        }
    }

    if (announce) {
        std::lock_guard<std::mutex> lock(m_hiscore_status_mutex);
        m_hiscore_status = _("Refreshing highscores…");
        m_hiscore_refresh_dispatcher.emit();
    }

    std::thread([this, announce, community, alive = m_alive_token]() {
        // Le hiscore.dat de l'émulateur d'abord : sans lui une trentaine de
        // jeux n'écrivent aucun fichier, et leur pastille promet un classement
        // que rien ne peut alimenter. Il vient du service comme le reste, et
        // n'est demandé que parce que les classements sont activés.
        HiscoreClient::sync_hiscore_dat(fbneo_hiscores_dir());

        const auto flushed = HiscoreClient::flush_outbox();
        if (flushed.published || flushed.pending || flushed.ignored
            || flushed.dropped || !flushed.reason.empty()) {
            std::cerr << "[HISCORE] file videe : publies=" << flushed.published
                      << " attente=" << flushed.pending << " sans objet=" << flushed.ignored
                      << " gardes=" << flushed.kept << " ecartes=" << flushed.dropped << std::endl;
            std::lock_guard<std::mutex> live(alive->mutex);
            if (!alive->alive) return;
            std::lock_guard<std::mutex> lock(m_hiscore_result_mutex);
            /* Dire ce qui est arrive, pas << envoye >> pour tout.
             *
             * << Publie >> se verifie au classement, << en attente >> pas
             * encore, et << rien a enregistrer >> jamais. Le message annoncait
             * les trois de la meme facon, et le joueur cherchait ensuite une
             * ligne qui n'existait pas. */
            if (flushed.published > 0)
                m_hiscore_results.push_back(Glib::ustring::compose(
                    _("%1 score(s) saved offline are now published."),
                    flushed.published).raw());
            if (flushed.pending > 0)
                m_hiscore_results.push_back(Glib::ustring::compose(
                    _("%1 score(s) sent : awaiting review."),
                    flushed.pending).raw());
            if (flushed.ignored > 0)
                m_hiscore_results.push_back(Glib::ustring::compose(
                    _("%1 score(s) sent, but the service found nothing to "
                      "record : the game's table had no new result."),
                    flushed.ignored).raw());
            /* Un score refuse mais CONSERVE : le joueur doit lire la raison,
             * sinon il ne comprend pas pourquoi sa file ne se vide pas. */
            if (flushed.kept > 0 && !flushed.reason.empty())
                m_hiscore_results.push_back(Glib::ustring::compose(
                    _("%1 score(s) still waiting : %2"),
                    flushed.kept, flushed.reason).raw());
            // Ecarte parce que le SERVICE l'a declare definitif, jamais parce
            // que le launcher l'a suppose.
            if (flushed.dropped > 0)
                m_hiscore_results.push_back(Glib::ustring::compose(
                    _("%1 score(s) refused by the service : %2"),
                    flushed.dropped,
                    flushed.reason.empty() ? Glib::ustring(_("no reason given"))
                                           : Glib::ustring(flushed.reason)).raw());
            m_hiscore_result_dispatcher.emit();
        }

        /* Nos reponses a ses signalements.
         *
         * C'est ce qui montre au joueur qu'on l'a lu. Seules les reponses
         * arrivees depuis la derniere fois qu'il les a vues sont annoncees ;
         * la fenetre « Mes signalements » les marque lues. */
        if (!BootcadeAuth::access_token().empty()) {
            const auto reports = HiscoreClient::fetch_my_reports();
            if (reports.ok) {
                const std::string seen = HiscoreClient::reports_seen_at();
                std::vector<const HiscoreClient::PlayerReport*> fresh;
                for (const auto& rp : reports.value)
                    if (!rp.replied_at.empty() && rp.replied_at > seen) fresh.push_back(&rp);
                if (!fresh.empty()) {
                    std::string title = fresh.front()->game;
                    if (fresh.size() == 1) {
                        const Game g = m_database->getGame(fresh.front()->game, fresh.front()->system);
                        if (!g.description.empty()) title = g.description;
                    }
                    std::lock_guard<std::mutex> live(alive->mutex);
                    if (!alive->alive) return;
                    HiscoreNotice notice{fresh.size() == 1
                        ? Glib::ustring::compose(_("We answered your report on %1."), title).raw()
                        : Glib::ustring::compose(_("We answered %1 of your reports."), (int)fresh.size()).raw()};
                    notice.offer_replies = true;
                    std::lock_guard<std::mutex> lock(m_hiscore_result_mutex);
                    m_hiscore_results.push_back(std::move(notice));
                    m_hiscore_result_dispatcher.emit();
                }
            }
        }

        auto supported = HiscoreClient::fetch_supported();

        /* Joignable veut dire REPONDU, pas << a renvoye des lignes >>.
         *
         * Une liste vide est une reponse parfaitement valide : un service
         * debout sans aucun jeu classe existe. La deduire du contenu metier
         * faisait annoncer le service injoignable alors qu'il tournait. */
        const bool reached = supported.answered;
        const bool usable  = supported.ok;

        // Tous les classements en une fois. C'est ce qui permet à la
        // sélection d'un jeu de n'émettre aucune requête.
        /* « Community features » : les classements des AUTRES.
         *
         * Eteint, on ne telecharge aucun tableau. Ce qui est personnel : le
         * fait qu'un jeu soit classe, et les scores qu'on y a soi-meme
         * publies : continue de fonctionner, parce que ce n'est pas de la
         * donnee communautaire.
         */
        HiscoreClient::Fetched<std::vector<HiscoreClient::Board>> boards;
        if (usable && community) boards = HiscoreClient::fetch_boards(10);

        std::lock_guard<std::mutex> live(alive->mutex);
        if (!alive->alive) return;

        /* Ce que la synchronisation vient d'apprendre vaut pour l'etat du
         * reseau. Il etait calcule ici puis jete : on_hiscore_refresh_done
         * appelait bien apply_online_state, mais celle-ci relit m_net, que
         * personne n'avait mis a jour. L'interface repeignait donc la vieille
         * reponse de la sonde du demarrage. */
        const NetState seen = reached ? NetState::Online : NetState::Offline;
        if (m_net.exchange(seen) != seen) m_online_state_changed.emit();

        if (usable) {
            HiscoreClient::cache_supported(supported.value);
            // On n'ecrase un cache existant que par une reponse exploitable :
            // une liste vide venue d'une requete ratee effacerait le travail
            // de la derniere session en ligne.
            if (boards.ok && !boards.value.empty()) {
                HiscoreClient::cache_boards(boards.value);
                // Le rang personnel est fige ICI, pendant qu'on connait le nom
                // du joueur. Au demarrage suivant il se lira sans compte ni
                // reseau.
                HiscoreClient::cache_personal_ranks(boards.value,
                                                    BootcadeAuth::username());
            }
            {
                std::lock_guard<std::mutex> lock(m_hiscore_supported_mutex);
                m_hiscore_supported = std::move(supported.value);
            }
            m_hiscore_supported_dispatcher.emit();
        }
        m_hiscore_refreshing = false;
        if (announce) {
            std::lock_guard<std::mutex> lock(m_hiscore_status_mutex);
            // Trois issues, trois messages : ne pas repondre, repondre mal,
            // et repondre bien ne se confondent pas.
            m_hiscore_status = !reached ? _("Score service unreachable.")
                             : !usable  ? _("The score service answered with an error.")
                                        : _("Highscores up to date.");
            m_hiscore_refresh_dispatcher.emit();
        }
    }).detach();
}

void MainWindow::on_hiscore_refresh_done() {
    // On vient d'apprendre si le service repond : l'interface doit suivre.
    apply_online_state();
    // Et redessiner le jeu affiche : le bloc des classements ne parait que
    // s'il a des lignes, or elles viennent peut-etre d'arriver.
    if (auto it = m_treeview_games.get_selection()->get_selected())
        show_game_details(*it);
    std::string text;
    {
        std::lock_guard<std::mutex> lock(m_hiscore_status_mutex);
        text = m_hiscore_status;
    }
    m_status_label.set_text(text);
}

void MainWindow::on_hiscore_supported_ready() {
    // The list lands after the games are already on screen, so the rows that
    // deserve a pill have to be revisited. Cheaper than delaying the whole
    // catalogue behind a network call the player did not ask for.
    for (auto& row : m_model_games->children()) {
        std::string system = Glib::ustring(row[m_columns.m_col_system]).raw();
        std::string name   = Glib::ustring(row[m_columns.m_col_name]).raw();
        row[m_columns.m_col_hiscore] = game_ranks_online(Glib::ustring(row[m_columns.m_col_emulator]).raw(), system, name)
                                     ? Glib::ustring("\u25cf") : Glib::ustring();
    }
    // The filter tree carries a count of ranked games, and the Highscore sort
    // depends on the same list: both were built before it arrived.
    populate_filter_tree();
    if (m_sort_mode == SortMode::Highscore) filter_games();
    else rebuild_mlist();

    auto sel = m_treeview_games.get_selection();
    if (sel) { if (auto it = sel->get_selected()) show_game_details(*it); }
}

void MainWindow::fetch_hiscore_top_async(const std::string& system, const std::string& game) {
    unsigned seq;
    {
        std::lock_guard<std::mutex> lock(m_hiscore_top_mutex);
        seq = ++m_hiscore_seq;
    }
    // Paint whatever we already knew, straight away. Offline this is all the
    // player will get; online it removes the blank moment before the reply.
    {
        std::string when;
        auto cached = HiscoreClient::cached_top(system, game, &when);
        if (!cached.empty()) {
            std::lock_guard<std::mutex> lock(m_hiscore_top_mutex);
            m_hiscore_top = std::move(cached);
            m_hiscore_top_stale = when;
            m_hiscore_seq_done = seq;
        }
    }
    m_hiscore_top_dispatcher.emit();

    std::thread([this, system, game, seq, alive = m_alive_token]() {
        auto rows = HiscoreClient::fetch_top(system, game, 50);
        std::lock_guard<std::mutex> live(alive->mutex);
        if (!alive->alive) return;

        // Cette requete dit elle aussi si le service repond.
        const NetState seen = rows.answered ? NetState::Online : NetState::Offline;
        if (m_net.exchange(seen) != seen) m_online_state_changed.emit();

        /* On ne garde l'affichage precedent que si la requete a ECHOUE. Une
         * table vide est une reponse legitime : un jeu classe dont personne
         * n'a encore de score existe, et le figer sur d'anciennes lignes
         * afficherait un classement qui n'a plus cours. */
        if (!rows.ok) return;
        HiscoreClient::cache_top(system, game, rows.value);
        {
            std::lock_guard<std::mutex> lock(m_hiscore_top_mutex);
            // A reply for a game the player has already scrolled past must not
            // overwrite the one they are looking at now.
            if (seq != m_hiscore_seq) return;
            m_hiscore_top = std::move(rows.value);
            m_hiscore_top_stale.clear();
            m_hiscore_seq_done = seq;
        }
        m_hiscore_top_dispatcher.emit();
    }).detach();
}

void MainWindow::show_cached_board(const std::string& system, const std::string& game) {
    m_board_game = {system, game};
    std::string when;
    auto rows = HiscoreClient::cached_top(system, game, &when);
    // La date n'est affichée que si le cache est vraiment vieux : au
    // démarrage il vient d'être rempli, la mentionner serait du bruit.
    bool old = false;
    if (!when.empty()) {
        std::time_t now = std::time(nullptr);
        std::tm tm{};
        if (strptime(when.c_str(), "%Y-%m-%dT%H:%M:%S", &tm))
            old = std::difftime(now, timegm(&tm)) > 3600;
    }
    render_board(rows, old ? when : std::string());
}

void MainWindow::render_board(const std::vector<HiscoreClient::Entry>& rows,
                              const std::string& stale) {
    /* Qui suis-je, sur cette table ?
     *
     * Le compte fait foi : c'est desormais la seule source du nom de joueur.
     * L'ancien reglage « hiscore_player » ne subsiste qu'en repli pour une
     * configuration ecrite avant les comptes, sinon un joueur connecte ne se
     * reconnaissait plus dans son propre classement : le champ etait vide,
     * et son rang ne s'affichait jamais.
     */
    /* Qui suis-je sur cette table ? Trois sources, dans cet ordre.
     *
     * La session courante d'abord. Mais se DECONNECTER ne doit pas effacer
     * ce que le launcher sait deja de moi : le classement en cache affichait
     * bien « 790 119 battousai90 » pendant que « Your best » pretendait ne
     * connaitre aucun score, parce que l'un lisait la donnee enregistree et
     * l'autre l'identite courante. Deux chemins de lecture pour une meme
     * question.
     *
     * Le cache de rangs porte le nom du joueur : il sert donc de repli, et
     * les donnees locales restent lisibles hors session. L'identite sert a
     * RAFRAICHIR ces donnees, pas a decider si elles ont le droit d'exister.
     */
    std::string me = BootcadeAuth::username();
    if (me.empty()) {
        std::string cached_user;
        HiscoreClient::cached_personal_ranks(&cached_user);
        me = cached_user;
    }
    if (me.empty()) {
        nlohmann::json j;
        std::ifstream fi(AppContext::get_config_path());
        if (fi) { try { fi >> j; } catch (...) {} }
        me = j.value("hiscore_player", std::string());
    }

    SettingsUi::destroy_children(m_hiscore_grid);

    int my_rank = 0;
    if (!me.empty())
        for (size_t i = 0; i < rows.size(); ++i)
            if (rows[i].player == me) { my_rank = (int)i + 1; break; }

    /* La MEILLEURE place du joueur, et pas une quelconque de ses lignes.
     *
     * `rows` est deja classe, donc la premiere correspondance est la
     * meilleure : c'est exactement la regle qui vaut sur le site. Un joueur
     * qui occupe les places 2 et 4 est 2e, l'afficher 4e serait faux et
     * decourageant.
     */
    /* La carte se remplit avec la MEILLEURE ligne du joueur.
     *
     * `rows` etant deja classe, la premiere correspondance est la meilleure.
     * Afficher une autre de ses lignes donnerait un rang moins bon que le
     * sien, ce qui est faux et decourageant. */
    if (my_rank > 0) {
        const std::string metric = HiscoreClient::metric_of(m_board_game.first,
                                                            m_board_game.second);
        m_best_rank.set_text("#" + std::to_string(my_rank));
        m_best_score.set_text(format_by_metric(rows[my_rank - 1].score, metric));
        std::string flag = country_flag(rows[my_rank - 1].country);
        m_best_who.set_text(rows[my_rank - 1].player + (flag.empty() ? "" : "  " + flag));
        m_best_hint.set_text("");
    } else {
        m_best_rank.set_text("");
        m_best_score.set_text("");
        m_best_who.set_text("");
        // Pas de score ici : une invitation plutot qu'une carte vide.
        m_best_hint.set_text(_("No score yet. Play to enter the leaderboard."));
    }
    /* show_all sur la LIGNE, pas sur ses enfants un par un.
     *
     * Le score vivait dans une boite intermediaire que personne ne montrait :
     * l'afficher lui ne suffisait pas, un enfant reste invisible tant que son
     * parent l'est. La carte n'affichait donc que la pastille de rang.
     */
    m_best_row.show_all();
    m_best_title.show();
    if (my_rank > 0) { m_best_rank.show(); m_best_link.show(); m_best_hint.hide(); }
    else             { m_best_rank.hide(); m_best_link.hide(); m_best_hint.show(); }
    m_best_box.show();

    m_hiscore_title.set_markup(
        "<b>" + escape_markup(_("World leaderboard")) + "</b>" +
        (my_rank > 0 ? "  <span foreground=\"" + SettingsUi::tone_hex(*this, "success") + "\" size=\"small\">" +
                       escape_markup(Glib::ustring::compose(_("you are %1st"), my_rank)) +
                       "</span>"
                     : std::string()));

    // Dix lignes, toujours. Les places libres sont dessinees, pas ecrites en
    // gris : elles doivent se lire comme des places a prendre.
    /* Cinq lignes, et seulement celles qui existent.
     *
     * Dix emplacements dont cinq remplis de « AAA » imitaient une borne
     * d'arcade, mais sur un classement mondial ils donnent l'impression que
     * le jeu n'interesse personne. On montre ce qu'il y a, et « Tout voir »
     * mene au reste. */
    const size_t BOARD_ROWS = std::min<size_t>(5, rows.size());
    auto cell = [](const std::string& text, const char* css, float xalign) {
        auto* l = Gtk::make_managed<Gtk::Label>(text);
        l->set_xalign(xalign);
        l->get_style_context()->add_class(css);
        return l;
    };
    for (size_t i = 0; i < BOARD_ROWS; ++i) {
        const bool free_slot = i >= rows.size();
        const bool mine = !free_slot && !me.empty() && rows[i].player == me;
        const char* tone = free_slot ? "hi-free" : (mine ? "hi-mine" : "hi-row");

        m_hiscore_grid.attach(*cell(std::to_string(i + 1), "hi-rank", 1.0f), 0, (int)i, 1, 1);
        const std::string metric = HiscoreClient::metric_of(m_board_game.first,
                                                            m_board_game.second);
        m_hiscore_grid.attach(*cell(free_slot ? "."
                                    : format_by_metric(rows[i].score, metric),
                                    free_slot ? "hi-free" : (i == 0 ? "hi-score-top" : "hi-score"),
                                    1.0f), 1, (int)i, 1, 1);
        auto* who = cell(free_slot ? "AAA" : rows[i].player, tone, 0.0f);
        who->set_ellipsize(Pango::ELLIPSIZE_END);
        m_hiscore_grid.attach(*who, 2, (int)i, 1, 1);

        std::string right;
        if (!free_slot) {
            std::string flag = country_flag(rows[i].country);
            std::string day  = short_date(rows[i].since);
            right = flag + (flag.empty() || day.empty() ? "" : "  ") + day;
        }
        m_hiscore_grid.attach(*cell(right, "hi-when", 1.0f), 3, (int)i, 1, 1);
    }
    m_hiscore_grid.show_all();

    if (stale.empty()) {
        m_label_hiscore.hide();
    } else {
        m_label_hiscore.set_text(
            Glib::ustring::compose(_("offline, last seen %1"), short_date(stale)));
        m_label_hiscore.show();
    }
}

void MainWindow::on_hiscore_top_ready() {
    std::vector<HiscoreClient::Entry> rows;
    std::string stale;
    {
        std::lock_guard<std::mutex> lock(m_hiscore_top_mutex);
        if (m_hiscore_seq_done != m_hiscore_seq) return;
        rows = m_hiscore_top;
        stale = m_hiscore_top_stale;
    }
    render_board(rows, stale);
}

void MainWindow::sort_games(std::vector<const Game*>& games) {
    // std::stable_sort throughout: within equal keys the DAT order survives,
    // so sorting by year does not also shuffle the games of a given year.
    switch (m_sort_mode) {
    case SortMode::Default:
        break;                                   // DAT order, grouped by system
    case SortMode::Name:
        std::stable_sort(games.begin(), games.end(), [](const Game* pa, const Game* pb) {
            const Game& a = *pa; const Game& b = *pb;
            // Compare the human title, which is what the views display; the
            // ROM name only breaks ties so the order stays deterministic.
            const std::string& ta = a.description.empty() ? a.name : a.description;
            const std::string& tb = b.description.empty() ? b.name : b.description;
            int c = g_utf8_collate(ta.c_str(), tb.c_str());
            return c != 0 ? c < 0 : a.name < b.name;
        });
        break;
    case SortMode::Year:
    case SortMode::YearAsc: {
        const bool ascending = (m_sort_mode == SortMode::YearAsc);
        std::stable_sort(games.begin(), games.end(), [ascending](const Game* pa, const Game* pb) {
            const Game& a = *pa; const Game& b = *pb;
            // Undated games sink to the bottom either way: they are the ones
            // the sort has nothing to say about, and floating them to the top
            // of "oldest first" would bury the answer the user asked for.
            bool ea = a.year.empty(), eb = b.year.empty();
            if (ea != eb) return !ea;
            if (ea) return false;
            return ascending ? a.year < b.year : a.year > b.year;
        });
        break;
    }
    case SortMode::RecentlyPlayed:
        std::stable_sort(games.begin(), games.end(), [](const Game* pa, const Game* pb) {
            const Game& a = *pa; const Game& b = *pb;
            bool ea = a.last_played.empty(), eb = b.last_played.empty();
            if (ea != eb) return !ea;            // never played goes last
            if (ea) return false;
            return a.last_played > b.last_played;   // ISO-8601 sorts as text
        });
        break;
    case SortMode::Highscore: {
        // Copied once under the lock rather than consulted per comparison: a
        // sort over 29 000 games makes tens of thousands of comparisons.
        std::set<std::string> ranked;
        {
            std::lock_guard<std::mutex> lock(m_hiscore_supported_mutex);
            ranked = m_hiscore_supported;
        }
        std::stable_sort(games.begin(), games.end(),
            [&ranked](const Game* pa, const Game* pb) {
                const Game& a = *pa; const Game& b = *pb;
                // FinalBurn Neo sets only, as game_ranks_online() says.
                bool ra = a.emulator == "fbneo" && ranked.count(HiscoreClient::key(a.system, a.name)) > 0;
                bool rb = b.emulator == "fbneo" && ranked.count(HiscoreClient::key(b.system, b.name)) > 0;
                return ra != rb ? ra : false;
            });
        break;
    }
    }
}

void MainWindow::submit_session_score(const std::string& system,
                                      const std::string& game,
                                      const std::string& fbneo_rom_name,
                                      const std::string& score_path,
                                      const std::string& hi_before,
                                      const std::string& player,
                                      const std::string& country,
                                      bool hiscore_enabled,
                                      bool share_playtime,
                                      std::time_t launch_time) {
    // Each condition is one the player controls. None is an error worth
    // reporting: a game with no leaderboard, an unconfigured service or an
    // unticked box are all perfectly ordinary states.
    if (!hiscore_enabled) return;
    if (HiscoreClient::base_url().empty()) return;

    // Playtime is reported for ANY game, ranked or not. A clone, a hack or a
    // console port has no leaderboard : their scoring may differ : but the
    // player still spent that time, and dropping it made whole evenings vanish
    // from the record for no reason they could see.
    // Read after watch_playtime has written this session in, so the figures
    // sent are the ones just earned rather than the previous ones.
    /* « Share play statistics », decide au lancement.
     *
     * Eteint, la structure reste a zero, ce que la couche de scores traite
     * deja comme « ne rien rapporter ». Le score, lui, part quand meme : c'est
     * un resultat de jeu, pas une habitude, et le joueur qui refuse l'un ne
     * refuse pas forcement l'autre.
     */
    HiscoreClient::Playtime pt;
    if (share_playtime) {
        Game g = m_database->getGame(game, system);
        pt.last    = g.last_session_secs;
        pt.longest = g.longest_session_secs;
        pt.total   = g.play_time_secs;
    }
    /* La SESSION part toujours, score ou pas.
     *
     * Un jeu classe dont la table n'a pas bouge (partie perdue avant le
     * tableau, ou .hi jamais ecrit) etait ecarte ici avant d'envoyer quoi que
     * ce soit : le service ne voyait ni la duree ni la partie, et son
     * compteur de parties restait en arriere. Le service compte une partie
     * quand le total envoye progresse : chaque session doit donc lui
     * parvenir, avec les cumuls du lanceur. Sans reseau, elle est garee comme
     * un score : la file la rejouera dans l'ordre.
     */
    auto send_playtime_only = [&] {
        if (pt.total <= 0) return;
        auto r = HiscoreClient::submit(system, game, player, country, pt, "", "");
        const NetState observed = r.answered ? NetState::Online : NetState::Offline;
        if (m_net.exchange(observed) != observed) m_online_state_changed.emit();
        if (!r.reached) {
            HiscoreClient::queue_submission(system, game, player, country, pt, "", "");
            std::cerr << "[HISCORE] playtime queued for later (" << r.error << ")" << std::endl;
        }
    };
    /* Deconnecte : le score est GARE, jamais jete.
     *
     * Un joueur dont la session avait expire a fait un record sur 1945k III,
     * a entre son nom, a quitte : rien, pas un mot. Cette fonction rendait la
     * main ici en silence, et FBNeo ecrasant le .hi a la partie suivante, la
     * preuve du record disparaissait avec. Le service prend le nom dans le
     * jeton, pas dans le champ `player` : une entree sans nom est donc
     * complete, et la file la rejoue d'elle-meme a la connexion suivante
     * (flush_outbox, appele par refresh_hiscore_data_async). Seul le score
     * est gare : la duree de jeu d'un joueur sans compte n'a personne a qui
     * etre attribuee, et empiler une entree par partie ne servirait a rien. */
    /* Le meme fichier qu'avant la partie. S'il n'existait pas au lancement
     * (premiere partie), celui qui vient d'apparaitre est le seul candidat :
     * il n'y a pas d'etat d'avant a confondre avec lui. */
    const std::string after_path = hi_before.empty()
        ? fbneo_score_state_path(system, game, fbneo_rom_name) : score_path;
    /* Un jeu non classe envoie aussi sa table, avant et apres : c'est ce qui
     * permet au service d'apprendre ou le jeu range son score, et d'en
     * ecrire la definition. Le joueur n'en entend parler que si un score est
     * publie : tout le reste (en attente, refuse, garde hors ligne) ne lui
     * promettait rien. */
    const bool ranked = game_ranks_online("fbneo", system, game);

    if (player.empty()) {
        std::string hi_after = read_file_bytes(after_path);
        if (hi_after.empty() || hi_after == hi_before) return;
        HiscoreClient::queue_submission(system, game, player, country, pt,
                                        hi_before, hi_after);
        if (!ranked) return;
        std::cerr << "[HISCORE] deconnecte : score gare pour " << system << "/"
                  << game << ", envoye a la prochaine connexion" << std::endl;
        std::lock_guard<std::mutex> lock(m_hiscore_result_mutex);
        m_hiscore_results.push_back(
            {_("You are not signed in : your score is saved and will be "
               "published once you sign in."), true});
        m_hiscore_result_dispatcher.emit();
        return;
    }

    std::string hi_after = read_file_bytes(after_path);
    if (hi_after.empty() || hi_after == hi_before) {   // no score table, or nothing new in it
        send_playtime_only();
        return;
    }

    // A FIRST SESSION IS SENT LIKE ANY OTHER. It used to be dropped here, on
    // the grounds that without a before state nothing in the table can be told
    // apart from the factory entries it ships with. That was true of the
    // launcher, which knows nothing about the game, but not of the service,
    // which holds the factory table for most games and can compare against it.
    //
    // Deciding here meant the service never even saw the session, so the work
    // done there to make a first run count could never fire: the player was
    // told their run had become the reference while the answer was one request
    // away. What to do with a missing before state is the service's call, not
    // this one's.

    auto r = HiscoreClient::submit(system, game, player, country, pt,
                                   hi_before, hi_after);

    /* Un envoi est une preuve de premiere main sur l'etat du reseau.
     *
     * La sonde ne tourne qu'a l'ouverture, dans un fil detache, et rien ne la
     * rejouait ensuite : l'indicateur gardait donc la reponse du demarrage.
     * On affichait Online, en vert, a l'instant meme ou le bandeau annoncait
     * le serveur injoignable. Le resultat de l'envoi est un fait plus recent
     * que la sonde, il doit primer. On n'emet que si l'etat change, pour ne
     * pas redessiner l'interface a chaque partie.
     */
    const NetState observed = r.answered ? NetState::Online : NetState::Offline;
    // Called from the play-monitor thread, which holds the alive token's lock
    // for the whole call : the window is guaranteed to outlive this emit.
    if (m_net.exchange(observed) != observed) m_online_state_changed.emit();

    // An unreachable service is deliberately silent. The player did not ask
    // to publish anything at that instant, and a network error popping up
    // after every offline session would be pure noise.
    if (!r.reached) {
        // Parked, not dropped. FBNeo overwrites the .hi on the next session,
        // so this is the last moment the evidence still exists.
        HiscoreClient::queue_submission(system, game, player, country, pt,
                                        hi_before, hi_after);
        std::cerr << "[HISCORE] queued for later (" << r.error << ")" << std::endl;
        if (!ranked) return;

        /* Dire la VRAIE raison. Le score est gare dans les trois cas, mais
         * une session expiree, une panne du service et une absence de reseau
         * n'appellent pas le meme geste du joueur. */
        Glib::ustring why;
        bool offer_signin = false;
        if (!r.answered)
            why = _("Server unreachable : your score is saved and will be sent later.");
        else if (r.http_status == 401) {
            why = _("Your session has expired : sign in again to publish. "
                    "Your score is saved.");
            // Le jeton est conserve, donc BootcadeAuth::signed_in() repond
            // encore oui : c'est ce message, et lui seul, qui sait que la
            // session est a refaire.
            offer_signin = true;
        } else if (r.code == "auth_unavailable")
            why = _("The account service is unavailable : your score will be sent later.");
        else
            why = _("The server could not record your score : it is saved and "
                    "will be sent later.");

        std::lock_guard<std::mutex> lock(m_hiscore_result_mutex);
        m_hiscore_results.push_back({why.raw(), offer_signin});
        m_hiscore_result_dispatcher.emit();
        return;
    }

    /* Le verdict du serveur, en un mot : c'est LUI qu'on traduit.
     *
     * Le serveur ecrit aussi une phrase, `detail`, dans sa langue a lui.
     * L'afficher donnait « Score not recorded : la table des scores n'a pas
     * bouge » a un joueur chinois : moitie traduit, et sans dire l'essentiel,
     * a savoir quel score il fallait battre. La phrase ne sert plus que pour
     * un code que ce launcher ne connait pas encore. Un serveur plus ancien,
     * sans code, garde le chemin d'avant, plus bas.
     */
    if (!r.code.empty()) {
        std::cerr << "[HISCORE] verdict " << r.code << " pour " << system << "/" << game
                  << (r.detail.empty() ? std::string() : " : " + r.detail) << std::endl;
        const std::string& c = r.code;
        if (c == "not_ranked" || c == "not_ranked_yet" || c == "playtime_only") return;

        const std::string metric = HiscoreClient::metric_of(system, game);
        HiscoreNotice notice{std::string()};
        if (c == "accepted")
            notice.message = r.has_score
                ? Glib::ustring::compose(_("Score %1 published on the leaderboard."),
                                         format_by_metric(r.score, metric)).raw()
                : std::string(_("Score published on the leaderboard."));
        else if (c == "not_in_table")
            notice.message = r.rows > 0 && r.has_to_beat
                ? Glib::ustring::compose(_("Your score did not make this game's top %1. "
                                           "Beat %2 to appear on the leaderboard."),
                                         r.rows, format_by_metric(r.to_beat, metric)).raw()
                : r.rows > 0
                ? Glib::ustring::compose(_("Your score did not make this game's top %1."), r.rows).raw()
                : std::string(_("Your score did not make it into this game's own table."));
        else if (c == "first_run_waiting")
            notice.message = _("First game on this title : your score is kept and will count very soon.");
        else if (c == "unreadable")
            notice.message = _("We could not read your score. This is on our side : "
                               "report it so we can check.");
        else if (c == "not_original")
            notice.message = _("No leaderboard for this version : only original versions are ranked.");
        else if (c == "missing_game" || c == "missing_file")
            notice.message = _("Incomplete submission : update the launcher.");
        else
            notice.message = r.detail;   // code inconnu : la phrase de secours

        if (!notice.message.empty()) {
            if (r.report) {
                notice.offer_report         = true;
                notice.report.submission    = r.submission;
                notice.report.system        = system;
                notice.report.game          = game;
                const Game g = m_database->getGame(game, system);
                notice.report_title         = g.description.empty() ? game : g.description;
                notice.report_rom           = fbneo_rom_name;
                notice.report_launch        = launch_time;
            }
            std::lock_guard<std::mutex> lock(m_hiscore_result_mutex);
            m_hiscore_results.push_back(std::move(notice));
            if (r.accepted) m_hiscore_refresh_target = {system, game};
            m_hiscore_result_dispatcher.emit();
            return;
        }
    }

    /* Sans objet : le service a regarde et n'a rien trouve a enregistrer.
     *
     * La raison part TOUJOURS dans le journal : sans elle, un joueur qui vient
     * de finir deuxieme au tableau du jeu n'a aucun moyen de savoir pourquoi
     * rien n'est monte, et moi non plus.
     *
     * Elle s'affiche quand le jeu porte la pastille Highscore. La pastille
     * PROMET un classement : rester muet sur un jeu qui l'affiche laisse
     * croire a une publication qui n'a pas eu lieu. Sur un jeu non classe, en
     * revanche, il n'y a rien a annoncer et le silence est la bonne reponse.
     */
    if (r.ignored) {
        std::cerr << "[HISCORE] sans objet pour " << system << "/" << game
                  << " : " << (r.reason.empty() ? "aucune raison donnee" : r.reason)
                  << std::endl;
        if (ranked && !r.reason.empty()) {
            std::lock_guard<std::mutex> lock(m_hiscore_result_mutex);
            m_hiscore_results.push_back(Glib::ustring::compose(
                _("Score not recorded : %1"), r.reason).raw());
            m_hiscore_result_dispatcher.emit();
        }
        return;
    }

    // Non classe : rien n'a ete promis au joueur. Seul un score publie
    // (le service a appris le jeu depuis) merite d'etre annonce.
    if (!ranked && !r.accepted) {
        std::cerr << "[HISCORE] non classe, table envoyee pour " << system << "/" << game
                  << (r.reason.empty() ? std::string() : " : " + r.reason) << std::endl;
        return;
    }

    // Le meme nombre se lit differemment selon le jeu : au chrono la barre
    // annoncerait 917504 au lieu de 14'00"00, et au golf -3 apparaitrait comme
    // un score negatif au lieu de trois sous le par.
    const std::string shown =
        format_by_metric(r.score, HiscoreClient::metric_of(system, game));

    std::string message;
    if (r.accepted) {
        message = r.has_score
            ? Glib::ustring::compose(_("Score %1 published on the leaderboard."),
                                     shown).raw()
            : _("Score published on the leaderboard.");
    } else if (r.pending) {
        // Said as a wait, not a suspicion: the usual causes are a first score
        // on a game or a missing baseline, neither of which is the player's
        // fault, and "rejected" would read as an accusation.
        message = r.has_score
            ? Glib::ustring::compose(_("Score %1 sent : awaiting review."),
                                     shown).raw()
            : _("Score sent : awaiting review.");
        // The server's reason is an administrator's diagnostic, written in the
        // server's language and in its vocabulary. Pasting it here produced a
        // half-translated sentence about baselines and table heads, which says
        // nothing to a player who just wants to know if their score counted.
        // It goes to the log, where it belongs.
        if (!r.reason.empty())
            std::cerr << "[HISCORE] queued: " << r.reason << std::endl;
    } else {
        // A refusal with an explanation, e.g. a clone having no leaderboard.
        message = r.reason.empty() ? _("Score not kept.") : r.reason;
    }

    {
        std::lock_guard<std::mutex> lock(m_hiscore_result_mutex);
        m_hiscore_results.push_back(message);
        // Seul un score publié change le classement : une soumission mise en
        // attente ou refusée laisse le tableau tel quel.
        if (r.accepted) m_hiscore_refresh_target = {system, game};
    }
    m_hiscore_result_dispatcher.emit();
}

void MainWindow::on_hiscore_result_ready() {
    HiscoreNotice notice{std::string()};
    {
        std::lock_guard<std::mutex> lock(m_hiscore_result_mutex);
        if (m_hiscore_results.empty()) { std::cerr << "[HISCORE] bandeau : rien a afficher" << std::endl; return; }
        notice = m_hiscore_results.front();
        m_hiscore_results.pop_front();
    }
    const std::string& message = notice.message;
    // Trace de ce que le joueur a lu : quand il dit << je n'ai rien vu >>,
    // c'est la seule facon de savoir si le message a ete affiche ou perdu.
    std::cerr << "[HISCORE] bandeau : " << message << std::endl;
    m_hiscore_infobar_label.set_text(message);
    auto style = m_hiscore_infobar.get_style_context();
    style->remove_class("bc-warn");
    style->add_class("bc-info");
    // Le bandeau est partage par tous les messages, y compris une publication
    // reussie. Proposer de se connecter sous un score publie laissait croire
    // au joueur deja connecte que son compte avait lache.
    if (notice.offer_signin) m_hiscore_signin_button->show();
    else                     m_hiscore_signin_button->hide();
    if (notice.offer_report) m_hiscore_report_button->show();
    else                     m_hiscore_report_button->hide();
    if (notice.offer_replies) m_hiscore_replies_button->show();
    else                      m_hiscore_replies_button->hide();
    m_hiscore_shown = notice;
    m_hiscore_nudge_shown = false;
    m_hiscore_infobar.show();

    // The leaderboard on screen is now out of date if it is the game just
    // played : refresh whatever the detail dock is showing.
    //
    // Re-rendering alone is not enough. The dock reads the cache filled by the
    // single bulk request made at startup, so a score published since then is
    // simply not in it : the panel kept showing the empty board's placeholder
    // rows until the next launch. The game just played is fetched again, on
    // its own, which is the one targeted request the design allows for.
    std::pair<std::string, std::string> target;
    {
        std::lock_guard<std::mutex> lock(m_hiscore_result_mutex);
        target = m_hiscore_refresh_target;
        m_hiscore_refresh_target = {};
    }
    if (!target.second.empty())
        fetch_hiscore_top_async(target.first, target.second);

    auto sel = m_treeview_games.get_selection();
    if (sel) { if (auto it = sel->get_selected()) show_game_details(*it); }
}

void MainWindow::open_report_dialog(const HiscoreNotice& notice) {
    // Les captures de la partie, prises avec F6 : la derniere est proposee
    // jointe, c'est souvent la preuve du score.
    std::vector<std::string> shots;
    if (!notice.report_rom.empty() && notice.report_launch > 0)
        shots = find_session_screenshots(notice.report_rom, notice.report_launch);
    ReportDialog dlg(*this, notice.report, notice.report_title, shots);
    dlg.run();
}

void MainWindow::open_my_reports() {
    if (!BootcadeAuth::signed_in()) {
        LoginDialog login(*this);
        login.run();
        refresh_account_button();
        if (!BootcadeAuth::signed_in()) return;
    }
    MyReportsDialog dlg(*this,
        [this](const std::string& system, const std::string& game) {
            const Game g = m_database->getGame(game, system);
            return g.description.empty() ? game : g.description;
        },
        [](const std::string& system, const std::string& game, long long value) {
            return format_by_metric(value, HiscoreClient::metric_of(system, game));
        });
    dlg.run();
}

// "1.0.9" contre "1.0.12" : une comparaison de chaines dirait que 9 est plus
// grand. Les segments se comparent donc en nombres.
static bool version_is_newer(const std::string& candidate, const std::string& current) {
    auto parts = [](const std::string& v) {
        std::vector<int> out;
        std::stringstream ss(v);
        std::string bit;
        while (std::getline(ss, bit, '.')) {
            try { out.push_back(std::stoi(bit)); } catch (...) { out.push_back(0); }
        }
        return out;
    };
    auto a = parts(candidate), b = parts(current);
    a.resize(std::max(a.size(), b.size()), 0);
    b.resize(a.size(), 0);
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) return a[i] > b[i];
    return false;
}

void MainWindow::check_app_update_async() {
    // Un build de DEVELOPPEMENT ne recoit jamais cette banniere.
    //
    // Elle s'adresse a un joueur qui a installe un paquet, dont la version est
    // exactement un tag. Un build local porte en plus le nombre de commits
    // depuis le tag et l'empreinte du commit ('+7.g1a2b3c4'), ou '.dirty' si
    // l'arbre a des modifications. Dans les deux cas son auteur sait mettre a
    // jour son depot, et lui proposer de telecharger une release est au mieux
    // inutile, au pire trompeur : son build contient souvent du code PLUS
    // recent que la release qu'on lui propose.
    const std::string me = BOOTCADE_VERSION;
    if (me.find('+') != std::string::npos || me.find(".dirty") != std::string::npos)
        return;

    // « Check for updates automatically ». Eteint, le lanceur ne demande plus
    // rien de lui-meme ; le bouton « Check for updates now » des reglages, lui,
    // reste utilisable : c'est bien la verification AUTOMATIQUE qu'on coupe.
    if (!m_settings_panel.checks_updates_auto()) return;

    std::thread([this, alive = m_alive_token]() {
        auto r = FbneoUpdateCheck::fetch_launcher_latest();
        if (!r.ok) return;                  // hors ligne ou quota : on se tait
        std::string tag = r.tag;
        if (!tag.empty() && tag[0] == 'v') tag.erase(0, 1);
        if (!version_is_newer(tag, BOOTCADE_VERSION)) return;

        std::lock_guard<std::mutex> live(alive->mutex);
        if (!alive->alive) return;
        m_app_update_tag = tag;
        m_app_update_dispatcher.emit();
    }).detach();
}

void MainWindow::on_app_update_result() {
    if (m_app_update_tag.empty()) return;
    m_app_update_label.set_text(Glib::ustring::compose(
        _("Bootcade %1 is available. You are running %2."),
        m_app_update_tag, BOOTCADE_VERSION));
    m_app_update_infobar.show();
}

void MainWindow::on_fbneo_update_check_result() {
    if (m_fbneo_update_sha.empty()) return; // fetch failed : stay silent, not an error the user needs to see

    nlohmann::json j;
    { std::ifstream fi(AppContext::get_config_path()); if (fi) { try { fi >> j; } catch (...) {} } }
    std::string known_sha = j.value("fbneo_release_sha", "");

    if (!known_sha.empty()) {
        // We have a launcher-recorded baseline (a download done through this
        // app) : the SHA comparison is exact, use it.
        if (known_sha != m_fbneo_update_sha) {
            m_fbneo_update_label.set_text(_("A new FBNeo version is available."));
            m_fbneo_update_infobar.show();
        }
        return;
    }

    // No baseline recorded: the configured executable was never downloaded
    // through this launcher (e.g. built by hand, or dropped in manually), so
    // there is no SHA to compare against. Fall back to comparing the
    // executable's mtime against the release's publish date : approximate,
    // but better than staying silent forever for these users.
    std::string fbneo_executable = m_settings_panel.get_fbneo_executable();
    if (fbneo_executable.empty()) return;

    std::time_t exe_mtime = get_file_mtime(fbneo_executable);
    if (exe_mtime < 0) return;

    std::time_t published = FbneoUpdateCheck::parse_iso8601(m_fbneo_update_published_at);
    if (published < 0) return; // can't tell, don't guess

    if (exe_mtime < published) {
        m_fbneo_update_label.set_text(_("A new FBNeo version is available."));
        m_fbneo_update_infobar.show();
    }
}

void MainWindow::on_fbneo_update_infobar_response(int response_id) {
    if (response_id == Gtk::RESPONSE_OK) {
        on_download_latest_fbneo(); // hides the infobar itself once the dialog opens
    } else {
        m_fbneo_update_infobar.hide();
    }
}

void MainWindow::on_download_latest_fbneo() {
    m_fbneo_update_infobar.hide();

    // $HOME, not current_path(): the working directory a launch happens to start
    // in is not stable across a desktop icon vs. a terminal vs. a dev checkout,
    // so this could silently extract into a different folder each time.
    const char* home_env = std::getenv("HOME");
    auto download_dialog = std::make_unique<DownloadDialog>(
        *this,
        // See SettingsPanel::on_download_fbneo_clicked for why this points at our
        // own fork instead of finalburnneo/FBNeo directly.
        "https://github.com/battousai90/FBNeo/releases/download/latest/linux-sdl2-x86_64.zip",
        home_env ? std::string(home_env) : std::filesystem::current_path().string()
    );

    download_dialog->set_settings_entry(&m_settings_panel.m_entry_fbneo);
    download_dialog->start_download();
    int result = download_dialog->run();
    // run() rend la main sans fermer la fenetre : elle restait affichee
    // derriere la question des DAT et toute leur generation.
    download_dialog.reset();

    // Record what "latest" pointed at just now, so a future startup check has a
    // baseline to compare against. The startup check already did this fetch in
    // most cases (m_fbneo_update_sha), so this usually costs nothing extra;
    // it only falls back to a fresh call if that never completed.
    auto record_sha = [](const std::string& sha) {
        nlohmann::json j;
        const std::string path = AppContext::get_config_path();
        { std::ifstream fi(path); if (fi) { try { fi >> j; } catch (...) { j = nlohmann::json{}; } } }
        j["fbneo_release_sha"] = sha;
        std::ofstream fo(path);
        if (fo) fo << j.dump(4);
    };
    if (!m_fbneo_update_sha.empty()) {
        record_sha(m_fbneo_update_sha);
    } else {
        // The startup check never answered : ask GitHub again, but off the
        // GTK thread (up to 10 s on a slow link), and write config.json back
        // on it, once the window is known to be alive.
        std::thread([record_sha, alive = m_alive_token] {
            auto r = FbneoUpdateCheck::fetch_latest();
            if (!r.ok || r.sha.empty()) return;
            std::lock_guard<std::mutex> live(alive->mutex);
            if (!alive->alive) return;
            Glib::signal_idle().connect_once([record_sha, sha = r.sha] { record_sha(sha); });
        }).detach();
    }

    if (result != Gtk::RESPONSE_OK) return; // download failed or was cancelled

    // A new build usually means new/changed game definitions : offer to chain
    // straight into DAT generation and the database update so the user ends
    // up with a working, up-to-date library in one flow instead of having to
    // remember these two extra steps.
    ConfirmationDialog confirm_dialog(*this,
        _("Generate DAT files?"),
        _("Generate DAT files from the new FBNeo build and update the game database now?"),
        "bc-generate-dat.svg");
    if (!confirm_dialog.show_and_confirm()) return;

    GenerateDAT::execute(*this, m_settings_panel.get_fbneo_executable(), m_settings_panel.get_dat_path());
    // Not on_update_dat_clicked(): that shows its own "continue?" confirmation,
    // which : coming right after this dialog's own confirm and GenerateDAT's
    // own success dialog : is an easy dialog to reflexively dismiss. Cancelling
    // it silently skipped the database reload entirely: the DAT files on disk
    // were current, but the games table (and the audit reading it) stayed on
    // the old snapshot with no error or indication anything was wrong.
    // A new FinalBurn Neo build changes FinalBurn Neo's DATs only.
    do_update_dat("fbneo");
}

void MainWindow::on_generate_dat_files() {
    GenerateDAT::execute(*this, m_settings_panel.get_fbneo_executable(), m_settings_panel.get_dat_path());
}

// === Thumbnail Download Methods ===

void MainWindow::show_download_progress(const std::string& filename, int current, int total, double percentage) {
    // Update progress bar
    m_download_progress_bar.set_fraction(percentage / 100.0);
    m_download_progress_bar.set_text(std::to_string(static_cast<int>(percentage)) + "%");
    
    // Update status label with filename and count
    std::string status_text = "Downloading: " + filename + " (" + 
                             std::to_string(current) + "/" + std::to_string(total) + ")";
    m_download_status_label.set_text(status_text);
    
    // Show the download progress box if not visible
    if (!m_download_progress_box.get_visible()) {
        m_download_status_label.show();
        m_download_progress_bar.show();
        m_download_cancel_button.show();
        m_download_progress_box.show();
        // Hide main status label during mass downloads to avoid confusion
        m_status_label.hide();
    }
    
}

void MainWindow::hide_download_progress() {
    m_download_progress_box.hide();
    // Show main status label again after mass download completes
    m_status_label.show();
    // Clear the main status bar or show completion message
    update_status_bar_stats();
}

void MainWindow::on_download_previews_clicked() {
    std::string previews_dir = m_settings_panel.get_previews_path();
    
    // Vérifier que le répertoire de previews est configuré
    if (previews_dir.empty()) {
        SettingsUi::notice(*this, _("Previews Directory Not Set"),
                           _("Please set the previews directory in Settings before downloading."),
                           "bc-warning.svg");
        return;
    }
    
    // Vérifier qu'il y a des jeux chargés
    if (m_cached_games.empty()) {
        SettingsUi::notice(*this, _("No Games Loaded"),
                           _("Please load or scan games before downloading previews."),
                           "bc-warning.svg");
        return;
    }
    
    // Vérifier si un téléchargement est déjà en cours
    if (m_thumbnail_downloader.is_downloading()) {
        SettingsUi::notice(*this, _("Download In Progress"),
                           _("Preview download is already in progress."),
                           "bc-info.svg");
        return;
    }
    
    // Demander confirmation à l'utilisateur
    ConfirmationDialog confirm_dialog(*this, _("Download Previews"),
        _("This will download previews for ") + std::to_string(m_cached_games.size())
            + " games from FBNeo-extras.\n\nContinue?",
        "bc-image.svg", /*destructive=*/false,
        _("This may take several minutes."));

    if (!confirm_dialog.show_and_confirm()) return;
    
    // Close settings dialog if it's open
    m_close_settings_signal.emit();
    
    std::cout << "[INFO] Starting previews download for " << m_cached_games.size() << " games" << std::endl;
    
    // Créer le callback de progression
    auto progress_callback = [this](const std::string& filename, int current, int total, double percentage) {
        std::cout << "[DEBUG] Progress callback called: " << filename << " - " << percentage << "%" << std::endl;
        
        // Mettre à jour les variables partagées (lues par le fil GTK)
        {
            std::lock_guard<std::mutex> lk(m_download_progress_mutex);
            m_current_download_file = filename;
            m_current_download_index = current;
            m_total_download_count = total;
            m_download_percentage = percentage;
        }
        
        // Déclencher le dispatcher approprié
        if (percentage >= 100.0) {
            m_download_finished_dispatcher.emit();
        } else {
            m_download_progress_dispatcher.emit();
        }
    };
    
    // Démarrer le téléchargement
    m_thumbnail_downloader.start_download(m_cached_games, previews_dir, ThumbnailDownloader::ArtworkType::Previews, progress_callback);
}

void MainWindow::on_download_titles_clicked() {
    std::string titles_dir = m_settings_panel.get_titles_path();
    
    // Vérifier que le répertoire de titles est configuré
    if (titles_dir.empty()) {
        SettingsUi::notice(*this, _("Titles Directory Not Set"),
                           _("Please set the titles directory in Settings before downloading."),
                           "bc-warning.svg");
        return;
    }
    
    // Vérifier qu'il y a des jeux chargés
    if (m_cached_games.empty()) {
        SettingsUi::notice(*this, _("No Games Loaded"),
                           _("Please load or scan games before downloading titles."),
                           "bc-warning.svg");
        return;
    }
    
    // Vérifier si un téléchargement est déjà en cours
    if (m_thumbnail_downloader.is_downloading()) {
        SettingsUi::notice(*this, _("Download In Progress"),
                           _("Titles download is already in progress."),
                           "bc-info.svg");
        return;
    }
    
    // Demander confirmation à l'utilisateur
    ConfirmationDialog confirm_dialog(*this, _("Download Titles"),
        _("This will download titles for ") + std::to_string(m_cached_games.size())
            + " games from FBNeo-extras.\n\nContinue?",
        "bc-image.svg", /*destructive=*/false,
        _("This may take several minutes."));

    if (!confirm_dialog.show_and_confirm()) return;
    
    // Close settings dialog if it's open
    m_close_settings_signal.emit();
    
    std::cout << "[INFO] Starting titles download for " << m_cached_games.size() << " games" << std::endl;
    
    // Créer le callback de progression
    auto progress_callback = [this](const std::string& filename, int current, int total, double percentage) {
        std::cout << "[DEBUG] Progress callback called: " << filename << " - " << percentage << "%" << std::endl;
        
        // Mettre à jour les variables partagées (lues par le fil GTK)
        {
            std::lock_guard<std::mutex> lk(m_download_progress_mutex);
            m_current_download_file = filename;
            m_current_download_index = current;
            m_total_download_count = total;
            m_download_percentage = percentage;
        }
        
        // Déclencher le dispatcher approprié
        if (percentage >= 100.0) {
            m_download_finished_dispatcher.emit();
        } else {
            m_download_progress_dispatcher.emit();
        }
    };
    
    // Démarrer le téléchargement
    m_thumbnail_downloader.start_download(m_cached_games, titles_dir, ThumbnailDownloader::ArtworkType::Titles, progress_callback);
}

void MainWindow::on_download_cancel_clicked() {
    std::cout << "[INFO] User cancelled download" << std::endl;
    
    // Cancel the download
    m_thumbnail_downloader.cancel_download();
    
    // Hide download progress and update status
    hide_download_progress();
    m_status_label.set_text(_("Download cancelled by user"));
}

void MainWindow::start_scan_thread(const std::vector<std::string>& roms_paths,
                                   const std::string& emulator) {
    if (m_scan_in_progress) {
        // Not dropped : "Move to library" can fill both libraries at once,
        // and each must be rescanned. Started when the current one is over.
        if (std::find(m_pending_scans.begin(), m_pending_scans.end(), emulator) == m_pending_scans.end())
            m_pending_scans.push_back(emulator);
        return;
    }

    m_scan_in_progress = true;
    m_scan_cancelled   = false;
    m_button_scan.set_sensitive(false);

    // Show inline scan progress in the status bar immediately.
    // show_all() is needed because set_no_show_all(true) was set at construction
    // (to prevent the box from appearing during the initial window show_all call).
    set_scan_status(_("Initializing scan…"), SettingsUi::State::Muted);
    m_scan_progress_bar.set_fraction(0.0);
    // IMPORTANT: show_all() is a no-op when no_show_all=true is set on the widget.
    // We must call show() on each child and the container individually.
    m_scan_status_dot.show();
    m_scan_progress_label.show();
    m_scan_progress_bar.show();
    m_scan_details_button.show();
    m_scan_status_box.show();

    std::cout << "[INFO] Starting background ROM scan in "
              << roms_paths.size() << " directories..." << std::endl;

    // rom_cache is the FinalBurn Neo scan's bookkeeping : its cleanup is
    // bounded by that library's roots, and only that scan needs it.
    if (emulator == "fbneo") m_database->startCacheCleanupThread(m_settings_panel.get_roms_paths());

    // Create dialog on the heap (non-modal) : destroyed when user closes it
    m_scan_dialog = std::make_unique<ROMScanDialog>(
        *this, m_database, roms_paths,
        m_settings_panel.is_scan_recursive(emulator),
        m_settings_panel.is_scan_loose_files(emulator),
        emulator);

    // Notify MainWindow when scan work is done (fires on GTK main thread)
    m_scan_dialog->signal_scan_complete().connect(
        sigc::mem_fun(*this, &MainWindow::on_scan_dialog_complete));

    // "Run in Background" button → start status-bar polling
    m_scan_dialog->signal_run_in_background().connect(
        sigc::mem_fun(*this, &MainWindow::on_scan_go_background));

    // Clean up only when the dialog is hidden AND the scan is already done.
    // If the scan is still running and the user hides the dialog ("Run in Background"),
    // we keep the dialog alive so polling can continue and "Details" can reopen it.
    m_scan_dialog->signal_hide().connect([this]() {
        if (m_scan_dialog && m_scan_dialog->is_scan_finished()) {
            m_scan_bg_poll_timer.disconnect();
            m_database->stopCacheCleanupThread();
            m_scan_dialog.reset();
        }
        // Scan still running → dialog is just hidden; timer & dialog stay alive
    });

    m_scan_dialog->start_scan();
    m_scan_dialog->show();    // non-modal: returns immediately

    // Start polling immediately so the status bar shows progress from the start
    m_scan_bg_poll_timer = Glib::signal_timeout().connect(
        sigc::mem_fun(*this, &MainWindow::on_scan_bg_poll), 300);
}

void MainWindow::on_scan_dialog_complete() {
    std::cout << "[INFO] Scan complete : refreshing game list" << std::endl;

    m_scan_bg_poll_timer.disconnect();

    m_cached_games = load_all_catalogs();
    m_search_blobs.clear();   // the haystacks describe the old vector
    // Et m_filtered_games pointait dedans : le laisser en place, c'est garder
    // des pointeurs vers un vecteur qui vient d'etre remplace.
    { std::lock_guard<std::mutex> lk(m_filter_mutex); m_filtered_games.clear(); }

    // Regenerate the filter cache so the left panel shows updated counts/systems
    m_filter_cache = FilterCache::generate_from_games(m_cached_games);
    save_filter_cache();
    m_filter_cache_loaded = true;
    populate_filter_tree();

    filter_games();
    update_status_bar_stats();

    // ROM Management's own views (Outbox/Quarantine/Library audit) are stale
    // the moment any scan finishes : most directly the one "Move to library"
    // itself triggers, closing the loop back to "the set now shows fixed".
    if (m_rom_manager) m_rom_manager->refresh_after_scan();
    AppContext::trim_heap();

    m_scan_in_progress = false;
    m_button_scan.set_sensitive(true);

    // Replace progress bar with a brief "done" (or "cancelled") message then hide after 4 s
    const std::string scanned = m_scan_dialog ? m_scan_dialog->emulator() : std::string("fbneo");
    size_t avail = m_database->getGameCountByStatus("available", scanned);
    bool was_cancelled = m_scan_dialog && m_scan_dialog->was_cancelled();
    if (was_cancelled) {
        set_scan_status(Glib::ustring::compose(
            _("Scan cancelled : %1 games available"), avail), SettingsUi::State::Warn);
    } else {
        set_scan_status(Glib::ustring::compose(
            _("Scan complete : %1 games available"), avail), SettingsUi::State::Ok);
    }
    m_scan_progress_bar.set_fraction(was_cancelled ? 0.0 : 1.0);
    m_scan_details_button.hide(); // no dialog to reopen anymore

    Glib::signal_timeout().connect_once([this]() {
        m_scan_status_box.hide();
        // Reset dialog pointer if still around (user never clicked Close)
        if (m_scan_dialog) {
            m_database->stopCacheCleanupThread();
            m_scan_dialog.reset();
        }
        // A scan asked for meanwhile : here, not in on_scan_dialog_complete,
        // which runs inside the finished dialog's own signal and must not
        // replace it under its feet.
        if (!m_pending_scans.empty() && !m_scan_in_progress) {
            const std::string next = m_pending_scans.front();
            m_pending_scans.erase(m_pending_scans.begin());
            start_scan_thread(m_settings_panel.get_roms_paths(next), next);
        }
    }, 4000);

    std::cout << "[INFO] Interface updated after scan" << std::endl;
}

/* L'etat du scan dans la barre du bas : la pastille ronde de la charte et
 * son texte, au lieu du pictogramme qui y tenait lieu de couleur. Le point
 * dit l'etat (en cours, termine, interrompu), le texte dit quoi. */
void MainWindow::set_scan_status(const Glib::ustring& text, SettingsUi::State state) {
    m_scan_progress_label.set_text(text);
    const char* tone = state == SettingsUi::State::Ok    ? "is-ok"
                     : state == SettingsUi::State::Warn  ? "is-warn"
                     : state == SettingsUi::State::Error ? "is-error" : nullptr;
    for (Gtk::Widget* w : {static_cast<Gtk::Widget*>(&m_scan_progress_label),
                           static_cast<Gtk::Widget*>(&m_scan_status_dot)}) {
        auto ctx = w->get_style_context();
        for (const char* c : {"is-ok", "is-warn", "is-error"}) ctx->remove_class(c);
        if (tone) ctx->add_class(tone);
    }
}

void MainWindow::on_scan_go_background() {
    // Dialog hid itself : polling was already running, nothing extra needed.
    // The "Details" button remains visible and reopens the dialog on click.
}

bool MainWindow::on_scan_bg_poll() {
    if (!m_scan_dialog) return false; // dialog destroyed : stop timer

    double pct = m_scan_dialog->get_scan_progress();
    std::string msg = m_scan_dialog->get_scan_message();

    // Shorten message for status bar (strip leading emoji + long paths)
    if (msg.size() > 45) msg = msg.substr(0, 42) + "…";

    int p = static_cast<int>(pct);
    set_scan_status(Glib::ustring::compose(_("%1%%  %2"), p, msg), SettingsUi::State::Muted);
    m_scan_progress_bar.set_fraction(pct / 100.0);

    return !m_scan_dialog->is_scan_finished(); // false stops the timer
}

void MainWindow::on_scan_progress() {
    // Update progress from scan thread via dispatcher
    int current = m_scan_current.load();
    int total = m_scan_total.load();
    
    if (total > 0) {
        double percentage = (double)current / total * 100.0;
        std::string status_text = "Scanning ROMs... " + std::to_string(current) + "/" + std::to_string(total) + " (" + std::to_string((int)percentage) + "%)";
        m_status_label.set_text(status_text);
        std::cout << status_text << std::endl;
    }
}

void MainWindow::on_scan_finished() {
    std::cout << "[INFO] ROM scan completed" << std::endl;
    
    // Reset scan state
    m_scan_in_progress = false;
    m_button_scan.set_sensitive(true);
    
    // Reload games from database and update display
    m_cached_games = load_all_catalogs();
    m_search_blobs.clear();   // the haystacks describe the old vector
    // Et m_filtered_games pointait dedans : le laisser en place, c'est garder
    // des pointeurs vers un vecteur qui vient d'etre remplace.
    { std::lock_guard<std::mutex> lk(m_filter_mutex); m_filtered_games.clear(); }
    filter_games();
    update_status_bar_stats();
    m_status_label.hide();
    
    // Join the worker thread
    if (m_scan_thread.joinable()) {
        m_scan_thread.join();
    }
    
    std::cout << "[INFO] Scan completed successfully - UI updated" << std::endl;
}

void MainWindow::load_filter_cache() {
    if (m_filter_cache_loaded) return;
    
    std::string cache_file = AppContext::get_user_config_dir() + "/filter_cache.json";
    
    if (FilterCache::load_from_file(cache_file, m_filter_cache)) {
        std::cout << "[INFO] Filter cache loaded successfully" << std::endl;
    } else {
        std::cout << "[WARNING] Filter cache not found. Use 'Update DAT' to generate filter cache." << std::endl;
        // Initialize with empty data - user needs to update DAT to populate filters
        m_filter_cache.systems.clear();
        m_filter_cache.manufacturers.clear();
        m_filter_cache.years.clear();
        m_filter_cache.sources.clear();
    }
    
    m_filter_cache_loaded = true;
}

void MainWindow::save_filter_cache() {
    std::string cache_file = AppContext::get_user_config_dir() + "/filter_cache.json";
    FilterCache::save_to_file(cache_file, m_filter_cache);
}

// Removed all ComboBox population methods - will implement MAMEUI-style filtering

void MainWindow::populate_filter_tree() {
    // Guard the whole rebuild: clearing the model drags the selection across the
    // remaining rows and fires selection-changed for each of them. The ★ Favorites
    // row is one of those, so a plain rescan used to silently switch the library
    // to favourites-only. Scoped so the early return below cannot leave it set.
    struct Guard {
        bool& flag;
        explicit Guard(bool& f) : flag(f) { flag = true; }
        ~Guard() { flag = false; }
    } guard(m_populating_filters);

    m_model_filters->clear();

    if (!m_filter_cache_loaded || m_cached_games.empty()) {
        return;
    }

    std::cout << "[INFO] Populating MAMEUI-style filter tree..." << std::endl;

    // Single pass over m_cached_games: precompute every per-category count.
    // The old code did 5+ separate passes over 25k games on the main thread,
    // which froze the UI long enough to trigger "Not Responding".
    std::unordered_map<std::string, int> system_counts;
    std::unordered_map<std::string, int> manuf_counts;
    std::unordered_map<std::string, int> year_counts;
    std::unordered_map<std::string, int> source_counts;
    std::unordered_map<std::string, int> aspect_counts;
    std::unordered_map<std::string, int> orientation_counts;
    std::unordered_map<std::string, int> status_counts;
    std::unordered_map<std::string, int> type_counts;
    // Ecrits dans le DAT par notre fork FBNeo. Restent vides avec un DAT
    // d'amont, et les categories ne s'affichent alors pas.
    std::unordered_map<std::string, int> genre_counts;
    std::unordered_map<std::string, int> family_counts;
    std::unordered_map<std::string, int> players_counts;
    // Compte par emulateur. Rempli seulement en portee « tous » : c'est le
    // seul cas ou la categorie s'affiche, et hors de la un increment de plus
    // par jeu serait paye pour rien sur 57 000 entrees.
    std::unordered_map<std::string, int> emulator_counts_map;
    const bool all_scope = m_active_emulator.empty();
    int favorite_count = 0;

    for (const auto& game : m_cached_games) {
        // Release type : a set can match several (a hack is usually a clone too).
        if (!emulator_in_scope(game)) continue;   // hors portee : ni compte ni ligne
        if (game.is_original())  type_counts["original"]++;
        if (game.is_clone())     type_counts["clone"]++;
        if (game.is_hack())      type_counts["hack"]++;
        if (game.is_homebrew())  type_counts["homebrew"]++;
        if (game.is_bootleg())   type_counts["bootleg"]++;
        if (game.is_prototype()) type_counts["prototype"]++;
        if (game.is_favorite)    favorite_count++;
        if (all_scope) emulator_counts_map[game.emulator]++;
        if (!game.system.empty())       system_counts[game.system]++;
        if (!game.manufacturer.empty()) manuf_counts[game.manufacturer]++;
        for (const auto& v : split_dat_values(game.genre))  genre_counts[v]++;
        for (const auto& v : split_dat_values(game.family)) family_counts[v]++;
        if (game.players > 0)     players_counts[std::to_string(game.players)]++;
        if (!game.year.empty())         year_counts[game.year]++;
        if (!game.sourcefile.empty()) {
            std::string game_source = game.sourcefile;
            size_t slash_pos = game_source.find('/');
            if (slash_pos != std::string::npos) game_source = game_source.substr(0, slash_pos);
            source_counts[game_source]++;
        }
        if (!game.aspect_x.empty() && !game.aspect_y.empty())
            aspect_counts[game.aspect_x + ":" + game.aspect_y]++;
        if (!game.orientation.empty()) orientation_counts[game.orientation]++;
        status_counts[game.status]++;
    }

    // Add "All Games" root item
    auto root = m_model_filters->append();
    (*root)[m_filter_columns.m_col_icon] = get_filter_icon("All Games");
    (*root)[m_filter_columns.m_col_name] = _("All Games");
    (*root)[m_filter_columns.m_col_type] = "root";
    (*root)[m_filter_columns.m_col_value] = "All";
    {
        int in_scope = 0;
        for (const auto& g : m_cached_games) if (emulator_in_scope(g)) ++in_scope;
        (*root)[m_filter_columns.m_col_count] = in_scope;
    }

    // Favourites : a top-level entry mirroring the header star toggle.
    {
        auto fav = m_model_filters->append();
        (*fav)[m_filter_columns.m_col_icon] = get_filter_icon("Favorites");
        (*fav)[m_filter_columns.m_col_name] = std::string("★ ") + _("Favorites");
        (*fav)[m_filter_columns.m_col_type] = "favorite";
        (*fav)[m_filter_columns.m_col_value] = "1";
        (*fav)[m_filter_columns.m_col_count] = favorite_count;
    }

    // Games the score service can rank. Counted here rather than kept as a
    // running total because the supported list can land after the tree is
    // first built, and the node has to show a truthful number either way.
    {
        int ranked_count = 0;
        for (const auto& game : m_cached_games)
            if (emulator_in_scope(game) && game_ranks_online(game.emulator, game.system, game.name)) ranked_count++;
        if (ranked_count > 0) {
            auto hi = m_model_filters->append();
            (*hi)[m_filter_columns.m_col_icon] = get_filter_icon("Highscore");
            (*hi)[m_filter_columns.m_col_name] = std::string("◆ ") + _("Highscore");
            (*hi)[m_filter_columns.m_col_type] = "hiscore";
            (*hi)[m_filter_columns.m_col_value] = "1";
            (*hi)[m_filter_columns.m_col_count] = ranked_count;
        }
    }

    // En-tetes de section. Purement visuels, type "section" : ils ne
    // filtrent rien et ne se selectionnent pas. La colonne portait onze
    // entrees de meme poids, ou la bibliotheque (tous les jeux, favoris,
    // classes) se confondait avec les criteres de tri.
    auto section = [this](const std::string& label) {
        auto row = m_model_filters->append();
        (*row)[m_filter_columns.m_col_name] = label;
        (*row)[m_filter_columns.m_col_type] = "section";
        (*row)[m_filter_columns.m_col_value] = "";
    };

    section(_("FILTERS"));

    // L'emulateur d'abord : quand les deux catalogues sont affiches ensemble,
    // c'est la question qu'on se pose avant le systeme ou le fabricant. Hors
    // de cette portee la categorie n'aurait qu'une entree et ne filtrerait
    // rien, donc elle ne parait pas.
    if (emulator_counts_map.size() > 1) {
        auto emu_root = m_model_filters->append();
        (*emu_root)[m_filter_columns.m_col_icon] = get_filter_icon("Systems");
        (*emu_root)[m_filter_columns.m_col_name] = _("Emulator");
        (*emu_root)[m_filter_columns.m_col_type] = "category";
        (*emu_root)[m_filter_columns.m_col_value] = "";

        // L'ordre du registre, pas celui de la table de hachage : il est
        // stable d'un lancement a l'autre, ce que le second n'est pas.
        for (const auto& e : EmulatorRegistry::all()) {
            auto it = emulator_counts_map.find(e.id);
            if (it == emulator_counts_map.end() || it->second == 0) continue;
            auto child = m_model_filters->append(emu_root->children());
            (*child)[m_filter_columns.m_col_icon]  = get_filter_icon("item");
            (*child)[m_filter_columns.m_col_name]  = e.name;
            (*child)[m_filter_columns.m_col_type]  = "emulator";
            (*child)[m_filter_columns.m_col_value] = e.id;
            (*child)[m_filter_columns.m_col_count] = it->second;
        }
    }

    // Systems
    //
    // La liste vient de ce que le catalogue contient vraiment, pas du cache sur
    // disque : celui-ci est ecrit pour une source donnee et ignorait donc les
    // systemes apportes par un second emulateur — les 15 851 machines
    // mecaniques de MAME etaient chargees et comptees, mais aucune ligne ne les
    // montrait. Le cache ne sert plus qu'a fixer l'ordre d'affichage.
    std::vector<std::string> systems_shown;
    {
        std::set<std::string> seen;
        for (const auto& s : m_filter_cache.systems)
            if (system_counts.count(s) && seen.insert(s).second)
                systems_shown.push_back(s);
        std::vector<std::string> extra;
        for (const auto& [s, n] : system_counts)
            if (n > 0 && seen.insert(s).second) extra.push_back(s);
        std::sort(extra.begin(), extra.end());
        systems_shown.insert(systems_shown.end(), extra.begin(), extra.end());
    }

    if (!systems_shown.empty()) {
        auto systems_root = m_model_filters->append();
        (*systems_root)[m_filter_columns.m_col_icon] = get_filter_icon("Systems");
        (*systems_root)[m_filter_columns.m_col_name] = _("Systems");
        (*systems_root)[m_filter_columns.m_col_type] = "category";
        (*systems_root)[m_filter_columns.m_col_value] = "";

        for (const auto& system : systems_shown) {
            int count = system_counts[system];
            auto child = m_model_filters->append(systems_root->children());
            (*child)[m_filter_columns.m_col_icon] = get_filter_icon("item");
            (*child)[m_filter_columns.m_col_name] = system;
            (*child)[m_filter_columns.m_col_type] = "system";
            (*child)[m_filter_columns.m_col_value] = system;
            (*child)[m_filter_columns.m_col_count] = count;
        }
    }

    // Release type: originals vs the derivative sets (clones, hacks, …).
    {
        auto type_root = m_model_filters->append();
        (*type_root)[m_filter_columns.m_col_icon] = get_filter_icon("Type");
        (*type_root)[m_filter_columns.m_col_name] = _("Type");
        (*type_root)[m_filter_columns.m_col_type] = "category";
        (*type_root)[m_filter_columns.m_col_value] = "";

        const std::vector<std::pair<std::string, std::string>> types = {
            {"original",  _("Original")},  {"clone",     _("Clone")},
            {"hack",      _("Hack")},      {"homebrew",  _("Homebrew")},
            {"bootleg",   _("Bootleg")},   {"prototype", _("Prototype")}};
        for (const auto& [key, label] : types) {
            int count = type_counts[key];
            if (count == 0) continue;
            auto child = m_model_filters->append(type_root->children());
            (*child)[m_filter_columns.m_col_icon] = get_filter_icon("item");
            (*child)[m_filter_columns.m_col_name] = label;
            (*child)[m_filter_columns.m_col_type] = "type";
            (*child)[m_filter_columns.m_col_value] = key;
            (*child)[m_filter_columns.m_col_count] = count;
        }
    }

    // Manufacturers (top 20)
    if (!m_filter_cache.manufacturers.empty()) {
        auto manuf_root = m_model_filters->append();
        (*manuf_root)[m_filter_columns.m_col_icon] = get_filter_icon("Manufacturers");
        (*manuf_root)[m_filter_columns.m_col_name] = _("Manufacturers");
        (*manuf_root)[m_filter_columns.m_col_type] = "category";
        (*manuf_root)[m_filter_columns.m_col_value] = "";

        std::vector<std::pair<std::string, int>> sorted_manufs(manuf_counts.begin(), manuf_counts.end());
        std::sort(sorted_manufs.begin(), sorted_manufs.end(),
                 [](const auto& a, const auto& b) { return a.second > b.second; });

        int added = 0;
        for (const auto& [manuf, count] : sorted_manufs) {
            if (added >= 20) break;
            auto child = m_model_filters->append(manuf_root->children());
            (*child)[m_filter_columns.m_col_icon] = get_filter_icon("item");
            (*child)[m_filter_columns.m_col_name] = manuf;
            (*child)[m_filter_columns.m_col_type] = "manufacturer";
            (*child)[m_filter_columns.m_col_value] = manuf;
            (*child)[m_filter_columns.m_col_count] = count;
            added++;
        }
    }

    /* Genre, serie et nombre de joueurs.
       Chaque categorie ne s'affiche que si les DAT installes portent
       l'information : un DAT d'amont ne la porte pas, et un filtre vide ne
       rend service a personne. */
    auto add_simple_category = [&](const char* label, const char* type,
                                   const std::unordered_map<std::string, int>& counts,
                                   bool numeric, size_t limit) {
        if (counts.empty()) return;
        auto root = m_model_filters->append();
        (*root)[m_filter_columns.m_col_icon] = get_filter_icon(label);   // the English key
        (*root)[m_filter_columns.m_col_name] = _(label);
        (*root)[m_filter_columns.m_col_type] = "category";
        (*root)[m_filter_columns.m_col_value] = "";

        std::vector<std::pair<std::string, int>> sorted(counts.begin(), counts.end());
        if (numeric) {
            // « 1, 2, 3, 4 » se lit ; « 2, 1, 4, 3 » demande un effort pour rien.
            std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
                return std::stoi(a.first) < std::stoi(b.first);
            });
        } else {
            std::sort(sorted.begin(), sorted.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
        }

        size_t added = 0;
        for (const auto& [value, count] : sorted) {
            if (limit && added >= limit) break;
            auto child = m_model_filters->append(root->children());
            (*child)[m_filter_columns.m_col_icon] = get_filter_icon("item");
            (*child)[m_filter_columns.m_col_name] = value;
            (*child)[m_filter_columns.m_col_type] = type;
            (*child)[m_filter_columns.m_col_value] = value;
            (*child)[m_filter_columns.m_col_count] = count;
            added++;
        }
    };
    add_simple_category(N_("Genres"),  "genre",   genre_counts,   false, 30);
    add_simple_category(N_("Series"),  "family",  family_counts,  false, 0);
    add_simple_category(N_("Players"), "players", players_counts, true,  0);

    /* Dit a voix haute si les DAT installes portent les champs FBNeo.
       Sans cette ligne, des DAT qui cesseraient de les porter feraient
       simplement disparaitre trois categories, sans que rien n'indique
       pourquoi : le joueur croirait a une regression du lanceur. */
    if (genre_counts.empty() && family_counts.empty() && players_counts.empty()) {
        std::cerr << "[INFO] Filtres genre/serie/joueurs absents : les DAT installes "
                     "ne portent pas ces champs (DAT d'amont)." << std::endl;
    } else {
        std::cerr << "[INFO] Filtres FBNeo : " << genre_counts.size() << " genres, "
                  << family_counts.size() << " series, " << players_counts.size()
                  << " valeurs de joueurs." << std::endl;
    }

    // Years (grouped by decade)
    if (!m_filter_cache.years.empty()) {
        auto years_root = m_model_filters->append();
        (*years_root)[m_filter_columns.m_col_icon] = get_filter_icon("Years");
        (*years_root)[m_filter_columns.m_col_name] = _("Years");
        (*years_root)[m_filter_columns.m_col_type] = "category";
        (*years_root)[m_filter_columns.m_col_value] = "";

        std::map<std::string, std::vector<std::string>> decades;
        for (const auto& year : m_filter_cache.years) {
            if (year.length() == 4) {
                std::string decade = year.substr(0, 3) + "0s";
                decades[decade].push_back(year);
            }
        }

        for (const auto& [decade, years] : decades) {
            auto decade_node = m_model_filters->append(years_root->children());
            (*decade_node)[m_filter_columns.m_col_icon] = get_filter_icon("item");
            (*decade_node)[m_filter_columns.m_col_name] = decade;
            (*decade_node)[m_filter_columns.m_col_type] = "category";
            (*decade_node)[m_filter_columns.m_col_value] = "";

            for (const auto& year : years) {
                int count = year_counts[year];
                auto child = m_model_filters->append(decade_node->children());
                (*child)[m_filter_columns.m_col_icon] = get_filter_icon("item");
                (*child)[m_filter_columns.m_col_name] = year;
                (*child)[m_filter_columns.m_col_type] = "year";
                (*child)[m_filter_columns.m_col_value] = year;
                (*child)[m_filter_columns.m_col_count] = count;
            }
        }
    }

    // Sources
    if (!m_filter_cache.sources.empty()) {
        auto sources_root = m_model_filters->append();
        (*sources_root)[m_filter_columns.m_col_icon] = get_filter_icon("Sources");
        (*sources_root)[m_filter_columns.m_col_name] = _("Sources");
        (*sources_root)[m_filter_columns.m_col_type] = "category";
        (*sources_root)[m_filter_columns.m_col_value] = "";

        for (const auto& source : m_filter_cache.sources) {
            int count = source_counts[source];
            auto child = m_model_filters->append(sources_root->children());
            (*child)[m_filter_columns.m_col_icon] = get_filter_icon("item");
            (*child)[m_filter_columns.m_col_name] = source;
            (*child)[m_filter_columns.m_col_type] = "source";
            (*child)[m_filter_columns.m_col_value] = source;
            (*child)[m_filter_columns.m_col_count] = count;
        }
    }


    section(_("MORE FILTERS"));

    // Aspect Ratio
    auto aspect_root = m_model_filters->append();
    (*aspect_root)[m_filter_columns.m_col_icon] = get_filter_icon("Aspect Ratio");
    (*aspect_root)[m_filter_columns.m_col_name] = _("Aspect Ratio");
    (*aspect_root)[m_filter_columns.m_col_type] = "category";
    (*aspect_root)[m_filter_columns.m_col_value] = "";

    for (const auto& [aspect, count] : aspect_counts) {
        auto child = m_model_filters->append(aspect_root->children());
        (*child)[m_filter_columns.m_col_icon] = get_filter_icon("item");
        (*child)[m_filter_columns.m_col_name] = aspect;
        (*child)[m_filter_columns.m_col_type] = "aspect";
        (*child)[m_filter_columns.m_col_value] = aspect;
        (*child)[m_filter_columns.m_col_count] = count;
    }

    // Orientation
    auto orientation_root = m_model_filters->append();
    (*orientation_root)[m_filter_columns.m_col_icon] = get_filter_icon("Orientation");
    (*orientation_root)[m_filter_columns.m_col_name] = _("Orientation");
    (*orientation_root)[m_filter_columns.m_col_type] = "category";
    (*orientation_root)[m_filter_columns.m_col_value] = "";

    for (const auto& [orientation, count] : orientation_counts) {
        auto child = m_model_filters->append(orientation_root->children());
        (*child)[m_filter_columns.m_col_icon] = get_filter_icon("item");
        (*child)[m_filter_columns.m_col_name] = orientation;
        (*child)[m_filter_columns.m_col_type] = "orientation";
        (*child)[m_filter_columns.m_col_value] = orientation;
        (*child)[m_filter_columns.m_col_count] = count;
    }

    // ROM Status
    auto status_root = m_model_filters->append();
    (*status_root)[m_filter_columns.m_col_icon] = get_filter_icon("ROM Status");
    (*status_root)[m_filter_columns.m_col_name] = _("ROM Status");
    (*status_root)[m_filter_columns.m_col_type] = "category";
    (*status_root)[m_filter_columns.m_col_value] = "";

    // Coloured status dots (green/amber/grey) instead of a plain checkmark.
    auto status_dot = [](const std::string& color) -> Glib::RefPtr<Gdk::Pixbuf> {
        std::string svg = "<svg width='14' height='14' xmlns='http://www.w3.org/2000/svg'>"
                          "<circle cx='7' cy='7' r='5' fill='" + color + "'/></svg>";
        try {
            auto loader = Gdk::PixbufLoader::create("svg");
            loader->set_size(14, 14);
            loader->write(reinterpret_cast<const guint8*>(svg.data()), svg.size());
            loader->close();
            return loader->get_pixbuf();
        } catch (...) { return {}; }
    };
    for (const auto& [status, count] : status_counts) {
        const std::string color = SettingsUi::tone_hex(*this,
                                    status == "available" ? "success"
                                  : status == "incorrect" ? "warning"
                                  : status == "missing"   ? "disabled" : "muted");
        auto child = m_model_filters->append(status_root->children());
        (*child)[m_filter_columns.m_col_icon] = status_dot(color);
        (*child)[m_filter_columns.m_col_name] = status;
        (*child)[m_filter_columns.m_col_type] = "status";
        (*child)[m_filter_columns.m_col_value] = status;
        (*child)[m_filter_columns.m_col_count] = count;
    }

    m_treeview_filters.collapse_all();
    std::cout << "[INFO] Filter tree populated successfully" << std::endl;
}

void MainWindow::on_filter_selection_changed() {
    // Selection changes emitted while the tree is being rebuilt are GTK bookkeeping,
    // not user intent.
    if (m_populating_filters) return;

    auto selection = m_treeview_filters.get_selection();
    auto iter = selection->get_selected();
    if (!iter) return;
    
    Gtk::TreeModel::Row row = *iter;
    Glib::ustring type_ustring = row[m_filter_columns.m_col_type];
    Glib::ustring value_ustring = row[m_filter_columns.m_col_value];
    std::string type = type_ustring.raw();
    std::string value = value_ustring.raw();
    
    if (type == "root") {
        // "All Games" is the only reset: drop every dimension and the star.
        m_active_filters.clear();
        m_show_favorites_only = false;
        m_suppress_fav_toggle = true;
        m_btn_favorites.set_active(false);
        m_suppress_fav_toggle = false;
    } else if (type == "category") {
        // A category row is just an expander. It used to clear every filter,
        // which made stacking impossible: reaching Type > Original meant
        // clicking "Type" first, which silently dropped the system filter.
        return;
    } else if (type == "favorite") {
        // Mirrors the header star; leaves the other dimensions alone.
        m_show_favorites_only = true;
        m_suppress_fav_toggle = true;
        m_btn_favorites.set_active(true);
        m_suppress_fav_toggle = false;
    } else {
        // One value per dimension: picking another system replaces the system,
        // but leaves the type/year/… filters standing.
        m_active_filters[type] = value;
    }

    apply_tree_filters();
}

// "name description manufacturer year", lower-cased, for the i-th cached
// game. Rebuilt lazily after the library vector was reloaded (every
// assignment of m_cached_games clears it).
const std::string& MainWindow::search_blob(size_t idx) {
    if (m_search_blobs.size() != m_cached_games.size()) {
        m_search_blobs.clear();
        m_search_blobs.reserve(m_cached_games.size());
        for (const auto& g : m_cached_games) {
            std::string b = g.name + ' ' + g.description + ' ' + g.manufacturer + ' ' + g.year;
            std::transform(b.begin(), b.end(), b.begin(), ::tolower);
            m_search_blobs.push_back(std::move(b));
        }
    }
    return m_search_blobs[idx];
}

void MainWindow::apply_tree_filters() {
    // Phase timings, printed with BOOTCADE_WATCHDOG=1 : this runs on the GTK
    // thread, so every millisecond here is a millisecond the window freezes.
    static const bool perf_log = [] { const char* v = std::getenv("BOOTCADE_WATCHDOG"); return v && *v && std::string(v) != "0"; }();
    using clk = std::chrono::steady_clock;
    const auto t_start = clk::now();
    auto ms_since = [](clk::time_point a) { return std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - a).count(); };

    rebuild_filter_chips();

    // Devant un seul catalogue, la colonne repeterait le meme mot sur toutes
    // les lignes : elle ne dit quelque chose que quand les deux se melangent.
    if (m_emulator_column) m_emulator_column->set_visible(show_emulator_marks());

    // Detach the model and disable sort during the bulk rebuild : GTK
    // otherwise refreshes the view (and re-sorts) on every append, which
    // freezes the UI long enough to trigger "Not Responding" on 25k+ rows.
    m_treeview_games.unset_model();
    int prev_sort_col = Gtk::TreeSortable::DEFAULT_SORT_COLUMN_ID;
    Gtk::SortType prev_sort_order = Gtk::SORT_ASCENDING;
    bool had_sort = m_model_games->get_sort_column_id(prev_sort_col, prev_sort_order);
    m_model_games->set_sort_column(Gtk::TreeSortable::DEFAULT_UNSORTED_COLUMN_ID, Gtk::SORT_ASCENDING);
    m_model_games->clear();

    std::string search_text = m_search_entry.get_text();
    std::transform(search_text.begin(), search_text.end(), search_text.begin(), ::tolower);
    
    // Des pointeurs vers m_cached_games, pas des copies : filtrer ne duplique
    // plus le catalogue. Les pointeurs ne survivent pas a une reaffectation de
    // m_cached_games, qui vide donc m_filtered_games au meme endroit.
    std::vector<const Game*> filtered_games;
    
    for (size_t idx = 0; idx < m_cached_games.size(); ++idx) {
        const auto& game = m_cached_games[idx];
        if (!emulator_in_scope(game)) continue;
        bool matches = true;
        
        // Apply active filters
        for (const auto& [filter_type, filter_value] : m_active_filters) {
            if (filter_type == "emulator" && game.emulator != filter_value) {
                matches = false; break;
            }
            if (filter_type == "system" && game.system != filter_value) {
                matches = false; break;
            }
            if (filter_type == "manufacturer" && game.manufacturer != filter_value) {
                matches = false; break;
            }
            if (filter_type == "year" && game.year != filter_value) {
                matches = false; break;
            }
            // Champ multiple : on cherche la valeur parmi celles du jeu.
            if (filter_type == "genre" && !value_list_contains(game.genre, filter_value)) {
                matches = false; break;
            }
            if (filter_type == "family" && !value_list_contains(game.family, filter_value)) {
                matches = false; break;
            }
            if (filter_type == "players" && std::to_string(game.players) != filter_value) {
                matches = false; break;
            }
            if (filter_type == "source") {
                std::string game_source = game.sourcefile;
                size_t slash_pos = game_source.find('/');
                if (slash_pos != std::string::npos) {
                    game_source = game_source.substr(0, slash_pos);
                }
                if (game_source != filter_value) {
                    matches = false; break;
                }
            }
            if (filter_type == "aspect") {
                std::string game_aspect = "";
                if (!game.aspect_x.empty() && !game.aspect_y.empty()) {
                    game_aspect = game.aspect_x + ":" + game.aspect_y;
                }
                if (game_aspect != filter_value) {
                    matches = false; break;
                }
            }
            if (filter_type == "orientation" && game.orientation != filter_value) {
                matches = false; break;
            }
            if (filter_type == "status" && game.status != filter_value) {
                matches = false; break;
            }
            if (filter_type == "mode" && filter_value == "console") {
                // Console mode - exclude arcade games
                if (game.system == "Arcade") {
                    matches = false; break;
                }
            }
            if (filter_type == "type") {
                bool ok = filter_value == "original"  ? game.is_original()
                        : filter_value == "clone"     ? game.is_clone()
                        : filter_value == "hack"      ? game.is_hack()
                        : filter_value == "homebrew"  ? game.is_homebrew()
                        : filter_value == "bootleg"   ? game.is_bootleg()
                        : filter_value == "prototype" ? game.is_prototype()
                        : true;
                if (!ok) { matches = false; break; }
            }
            if (filter_type == "favorite" && !game.is_favorite) {
                matches = false; break;
            }
            if (filter_type == "hiscore" && !game_ranks_online(game.emulator, game.system, game.name)) {
                matches = false; break;
            }
        }

        if (!matches) continue;

        // Header star toggle: an overlay on top of whichever filter is active,
        // so "Favorites + Neo Geo" narrows rather than replaces.
        if (m_show_favorites_only && !game.is_favorite) continue;
        
        // Apply search filter : one lower-cased haystack per game, built once
        // per library load rather than four string copies per game per key.
        if (!search_text.empty() && search_blob(idx).find(search_text) == std::string::npos) continue;

        filtered_games.push_back(&game);
    }
    
    const auto t_matched = clk::now();
    sort_games(filtered_games);
    const auto t_sorted = clk::now();

    // Update TreeView
    append_game_rows(filtered_games);
    
    // Restore sort + reattach model (single redraw instead of one per insert)
    if (had_sort &&
        prev_sort_col != Gtk::TreeSortable::DEFAULT_SORT_COLUMN_ID &&
        prev_sort_col != Gtk::TreeSortable::DEFAULT_UNSORTED_COLUMN_ID) {
        m_model_games->set_sort_column(prev_sort_col, prev_sort_order);
    }
    m_treeview_games.set_model(m_model_games);
    const auto t_store = clk::now();

    // Rebuild the active custom view so its row references stay valid.
    refresh_active_view();
    const auto t_view = clk::now();

    // Update stats
    {
        std::lock_guard<std::mutex> lock(m_filter_mutex);
        m_filtered_games = std::move(filtered_games);
    }

    update_status_bar_stats();
    // The previous rows' strings were just freed : hand the pages back,
    // otherwise every re-filter leaves the freed memory sitting in the heap.
    AppContext::trim_heap();
    if (perf_log)
        std::cout << "[PERF] filter: rows=" << m_filtered_games.size()
                  << " match=" << std::chrono::duration_cast<std::chrono::milliseconds>(t_matched - t_start).count()
                  << "ms sort=" << std::chrono::duration_cast<std::chrono::milliseconds>(t_sorted - t_matched).count()
                  << "ms store=" << std::chrono::duration_cast<std::chrono::milliseconds>(t_store - t_sorted).count()
                  << "ms view=" << std::chrono::duration_cast<std::chrono::milliseconds>(t_view - t_store).count()
                  << "ms stats=" << ms_since(t_view) << "ms total=" << ms_since(t_start) << "ms" << std::endl;
}

void MainWindow::update_filter_counts() {
    // This can be called to refresh counts without rebuilding the whole tree
    // For now, we'll just repopulate the tree
    populate_filter_tree();
}

Glib::RefPtr<Gdk::Pixbuf> MainWindow::get_filter_icon(const std::string& category) {
    // Cache pixbufs per category : called once per filter row and the "item"
    // bucket is hit dozens of times per filter-tree rebuild.
    static std::unordered_map<std::string, Glib::RefPtr<Gdk::Pixbuf>> cache;
    auto it = cache.find(category);
    if (it != cache.end()) return it->second;

    // Clean, consistent line icons drawn as SVG (accent for categories, muted for
    // items) so the sidebar matches the modern theme instead of mismatched bitmaps.
    const std::string accent = "#9a8cff";
    const std::string muted  = "#9aa0b0";
    std::string color = accent;
    std::string body;
    if (category == "All Games") {
        body = "<rect x='2' y='3' width='12' height='4' rx='1'/><rect x='2' y='9' width='12' height='4' rx='1'/>";
    } else if (category == "Systems") {
        body = "<rect x='2' y='3' width='12' height='8' rx='1'/><path d='M6 13h4M8 11v2'/>";
    } else if (category == "Manufacturers") {
        body = "<rect x='3' y='2' width='10' height='12' rx='1'/><path d='M6 5h1M9 5h1M6 8h1M9 8h1M7 14v-3h2v3'/>";
    } else if (category == "Years") {
        body = "<rect x='2' y='3' width='12' height='11' rx='1.5'/><path d='M2 6h12M5 2v3M11 2v3'/>";
    } else if (category == "Sources") {
        body = "<path d='M4 2h5l4 4v8a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V3a1 1 0 0 1 1-1z'/><path d='M9 2v4h4'/>";
    } else if (category == "Aspect Ratio") {
        body = "<rect x='2' y='4' width='12' height='8' rx='1'/>";
    } else if (category == "Orientation") {
        body = "<path d='M12 8a4 4 0 1 1-1.5-3.1'/><path d='M12 3v3h-3'/>";
    } else if (category == "ROM Status") {
        body = "<circle cx='8' cy='8' r='6'/><path d='M8 7v4'/><circle cx='8' cy='5' r='0.7' fill='" + accent + "' stroke='none'/>";
    } else if (category == "Favorites") {
        body = "<path d='M8 2l1.9 3.8 4.1.6-3 2.9.7 4.1L8 11.5 4.3 13.4l.7-4.1-3-2.9 4.1-.6z'/>";
    } else if (category == "Highscore") {
        body = "<path d='M8 2l4 6-4 6-4-6z'/>";
    } else if (category == "Emulator") {   // une puce
        body = "<rect x='4' y='4' width='8' height='8' rx='1'/>"
               "<path d='M6.5 1.5v2.5M9.5 1.5v2.5M6.5 12v2.5M9.5 12v2.5"
               "M1.5 6.5h2.5M1.5 9.5h2.5M12 6.5h2.5M12 9.5h2.5'/>";
    } else if (category == "Type") {
        body = "<path d='M8 2l5 3v6l-5 3-5-3V5z'/><path d='M8 8l5-3M8 8v6M8 8L3 5'/>";
    } else if (category == "Genres") {   // a tag
        body = "<path d='M2 3.5V8l6 6 6-6-6-6H3.5A1.5 1.5 0 0 0 2 3.5z'/><circle cx='5.5' cy='5.5' r='1'/>";
    } else if (category == "Series") {   // stacked layers
        body = "<path d='M8 2l6 3-6 3-6-3z'/><path d='M2 8l6 3 6-3M2 11l6 3 6-3'/>";
    } else if (category == "Players") {  // two people
        body = "<circle cx='5.5' cy='5' r='2.2'/><circle cx='11' cy='5.5' r='1.8'/><path d='M1.5 13.5c0-2.5 1.8-4 4-4s4 1.5 4 4M9.5 13.5c0-2 1.2-3.2 3-3.2 1.2 0 2 .5 2 1.5'/>";
    } else { // leaf item
        color = muted;
        body = "<path d='M8 3.5 13 8 8 12.5 3 8Z'/>";
    }

    std::string svg =
        "<svg width='16' height='16' viewBox='0 0 16 16' xmlns='http://www.w3.org/2000/svg' "
        "fill='none' stroke='" + color + "' stroke-width='1.3' stroke-linejoin='round' stroke-linecap='round'>"
        + body + "</svg>";

    Glib::RefPtr<Gdk::Pixbuf> pixbuf;
    try {
        auto loader = Gdk::PixbufLoader::create("svg");
        loader->set_size(18, 18);
        loader->write(reinterpret_cast<const guint8*>(svg.data()), svg.size());
        loader->close();
        pixbuf = loader->get_pixbuf();
    } catch (...) {
        pixbuf = Gdk::Pixbuf::create(Gdk::COLORSPACE_RGB, true, 8, 16, 16);
    }

    cache.emplace(category, pixbuf);
    return pixbuf;
}

// ── Launch preference persistence ─────────────────────────────────────────

void MainWindow::rebuild_filter_chips() {
    SettingsUi::destroy_children(m_chips_box);

    // Human labels for the dimension keys stored in m_active_filters.
    auto dim_label = [](const std::string& k) -> std::string {
        if (k == "emulator")     return _("Emulator");
        if (k == "system")       return _("System");
        if (k == "manufacturer") return _("Manufacturer");
        if (k == "year")         return _("Year");
        if (k == "source")       return _("Source");
        if (k == "aspect")       return _("Aspect");
        if (k == "orientation")  return _("Orientation");
        if (k == "status")       return _("Status");
        if (k == "type")         return _("Type");
        if (k == "genre")        return _("Genre");
        if (k == "family")       return _("Series");
        if (k == "players")      return _("Players");
        if (k == "mode")         return _("Mode");
        if (k == "hiscore")      return _("Highscore");
        return k;
    };
    auto value_label = [](const std::string& k, const std::string& v) -> std::string {
        // La puce porte « MAME », pas « mame » : l'identifiant ne se montre pas.
        if (k == "emulator") return EmulatorRegistry::display_name(v);
        if (k != "type") return v;
        if (v == "original")  return _("Original");
        if (v == "clone")     return _("Clone");
        if (v == "hack")      return _("Hack");
        if (v == "homebrew")  return _("Homebrew");
        if (v == "bootleg")   return _("Bootleg");
        if (v == "prototype") return _("Prototype");
        return v;
    };

    // One chip per active dimension, plus the star as its own chip.
    auto add_chip = [&](const std::string& text, std::function<void()> on_remove) {
        auto* chip = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 4);
        chip->get_style_context()->add_class("filter-chip");
        auto* lbl = Gtk::make_managed<Gtk::Label>(text);
        auto* x = Gtk::make_managed<Gtk::Button>();
        x->set_image_from_icon_name("window-close-symbolic", Gtk::ICON_SIZE_MENU);
        x->set_relief(Gtk::RELIEF_NONE);
        x->get_style_context()->add_class("chip-x");
        x->signal_clicked().connect([this, on_remove] {
            on_remove();
            apply_tree_filters();
        });
        chip->pack_start(*lbl, Gtk::PACK_SHRINK);
        chip->pack_start(*x, Gtk::PACK_SHRINK);
        m_chips_box.pack_start(*chip, Gtk::PACK_SHRINK);
    };

    if (m_show_favorites_only) {
        add_chip(std::string("★ ") + _("Favorites"), [this] {
            m_show_favorites_only = false;
            m_suppress_fav_toggle = true;
            m_btn_favorites.set_active(false);
            m_suppress_fav_toggle = false;
        });
    }
    for (const auto& [k, v] : m_active_filters) {
        add_chip(dim_label(k) + ": " + value_label(k, v),
                 [this, k] { m_active_filters.erase(k); });
    }

    // "Clear all" only earns its place once something is actually filtering.
    if (!m_chips_box.get_children().empty()) {
        auto* clear = Gtk::make_managed<Gtk::Button>(_("Clear all"));
        clear->set_relief(Gtk::RELIEF_NONE);
        clear->get_style_context()->add_class("chip-clear");
        clear->signal_clicked().connect([this] {
            m_active_filters.clear();
            m_show_favorites_only = false;
            m_suppress_fav_toggle = true;
            m_btn_favorites.set_active(false);
            m_suppress_fav_toggle = false;
            m_treeview_filters.get_selection()->unselect_all();
            apply_tree_filters();
        });
        m_chips_box.pack_start(*clear, Gtk::PACK_SHRINK);
    }

    m_chips_box.show_all();
    m_chips_box.set_visible(!m_chips_box.get_children().empty());
}

void MainWindow::set_favorites_only(bool on) {
    if (m_show_favorites_only == on) return;
    m_show_favorites_only = on;

    m_suppress_fav_toggle = true;
    m_btn_favorites.set_active(on);
    m_suppress_fav_toggle = false;

    apply_tree_filters();
}

void MainWindow::on_language_selected(const std::string& code) {
    // Persist the choice and prompt for a restart (labels are built once at
    // startup).
    m_settings_panel.set_language(code); // suppressed inside SettingsPanel
    m_settings_panel.save_to_file(AppContext::get_config_path());

    // La boite du bureau jurait a cote des reglages : meme langage visuel que
    // les cartes, tuile a pictogramme et bouton principal compris.
    SettingsUi::notice(*this, _("Language changed"),
                       _("Restart the launcher to fully apply the new language."),
                       "bc-globe.svg");
}

// Does the desktop ask for dark surfaces ? GNOME publishes it as
// org.gnome.desktop.interface color-scheme ; elsewhere the theme name says it.
static bool desktop_prefers_dark(const Glib::RefPtr<Gio::Settings>& desktop) {
    try {
        if (desktop) {
            const std::string scheme = desktop->get_string("color-scheme");
            if (scheme == "prefer-dark") return true;
            if (scheme == "prefer-light") return false;
        }
    } catch (...) {}
    if (auto settings = Gtk::Settings::get_default()) {
        Glib::ustring theme = settings->property_gtk_theme_name();
        if (theme.lowercase().find("dark") != Glib::ustring::npos) return true;
        // Not gtk-application-prefer-dark-theme : that one is ours (set by
        // apply_theme), reading it back would make System stick to Dark.
    }
    return false;
}

/* Trois feuilles : la charte (style-common.css, ecrite en jetons) et une
 * palette, sombre ou claire, qui definit ces jetons. Le mode System choisit
 * la palette d'apres le bureau ; le dessin des composants, lui, est celui
 * de Bootcade dans tous les cas. */
void MainWindow::apply_theme(const std::string& mode) {
    auto screen = Gdk::Screen::get_default();
    if (!screen) return;

    auto load = [](const char* file) {
        auto p = Gtk::CssProvider::create();
        try { p->load_from_path(AppContext::get_asset_path(file)); }
        catch (const Glib::Error& e) { std::cerr << "[WARN] " << file << ": " << e.what() << std::endl; }
        return p;
    };
    if (!m_css_common) m_css_common = load("style-common.css");
    // System follows the desktop live : when it flips between light and dark,
    // the palette flips with it, without a restart.
    if (!m_desktop_settings) {
        try {
            auto schemas = Gio::SettingsSchemaSource::get_default();
            if (schemas && schemas->lookup("org.gnome.desktop.interface", true)) {
                m_desktop_settings = Gio::Settings::create("org.gnome.desktop.interface");
                // GSettings only notifies keys it has been asked for : read once.
                (void)m_desktop_settings->get_string("color-scheme");
                m_desktop_settings->signal_changed("color-scheme").connect([this](const Glib::ustring&) {
                    if (m_theme_mode == "system") apply_theme("system");
                });
            }
        } catch (...) {}
    }
    if (!m_css_dark)   m_css_dark   = load("style-dark.css");
    if (!m_css_light)  m_css_light  = load("style-light.css");

    const bool dark = mode == "dark" || (mode != "light" && desktop_prefers_dark(m_desktop_settings));

    // The base GTK theme follows, for what Bootcade does not draw itself
    // (file choosers, tooltips of the desktop, window decorations).
    if (auto settings = Gtk::Settings::get_default())
        settings->property_gtk_application_prefer_dark_theme() = dark;

    // Palette first, charter on top : the charter only names the tokens.
    Gtk::StyleContext::remove_provider_for_screen(screen, m_css_common);
    Gtk::StyleContext::remove_provider_for_screen(screen, m_css_dark);
    Gtk::StyleContext::remove_provider_for_screen(screen, m_css_light);
    Gtk::StyleContext::add_provider_for_screen(screen, dark ? m_css_dark : m_css_light, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    Gtk::StyleContext::add_provider_for_screen(screen, m_css_common, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);

    m_theme_mode = mode;
    m_theme_dark = dark;
}

void MainWindow::load_launch_prefs() {
    std::ifstream fi(AppContext::get_config_path());
    if (!fi) return;
    try {
        nlohmann::json j;
        fi >> j;
        if (j.contains("launch_fullscreen"))   m_launch_fullscreen   = j["launch_fullscreen"].get<bool>();
        if (j.contains("launch_integerscale")) m_launch_integerscale = j["launch_integerscale"].get<bool>();
        if (j.contains("detail_dock_position")) {
            std::string p = j["detail_dock_position"].get<std::string>();
            m_dock_position = (p == "right") ? "right" : "bottom";
        }
        if (j.contains("grid_columns")) {
            int n = j["grid_columns"].get<int>();
            // Une configuration d'avant ce changement contient un NOMBRE DE
            // COLONNES (3 a 5). On le convertit en largeur plausible plutot
            // que de le refuser, sinon la grille repartirait a 120 px.
            if (n >= 3 && n <= 5) n = 300 - (n - 3) * 60;
            m_grid_columns = (n < kMinCardWidth) ? kMinCardWidth
                           : (n > kMaxCardWidth) ? kMaxCardWidth : n;
        }
        /* Geometrie : appliquee avant le premier affichage, et seulement si
         * le joueur le demande. Eteint, la fenetre s'ouvre a sa taille par
         * defaut a chaque lancement : c'est ce que « ne pas retenir l'etat de
         * la fenetre » veut dire. */
        const bool restore_geometry = j.value("restore_window_state", true);
        if (restore_geometry && j.contains("win_w") && j.contains("win_h"))
            set_default_size(j["win_w"].get<int>(), j["win_h"].get<int>());
        if (restore_geometry && j.contains("win_x") && j.contains("win_y"))
            move(j["win_x"].get<int>(), j["win_y"].get<int>());
        if (restore_geometry && j.value("win_max", false)) maximize();

        m_last_selected_rom    = j.value("startup_last_selected_game", std::string());
        m_last_selected_system = j.value("startup_last_selected_system", std::string());
        m_last_selected_emulator = j.value("startup_last_selected_emulator", std::string());

        if (j.value("dock_sections_set", false)) {
            m_dock_prefs_known = true;
            m_activity_exp.set_expanded(j.value("dock_activity_open", true));
            m_specs_exp.set_expanded(j.value("dock_specs_open", true));
        }
    } catch (...) {}
    // Reflect loaded state in menu checkitems (block toggled signal to avoid side-effect)
    m_syncing_launch_menu = true;
    m_menu_item_fullscreen_mode.set_active(m_launch_fullscreen);
    m_menu_item_integerscale_mode.set_active(m_launch_integerscale);
    m_syncing_launch_menu = false;
    m_settings_panel.set_launch_flags(m_launch_fullscreen, m_launch_integerscale);
}

void MainWindow::save_launch_prefs() {
    const std::string cfg = AppContext::get_config_path();
    nlohmann::json j;
    {
        std::ifstream fi(cfg);
        if (fi) { try { fi >> j; } catch (...) {} }
    }
    j["launch_fullscreen"]   = m_launch_fullscreen;
    j["launch_integerscale"] = m_launch_integerscale;
    j["detail_dock_position"] = m_dock_position;
    j["grid_columns"] = m_grid_columns;
    j["startup_last_selected_game"]   = m_last_selected_rom;
    j["startup_last_selected_system"] = m_last_selected_system;
    j["startup_last_selected_emulator"] = m_last_selected_emulator;
    // Geometrie de la fenetre : retrouver son ecran et sa taille au
    // redemarrage est le minimum attendu d'une application de bureau : mais
    // le joueur peut ne pas vouloir de cette memoire, et alors on n'ecrit
    // rien plutot que d'ecrire des valeurs qu'on ignorera.
    if (m_settings_panel.restores_window_state()
        && get_realized() && !is_maximized()) {
        int w = 0, h = 0, x = 0, y = 0;
        get_size(w, h);
        get_position(x, y);
        if (w > 200 && h > 200) { j["win_w"] = w; j["win_h"] = h; }
        j["win_x"] = x; j["win_y"] = y;
    }
    if (m_settings_panel.restores_window_state()) j["win_max"] = is_maximized();
    /* Etat des sections repliables.
     *
     * « dock_sections_set » distingue « le joueur n'a jamais choisi » de
     * « il a tout ouvert » : sans ce drapeau, deux sections ouvertes
     * ressembleraient a une absence de preference et le repli automatique
     * viendrait contredire un choix explicite a chaque redemarrage.
     */
    j["dock_sections_set"] = m_dock_prefs_known;
    j["dock_activity_open"] = m_activity_exp.get_expanded();
    j["dock_specs_open"]    = m_specs_exp.get_expanded();
    std::ofstream fo(cfg);
    if (fo) fo << j.dump(4) << std::endl;
}

// ── ZIP path lookup ────────────────────────────────────────────────────────

std::string MainWindow::find_rom_zip_path(const std::string& rom_name) {
    const std::string zip_name = rom_name + ".zip";
    for (const auto& dir : m_settings_panel.get_roms_paths()) {
        try {
            for (const auto& entry :
                 std::filesystem::recursive_directory_iterator(
                     dir, std::filesystem::directory_options::skip_permission_denied))
            {
                if (entry.path().filename() == zip_name)
                    return entry.path().string();
            }
        } catch (...) {}
    }
    return "";
}

// ── ZIP integrity check ────────────────────────────────────────────────────

bool MainWindow::verify_zip_integrity(const std::string& zip_path) {
    int err = 0;
    zip_t* z = zip_open(zip_path.c_str(), ZIP_RDONLY | ZIP_CHECKCONS, &err);
    if (!z) return false;
    zip_close(z);
    return true;
}

// ── Duplicate ROM detection ────────────────────────────────────────────────

void MainWindow::on_rom_manager() {
    if (!m_rom_manager) {
        m_rom_manager = std::make_unique<RomManagerWindow>(*this, m_database);

        // The DAT directory is the one setting both UIs own, so mirror it back
        // into the Settings panel and persist it there too.
        m_rom_manager->signal_dat_path_changed().connect([this](std::string path) {
            m_settings_panel.set_dat_path(path);
            m_settings_panel.save_to_file(AppContext::get_config_path());
        });

        m_rom_manager->signal_update_dat().connect([this](bool confirm, std::string emulator) {
            if (confirm) confirm_update_dat(emulator); else do_update_dat(emulator);
        });

        // "Move to library" already moved files straight into existing ROM
        // directories : nothing to add, just verify the result with a scan.
        m_rom_manager->signal_scan_requested().connect([this](std::string emulator) {
            start_scan_thread(m_settings_panel.get_roms_paths(emulator), emulator);
        });
        m_rom_manager->signal_rescan_requested().connect(
            sigc::mem_fun(*this, &MainWindow::on_library_scan_requested));
    }

    // Pick up any path the Settings dialog changed while the window was closed.
    m_rom_manager->reload_settings();
    m_rom_manager->show();
    m_rom_manager->present();
}

void MainWindow::on_find_duplicate_roms() {
    auto rom_paths = m_settings_panel.get_roms_paths();
    if (rom_paths.empty()) {
        SettingsUi::notice(*this, _("No ROM directories configured"),
                           _("Please add at least one ROM directory in Settings."),
                           "bc-info.svg");
        return;
    }

    // Collect zip files: name (stem) → list of full paths. The walk runs on
    // its own thread : on a network share it takes seconds, and doing it on
    // the GTK thread froze the window with nothing on screen.
    std::map<std::string, std::vector<std::string>> seen;
    {
        Gtk::Dialog wait(_("Find Duplicate ROMs"), *this, true);
        wait.set_deletable(false);
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 12);
        box->set_margin_start(24); box->set_margin_end(24); box->set_margin_top(20); box->set_margin_bottom(20);
        box->pack_start(*Gtk::make_managed<Gtk::Label>(_("Looking through the ROM directories…")), Gtk::PACK_SHRINK);
        auto* bar = Gtk::make_managed<Gtk::ProgressBar>();
        box->pack_start(*bar, Gtk::PACK_SHRINK);
        wait.get_content_area()->pack_start(*box, Gtk::PACK_EXPAND_WIDGET);
        wait.show_all_children();

        Glib::Dispatcher done;
        std::atomic<bool> finished{false};
        done.connect([&wait, &finished] { finished = true; wait.response(Gtk::RESPONSE_OK); });
        std::thread walker([&rom_paths, &seen, &done] {
            for (const auto& dir : rom_paths) {
                try {
                    for (const auto& entry :
                         std::filesystem::recursive_directory_iterator(
                             dir, std::filesystem::directory_options::skip_permission_denied))
                    {
                        if (entry.path().extension() == ".zip")
                            seen[entry.path().stem().string()].push_back(entry.path().string());
                    }
                } catch (...) {}
            }
            done.emit();
        });
        auto pulse = Glib::signal_timeout().connect([bar] { bar->pulse(); return true; }, 120);
        while (!finished) wait.run();
        pulse.disconnect();
        walker.join();
    }

    // Filter to actual duplicates
    std::vector<std::pair<std::string, std::vector<std::string>>> dupes;
    for (auto& [name, paths] : seen)
        if (paths.size() > 1) dupes.push_back({name, paths});

    if (dupes.empty()) {
        SettingsUi::notice(*this, _("No duplicates found"),
                           _("No duplicate ROM ZIP files were found across the configured directories."),
                           "bc-info.svg");
        return;
    }

    // Sort by name
    std::sort(dupes.begin(), dupes.end(),
              [](const auto& a, const auto& b){ return a.first < b.first; });

    std::ostringstream oss;
    oss << dupes.size() << " duplicate ROM(s) found:\n\n";
    for (const auto& [name, paths] : dupes) {
        oss << name << ".zip (" << paths.size() << " copies):\n";
        for (const auto& p : paths)
            oss << "  " << p << "\n";
        oss << "\n";
    }

    // Show in a scrollable dialog
    Gtk::Dialog dlg("Duplicate ROMs", *this, Gtk::DIALOG_DESTROY_WITH_PARENT);
    dlg.set_default_size(700, 420);
    dlg.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);

    auto* sw = Gtk::make_managed<Gtk::ScrolledWindow>();
    sw->set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    sw->set_margin_start(12); sw->set_margin_end(12);
    sw->set_margin_top(10);  sw->set_margin_bottom(10);
    sw->set_vexpand(true);

    auto* tv = Gtk::make_managed<Gtk::TextView>();
    tv->set_editable(false);
    tv->set_monospace(true);
    tv->get_buffer()->set_text(oss.str());
    sw->add(*tv);

    dlg.get_content_area()->pack_start(*sw, Gtk::PACK_EXPAND_WIDGET);
    dlg.add_button(_("Close"), Gtk::RESPONSE_CLOSE);
    dlg.show_all_children();
    dlg.run();
}


// ── Compte dans la barre du haut ────────────────────────────────────────
void MainWindow::open_web(const std::string& path) {
    // Le site publie porte le profil et le classement : le launcher n'a pas a
    // les redessiner, il y renvoie.
    //
    // ?sso=1 quand une session est ouverte ici. Le navigateur et le launcher
    // ont chacun la leur : sans cet indice, un joueur connecte dans le
    // launcher arrivait sur le site avec un bouton « Se connecter », ce qui
    // donne l'impression de devoir s'authentifier deux fois. L'indice ne
    // transporte AUCUN jeton, il demande seulement au site de verifier en
    // silence aupres de Keycloak, qui reconnait la session du navigateur.
    std::string url = "https://bootcade.netlify.app" + path;
    if (BootcadeAuth::signed_in()) url += "?sso=1";
    gtk_show_uri_on_window(GTK_WINDOW(gobj()), url.c_str(), GDK_CURRENT_TIME, nullptr);
}

void MainWindow::build_account_button() {
    m_account_label.set_ellipsize(Pango::ELLIPSIZE_END);
    m_account_label.set_max_width_chars(14);   // un nom long ne doit pas
                                               // repousser le reste de la barre
    /* Avatar, nom, etat, fleche.
     *
     * L'etat en ligne se lit sous le nom : c'est ce qui repond a « mes
     * scores partent-ils ? » sans avoir a ouvrir les reglages. La fleche
     * annonce qu'un menu se cache derriere, ce qu'un simple bouton portant
     * un nom ne laisse pas deviner.
     */
    /* Le bloc reste centre, connecte comme deconnecte.
     *
     * Deconnecte, l'avatar et la ligne d'etat disparaissent : le libelle
     * « Sign in » restait alors cale en haut de la boite verticale, decale
     * par rapport aux autres boutons de la barre. Centrer la boite ET son
     * contenu regle les deux axes d'un coup.
     */
    auto* names = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
    names->set_valign(Gtk::ALIGN_CENTER);
    m_account_face.set_valign(Gtk::ALIGN_CENTER);
    m_account_avatar.set_valign(Gtk::ALIGN_CENTER);
    m_account_label.set_valign(Gtk::ALIGN_CENTER);
    m_account_label.set_xalign(0.0f);
    m_account_state.set_xalign(0.0f);
    m_account_state.get_style_context()->add_class("account-state");
    names->pack_start(m_account_label, Gtk::PACK_SHRINK);
    names->pack_start(m_account_state, Gtk::PACK_SHRINK);

    auto* arrow = Gtk::make_managed<Gtk::Image>();
    arrow->set_from_icon_name("pan-down-symbolic", Gtk::ICON_SIZE_BUTTON);

    m_account_face.pack_start(m_account_avatar, Gtk::PACK_SHRINK);
    m_account_face.pack_start(*names,           Gtk::PACK_SHRINK);
    m_account_face.pack_start(*arrow,           Gtk::PACK_SHRINK);
    m_btn_account.get_style_context()->add_class("account-btn");
    // Meme hauteur que les autres commandes de la barre.
    m_btn_account.set_size_request(-1, 40);
    m_btn_account.add(m_account_face);

    m_mi_profile.set_label(_("My profile"));
    m_mi_reports.set_label(_("My reports"));
    m_mi_leaderboard.set_label(_("Leaderboard"));
    m_mi_settings.set_label(_("Settings"));
    m_mi_signout.set_label(_("Sign out"));

    m_mi_profile.signal_activate().connect([this] { open_web("/profile/"); });
    m_mi_reports.signal_activate().connect([this] { open_my_reports(); });
    m_mi_leaderboard.signal_activate().connect([this] { open_web("/leaderboard/"); });
    m_mi_settings.signal_activate().connect(
        sigc::mem_fun(*this, &MainWindow::on_settings_clicked));
    m_mi_signout.signal_activate().connect([this] {
        BootcadeAuth::sign_out();
        refresh_account_button();
        refresh_hiscore_data_async(false);
    });

    for (Gtk::MenuItem* mi : {&m_mi_profile, &m_mi_reports, &m_mi_leaderboard, &m_mi_settings})
        m_account_menu.append(*mi);
    m_account_menu.append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());
    m_account_menu.append(m_mi_signout);
    m_account_menu.show_all();

    // Un second menu, d'une seule entree, pour l'etat deconnecte. Un
    // Gtk::MenuButton exige TOUJOURS un menu : une premiere version passait un
    // pointeur nul dereference pour n'en donner aucun, ce qui compile et
    // plante a l'ouverture.
    m_mi_signin.set_label(_("Sign in"));
    m_mi_signin.signal_activate().connect([this] {
        LoginDialog dlg(*this);
        dlg.run();
        refresh_account_button();
        refresh_hiscore_data_async(false);
    });
    m_account_menu_out.append(m_mi_signin);
    m_account_menu_out.show_all();

    m_btn_account.set_popup(m_account_menu);

    refresh_account_button();
}

void MainWindow::refresh_account_button() {
    /* Rafraichit les DEUX endroits qui montrent le compte, pas seulement le
     * bouton dont cette fonction porte le nom.
     *
     * Le compte change par quatre chemins : restauration au demarrage,
     * connexion depuis le menu de la barre, deconnexion depuis ce menu, et
     * les deux memes boutons dans les reglages. Chacun appelait ce qu'il
     * avait sous la main, et il a suffi d'en oublier un pour que la barre
     * affiche « battousai90 » pendant que les reglages affichaient « Not
     * signed in ». Le bug est revenu deux fois par ce mecanisme.
     *
     * La garantie est donc placee ICI, dans la fonction, et non dans la
     * discipline de ceux qui l'appellent : plus aucun chemin ne peut
     * rafraichir un seul des deux. Le sens inverse, reglages vers barre, est
     * assure par signal_account_changed().
     */
    m_settings_panel.refresh_account();

    if (BootcadeAuth::signed_in()) {
        std::string id = BootcadeAuth::avatar_id();
        if (id.empty()) id = "joystick";
        m_account_avatar.set(IconManager::load("avatars/" + id + ".svg", 22, 22));
        m_account_avatar.show();
        m_account_label.set_text(BootcadeAuth::username());
        // L'etat n'est PAS ecrit ici : c'est apply_online_state qui le pose,
        // appele a la fin. Une version precedente le fixait en dur a « Online »
        // juste apres, ce qui ecrasait l'etat calcule et rendait le mode hors
        // ligne invisible.
        m_btn_account.set_tooltip_text(_("Your Bootcade account"));
        m_btn_account.set_popup(m_account_menu);
        m_btn_account.set_sensitive(true);
    } else {
        // Deconnecte, le bouton n'ouvre pas un menu : il fait la seule chose
        // qui ait du sens, ouvrir la connexion. Un menu a une seule entree
        // serait un clic de plus pour rien.
        m_account_avatar.hide();
        m_account_label.set_text(_("Sign in"));
        // L'etat reste VISIBLE deconnecte : « en ligne mais pas connecte » et
        // « hors ligne » sont deux situations differentes, et c'est
        // precisement la que la distinction compte.
        m_btn_account.set_tooltip_text(_("Sign in to publish your scores"));
        m_btn_account.set_popup(m_account_menu_out);
        m_btn_account.set_sensitive(true);
    }

    // En dernier : il a le dernier mot sur l'indicateur d'etat.
    apply_online_state();
}


/* L'etat de l'emulateur, en clair et en permanence.
 *
 * Un chemin renseigne ne suffit pas : le binaire peut avoir ete deplace ou
 * ne plus etre executable. On verifie donc ce qui compte vraiment, pouvoir
 * le lancer, plutot que le fait qu'un reglage soit rempli. */
void MainWindow::refresh_emu_state() {
    // L'etat suit la portee affichee : annoncer FinalBurn Neo pendant que le
    // catalogue MAME est a l'ecran repondait a une question que personne ne
    // posait, et laissait croire qu'un \u00AB FBNeo not set \u00BB empechait de lancer
    // une machine MAME.
    bool ready = false;
    std::string label_ready, label_missing, version;

    // Portee « tous » : parler d'un seul emulateur serait faux, et taire
    // l'autre reviendrait a masquer qu'il manque. On annonce donc combien
    // sont prets, et on ne nomme personne.
    if (m_active_emulator.empty() && m_multi_emulator) {
        int total = 0, up = 0;
        for (const auto& e : EmulatorRegistry::all()) {
            ++total;
            if (e.id == "mame") {
                if (!m_settings_panel.mame_executable().empty()) ++up;
            } else {
                const std::string exe = m_settings_panel.get_fbneo_executable();
                if (!exe.empty() && ::access(exe.c_str(), X_OK) == 0) ++up;
            }
        }
        const bool all_up = (up == total);
        m_lbl_emu_state.set_text(
            (all_up ? "\u25CF " : "\u25CB ") +
            Glib::ustring::compose(_("%1 of %2 emulators ready"),
                                   std::to_string(up), std::to_string(total)).raw());
        m_lbl_emu_state.get_style_context()->remove_class("emu-ready");
        m_lbl_emu_state.get_style_context()->remove_class("emu-missing");
        m_lbl_emu_state.get_style_context()->add_class(all_up ? "emu-ready" : "emu-missing");
        return;
    }

    if (m_active_emulator == "mame") {
        // MAME est cherche dans le PATH et dans les emplacements usuels : il
        // n'y a pas de reglage a remplir, donc pas de \u00AB not set \u00BB a afficher.
        const std::string exe = m_settings_panel.mame_executable();
        ready         = !exe.empty();
        label_ready   = _("MAME ready");
        label_missing = _("MAME not found");
        // Interroger le binaire coute un processus : on garde sa reponse, le
        // chemin en main, parce que cet indicateur se rafraichit a chaque
        // changement de portee et de reglage.
        if (ready) {
            static std::string cached_exe, cached_build;
            if (exe != cached_exe) {
                cached_exe   = exe;
                cached_build = MameCatalog::installed_build(exe);
            }
            version = cached_build;
        }
    } else {
        const std::string exe = m_settings_panel.get_fbneo_executable();
        ready         = !exe.empty() && ::access(exe.c_str(), X_OK) == 0;
        label_ready   = _("FBNeo ready");
        label_missing = _("FBNeo not set");
        // La meme identite de version que la page Reglages > Emulator :
        // l'empreinte du commit telechargee, seule version que cette
        // distribution de FinalBurn Neo expose. En inventer une ici, c'est
        // annoncer deux versions differentes du meme binaire.
        if (ready) {
            nlohmann::json j;
            std::ifstream fi(AppContext::get_config_path());
            if (fi) { try { fi >> j; } catch (...) {} }
            const std::string sha = j.value("fbneo_release_sha", std::string());
            if (!sha.empty()) version = sha.substr(0, 7);
        }
    }
    // MAME repond « 0.279 (mame0279) » : le pied de la colonne n'a la place
    // que du numero.
    version = version.substr(0, version.find(' '));
    // Version inconnue : le libelle seul, jamais un \u00AB unknown \u00BB qui
    // occuperait la place d'une information sans en etre une.
    m_lbl_emu_state.set_text(ready ? "\u25CF " + label_ready
                                     + (version.empty() ? "" : " \u00B7 " + version)
                                   : "\u25CB " + label_missing);
    m_lbl_emu_state.get_style_context()->remove_class("emu-ready");
    m_lbl_emu_state.get_style_context()->remove_class("emu-missing");
    m_lbl_emu_state.get_style_context()->add_class(ready ? "emu-ready" : "emu-missing");
}


/* En-tetes de la liste.
 *
 * Batis a la main parce que la liste est une ListBox et non une TreeView.
 * Chaque cellule reprend la constante de largeur de la ligne correspondante,
 * jamais un nombre recopie : c'est ce qui garantit que les deux restent
 * alignes quand l'une des largeurs changera.
 *
 * Les trois premieres colonnes sont des boutons plats et pilotent le MEME
 * reglage que le menu « Sort » de la barre. Deux commandes de tri qui ne se
 * parleraient pas afficheraient tot ou tard deux etats contradictoires, le
 * meme defaut qui a fait revenir deux fois le bug du compte.
 */
void MainWindow::build_mlist_header() {
    auto flat = [](Gtk::Button& b, const std::string& text, int width, float xalign) {
        b.set_label(text);
        b.set_relief(Gtk::RELIEF_NONE);
        b.get_style_context()->add_class("mlist-head-btn");
        if (width > 0) b.set_size_request(width, -1);
        if (auto* l = dynamic_cast<Gtk::Label*>(b.get_child())) l->set_xalign(xalign);
    };
    auto plain = [](Gtk::Label& l, const std::string& text, int width) {
        l.set_text(text);
        l.set_size_request(width, -1);
        l.get_style_context()->add_class("mlist-head-btn");
    };

    // Une cale de la largeur de la vignette : sans elle, « GAME » commence
    // au bord et non au-dessus du titre qu'il nomme.
    auto* spacer = Gtk::make_managed<Gtk::Box>();
    spacer->set_size_request(kColThumb, -1);
    m_mlist_head.pack_start(*spacer, Gtk::PACK_SHRINK);

    flat(m_hdr_game,   _("GAME"),   -1,         0.0f);
    flat(m_hdr_emu,    _("EMULATOR"), kColEmu, 0.0f);
    flat(m_hdr_system, _("SYSTEM"), kColSystem, 0.0f);
    flat(m_hdr_year,   _("YEAR"),   kColYear,   0.0f);
    plain(m_hdr_status, _("STATUS"), kColStatus);
    plain(m_hdr_hs,     _("HS"),     kColHs);

    m_mlist_head.pack_start(m_hdr_game,   Gtk::PACK_EXPAND_WIDGET);
    m_mlist_head.pack_start(m_hdr_emu,    Gtk::PACK_SHRINK);
    m_mlist_head.pack_start(m_hdr_system, Gtk::PACK_SHRINK);
    m_mlist_head.pack_start(m_hdr_year,   Gtk::PACK_SHRINK);
    m_mlist_head.pack_start(m_hdr_status, Gtk::PACK_SHRINK);
    m_mlist_head.pack_start(m_hdr_hs,     Gtk::PACK_SHRINK);
    m_mlist_head.get_style_context()->add_class("mlist-head");

    // Le tri passe par le combo, source unique. Cliquer « GAME » quand il est
    // deja actif inverse le sens, ce qu'on attend d'un en-tete de tableau.
    m_hdr_game.signal_clicked().connect([this] {
        m_combo_sort.set_active_id(m_sort_mode == SortMode::Name ? "default" : "name");
    });
    m_hdr_system.signal_clicked().connect([this] {
        m_combo_sort.set_active_id("default");
    });
    m_hdr_year.signal_clicked().connect([this] {
        m_combo_sort.set_active_id(m_sort_mode == SortMode::Year ? "yearAsc" : "year");
    });
    refresh_mlist_header();
}

/* Le chevron marque la colonne qui trie, et son sens. Sans lui, trois
 * en-tetes cliquables ne disent pas lequel est actif. */
void MainWindow::refresh_mlist_header() {
    auto mark = [](Gtk::Button& b, const std::string& base, const char* arrow) {
        b.set_label(arrow ? base + "  " + arrow : base);
    };
    mark(m_hdr_game,   _("GAME"),
         m_sort_mode == SortMode::Name ? "\u25B2" : nullptr);
    mark(m_hdr_system, _("SYSTEM"),
         m_sort_mode == SortMode::Default ? "\u25B2" : nullptr);
    mark(m_hdr_year,   _("YEAR"),
         m_sort_mode == SortMode::Year ? "\u25BC"
       : m_sort_mode == SortMode::YearAsc ? "\u25B2" : nullptr);
}


/* La largeur du volet suit celle de la fenetre.
 *
 * On agit sur la largeur MINIMALE du volet et non sur la position du
 * separateur : le joueur peut donc toujours l'elargir a la souris, et rien
 * ne vient contrarier son geste au redimensionnement suivant. Les bornes
 * garantissent l'invariant demande : le volet ne disparait jamais, et il
 * n'avale pas la liste sur un ecran modeste.
 */
void MainWindow::update_dock_width() {
    if (m_dock_position != "right") return;
    const int w = get_allocated_width();
    if (w <= 0 || w == m_last_alloc_width) return;
    m_last_alloc_width = w;

    int target = static_cast<int>(w * 0.26);
    if (target < 460) target = 460;
    if (target > 820) target = 820;
    // Jamais plus du tiers : sur une fenetre etroite, la liste doit rester
    // le sujet principal.
    if (target > w / 3) target = w / 3;
    // Plancher absolu : sous cette largeur les valeurs de la fiche ne
    // tiennent plus sur une ligne, et on retomberait dans les coupures.
    if (target < 430) target = 430;
    m_details_scroll.set_min_content_width(target);
}


/* Une section repliable du volet.
 *
 * L'en-tete porte le titre ET un resume. Replier ne doit pas tout effacer :
 * « Arcade, 1996, Capcom » sur une seule ligne suffit le plus souvent, et
 * c'est ce qui rend le repli acceptable plutot que subi.
 */
void MainWindow::build_dock_section(Gtk::Expander& exp, Gtk::Label& sum,
                                    const std::string& title, Gtk::Widget& body) {
    auto* head = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    auto* lbl  = Gtk::make_managed<Gtk::Label>(title);
    lbl->get_style_context()->add_class("dock-section");
    sum.set_xalign(0.0f);
    sum.set_ellipsize(Pango::ELLIPSIZE_END);
    sum.get_style_context()->add_class("dock-summary");
    head->pack_start(*lbl, Gtk::PACK_SHRINK);
    head->pack_start(sum,  Gtk::PACK_SHRINK);
    head->show_all();

    exp.set_label_widget(*head);
    /* Repliees par defaut.
     *
     * Le repli automatique selon la hauteur partait d'une bonne intention
     * mais devinait mal et changeait d'avis au redimensionnement. Les deux
     * sections partent donc fermees : le classement et le bouton Play sont
     * visibles d'emblee quelle que soit la fenetre, et le joueur ouvre ce
     * dont il a besoin. Son choix est ensuite retenu.
     */
    exp.set_expanded(false);
    exp.add(body);
    // La carte garde la hauteur de son contenu, ouverte comme fermee.
    exp.set_valign(Gtk::ALIGN_START);
    body.set_valign(Gtk::ALIGN_START);
    exp.property_expanded().signal_changed().connect([this] {
        m_dock_prefs_known = true;   // choix explicite : on ne le contredira plus
        save_launch_prefs();
    });
}



/* Choisit le jeu montre au demarrage.
 *
 * Cinq strategies, un seul invariant : si celle qui est demandee ne rend
 * rien, on prend le premier jeu de la liste. Un volet vide alors que la
 * bibliotheque est pleine est toujours un echec, quelle que soit la raison.
 */
void MainWindow::select_startup_game() {
    if (m_startup_selection_done) return;
    auto rows = m_model_games->children();
    if (rows.empty()) return;                 // rien a montrer, ce n'est pas un echec
    m_startup_selection_done = true;

    const std::string mode = m_settings_panel.get_startup_selection();

    /* Le choix se fait EN MEMOIRE, sur m_cached_games.
     *
     * La premiere version interrogeait la base pour chaque ligne du modele :
     * 29 461 requetes SQLite, chacune prenant un verrou, dans le fil de
     * l'interface. Le launcher ne « ne selectionnait pas » : il ramait
     * plusieurs minutes avant d'y arriver, ce qui revient au meme pour qui
     * regarde l'ecran. m_cached_games porte deja last_played, play_count et
     * play_time_secs : les requetes etaient entierement gratuites.
     *
     * Meme faute pour « meilleur score » : le fichier de cache des
     * classements etait relu et reanalyse a chaque ligne. Il est desormais
     * lu une fois.
     */
    std::string want_name, want_system;

    if (mode == "last_played" || mode == "most_played") {
        std::string best_when;
        long long   best_secs = -1;
        for (const auto& g : m_cached_games) {
            if (mode == "last_played") {
                if (g.last_played.empty() || g.last_played <= best_when) continue;
                best_when   = g.last_played;
                want_name   = g.name;
                want_system = g.system;
            } else {
                if (g.play_time_secs <= best_secs) continue;
                best_secs   = g.play_time_secs;
                want_name   = g.name;
                want_system = g.system;
            }
        }
    } else if (mode == "best_score") {
        /* Cache local uniquement.
         *
         * La version precedente demandait le nom du compte a
         * BootcadeAuth::username(), restaure de facon ASYNCHRONE : au
         * demarrage il est vide, la strategie ne trouvait jamais rien et
         * retombait toujours sur le repli. Le fichier de rangs porte le nom
         * du joueur avec ses places, donc rien a attendre : ni reseau, ni
         * Keycloak, ni session.
         */
        int best_rank = 0;
        for (const auto& r : HiscoreClient::cached_personal_ranks()) {
            if (best_rank != 0 && r.last_known_rank >= best_rank) continue;
            best_rank   = r.last_known_rank;
            want_name   = r.game;
            want_system = r.system;
        }
    } else if (mode == "last_selected") {
        want_name   = m_last_selected_rom;
        want_system = m_last_selected_system;
    }

    // Retrouver la ligne voulue dans le modele affiche. Elle peut en etre
    // absente : un filtre actif, un jeu retire de la collection. Le repli
    // vaut alors, comme promis, premier jeu disponible.
    /* Retrouver la ligne SANS parcourir le modele.
     *
     * Le parcours precedent extrayait deux Glib::ustring par ligne sur
     * 29 466 lignes : 4 800 ms mesures. m_filtered_games porte exactement le
     * meme ordre que le modele, en std::string : on y cherche l'index, puis
     * on accede a la ligne directement. Comparer des std::string coute
     * quelques millisecondes la ou convertir des GValue coutait cinq
     * secondes.
     */
    Gtk::TreeModel::iterator chosen;
    if (!want_name.empty()) {
        int index = -1;
        {
            std::lock_guard<std::mutex> lock(m_filter_mutex);
            for (size_t i = 0; i < m_filtered_games.size(); ++i) {
                if (m_filtered_games[i]->name != want_name) continue;
                if (!want_system.empty() && m_filtered_games[i]->system != want_system) continue;
                index = static_cast<int>(i);
                break;
            }
        }
        if (index >= 0 && index < static_cast<int>(rows.size())) {
            // Un chemin de profondeur 1 : la liste est plate.
            Gtk::TreeModel::Path path;
            path.push_back(index);
            chosen = m_model_games->get_iter(path);
        }
    }
    if (!chosen) chosen = rows.begin();

    if (auto sel = m_treeview_games.get_selection()) sel->select(chosen);
    show_game_details(*chosen);

    /* Surligner la ligne SI elle est deja construite, sans jamais forcer.
     *
     * La version precedente etendait les lots de la liste jusqu'a la cible
     * pour pouvoir la surligner : 17 100 lignes construites au demarrage,
     * mesurees a 3 167 ms sur cette machine, et autant de vignettes mises en
     * file. Sur une bibliotheque dont les visuels vivent sur un disque
     * externe, cela suffit a expliquer une minute d'attente.
     *
     * La selection du MODELE, elle, est faite et ne coute rien : c'est elle
     * qui alimente le volet de details, donc le jeu s'affiche bien. Seul le
     * surlignage dans la liste visible est abandonne quand la cible est trop
     * loin, ce qui est un defaut d'agrement, pas de fonction.
     */
    const auto path = m_model_games->get_path(chosen);
    for (size_t i = 0; i < m_mlist_refs.size(); ++i) {
        if (!m_mlist_refs[i] || m_mlist_refs[i].get_path() != path) continue;
        if (auto* r = m_mlist.get_row_at_index(static_cast<int>(i))) {
            m_mlist.select_row(*r);
            r->grab_focus();
        }
        break;
    }
}



/* Repercute les TROIS etats sur l'interface.
 *
 * Chaque commande est desactivee pour SA raison, et son infobulle dit
 * laquelle. Mettre « Indisponible hors ligne » partout mentirait dans la
 * plupart des cas : une fonction peut etre inaccessible parce qu'il n'y a
 * pas de compte, ou parce que le joueur a desactive les classements, sur
 * une machine parfaitement connectee.
 *
 * Rien ici ne touche a Play, a la bibliotheque, aux filtres, aux favoris,
 * a l'historique local, a ROM Manager, aux manettes ni aux reglages : ces
 * fonctions marchent identiquement dans tous les etats, y compris pour un
 * joueur qui n'aura jamais de compte.
 */
bool MainWindow::has_account() const { return BootcadeAuth::signed_in(); }
bool MainWindow::hiscores_on() const { return m_settings_panel.is_hiscore_enabled(); }

void MainWindow::apply_online_state() {
    const bool online  = is_online();
    const bool account = has_account();
    const bool hs      = hiscores_on();

    // Statut discret, affiche dans TOUS les cas : « connecte sans compte »
    // et « hors ligne » sont deux situations differentes.
    m_account_state.set_markup(
        std::string("<span foreground=\"") + (online ? "#3fb950" : "#8b949e") +
        "\">\u25CF</span> " +
        escape_markup(online ? _("Online") : _("Offline")));
    m_account_state.show();

    auto gate = [](Gtk::Widget& w, bool ok, const std::string& why) {
        w.set_sensitive(ok);
        w.set_tooltip_text(ok ? "" : why);
    };

    const std::string need_net     = _("Available when online");
    const std::string need_account = _("Sign in to use this feature");
    const std::string need_hs      = _("Enable online highscores in Settings");

    // Le profil demande le reseau ET un compte ; la raison affichee est la
    // premiere qui manque, celle que le joueur peut corriger tout de suite.
    gate(m_mi_profile,     online && account, !online ? need_net : need_account);
    gate(m_mi_leaderboard, online,            need_net);
    gate(m_mi_signin,      online,            need_net);
    // Se deconnecter reste possible hors ligne : c'est une operation locale.

    // Rafraichir un classement demande le reseau et la fonction activee.
    gate(m_btn_hiscore_refresh, online && hs, !online ? need_net : need_hs);
    gate(m_best_link,  online, need_net);
    gate(m_board_link, online, need_net);
}


/* Ouvre une fenetre par son nom.
 *
 * Point d'entree unique pour l'automatisation et pour un raccourci futur.
 * Il passe par les MEMES gestionnaires que les menus : ce qu'on photographie
 * est donc exactement ce que le joueur obtient, et non un chemin de test
 * parallele qui pourrait diverger.
 */
void MainWindow::open_named_window(const std::string& which) {
    if (which == "controller")    on_input_settings();
    else if (which == "settings") on_settings_clicked();
    else if (which == "roms")     on_rom_manager();
    else std::cerr << "[BOOTCADE] --open : nom inconnu \"" << which
                   << "\" (attendu : controller, settings, roms)" << std::endl;
}

// ── Selecteur d'emulateur ───────────────────────────────────────────────────
//
// Un encart au-dessus des filtres montre le catalogue courant ; le clic ouvre
// une modale d'une carte par emulateur. Tout se construit depuis
// EmulatorRegistry : un troisieme emulateur apparaitra sans qu'aucun de ces
// ecrans ne soit retouche.

// Une marque, chargee sans teinte : SettingsUi::image() repeint les
// pictogrammes avec l'encre du contexte, ce qui aurait efface les couleurs
// propres a chaque emulateur. Rend une reference vide si le fichier manque,
// et l'appelant se contente alors du nom.
static Glib::RefPtr<Gdk::Pixbuf> brand_pixbuf(const std::string& rel, int w, int h) {
    if (rel.empty()) return {};
    try {
        return Gdk::Pixbuf::create_from_file(
            AppContext::get_asset_path("icons/" + rel), w, h, true);
    } catch (const Glib::Error&) {
        return {};
    }
}

std::map<std::string, int> MainWindow::emulator_counts() const {
    std::map<std::string, int> counts;
    for (const auto& g : m_cached_games) counts[g.emulator]++;
    return counts;
}

void MainWindow::build_emulator_picker() {
    m_emu_picker_logo.set_valign(Gtk::ALIGN_CENTER);

    auto* text = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 1);
    text->set_valign(Gtk::ALIGN_CENTER);
    m_emu_picker_count.set_xalign(0.0f);
    m_emu_picker_count.get_style_context()->add_class("dim-label");
    text->pack_start(m_emu_picker_count, Gtk::PACK_SHRINK);

    m_emu_picker_box.set_margin_start(4);
    m_emu_picker_box.set_margin_end(4);
    m_emu_picker_box.pack_start(m_emu_picker_logo, Gtk::PACK_SHRINK);
    m_emu_picker_box.pack_start(*text,             Gtk::PACK_EXPAND_WIDGET);
    m_emu_picker_box.pack_start(
        *SettingsUi::image("bc-chevron-right.svg", 15), Gtk::PACK_SHRINK);

    m_btn_emu_picker.add(m_emu_picker_box);
    m_btn_emu_picker.set_relief(Gtk::RELIEF_NONE);
    m_btn_emu_picker.get_style_context()->add_class("emu-picker");
    m_btn_emu_picker.signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_emulator_picker_clicked));
}

void MainWindow::refresh_emulator_picker() {
    const auto counts = emulator_counts();

    // Un seul catalogue disponible : l'encart ne servirait qu'a ouvrir une
    // modale sans choix, autant ne pas l'afficher du tout.
    int present = 0;
    for (const auto& e : EmulatorRegistry::all())
        if (counts.count(e.id) && counts.at(e.id) > 0) ++present;
    m_btn_emu_picker.set_no_show_all(present < 2);
    if (present < 2) { m_btn_emu_picker.hide(); return; }
    m_btn_emu_picker.show_all();

    int shown = 0;
    if (m_active_emulator.empty()) {
        for (const auto& [id, n] : counts) { (void)id; shown += n; }
        m_emu_picker_logo.clear();
        m_emu_picker_count.set_markup(
            "<b>" + Glib::Markup::escape_text(_("All libraries")) + "</b>\n"
            "<small>" + std::to_string(shown) + " " + _("games") + "</small>");
    } else {
        const EmulatorInfo* e = EmulatorRegistry::find(m_active_emulator);
        shown = counts.count(m_active_emulator) ? counts.at(m_active_emulator) : 0;
        if (auto pb = brand_pixbuf(e ? e->logo : std::string(), 112, 40))
            m_emu_picker_logo.set(pb);
        else
            m_emu_picker_logo.clear();
        m_emu_picker_count.set_markup(
            "<b>" + Glib::Markup::escape_text(
                        EmulatorRegistry::display_name(m_active_emulator)) + "</b>\n"
            "<small>" + std::to_string(shown) + " " + _("games") + "</small>");
    }
}

void MainWindow::set_active_emulator(const std::string& id) {
    if (id == m_active_emulator) return;
    m_active_emulator = id;

    // Changer de catalogue remet les criteres a zero : un fabricant ou un genre
    // choisi dans l'un n'a aucune raison d'exister dans l'autre, et un filtre
    // actif sur une dimension absente donnerait une liste vide sans rien dire.
    m_active_filters.clear();
    // L'indicateur du bas parle de l'emulateur affiche : il change avec lui.
    refresh_emu_state();
    refresh_emulator_picker();
    populate_filter_tree();
    rebuild_filter_chips();
    apply_tree_filters();
}

// La modale de choix : une carte par emulateur, plus une carte « tout ». Elle
// se construit entierement depuis EmulatorRegistry et depuis les comptes reels,
// de sorte qu'un emulateur ajoute au registre y apparaisse sans retouche, et
// qu'un emulateur sans aucun jeu n'y apparaisse pas du tout.
void MainWindow::on_emulator_picker_clicked() {
    Gtk::Dialog dlg(_("Choose a library"), *this, true);
    dlg.set_default_size(520, -1);
    dlg.set_resizable(false);

    auto* content = dlg.get_content_area();
    content->set_spacing(14);
    content->set_border_width(18);

    auto* intro = Gtk::make_managed<Gtk::Label>();
    intro->set_markup("<small>" + Glib::Markup::escape_text(
        _("Pick which collection the library shows. Filters reset when you switch.")) +
        "</small>");
    intro->set_xalign(0.0f);
    intro->set_line_wrap(true);
    content->pack_start(*intro, Gtk::PACK_SHRINK);

    const auto counts = emulator_counts();
    std::string chosen = m_active_emulator;
    bool picked = false;

    auto add_card = [&](const std::string& id, const std::string& title,
                        const std::string& logo, const std::string& tagline,
                        int count) {
        auto* row = Gtk::make_managed<Gtk::Button>();
        row->set_relief(Gtk::RELIEF_NONE);
        row->get_style_context()->add_class("emu-card");
        if (id == m_active_emulator) row->get_style_context()->add_class("selected");

        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 14);
        box->set_border_width(10);

        // Les marques n'ont pas les memes proportions : FinalBurn Neo est
        // compact et haut, MAME long et plat. Sans une case de largeur fixe,
        // chaque carte commencait son texte a un endroit different.
        auto* slot = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 0);
        slot->set_size_request(136, -1);
        if (auto pb = brand_pixbuf(logo, 132, 46)) {
            auto* img = Gtk::make_managed<Gtk::Image>(pb);
            img->set_valign(Gtk::ALIGN_CENTER);
            slot->pack_start(*img, Gtk::PACK_EXPAND_WIDGET);
        } else {
            auto* name = Gtk::make_managed<Gtk::Label>();
            name->set_markup("<b>" + Glib::Markup::escape_text(title) + "</b>");
            name->set_xalign(0.5f);
            slot->pack_start(*name, Gtk::PACK_EXPAND_WIDGET);
        }
        box->pack_start(*slot, Gtk::PACK_SHRINK);

        auto* text = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 3);
        text->set_valign(Gtk::ALIGN_CENTER);
        auto* count_lbl = Gtk::make_managed<Gtk::Label>();
        count_lbl->set_markup("<b>" + std::to_string(count) + "</b> " +
                              Glib::Markup::escape_text(_("games")));
        count_lbl->set_xalign(0.0f);
        text->pack_start(*count_lbl, Gtk::PACK_SHRINK);
        if (!tagline.empty()) {
            auto* tag = Gtk::make_managed<Gtk::Label>();
            tag->set_markup("<small>" + Glib::Markup::escape_text(tagline) + "</small>");
            tag->set_xalign(0.0f);
            tag->get_style_context()->add_class("dim-label");
            tag->set_line_wrap(true);
            text->pack_start(*tag, Gtk::PACK_SHRINK);
        }
        box->pack_start(*text, Gtk::PACK_EXPAND_WIDGET);

        if (id == m_active_emulator)
            box->pack_start(*SettingsUi::image("bc-check.svg", 18), Gtk::PACK_SHRINK);

        row->add(*box);
        row->signal_clicked().connect([&dlg, &chosen, &picked, id] {
            chosen = id;
            picked = true;
            dlg.response(Gtk::RESPONSE_OK);
        });
        content->pack_start(*row, Gtk::PACK_SHRINK);
    };

    int total = 0;
    for (const auto& [id, n] : counts) { (void)id; total += n; }
    add_card("", _("All libraries"), "",
             _("Everything at once, from every emulator"), total);

    for (const auto& e : EmulatorRegistry::all()) {
        const auto it = counts.find(e.id);
        if (it == counts.end() || it->second == 0) continue;   // rien a montrer
        add_card(e.id, e.name, e.logo, e.tagline, it->second);
    }

    dlg.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dlg.show_all();
    dlg.run();

    if (picked) set_active_emulator(chosen);
}

// Verifier une collection MAME, c'est demander son avis a MAME : il connait
// les sets splits, les BIOS et les peripheriques mieux que nous, et il ne peut
// pas etre en desaccord avec lui-meme. On ne fait que traduire sa reponse.
std::vector<Game> MainWindow::load_all_catalogs() {
    std::vector<Game> all = m_database->getAllGamesLight();
    auto mame = m_database->getMameCatalog(m_settings_panel.shows_mechanical());
    all.insert(all.end(), std::make_move_iterator(mame.begin()),
                          std::make_move_iterator(mame.end()));

    // Les deux sources arrivent triees chacune de son cote : les mettre bout a
    // bout donnerait toute la bibliotheque FinalBurn Neo, puis toute celle de
    // MAME. On retrouve donc l'ordre alphabetique d'avant, celui que la base
    // rend deja pour une source seule (ORDER BY description).
    std::stable_sort(all.begin(), all.end(),
                     [](const Game& a, const Game& b) {
                         return a.description < b.description;
                     });
    return all;
}

void MainWindow::run_mame_audit() {
    const std::string mame = m_settings_panel.mame_executable();
    if (mame.empty()) {
        SettingsUi::notice(*this, _("MAME not found"),
                           _("MAME does not seem to be installed on this system."),
                           "bc-error.svg");
        return;
    }

    std::vector<std::string> paths;
    {
        const std::string configured = m_settings_panel.mame_rompaths();
        std::istringstream ss(configured);
        std::string one;
        while (std::getline(ss, one, ';')) if (!one.empty()) paths.push_back(one);
        if (paths.empty()) paths = m_settings_panel.get_roms_paths();

        std::vector<std::string> expanded;
        std::error_code ec;
        for (const auto& q : paths) {
            if (!std::filesystem::is_directory(q, ec)) continue;
            expanded.push_back(q);
            for (const auto& e : std::filesystem::directory_iterator(q, ec))
                if (e.is_directory(ec)) expanded.push_back(e.path().string());
        }
        paths.swap(expanded);
    }
    if (paths.empty()) {
        SettingsUi::notice(*this, _("No ROM folder to check"),
                           _("Add the folder holding your MAME sets in Settings, "
                             "Library tab."), "bc-error.svg");
        return;
    }

    ConfirmationDialog confirm(*this, _("Check MAME ROMs"),
        _("Ask MAME to check every set it knows against your collection.\n\n"
          "Continue?"),
        "bc-search.svg", /*destructive=*/false,
        _("This takes several minutes, longer still on an external drive."));
    if (!confirm.show_and_confirm()) return;

    Gtk::Dialog dlg(_("Checking MAME ROMs"), *this, true);
    dlg.set_default_size(430, -1);
    auto* box = dlg.get_content_area();
    box->set_border_width(18);
    box->set_spacing(12);
    auto* label = Gtk::make_managed<Gtk::Label>(_("Asking MAME about your collection…"));
    label->set_xalign(0.0f);
    box->pack_start(*label, Gtk::PACK_SHRINK);
    Gtk::ProgressBar bar;
    bar.set_show_text(true);
    box->pack_start(bar, Gtk::PACK_SHRINK);
    dlg.show_all();

    // Le fil de travail ne touche jamais aux widgets : il depose son avancee
    // dans des variables partagees et reveille l'interface par un Dispatcher,
    // seul mecanisme sur pour revenir dans le fil principal.
    std::atomic<int> done{0};
    std::atomic<bool> finished{false};
    MameCatalog::AuditResult result;
    Glib::Dispatcher tick;
    tick.connect([&] {
        if (finished.load()) { dlg.response(Gtk::RESPONSE_OK); return; }
        bar.pulse();
        bar.set_text(Glib::ustring::compose(_("%1 sets checked"),
                                            std::to_string(done.load())));
    });

    std::thread worker([&] {
        result = MameCatalog::audit(m_database, mame, paths,
                                    [&](int n) { done.store(n); tick.emit(); return true; });
        finished.store(true);
        tick.emit();
    });

    dlg.run();
    worker.join();
    dlg.hide();

    // Le catalogue en memoire porte les anciens statuts : le relire est plus
    // simple, et plus sur, que de le corriger entree par entree.
    m_cached_games = load_all_catalogs();
    m_search_blobs.clear();
    { std::lock_guard<std::mutex> lk(m_filter_mutex); m_filtered_games.clear(); }
    refresh_emulator_picker();
    populate_filter_tree();
    filter_games();
    update_status_bar_stats();

    SettingsUi::notice(*this, _("MAME check complete"),
        Glib::ustring::compose(
            _("%1 sets are ready to play, %2 are damaged and %3 are missing."),
            std::to_string(result.good + result.playable),
            std::to_string(result.bad),
            std::to_string(result.missing)),
        "bc-check.svg");
}
