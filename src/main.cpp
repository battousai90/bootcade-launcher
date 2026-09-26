// src/main.cpp
#include "MainWindow.h"
#include "SplashScreen.h"
#include "DatabaseManager.h"
#include "MameCatalog.h"
#include "DatSource.h"
#include "RomAudit.h"
#include "RomScanner.h"
#include "DATUpdateDialog.h"
#include <algorithm>
#include <sstream>
#include "AppContext.h"
#include "i18n.h"
#include <gtkmm.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <malloc.h>
#include <iostream>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <curl/curl.h>

/* Chien de garde de la boucle GTK (diagnostic, BOOTCADE_WATCHDOG=1).
 *
 * Un battement est pose sur la boucle principale tous les 100 ms ; un fil a
 * part signale chaque retard. C'est exactement ce que le bureau mesure avant
 * d'afficher « l'application ne repond pas » (mutter attend 5 s a son ping) :
 * tout blocage releve ici est un travail qui aurait du quitter le fil GTK.
 * Ecrit sur stdout pour se lire a cote du journal des actions.
 */
static void start_main_loop_watchdog() {
    static std::atomic<int64_t> last_beat{0};
    auto now_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    last_beat = now_ms();
    Glib::signal_timeout().connect([now_ms] { last_beat = now_ms(); return true; }, 100);
    std::thread([now_ms] {
        bool blocked = false;
        int64_t since = 0;
        int ticks = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            // Every 5 s : what the allocator holds, against what it hands out.
            // In-use bytes that climb are a leak or a growing cache ; a total
            // that climbs while in-use stays flat is fragmentation.
            if (++ticks % 50 == 0) {
                struct mallinfo2 mi = mallinfo2();
                std::cout << "[MEM] t=" << now_ms() / 1000 << "s malloc_inuse=" << (mi.uordblks + mi.hblkhd) / (1024 * 1024)
                          << "MB malloc_total=" << (mi.arena + mi.hblkhd) / (1024 * 1024) << "MB" << std::endl;
            }
            const int64_t gap = now_ms() - last_beat;
            if (gap > 500 && !blocked) { blocked = true; since = last_beat; }
            else if (gap <= 500 && blocked) {
                blocked = false;
                const int64_t d = now_ms() - since;
                std::cout << "[WATCHDOG] main loop blocked " << d << " ms"
                          << (d >= 5000 ? " (desktop would report the application as not responding)" : "")
                          << std::endl;
            }
        }
    }).detach();
}

int main(int argc, char *argv[]) {
    // Once, before any thread : curl_global_init is not thread-safe, and the
    // hiscore probe, the artwork downloader and the DAT client all use curl.
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Redirect stderr to a log file for debugging
    std::string log_path = AppContext::get_user_config_dir() + "/debug.log";
    static std::ofstream debug_log(log_path, std::ios::app);
    std::cerr.rdbuf(debug_log.rdbuf());
    std::cerr << "\n=== Application started at " << std::time(nullptr) << " ===" << std::endl;

    /* Nos propres options sont retirees d'argv AVANT Gtk::Application.
     *
     * GApplication analyse la ligne de commande et refuse ce qu'il ne
     * connait pas : « Unknown option --open=controller », puis il quitte
     * immediatement. Constate au lancement, run() rendait la main en 2 ms.
     * On preleve donc notre drapeau et on compacte le tableau.
     */
    std::string open_window;
    {
        int out = 1;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            if (a.rfind("--open=", 0) == 0) { open_window = a.substr(7); continue; }
            argv[out++] = argv[i];
        }
        argc = out;
        argv[argc] = nullptr;
    }

    auto app = Gtk::Application::create(argc, argv, "org.gilbert.bootcade");
    if (const char* wd = std::getenv("BOOTCADE_WATCHDOG"); wd && *wd && std::string(wd) != "0")
        start_main_loop_watchdog();

    // Initialize translations before any UI string is built. Use the language saved
    // in settings if any; otherwise auto-detect the system language (English fallback).
    std::string ui_lang;
    try {
        std::ifstream cfg(AppContext::get_config_path());
        if (cfg) {
            nlohmann::json j; cfg >> j;
            if (j.contains("language")) ui_lang = j["language"].get<std::string>();
        }
    } catch (...) {}
    i18n::init(AppContext::get_locale_dir(), ui_lang);

    // Créer et afficher le splash screen
    SplashScreen splash;
    splash.show_splash();
    
    // Initialisation
    splash.set_progress(0.1, "Initializing application...");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    splash.set_progress(0.2, "Loading configuration...");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // === Initialize Database ===
    splash.set_progress(0.3, "Connecting to database...");
    std::string db_path = AppContext::get_user_config_dir() + "/games.db";
    
    std::cout << "[DEBUG] Using database: " << db_path << std::endl;
    auto database = std::make_shared<DatabaseManager>(db_path);
    if (!database->initialize()) {
        std::cerr << "[ERROR] Failed to initialize database" << std::endl;
        splash.set_progress(1.0, "Database error - continuing anyway...");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    /* Diagnostics du gestionnaire de ROMs, sur le modele de
     * BOOTCADE_MAME_GENDAT : sans ouvrir la fenetre principale, puis quitter.
     *
     *  BOOTCADE_UPDATE_DAT=1       recharge la base depuis les groupes DAT
     *                              actifs (la boite « Update DAT » elle-meme) ;
     *                              =fbneo ou =mame : ce seul emulateur ;
     *  BOOTCADE_ROM_SCAN=<emu>     scan de la bibliotheque d'un emulateur autre
     *                              que FinalBurn Neo (RomScanner::scan_into_cache) ;
     *  BOOTCADE_ROM_AUDIT=<groupe> audit de l'onglet Bibliotheque pour ce
     *                              groupe ; BOOTCADE_ROM_AUDIT_SETS=1 ecrit en
     *                              plus le verdict de chaque set.
     *  BOOTCADE_DAT_RULES=1        pour chaque DAT charge : ses liens, le mode
     *                              que son en-tete declare, celui retenu.
     * Ils peuvent se combiner, dans cet ordre.
     */
    {
        const char* upd  = std::getenv("BOOTCADE_UPDATE_DAT");
        const char* scan = std::getenv("BOOTCADE_ROM_SCAN");
        const char* grp  = std::getenv("BOOTCADE_ROM_AUDIT");
        const char* rules = std::getenv("BOOTCADE_DAT_RULES");
        const bool any = (upd && *upd) || (scan && *scan) || (grp && *grp) || (rules && *rules == '1');
        if (upd && *upd) {
            Gtk::Window host;
            const auto t0 = std::chrono::steady_clock::now();
            const std::string scope = std::string(upd) == "1" ? std::string() : std::string(upd);
            DATUpdateDialog dialog(host, database, "the active DAT groups",
                                   DATUpdateDialog::files_for_update(scope), scope);
            dialog.start_update();
            dialog.run();
            std::cout << "[UPDATEDAT] seconds=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0).count() / 1000.0
                      << " fbneo=" << database->getGameCount("fbneo")
                      << " mame=" << database->getGameCount("mame") << std::endl;
        }
        if (rules && *rules == '1') {
            const auto groups = DatSource::load_groups();
            for (const auto& [file, st] : database->getDatFileStats()) {
                const DatSource::DatRule* rule = DatSource::rule_of(groups, file);
                const std::string mode = DatSource::effective_merge({st.linked, st.declared}, rule);
                std::cout << "[DATRULE] " << file << " linked=" << st.linked
                          << " declared=" << (st.declared.empty() ? "-" : st.declared)
                          << " rule=" << (rule ? (rule->merge.empty() ? "-" : rule->merge) + (rule->override_dat ? "+override" : "") : "none")
                          << " mode=" << (mode.empty() ? "as-is" : mode) << std::endl;
            }
        }
        if (scan && *scan) {
            const auto t0 = std::chrono::steady_clock::now();
            const auto r = RomScanner::scan_into_cache(database, DatSource::roms_paths_for(scan), scan, true,
                {}, [](const std::string& l, bool w) { std::cout << "[ROMSCAN]" << (w ? " WARN " : " ") << l << std::endl; });
            std::cout << "[ROMSCAN] seconds=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0).count() / 1000.0
                      << " archives=" << r.archives << " reread=" << r.reread
                      << " available=" << r.statuses.available << " incorrect=" << r.statuses.incorrect
                      << " missing=" << r.statuses.missing << " changed=" << r.statuses.changed << std::endl;
        }
        if (grp && *grp) {
            for (const auto& g : DatSource::load_groups()) {
                if (g.id != grp) continue;
                const auto rep = RomAudit::audit(database, DatSource::roms_paths_for(g.emulator),
                                                 /*problems_only=*/false, {},
                                                 DatSource::selected_in_folder(g), g.emulator);
                std::cout << "[ROMAUDIT] group=" << g.id << " total=" << rep.total
                          << " available=" << rep.available << " incorrect=" << rep.incorrect
                          << " missing=" << rep.missing << " ignored=" << rep.ignored
                          << " repairable=" << rep.repairable << " orphans=" << rep.orphans.size()
                          << " missing_bios=" << rep.missing_bios.size() << std::endl;
                if (const char* v = std::getenv("BOOTCADE_ROM_AUDIT_SETS"); v && *v == '1') {
                    for (const auto& e : rep.games) {
                        std::cout << "[ROMAUDIT-SET] " << e.name << " [" << e.system << "] " << e.status
                                  << " absent=" << e.absent << " wrong=" << e.wrong
                                  << " corrupt=" << e.corrupt << " archive=" << e.archive << std::endl;
                        if (v[1] == 'v')
                            for (const auto& r : e.roms)
                                if (r.state != RomAudit::RomState::Present || !r.inherited_from.empty())
                                    std::cout << "[ROMAUDIT-ROM]   " << r.name << " state=" << (int)r.state
                                              << " from=" << r.inherited_from << " found_as=" << r.found_as << std::endl;
                    }
                    for (const auto& o : rep.orphans) std::cout << "[ROMAUDIT-ORPHAN] " << o.path << std::endl;
                    for (const auto& b : rep.missing_bios) std::cout << "[ROMAUDIT-BIOS] " << b.name << " " << b.status << " dependents=" << b.dependents << std::endl;
                }
            }
        }
        if (any) return 0;
    }

    // === Load Games from Database ===
    splash.set_progress(0.4, "Loading game database...");
    std::vector<Game> preloaded_games;
    try {
        preloaded_games = database->getAllGamesLight();
        
        if (preloaded_games.empty()) {
            std::cout << "[INFO] Database is empty - will show empty interface" << std::endl;
            splash.set_progress(0.7, "Database is empty...");
        } else {
            std::cout << "[INFO] Loaded " << preloaded_games.size() << " games from database" << std::endl;
            splash.set_progress(0.7, "Loaded " + std::to_string(preloaded_games.size()) + " games...");
        }
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to load games: " << e.what() << std::endl;
        splash.set_progress(0.7, "Failed to load games - continuing...");
    }

    // === Catalogue MAME ===
    //
    // Interroge l'emulateur installe plutot que de recopier ses donnees : la
    // table n'est regeneree que si MAME a change de version, et le catalogue
    // FBNeo n'est pas touche. Sans MAME sur la machine, on passe simplement
    // notre chemin.
    try {
        const std::string mame = MameCatalog::find_executable();
        if (!mame.empty()) {
            splash.set_progress(0.72, "Reading the MAME catalog...");
            const int n = MameCatalog::sync(database, mame,
                [&splash](int done) {
                    splash.set_progress(0.72, "Reading the MAME catalog... " +
                                              std::to_string(done));
                });
            if (n > 0) {
                // Les machines mecaniques ne sont chargees que si l'utilisateur
                // les a demandees : sinon elles pesent un tiers du catalogue MAME
                // pour des jeux qu'on ne peut pas vraiment jouer ici.
                bool show_mech = false;
                try {
                    std::ifstream cfgf(AppContext::get_config_path());
                    if (cfgf) {
                        nlohmann::json cj; cfgf >> cj;
                        show_mech = cj.value("mame_show_mechanical", false);
                    }
                } catch (...) { /* defaut : masquees */ }
                std::vector<Game> mame_games = MameCatalog::load(database, show_mech);
                std::cout << "[INFO] MAME: " << mame_games.size()
                          << " playable machines (" << n << " in catalog)" << std::endl;
                preloaded_games.insert(preloaded_games.end(),
                                       std::make_move_iterator(mame_games.begin()),
                                       std::make_move_iterator(mame_games.end()));

                // Chaque source arrive triee de son cote : les mettre bout a
                // bout donnerait toute la bibliotheque FinalBurn Neo, puis
                // toute celle de MAME. L'ordre alphabetique est celui qu'on
                // avait avant d'avoir deux catalogues, et celui qu'on attend.
                std::stable_sort(preloaded_games.begin(), preloaded_games.end(),
                                 [](const Game& a, const Game& b) {
                                     return a.description < b.description;
                                 });
            }
            // Diagnostic, sur le modele de BOOTCADE_WATCHDOG : demander le
            // verdict de MAME sur la collection sans passer par l'interface.
            // BOOTCADE_MAME_ROMPATH surcharge les dossiers de mame.ini, qui
            // peuvent parfaitement designer des chemins disparus.
            // Diagnostic : produire les DAT sans passer par l'interface.
            if (const char* gd = std::getenv("BOOTCADE_MAME_GENDAT")) {
                if (*gd) {
                    const auto t0 = std::chrono::steady_clock::now();
                    const int files = MameCatalog::generate_dats(mame, gd,
                        [](int done) { if (done % 16384 == 0)
                                           std::cout << "[GENDAT] " << done << std::endl;
                                       return true; });
                    std::cout << "[GENDAT] files=" << files << " seconds="
                              << std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - t0).count() / 1000.0
                              << std::endl;
                }
            }

            if (const char* want = std::getenv("BOOTCADE_MAME_AUDIT")) {
                if (*want && *want != '0') {
                    std::vector<std::string> paths;
                    if (const char* rp = std::getenv("BOOTCADE_MAME_ROMPATH")) {
                        std::string all(rp), one;
                        std::istringstream ss(all);
                        while (std::getline(ss, one, ';')) if (!one.empty()) paths.push_back(one);
                    } else {
                        paths = MameCatalog::rompaths_from_mame_ini();
                    }
                    const auto r = MameCatalog::audit(database, mame, paths);
                    std::cout << "[AUDIT] good=" << r.good << " playable=" << r.playable
                              << " bad=" << r.bad << " missing=" << r.missing << std::endl;
                }
            }
        } else {
            std::cout << "[INFO] MAME not found; its catalog is skipped" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "[WARN] MAME catalog unavailable: " << e.what() << std::endl;
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // === Create Main Window ===
    splash.set_progress(0.8, "Setting up interface...");
    
    // Créer la fenêtre principale avec callback de progression et jeux préchargés
    MainWindow window(database, [&splash](double progress, const std::string& message) {
        splash.set_progress(progress, message);
    }, std::move(preloaded_games));
    
    // Finalisation
    splash.set_progress(1.0, "Ready!");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Masquer le splash et afficher la fenêtre principale
    splash.hide_splash();

    /* --open=<fenetre> : ouvre directement l'ecran demande.
     *
     * Sert a l'automatisation et aux captures. Il passe par le MEME
     * gestionnaire que le menu, donc ce qu'on photographie est exactement ce
     * que le joueur obtient, et non un chemin de test parallele.
     */
    if (!open_window.empty()) {
        window.signal_show().connect([&window, open_window] {
            window.open_named_window(open_window);
        });
    }

    int rc = app->run(window);
    return rc;
}