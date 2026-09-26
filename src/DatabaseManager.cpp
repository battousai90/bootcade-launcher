// src/DatabaseManager.cpp
#include "DatabaseManager.h"
#include "DatParser.h"
#include <sqlite3.h>
#include <iostream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <cmath>
#include <thread>
#include <atomic>
#include <fstream>
#include <zlib.h>
#include <cstdlib>
#include <set>
#include <unordered_map>
#include <iomanip>
#include <cctype>
#include <cstring>
#include <tuple>
#include "RomScanner.h"

// Helper function to safely get text from SQLite column
static std::string safe_column_text(sqlite3_stmt* stmt, int column) {
    const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, column));
    return text ? std::string(text) : std::string("");
}

// Sanitize stored filesystem paths: remove control characters and trim
static std::string sanitize_path(const std::string& p) {
    std::string out;
    out.reserve(p.size());
    for (unsigned char c : p) {
        if (c == '\r' || c == '\n' || c == '\t') continue;
        out.push_back(static_cast<char>(c));
    }
    // Trim leading/trailing spaces
    size_t start = 0;
    while (start < out.size() && std::isspace(static_cast<unsigned char>(out[start]))) start++;
    size_t end = out.size();
    while (end > start && std::isspace(static_cast<unsigned char>(out[end-1]))) end--;
    return out.substr(start, end - start);
}

DatabaseManager::DatabaseManager(const std::string& db_path) : m_db(nullptr), m_db_path(db_path) {}

DatabaseManager::~DatabaseManager() {
    if (m_db) {
        sqlite3_close(m_db);
    }
}

bool DatabaseManager::initialize() {
    std::cerr << "[DEBUG] Opening database: " << m_db_path << std::endl;
    // The connection is shared by the GTK thread and the scan / audit /
    // import workers : ask for the serialized mode explicitly rather than
    // relying on the distribution's compile-time default, and say so if the
    // library cannot provide it.
    if (sqlite3_threadsafe() == 0)
        std::cerr << "[WARN] SQLite was built without thread safety : concurrent scans are unsafe" << std::endl;
    int rc = sqlite3_open_v2(m_db_path.c_str(), &m_db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Erreur ouverture base de données: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }
    std::cerr << "[DEBUG] Database opened successfully" << std::endl;

    // Performance + concurrency settings
    sqlite3_exec(m_db, "PRAGMA journal_mode=WAL;",      nullptr, nullptr, nullptr);
    sqlite3_exec(m_db, "PRAGMA synchronous=NORMAL;",    nullptr, nullptr, nullptr);
    sqlite3_exec(m_db, "PRAGMA cache_size=-32768;",     nullptr, nullptr, nullptr); // 32 MB page cache
    sqlite3_exec(m_db, "PRAGMA temp_store=MEMORY;",     nullptr, nullptr, nullptr);
    sqlite3_exec(m_db, "PRAGMA foreign_keys=ON;",       nullptr, nullptr, nullptr);

    if (!createTables()) return false;

    // Repair any existing rom_cache rows with stray control characters in filepath
    const char* sel_sql = "SELECT id, filepath FROM rom_cache;";
    sqlite3_stmt* sel_stmt;
    if (sqlite3_prepare_v2(m_db, sel_sql, -1, &sel_stmt, nullptr) == SQLITE_OK) {
        const char* upd_sql = "UPDATE rom_cache SET filepath = ? WHERE id = ?;";
        sqlite3_stmt* upd_stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, upd_sql, -1, &upd_stmt, nullptr) != SQLITE_OK) upd_stmt = nullptr;
        while (sqlite3_step(sel_stmt) == SQLITE_ROW) {
            int id = sqlite3_column_int(sel_stmt, 0);
            std::string fp = safe_column_text(sel_stmt, 1);
            std::string san = sanitize_path(fp);
            if (san != fp) {
                if (upd_stmt) {
                    sqlite3_reset(upd_stmt);
                    sqlite3_bind_text(upd_stmt, 1, san.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int(upd_stmt, 2, id);
                    sqlite3_step(upd_stmt);
                }
            }
        }
        if (upd_stmt) sqlite3_finalize(upd_stmt);
        sqlite3_finalize(sel_stmt);
    }

    return true;
}

// ── Cle (emulator, name, system) pour les donnees du joueur ─────────────────
// `mslug` est a la fois un set FinalBurn Neo et un set MAME, tous deux en
// system 'Arcade' : sous l'ancienne cle (name, system), les favoris et le
// temps de jeu de l'un ecrasaient ceux de l'autre. SQLite ne sait pas changer
// une cle primaire par ALTER TABLE, chaque table est donc reconstruite, et
// une cle de version dans scan_metadata garantit que ca n'arrive qu'une fois :
// une reconstruction rejouee sur une base deja migree perdrait la colonne.
static const char* kEmulatorKeyMigration = "player_data_emulator_key_v1";

bool DatabaseManager::migratePlayerDataToEmulatorKey() {
    // scan_metadata n'est creee que plus loin dans createTables() : sans elle
    // la garde de version ne pourrait pas s'ecrire et la migration se
    // rejouerait a chaque lancement.
    sqlite3_exec(m_db,
                 "CREATE TABLE IF NOT EXISTS scan_metadata ("
                 " key TEXT PRIMARY KEY, value INTEGER NOT NULL);",
                 nullptr, nullptr, nullptr);

    if (getScanMetadata(kEmulatorKeyMigration, 0) == 1) return true;

    // Une table absente (base neuve) est deja creee a la bonne cle plus bas ;
    // une table qui porte deja la colonne a ete migree par une version
    // precedente dont le drapeau s'est perdu.
    auto needs_rebuild = [&](const char* table) -> bool {
        std::string sql = std::string("PRAGMA table_info(") + table + ");";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
        bool exists = false, has_emulator = false;
        while (sqlite3_step(st) == SQLITE_ROW) {
            exists = true;
            const unsigned char* col = sqlite3_column_text(st, 1);
            if (col && std::string(reinterpret_cast<const char*>(col)) == "emulator")
                has_emulator = true;
        }
        sqlite3_finalize(st);
        return exists && !has_emulator;
    };

    const bool stats_todo   = needs_rebuild("player_stats");
    const bool ignored_todo = needs_rebuild("ignored_sets");
    if (!stats_todo && !ignored_todo) {
        setScanMetadata(kEmulatorKeyMigration, 1);
        return true;
    }

    std::vector<std::string> steps;
    // Les deux declencheurs sur `games` ecrivent dans player_stats : un RENAME
    // ne reecrit pas leur corps de maniere fiable, et ils referenceraient une
    // table disparue. createTables() les recree juste apres, a la bonne cle.
    steps.push_back("DROP TRIGGER IF EXISTS games_keep_player_data;");
    steps.push_back("DROP TRIGGER IF EXISTS games_restore_player_data;");

    if (stats_todo) {
        steps.push_back(
            "CREATE TABLE player_stats_migr ("
            " emulator TEXT NOT NULL DEFAULT 'fbneo',"
            " name TEXT NOT NULL, system TEXT NOT NULL,"
            " is_favorite INTEGER NOT NULL DEFAULT 0,"
            " last_played TEXT,"
            " play_count INTEGER NOT NULL DEFAULT 0,"
            " play_time_secs INTEGER NOT NULL DEFAULT 0,"
            " last_session_secs INTEGER NOT NULL DEFAULT 0,"
            " longest_session_secs INTEGER NOT NULL DEFAULT 0,"
            " PRIMARY KEY(emulator, name, system));");
        // Tout l'existant vient de FinalBurn Neo : MAME n'a jamais rien ecrit
        // ici avant cette colonne.
        steps.push_back(
            "INSERT INTO player_stats_migr(emulator, name, system, is_favorite,"
            " last_played, play_count, play_time_secs, last_session_secs,"
            " longest_session_secs)"
            " SELECT 'fbneo', name, system, is_favorite, last_played, play_count,"
            " play_time_secs, COALESCE(last_session_secs,0),"
            " COALESCE(longest_session_secs,0) FROM player_stats;");
        steps.push_back("DROP TABLE player_stats;");
        steps.push_back("ALTER TABLE player_stats_migr RENAME TO player_stats;");
    }

    if (ignored_todo) {
        steps.push_back(
            "CREATE TABLE ignored_sets_migr ("
            " emulator TEXT NOT NULL DEFAULT 'fbneo',"
            " name TEXT NOT NULL, system TEXT NOT NULL,"
            " note TEXT DEFAULT '', added_at TEXT NOT NULL,"
            " PRIMARY KEY(emulator, name, system));");
        steps.push_back(
            "INSERT INTO ignored_sets_migr(emulator, name, system, note, added_at)"
            " SELECT 'fbneo', name, system, COALESCE(note,''), added_at FROM ignored_sets;");
        steps.push_back("DROP TABLE ignored_sets;");
        steps.push_back("ALTER TABLE ignored_sets_migr RENAME TO ignored_sets;");
    }

    // Tout ou rien : ces lignes sont les seules que les fichiers DAT ne savent
    // pas reconstruire.
    char* err = nullptr;
    if (sqlite3_exec(m_db, "BEGIN IMMEDIATE;", nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[WARN] Player data migration could not start: " << err << std::endl;
        sqlite3_free(err);
        return false;
    }
    for (const auto& sql : steps) {
        if (sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
            std::cerr << "[ERROR] Player data migration failed: " << err << std::endl;
            sqlite3_free(err);
            sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
    }
    if (sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[ERROR] Player data migration could not commit: " << err << std::endl;
        sqlite3_free(err);
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    setScanMetadata(kEmulatorKeyMigration, 1);
    std::cerr << "[INFO] Player data now keyed by emulator" << std::endl;
    return true;
}

// ── Cle (emulator, name, system) pour le catalogue lui-meme ─────────────────
// Meme collision que pour les donnees du joueur, mais cette fois sur la table
// qui porte les jeux : `mslug` est un set FinalBurn Neo ET un set MAME, tous
// deux en system 'Arcade'. Tant que `games` porte UNIQUE(name, system),
// l'import du catalogue MAME ecraserait silencieusement le set FBNeo du meme
// nom (et reciproquement) : un seul des deux survivrait a l'insertion.
// SQLite ne sait pas remplacer une contrainte UNIQUE par ALTER TABLE, la
// table est donc reconstruite, et sa propre cle de version garantit que la
// reconstruction n'a lieu qu'une fois : rejouee sur une base deja migree,
// elle perdrait la colonne.
static const char* kGamesEmulatorKeyMigration = "games_emulator_key_v1";

bool DatabaseManager::migrateGamesToEmulatorKey() {
    sqlite3_exec(m_db,
                 "CREATE TABLE IF NOT EXISTS scan_metadata ("
                 " key TEXT PRIMARY KEY, value INTEGER NOT NULL);",
                 nullptr, nullptr, nullptr);

    if (getScanMetadata(kGamesEmulatorKeyMigration, 0) == 1) return true;

    // Base neuve : createTables() vient de creer `games` a la bonne cle, il
    // n'y a rien a reconstruire. Colonne deja presente : une version
    // precedente a migre et seul le drapeau s'est perdu.
    bool exists = false, has_emulator = false;
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(m_db, "PRAGMA table_info(games);", -1, &st, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                exists = true;
                const unsigned char* col = sqlite3_column_text(st, 1);
                if (col && std::string(reinterpret_cast<const char*>(col)) == "emulator")
                    has_emulator = true;
            }
            sqlite3_finalize(st);
        }
    }
    if (!exists || has_emulator) {
        setScanMetadata(kGamesEmulatorKeyMigration, 1);
        return true;
    }

    // L'ordre des colonnes est celui qu'une base existante a reellement, une
    // fois tous les ALTER TABLE appliques : buildGameFromQuery() lit encore
    // les vingt premieres PAR INDICE sur un SELECT *, le changer casserait la
    // lecture. `emulator` est donc ajoutee en queue, la ou elle est lue par
    // nom comme les colonnes ajoutees apres coup.
    static const char* kGamesColumns =
        "id,name,description,year,manufacturer,system,status,video_type,orientation,"
        "width,height,aspect_x,aspect_y,driver_status,comment,cloneof,romof,sourcefile,"
        "snapshot_path,dat_source,is_favorite,last_played,play_count,play_time_secs,"
        "source_directory,dat_header,last_session_secs,longest_session_secs,"
        "genre,family,players,fb_hiscore,is_bios";

    std::vector<std::string> steps;
    // Les deux declencheurs portes par `games` recopient les favoris et le
    // temps de jeu vers player_stats avant une suppression, et les y
    // reprennent apres une insertion : c'est la seule chose ici que les
    // fichiers DAT ne savent pas reconstruire. Ils doivent tomber avant la
    // reconstruction (leur corps designe une table qui va disparaitre, et un
    // RENAME ne le reecrit pas de facon fiable) ; createTables() les recree
    // juste apres, cette fois sur NEW.emulator / OLD.emulator.
    steps.push_back("DROP TRIGGER IF EXISTS games_keep_player_data;");
    steps.push_back("DROP TRIGGER IF EXISTS games_restore_player_data;");

    steps.push_back(
        "CREATE TABLE games_migr ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " name TEXT NOT NULL, description TEXT, year TEXT, manufacturer TEXT, system TEXT,"
        " status TEXT DEFAULT 'missing', video_type TEXT, orientation TEXT,"
        " width TEXT, height TEXT, aspect_x TEXT, aspect_y TEXT, driver_status TEXT,"
        " comment TEXT, cloneof TEXT, romof TEXT, sourcefile TEXT, snapshot_path TEXT,"
        " dat_source TEXT, is_favorite INTEGER DEFAULT 0, last_played TEXT DEFAULT NULL,"
        " play_count INTEGER DEFAULT 0, play_time_secs INTEGER DEFAULT 0,"
        " source_directory TEXT DEFAULT NULL, dat_header TEXT DEFAULT NULL,"
        " last_session_secs INTEGER DEFAULT 0, longest_session_secs INTEGER DEFAULT 0,"
        " genre TEXT DEFAULT NULL, family TEXT DEFAULT NULL, players INTEGER DEFAULT 0,"
        " fb_hiscore INTEGER DEFAULT 0, is_bios INTEGER DEFAULT 0,"
        " emulator TEXT NOT NULL DEFAULT 'fbneo',"
        " UNIQUE(emulator, name, system));");
    // Tout ce qui est deja en base vient de FinalBurn Neo : MAME n'y a jamais
    // rien ecrit avant cette colonne. Les id sont repris tels quels, la table
    // roms y renvoie par game_id.
    steps.push_back(std::string("INSERT INTO games_migr(") + kGamesColumns + ",emulator)"
                    " SELECT " + kGamesColumns + ",'fbneo' FROM games;");
    steps.push_back("DROP TABLE games;");
    steps.push_back("ALTER TABLE games_migr RENAME TO games;");

    // Les cles etrangeres sont actives a l'ouverture (constructeur), et la
    // table roms renvoie a games(id) : DROP TABLE games declenche une
    // suppression en chaine que SQLite refuse. Elles sont donc desactivees le
    // temps de la reconstruction, ce que la procedure documentee par SQLite
    // demande explicitement. Les id sont repris a l'identique, aucune ligne de
    // roms ne se retrouve orpheline. Le reglage se change hors transaction :
    // a l'interieur, le PRAGMA ne ferait rien du tout.
    sqlite3_exec(m_db, "PRAGMA foreign_keys=OFF;", nullptr, nullptr, nullptr);

    auto restore_fk = [&]() {
        sqlite3_exec(m_db, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);
    };

    // Tout ou rien : une reconstruction interrompue a mi-chemin laisserait le
    // catalogue et les favoris dans un etat que rien ne sait rattraper.
    char* err = nullptr;
    if (sqlite3_exec(m_db, "BEGIN IMMEDIATE;", nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[WARN] Games migration could not start: " << err << std::endl;
        sqlite3_free(err);
        restore_fk();
        return false;
    }
    for (const auto& sql : steps) {
        if (sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
            std::cerr << "[ERROR] Games migration failed: " << err << std::endl;
            sqlite3_free(err);
            sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
            restore_fk();
            return false;
        }
    }
    if (sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[ERROR] Games migration could not commit: " << err << std::endl;
        sqlite3_free(err);
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        restore_fk();
        return false;
    }
    restore_fk();

    setScanMetadata(kGamesEmulatorKeyMigration, 1);
    std::cerr << "[INFO] Games catalogue now keyed by emulator" << std::endl;
    return true;
}

bool DatabaseManager::createTables() {
    // Create tables only if they don't exist (preserve existing data)
    char* err_msg = nullptr;
    
    // L'ordre des colonnes n'est pas decoratif : buildGameFromQuery() lit les
    // vingt premieres PAR INDICE sur un SELECT *. Il reproduit donc exactement
    // celui qu'une base existante obtient une fois tous les ALTER TABLE
    // ci-dessous appliques, pour qu'une base neuve et une base migree se
    // lisent de la meme facon.
    //
    // `emulator` fait partie de la cle : `mslug` est un set FinalBurn Neo ET
    // un set MAME, tous deux en system 'Arcade'. Sous l'ancien
    // UNIQUE(name, system), importer le catalogue MAME ecraserait le set FBNeo
    // du meme nom. Elle est ajoutee en queue, la ou migrateGamesToEmulatorKey()
    // la place aussi, et lue par nom.
    const char* create_games_sql = R"(
        CREATE TABLE IF NOT EXISTS games (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            description TEXT,
            year TEXT,
            manufacturer TEXT,
            system TEXT,
            status TEXT DEFAULT 'missing',
            video_type TEXT,
            orientation TEXT,
            width TEXT,
            height TEXT,
            aspect_x TEXT,
            aspect_y TEXT,
            driver_status TEXT,
            comment TEXT,
            cloneof TEXT,
            romof TEXT,
            sourcefile TEXT,
            snapshot_path TEXT,
            dat_source TEXT,
            is_favorite INTEGER DEFAULT 0,
            last_played TEXT DEFAULT NULL,
            play_count INTEGER DEFAULT 0,
            play_time_secs INTEGER DEFAULT 0,
            source_directory TEXT DEFAULT NULL,
            dat_header TEXT DEFAULT NULL,
            last_session_secs INTEGER DEFAULT 0,
            longest_session_secs INTEGER DEFAULT 0,
            genre TEXT DEFAULT NULL,
            family TEXT DEFAULT NULL,
            players INTEGER DEFAULT 0,
            fb_hiscore INTEGER DEFAULT 0,
            is_bios INTEGER DEFAULT 0,
            emulator TEXT NOT NULL DEFAULT 'fbneo',
            UNIQUE(emulator, name, system)
        );
    )";
    
    const char* create_roms_sql = R"(
        CREATE TABLE IF NOT EXISTS roms (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            game_id INTEGER NOT NULL,
            name TEXT NOT NULL,
            size INTEGER,
            crc TEXT,
            file_path TEXT,
            merge TEXT DEFAULT NULL,
            FOREIGN KEY(game_id) REFERENCES games(id),
            UNIQUE(game_id, name)
        );
    )";
    
    const char* create_dat_files_sql = R"(
        CREATE TABLE IF NOT EXISTS dat_files (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            filename TEXT UNIQUE NOT NULL,
            filepath TEXT NOT NULL,
            last_modified INTEGER NOT NULL,
            file_size INTEGER NOT NULL,
            games_count INTEGER DEFAULT 0
        );
    )";

    const char* create_rom_cache_sql = R"(
        CREATE TABLE IF NOT EXISTS rom_cache (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            rom_id INTEGER,
            filename TEXT NOT NULL,
            filepath TEXT NOT NULL,
            last_modified INTEGER NOT NULL,
            file_size INTEGER NOT NULL,
            last_scan_time INTEGER NOT NULL,
            dat_timestamp INTEGER NOT NULL,
            file_crc INTEGER DEFAULT 0,
            FOREIGN KEY(rom_id) REFERENCES roms(id) ON DELETE SET NULL
        );
    )";

    const char* create_scan_metadata_sql = R"(
        CREATE TABLE IF NOT EXISTS scan_metadata (
            key TEXT PRIMARY KEY,
            value INTEGER NOT NULL
        );
    )";

    const char* create_snapshots_sql = R"(
        CREATE TABLE IF NOT EXISTS directory_snapshots (
            path TEXT PRIMARY KEY,
            file_count INTEGER NOT NULL,
            last_modified INTEGER NOT NULL
        );
    )";

    const char* create_directory_files_sql = R"(
        CREATE TABLE IF NOT EXISTS directory_files (
            path TEXT NOT NULL,
            filename TEXT NOT NULL,
            file_size INTEGER NOT NULL,
            last_modified INTEGER NOT NULL,
            PRIMARY KEY(path, filename)
        );
    )";

    const char* create_saved_roots_sql = R"(
        CREATE TABLE IF NOT EXISTS saved_roots (
            path TEXT PRIMARY KEY
        );
    )";

    // Content-addressed ROM cache: the {entry_name, crc} contents of every scanned
    // ZIP (or loose file), keyed by canonical filepath. This lets us re-derive game
    // availability after a DAT update purely from cache, with no disk I/O.
    const char* create_zip_contents_sql = R"(
        CREATE TABLE IF NOT EXISTS zip_contents (
            filepath TEXT NOT NULL,
            entry_name TEXT NOT NULL,
            crc INTEGER NOT NULL,
            PRIMARY KEY(filepath, entry_name)
        );
    )";

    // Execute table creation
    
    if (sqlite3_exec(m_db, create_games_sql, 0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "Erreur création table games: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    
    if (sqlite3_exec(m_db, create_roms_sql, 0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "Erreur création table roms: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    
    if (sqlite3_exec(m_db, create_dat_files_sql, 0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "Erreur création table dat_files: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    if (sqlite3_exec(m_db, create_rom_cache_sql, 0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "Erreur création table rom_cache: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    // Ensure `file_crc` column exists for rom_cache (add if missing)
    const char* pragma_sql = "PRAGMA table_info(rom_cache);";
    sqlite3_stmt* pragma_stmt;
    bool has_file_crc = false;
    if (sqlite3_prepare_v2(m_db, pragma_sql, -1, &pragma_stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(pragma_stmt) == SQLITE_ROW) {
            std::string colname = safe_column_text(pragma_stmt, 1);
            if (colname == "file_crc") { has_file_crc = true; break; }
        }
        sqlite3_finalize(pragma_stmt);
    }

    if (!has_file_crc) {
        const char* alter_sql = "ALTER TABLE rom_cache ADD COLUMN file_crc INTEGER DEFAULT 0;";
        if (sqlite3_exec(m_db, alter_sql, 0, 0, &err_msg) != SQLITE_OK) {
            sqlite3_free(err_msg);
        }
    }

    // rom_cache had no UNIQUE constraint on filepath, so INSERT OR REPLACE never
    // actually replaced anything : every scan just appended a new row for the
    // same file. Lookups (isRomFileCached) have no ORDER BY, so they kept
    // returning whichever row happened to be oldest, comparing fresh disk state
    // against a cache entry that could be scans out of date and never updating // the file looked permanently "unchanged" no matter how many times it was
    // rescanned. Dedupe once (keep the newest row per path, by id) and add the
    // missing unique index so INSERT OR REPLACE finally does its job.
    if (sqlite3_exec(m_db,
            "DELETE FROM rom_cache WHERE id NOT IN "
            "(SELECT MAX(id) FROM rom_cache GROUP BY filepath);",
            0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "[WARN] Could not dedupe rom_cache: " << err_msg << std::endl;
        sqlite3_free(err_msg);
    }
    if (sqlite3_exec(m_db,
            "CREATE UNIQUE INDEX IF NOT EXISTS idx_rom_cache_filepath_unique ON rom_cache(filepath);",
            0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "[WARN] Could not create rom_cache filepath unique index: " << err_msg << std::endl;
        sqlite3_free(err_msg);
    }

    // ── Migrate games table: add new columns if missing ─────────────────────
    struct ColDef { const char* name; const char* ddl; };
    static const ColDef new_cols[] = {
        { "is_favorite",      "ALTER TABLE games ADD COLUMN is_favorite INTEGER DEFAULT 0;" },
        { "last_played",      "ALTER TABLE games ADD COLUMN last_played TEXT DEFAULT NULL;" },
        { "play_count",       "ALTER TABLE games ADD COLUMN play_count INTEGER DEFAULT 0;" },
        { "play_time_secs",   "ALTER TABLE games ADD COLUMN play_time_secs INTEGER DEFAULT 0;" },
        { "source_directory", "ALTER TABLE games ADD COLUMN source_directory TEXT DEFAULT NULL;" },
        { "dat_header",       "ALTER TABLE games ADD COLUMN dat_header TEXT DEFAULT NULL;" },
        { "last_session_secs",    "ALTER TABLE games ADD COLUMN last_session_secs INTEGER DEFAULT 0;" },
        { "longest_session_secs", "ALTER TABLE games ADD COLUMN longest_session_secs INTEGER DEFAULT 0;" },
        // Ecrits dans le DAT par notre fork FBNeo depuis la structure du
        // pilote. Restent vides tant que les DAT installes sont ceux d'amont,
        // et se remplissent au prochain import : rien a faire pour le joueur.
        { "genre",      "ALTER TABLE games ADD COLUMN genre TEXT DEFAULT NULL;" },
        { "family",     "ALTER TABLE games ADD COLUMN family TEXT DEFAULT NULL;" },
        { "players",    "ALTER TABLE games ADD COLUMN players INTEGER DEFAULT 0;" },
        { "fb_hiscore", "ALTER TABLE games ADD COLUMN fb_hiscore INTEGER DEFAULT 0;" },
        { "is_bios",    "ALTER TABLE games ADD COLUMN is_bios INTEGER DEFAULT 0;" },
    };
    // Same mechanism for the roms table. `merge` is the DAT's merge= attribute,
    // the one piece of information that tells a split set apart from a broken
    // non-merged one : without it every listed ROM is demanded from the game's
    // own zip, and a collection in split form reads as mostly missing.
    static const ColDef new_rom_cols[] = {
        { "merge", "ALTER TABLE roms ADD COLUMN merge TEXT DEFAULT NULL;" },
    };
    auto migrate = [&](const char* table, const ColDef* cols, size_t n) -> bool {
        std::string pragma = std::string("PRAGMA table_info(") + table + ");";
        sqlite3_stmt* pi = nullptr;
        sqlite3_prepare_v2(m_db, pragma.c_str(), -1, &pi, nullptr);
        std::set<std::string> existing_cols;
        while (sqlite3_step(pi) == SQLITE_ROW)
            existing_cols.insert(safe_column_text(pi, 1));
        sqlite3_finalize(pi);

        bool added = false;
        for (size_t i = 0; i < n; ++i) {
            const auto& col = cols[i];
            if (existing_cols.find(col.name) != existing_cols.end()) continue;
            if (sqlite3_exec(m_db, col.ddl, 0, 0, &err_msg) != SQLITE_OK) {
                std::cerr << "[WARN] Could not add column " << col.name << ": " << err_msg << std::endl;
                sqlite3_free(err_msg);
            } else {
                added = true;
            }
        }
        return added;
    };
    migrate("games", new_cols, sizeof(new_cols) / sizeof(new_cols[0]));
    if (migrate("roms", new_rom_cols, sizeof(new_rom_cols) / sizeof(new_rom_cols[0]))) {
        // A freshly added column is empty for every ROM already loaded : the
        // games table has to be re-read from the DAT files once. The flag lets
        // the main window offer that up front rather than leave the user to
        // guess why nothing changed. dat_files records which DATs were loaded
        // in full, and by mtime/size those look current when they are not :
        // forgetting them keeps the incremental sync (DatParser::
        // synchronizeDatsToDatabase) honest should it ever be the one to run.
        // Games and player data are untouched here.
        if (sqlite3_exec(m_db,
                "DELETE FROM dat_files;"
                "INSERT OR REPLACE INTO scan_metadata (key, value) VALUES ('needs_dat_resync', 1);",
                0, 0, &err_msg) != SQLITE_OK) {
            std::cerr << "[WARN] Could not flag the DAT resync: " << err_msg << std::endl;
            sqlite3_free(err_msg);
        }
    }

    // Bootcade pilote deux emulateurs et le meme nom de set existe des deux
    // cotes : `mslug` est un jeu FinalBurn Neo ET un jeu MAME, tous deux en
    // system 'Arcade'. Tant que la cle se limite a (name, system), leurs
    // favoris et leur temps de jeu se confondent sans que rien ne le signale.
    // A faire avant les CREATE ci-dessous, qui decrivent deja la nouvelle cle.
    migratePlayerDataToEmulatorKey();
    // Et le catalogue lui-meme, pour la meme raison : il doit pouvoir porter
    // `mslug` deux fois, une fois par emulateur. A faire avant les
    // declencheurs plus bas, qui vont lire NEW.emulator / OLD.emulator.
    migrateGamesToEmulatorKey();

    // ── Sets the user chose to stop hearing about ─────────────────────────
    // Own table, no foreign key: like player_stats below, this is a decision
    // the user made, and the games table it refers to is rebuilt from the DAT
    // files whenever they change.
    if (sqlite3_exec(m_db,
            "CREATE TABLE IF NOT EXISTS ignored_sets ("
            "  emulator TEXT NOT NULL DEFAULT 'fbneo',"
            "  name TEXT NOT NULL, system TEXT NOT NULL,"
            "  note TEXT DEFAULT '', added_at TEXT NOT NULL,"
            "  PRIMARY KEY(emulator, name, system));",
            0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "[WARN] Could not create ignored_sets: " << err_msg << std::endl;
        sqlite3_free(err_msg);
    }

    // ── Player data must outlive the games table ────────────────────────────
    // Favourites and play history are the only data here that no file on disk
    // can rebuild : and the games table is wiped and rebuilt whenever the DAT
    // files change. That happened in practice: a routine FBNeo sync erased
    // every play counter and favourite in one go.
    //
    // Four separate code paths delete from `games` (a full DAT reload, a DAT
    // update, a changed DAT, a removed DAT), so protecting them one by one is
    // a promise the next path will break. These two triggers move the
    // guarantee into the database itself: whatever deletes a row, its player
    // data is copied out first; whatever re-inserts it, the data comes back.
    //
    // player_stats deliberately has NO foreign key to games : a cascade is
    // exactly what must not happen here.
    if (sqlite3_exec(m_db,
            "CREATE TABLE IF NOT EXISTS player_stats ("
            " emulator TEXT NOT NULL DEFAULT 'fbneo',"
            " name TEXT NOT NULL, system TEXT NOT NULL,"
            " is_favorite INTEGER NOT NULL DEFAULT 0,"
            " last_played TEXT,"
            " play_count INTEGER NOT NULL DEFAULT 0,"
            " play_time_secs INTEGER NOT NULL DEFAULT 0,"
            " last_session_secs INTEGER NOT NULL DEFAULT 0,"
            " longest_session_secs INTEGER NOT NULL DEFAULT 0,"
            " PRIMARY KEY(emulator, name, system));",
            0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "[WARN] Could not create player_stats: " << err_msg << std::endl;
        sqlite3_free(err_msg);
    }

    // Seed it from whatever the games table still holds, so an existing
    // install is protected from the first launch after this change.
    if (sqlite3_exec(m_db,
            "INSERT INTO player_stats(emulator, name, system, is_favorite, last_played,"
            " play_count, play_time_secs, last_session_secs, longest_session_secs)"
            " SELECT emulator, name, system, is_favorite, last_played, play_count,"
            " play_time_secs, COALESCE(last_session_secs,0),"
            " COALESCE(longest_session_secs,0) FROM games"
            " WHERE emulator='fbneo' AND (is_favorite=1 OR play_count>0 OR play_time_secs>0)"
            " ON CONFLICT(emulator, name, system) DO NOTHING;",
            0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "[WARN] Could not seed player_stats: " << err_msg << std::endl;
        sqlite3_free(err_msg);
    }

    // Copied out on the way to deletion. The WHEN clause keeps the shadow
    // table to the handful of games actually touched rather than mirroring a
    // 29 000-row catalogue of zeroes.
    // DROP puis CREATE plutot que CREATE IF NOT EXISTS : le corps d'un
    // declencheur est fige a sa creation. Une base installee avant que
    // `games` ne porte un emulateur garde sinon l'ancienne version, celle qui
    // ecrivait la constante 'fbneo', et rangerait les favoris MAME sous
    // FinalBurn Neo sans que rien ne le signale. Les recreer a chaque
    // demarrage ne coute qu'une ecriture de schema.
    sqlite3_exec(m_db, "DROP TRIGGER IF EXISTS games_keep_player_data;", 0, 0, nullptr);
    sqlite3_exec(m_db, "DROP TRIGGER IF EXISTS games_restore_player_data;", 0, 0, nullptr);

    if (sqlite3_exec(m_db,
            "CREATE TRIGGER games_keep_player_data"
            " BEFORE DELETE ON games"
            // FinalBurn Neo seulement. Les donnees de joueur d'un jeu MAME ne
            // vivent QUE dans player_stats (voir toggleFavorite) : la ligne
            // `games` qu'un DAT MAME du gestionnaire de ROMs y ajoute n'en
            // porte qu'une copie figee a l'insertion. La recopier a la
            // suppression ecraserait les parties jouees depuis.
            " WHEN OLD.emulator='fbneo' AND (OLD.is_favorite=1 OR OLD.play_count>0 OR OLD.play_time_secs>0)"
            " BEGIN"
            // `games` porte maintenant les deux catalogues : l'emulateur se
            // lit sur la ligne supprimee, il n'est plus constant.
            "  INSERT INTO player_stats(emulator, name, system, is_favorite, last_played,"
            "   play_count, play_time_secs, last_session_secs, longest_session_secs)"
            "  VALUES(OLD.emulator, OLD.name, OLD.system, OLD.is_favorite, OLD.last_played,"
            "   OLD.play_count, OLD.play_time_secs,"
            "   COALESCE(OLD.last_session_secs,0), COALESCE(OLD.longest_session_secs,0))"
            "  ON CONFLICT(emulator, name, system) DO UPDATE SET"
            "   is_favorite=excluded.is_favorite, last_played=excluded.last_played,"
            "   play_count=excluded.play_count, play_time_secs=excluded.play_time_secs,"
            "   last_session_secs=excluded.last_session_secs,"
            "   longest_session_secs=excluded.longest_session_secs;"
            " END;",
            0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "[WARN] Could not create keep trigger: " << err_msg << std::endl;
        sqlite3_free(err_msg);
    }

    // Put back on the way in. The EXISTS guard means the UPDATE does nothing
    // for the overwhelming majority of rows, so a full catalogue reload pays
    // only one indexed lookup per game against a tiny table.
    if (sqlite3_exec(m_db,
            "CREATE TRIGGER games_restore_player_data"
            " AFTER INSERT ON games"
            // Meme raison que ci-dessus : la ligne inseree dit de quel
            // emulateur elle vient, et c'est cette ligne-la de player_stats
            // qu'il faut reprendre : `mslug` FBNeo et `mslug` MAME ont chacun
            // la leur.
            " WHEN NEW.emulator='fbneo' AND EXISTS(SELECT 1 FROM player_stats"
            "             WHERE emulator=NEW.emulator AND name=NEW.name AND system=NEW.system)"
            " BEGIN"
            "  UPDATE games SET"
            "   is_favorite=(SELECT is_favorite FROM player_stats WHERE emulator=NEW.emulator AND name=NEW.name AND system=NEW.system),"
            "   last_played=(SELECT last_played FROM player_stats WHERE emulator=NEW.emulator AND name=NEW.name AND system=NEW.system),"
            "   play_count=(SELECT play_count FROM player_stats WHERE emulator=NEW.emulator AND name=NEW.name AND system=NEW.system),"
            "   play_time_secs=(SELECT play_time_secs FROM player_stats WHERE emulator=NEW.emulator AND name=NEW.name AND system=NEW.system),"
            "   last_session_secs=(SELECT last_session_secs FROM player_stats WHERE emulator=NEW.emulator AND name=NEW.name AND system=NEW.system),"
            "   longest_session_secs=(SELECT longest_session_secs FROM player_stats WHERE emulator=NEW.emulator AND name=NEW.name AND system=NEW.system)"
            "  WHERE id=NEW.id;"
            " END;",
            0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "[WARN] Could not create restore trigger: " << err_msg << std::endl;
        sqlite3_free(err_msg);
    }

    {
    }

    if (sqlite3_exec(m_db, create_scan_metadata_sql, 0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "Erreur création table scan_metadata: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    if (sqlite3_exec(m_db, create_snapshots_sql, 0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "Erreur création table directory_snapshots: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    if (sqlite3_exec(m_db, create_directory_files_sql, 0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "Erreur création table directory_files: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    if (sqlite3_exec(m_db, create_saved_roots_sql, 0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "Erreur création table saved_roots: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    if (sqlite3_exec(m_db, create_zip_contents_sql, 0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "Erreur création table zip_contents: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    // Size and mtime of each archive as it was when zip_contents read it.
    // rom_cache already says as much, but for FinalBurn Neo's scan only, and
    // with a DAT timestamp attached : a file another emulator's scan reads
    // must not appear "already scanned" to that one. A table of its own, new
    // and additive : nothing existing is altered.
    if (sqlite3_exec(m_db,
            "CREATE TABLE IF NOT EXISTS zip_contents_stamp ("
            "  filepath TEXT PRIMARY KEY,"
            "  file_size INTEGER NOT NULL,"
            "  last_modified INTEGER NOT NULL);",
            0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "[WARN] Could not create zip_contents_stamp: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        err_msg = nullptr;
    }

    // Les CHD des sets MAME. Table a part, nouvelle et
    // additive : ni games ni roms ne changent de forme, et une base anterieure
    // l'obtient vide a son premier demarrage. Pas de cle etrangere : les
    // reconstructions de games (migrations) n'ont pas a la connaitre, et
    // chaque DELETE de jeux efface d'abord ses disques.
    if (sqlite3_exec(m_db,
            "CREATE TABLE IF NOT EXISTS disks ("
            "  game_id INTEGER NOT NULL,"
            "  name TEXT NOT NULL,"
            "  sha1 TEXT NOT NULL,"
            "  merge TEXT NOT NULL DEFAULT '',"
            "  PRIMARY KEY(game_id, name));",
            0, 0, &err_msg) != SQLITE_OK) {
        std::cerr << "[WARN] Could not create disks: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        err_msg = nullptr;
    }
    // Une table creee par une version de travail, avant la colonne merge :
    // l'ajouter echoue sans consequence quand elle existe deja.
    sqlite3_exec(m_db, "ALTER TABLE disks ADD COLUMN merge TEXT NOT NULL DEFAULT '';", 0, 0, nullptr);

    // Les devices des machines MAME (<device_ref>), meme logique que disks :
    // table a part, sans cle etrangere, videe avec les jeux. Une base qui
    // porte deja des sets MAME ne les connait qu'apres une relecture des DAT
    // MAME : le drapeau needs_mame_dat_resync la fait proposer au demarrage.
    {
        bool existed = false;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(m_db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='devices';",
                               -1, &st, nullptr) == SQLITE_OK) {
            existed = sqlite3_step(st) == SQLITE_ROW;
            sqlite3_finalize(st);
        }
        if (sqlite3_exec(m_db,
                "CREATE TABLE IF NOT EXISTS devices ("
                "  game_id INTEGER NOT NULL,"
                "  name TEXT NOT NULL,"
                "  PRIMARY KEY(game_id, name));",
                0, 0, &err_msg) != SQLITE_OK) {
            std::cerr << "[WARN] Could not create devices: " << err_msg << std::endl;
            sqlite3_free(err_msg);
            err_msg = nullptr;
        } else if (!existed) {
            sqlite3_exec(m_db,
                "INSERT OR REPLACE INTO scan_metadata (key, value) "
                "SELECT 'needs_mame_dat_resync', 1 WHERE EXISTS (SELECT 1 FROM games WHERE emulator = 'mame');",
                0, 0, nullptr);
        }
    }

    // Index creation is best-effort and executed statement-by-statement: a missing
    // column on a legacy DB (e.g. an old rom_cache without rom_id) must neither abort
    // startup nor prevent the remaining indexes from being created. Indexes are pure
    // optimizations, so a failure here is a warning, not a fatal error.
    static const char* index_stmts[] = {
        "CREATE INDEX IF NOT EXISTS idx_games_name ON games(name);",
        "CREATE INDEX IF NOT EXISTS idx_games_system ON games(system);",
        // Les recherches par cle complete (emulator, name, system) passent
        // deja par l'index implicite de la contrainte UNIQUE. Celui-ci ne sert
        // qu'a isoler un catalogue entier : compter les jeux MAME, ou n'en
        // afficher qu'un des deux.
        "CREATE INDEX IF NOT EXISTS idx_games_emulator ON games(emulator);",
        "CREATE INDEX IF NOT EXISTS idx_games_status ON games(status);",
        "CREATE INDEX IF NOT EXISTS idx_roms_game_id ON roms(game_id);",
        // Expression index matching getGamesByRomCrc's WHERE LOWER(r.crc) = ?
        // exactly. Without it every CRC lookup scans all ~225k ROM rows, which the
        // ROM manager does thousands of times per analysis.
        "CREATE INDEX IF NOT EXISTS idx_roms_crc_lower ON roms(LOWER(crc));",
        "CREATE INDEX IF NOT EXISTS idx_dat_files_filename ON dat_files(filename);",
        "CREATE INDEX IF NOT EXISTS idx_rom_cache_filename ON rom_cache(filename);",
        "CREATE INDEX IF NOT EXISTS idx_rom_cache_rom_id ON rom_cache(rom_id);",
        "CREATE INDEX IF NOT EXISTS idx_zip_contents_crc ON zip_contents(crc);",
        "CREATE INDEX IF NOT EXISTS idx_zip_contents_path ON zip_contents(filepath);",
    };
    for (const char* idx_sql : index_stmts) {
        if (sqlite3_exec(m_db, idx_sql, 0, 0, &err_msg) != SQLITE_OK) {
            std::cerr << "[WARN] Index skipped (" << (err_msg ? err_msg : "?") << "): " << idx_sql << std::endl;
            sqlite3_free(err_msg);
            err_msg = nullptr;
        }
    }

    return true;
}

// Helper: compute CRC32 of a file (used for quick whole-file fingerprint)
static long compute_file_crc32(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) return 0;
    uLong crc = crc32(0L, Z_NULL, 0);
    char buffer[8192];
    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
        crc = crc32(crc, (const Bytef*)buffer, file.gcount());
    }
    return static_cast<long>(crc);
}

// Background cleanup thread data
static void cache_cleanup_thread_func(DatabaseManager* db, std::vector<std::string> rom_paths, std::atomic<bool>* running) {
    while (running->load()) {
        db->cleanupRomCache(rom_paths);
        for (int i = 0; i < 60 && running->load(); ++i) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

bool DatabaseManager::insertGame(const Game& game) {
    const char* sql = R"(
        INSERT INTO games 
        (name, description, year, manufacturer, system, status, video_type, orientation, 
         width, height, aspect_x, aspect_y, driver_status, comment, cloneof, romof, sourcefile, snapshot_path, dat_source, dat_header,
         genre, family, players, fb_hiscore, is_bios, emulator)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Erreur préparation requête insertGame: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, game.name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, game.description.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, game.year.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, game.manufacturer.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, game.system.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, game.status.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 7, game.video_type.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 8, game.orientation.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 9, game.width.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 10, game.height.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 11, game.aspect_x.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 12, game.aspect_y.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 13, game.driver_status.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 14, game.comment.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 15, game.cloneof.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 16, game.romof.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 17, game.sourcefile.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 18, "", -1, SQLITE_STATIC); // snapshot_path (empty for now)
    sqlite3_bind_text(stmt, 19, game.dat_source.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 20, game.dat_header.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 21, game.genre.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 22, game.family.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int (stmt, 23, game.players);
    sqlite3_bind_int (stmt, 24, game.fb_hiscore ? 1 : 0);
    sqlite3_bind_int (stmt, 25, game.is_bios ? 1 : 0);
    // Sans elle, `mslug` MAME et `mslug` FBNeo se disputeraient la meme ligne.
    // Un appelant qui ne renseigne rien decrit un jeu FinalBurn Neo : c'est le
    // seul catalogue qui existait avant.
    const char* emu = game.emulator.empty() ? "fbneo" : game.emulator.c_str();
    sqlite3_bind_text(stmt, 26, emu, -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        std::cerr << "Erreur insertion game: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }
    
    // Get the inserted game's ID
    int64_t game_id = sqlite3_last_insert_rowid(m_db);
    
    // Insérer les ROMs
    for (const auto& rom : game.roms) {
        if (!insertRom(game_id, rom)) {
            return false;
        }
    }

    if (!game.disks.empty()) {
        sqlite3_stmt* ds = nullptr;
        if (sqlite3_prepare_v2(m_db, "INSERT OR REPLACE INTO disks (game_id, name, sha1, merge) VALUES (?, ?, ?, ?);",
                               -1, &ds, nullptr) != SQLITE_OK) {
            std::cerr << "Erreur préparation insertion disks: " << sqlite3_errmsg(m_db) << std::endl;
            return false;
        }
        for (const auto& d : game.disks) {
            sqlite3_reset(ds);
            sqlite3_bind_int64(ds, 1, game_id);
            sqlite3_bind_text(ds, 2, d.name.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(ds, 3, d.sha1.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(ds, 4, d.merge.c_str(), -1, SQLITE_STATIC);
            if (sqlite3_step(ds) != SQLITE_DONE) { sqlite3_finalize(ds); return false; }
        }
        sqlite3_finalize(ds);
    }

    if (!game.devices.empty()) {
        sqlite3_stmt* dv = nullptr;
        if (sqlite3_prepare_v2(m_db, "INSERT OR REPLACE INTO devices (game_id, name) VALUES (?, ?);",
                               -1, &dv, nullptr) != SQLITE_OK) {
            std::cerr << "Erreur préparation insertion devices: " << sqlite3_errmsg(m_db) << std::endl;
            return false;
        }
        for (const auto& name : game.devices) {
            sqlite3_reset(dv);
            sqlite3_bind_int64(dv, 1, game_id);
            sqlite3_bind_text(dv, 2, name.c_str(), -1, SQLITE_STATIC);
            if (sqlite3_step(dv) != SQLITE_DONE) { sqlite3_finalize(dv); return false; }
        }
        sqlite3_finalize(dv);
    }

    return true;
}

bool DatabaseManager::insertRom(int64_t game_id, const Rom& rom) {
    const char* sql = "INSERT OR REPLACE INTO roms (game_id, name, size, crc, merge) VALUES (?, ?, ?, ?, ?);";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Erreur préparation requête insertRom: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }
    
    sqlite3_bind_int64(stmt, 1, game_id);
    sqlite3_bind_text(stmt, 2, rom.name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, rom.size);
    sqlite3_bind_text(stmt, 4, rom.crc.c_str(), -1, SQLITE_STATIC);
    if (rom.merge.empty()) sqlite3_bind_null(stmt, 5);
    else                   sqlite3_bind_text(stmt, 5, rom.merge.c_str(), -1, SQLITE_STATIC);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return rc == SQLITE_DONE;
}

bool DatabaseManager::updateRomPath(const std::string& game_name, const std::string& rom_name, const std::string& file_path) {
    const char* sql = "UPDATE roms SET file_path = ? WHERE game_name = ? AND name = ?;";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, file_path.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, game_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, rom_name.c_str(), -1, SQLITE_STATIC);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return rc == SQLITE_DONE;
}

bool DatabaseManager::updateGameStatus(const std::string& game_name, const std::string& status) {
    const char* sql = "UPDATE games SET status = ? WHERE name = ?;";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, game_name.c_str(), -1, SQLITE_STATIC);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return rc == SQLITE_DONE;
}

bool DatabaseManager::updateGameStatus(const std::string& game_name, const std::string& status,
                                      const std::string& system, const std::string& emulator) {
    // Sans l'emulateur dans le WHERE, marquer `mslug` disponible cote FBNeo
    // marquerait aussi le `mslug` de MAME, dont les fichiers n'ont rien a voir.
    const char* sql = "UPDATE games SET status = ? WHERE name = ? AND system = ? AND emulator = ?;";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[ERROR] Failed to prepare updateGameStatus: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, game_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, system.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, emulator.c_str(), -1, SQLITE_STATIC);
    
    int rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(m_db);
    
    
    sqlite3_finalize(stmt);
    
    if (rc == SQLITE_DONE && changes > 0) {
        // Force a sync to disk to ensure the change is persisted
        sqlite3_exec(m_db, "PRAGMA synchronous = FULL;", nullptr, nullptr, nullptr);
    }
    
    return rc == SQLITE_DONE;
}

bool DatabaseManager::updateGameStatusWithSource(const std::string& game_name, const std::string& status, const std::string& system, const std::string& source_directory, const std::string& emulator) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    // Meme raison qu'au-dessus : deux jeux peuvent porter ce nom et ce
    // systeme, un par emulateur.
    const char* sql = "UPDATE games SET status = ?, source_directory = ? WHERE name = ? AND system = ? AND emulator = ?;";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[ERROR] Failed to prepare updateGameStatusWithSource: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_STATIC);
    if (source_directory.empty())
        sqlite3_bind_null(stmt, 2);
    else
        sqlite3_bind_text(stmt, 2, source_directory.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, game_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, system.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, emulator.c_str(), -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool DatabaseManager::resetGamesFromDirectory(const std::string& directory, const std::string& emulator) {
    // Un dossier retire des reglages d'un emulateur ne retire rien a l'autre :
    // le meme dossier peut tres bien figurer dans les deux bibliotheques.
    const char* sql = "UPDATE games SET status = 'missing', source_directory = NULL WHERE source_directory = ? AND emulator = ?;";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[ERROR] Failed to prepare resetGamesFromDirectory: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    sqlite3_bind_text(stmt, 1, directory.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, emulator.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(m_db);
    sqlite3_finalize(stmt);

    std::cerr << "[DEBUG] resetGamesFromDirectory(" << directory << "): reset " << changes << " games to missing" << std::endl;
    return rc == SQLITE_DONE;
}

bool DatabaseManager::updateGameSnapshot(const std::string& game_name, const std::string& snapshot_path) {
    const char* sql = "UPDATE games SET snapshot_path = ? WHERE name = ?;";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, snapshot_path.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, game_name.c_str(), -1, SQLITE_STATIC);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return rc == SQLITE_DONE;
}

bool DatabaseManager::resetAllGamesToMissing() {
    const char* sql = "UPDATE games SET status = 'missing';";
    
    char* err_msg = nullptr;
    int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &err_msg);
    
    if (rc != SQLITE_OK) {
        std::cerr << "[ERROR] Failed to reset games to missing: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    
    // Force sync to disk
    sqlite3_exec(m_db, "PRAGMA synchronous = FULL;", nullptr, nullptr, nullptr);
    
    return true;
}

// Le catalogue avec ses ROMs : pour l'audit et RomResolve, qui comparent
// fichier par fichier.
std::vector<Game> DatabaseManager::getAllGames(const std::string& emulator) { return loadAllGames(true, emulator); }

// Le catalogue seul : pour l'interface, qui trie, filtre et affiche mais
// n'inspecte jamais le contenu d'un set.
std::vector<Game> DatabaseManager::getAllGamesLight(const std::string& emulator) { return loadAllGames(false, emulator); }

std::vector<Game> DatabaseManager::loadAllGames(bool with_roms, const std::string& emulator) {
    std::vector<Game> games;

    // Step 1: load every game in a single query, indexed by id for ROM stitching.
    const char* games_sql =
        /* Les quatre dernieres colonnes sont indispensables et manquaient.
         *
         * buildGameFromQuery ne remplit is_favorite, last_played, play_count
         * et play_time_secs que si la requete a rapporte assez de colonnes
         * (ncols > 20/21/22/23). Cette requete s'arretait a dat_header, soit
         * 21 colonnes : les statistiques de jeu etaient donc TOUJOURS a zero
         * en memoire, alors qu'elles sont bien ecrites en base par
         * recordLaunch() et addPlayTime(). Les strategies « dernier joue » et
         * « le plus joue » ne pouvaient rien trouver. Constate au lancement :
         * la base donnait turfmast a 5738 s, la memoire 0 partout.
         */
        "SELECT id, name, description, year, manufacturer, system, cloneof, romof, "
        "sourcefile, comment, video_type, orientation, width, height, aspect_x, "
        "aspect_y, driver_status, status, snapshot_path, dat_source, dat_header, "
        "is_favorite, last_played, play_count, play_time_secs, "
        "genre, family, players, fb_hiscore, is_bios, emulator "
        "FROM games WHERE (? = '' OR emulator = ?) ORDER BY description;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, games_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return games;
    }
    sqlite3_bind_text(stmt, 1, emulator.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, emulator.c_str(), -1, SQLITE_TRANSIENT);

    std::unordered_map<int64_t, size_t> id_to_index;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Game game;
        int64_t game_id   = sqlite3_column_int64(stmt, 0);
        game.name         = safe_column_text(stmt, 1);
        game.description  = safe_column_text(stmt, 2);
        game.year         = safe_column_text(stmt, 3);
        game.manufacturer = safe_column_text(stmt, 4);
        game.system       = safe_column_text(stmt, 5);
        game.cloneof      = safe_column_text(stmt, 6);
        game.romof        = safe_column_text(stmt, 7);
        game.sourcefile   = safe_column_text(stmt, 8);
        game.comment      = safe_column_text(stmt, 9);
        game.video_type   = safe_column_text(stmt, 10);
        game.orientation  = safe_column_text(stmt, 11);
        game.width        = safe_column_text(stmt, 12);
        game.height       = safe_column_text(stmt, 13);
        game.aspect_x     = safe_column_text(stmt, 14);
        game.aspect_y     = safe_column_text(stmt, 15);
        game.driver_status= safe_column_text(stmt, 16);
        game.status       = safe_column_text(stmt, 17);
        game.snapshot_path= safe_column_text(stmt, 18);
        game.dat_source   = safe_column_text(stmt, 19);
        game.dat_header   = safe_column_text(stmt, 20);
        /* Les statistiques locales de jeu, lues ici et nulle part ailleurs.
         *
         * Cette boucle lit chaque colonne par INDICE : ajouter les colonnes a
         * la requete ne suffisait pas, il fallait aussi les lire. Sans ces
         * quatre lignes, m_cached_games rend 0 partout et les strategies de
         * demarrage « dernier joue » et « le plus joue » n'ont rien sur quoi
         * travailler, alors que la base est correctement alimentee par
         * recordLaunch() et addPlayTime(). */
        game.is_favorite    = sqlite3_column_int(stmt, 21) != 0;
        game.last_played    = safe_column_text(stmt, 22);
        game.play_count     = sqlite3_column_int(stmt, 23);
        game.play_time_secs = sqlite3_column_int(stmt, 24);
        game.genre        = safe_column_text(stmt, 25);
        game.family       = safe_column_text(stmt, 26);
        game.players      = sqlite3_column_int(stmt, 27);
        game.fb_hiscore   = sqlite3_column_int(stmt, 28) != 0;
        game.is_bios      = sqlite3_column_int(stmt, 29) != 0;
        /* Lue ici comme les autres : PAR INDICE. La liste de colonnes
         * ci-dessus est explicite, l'ajouter a la requete sans l'ajouter ici
         * laisserait tous les jeux MAME se presenter comme du FinalBurn Neo. */
        game.emulator     = safe_column_text(stmt, 30);
        if (game.emulator.empty()) game.emulator = "fbneo";

        id_to_index[game_id] = games.size();
        games.push_back(std::move(game));
    }
    sqlite3_finalize(stmt);

    // Step 2: load every ROM in a single query and dispatch to its game.
    //
    // Facultatif, et c'est le point : l'interface n'ouvre jamais game.roms
    // (aucune occurrence dans MainWindow), alors que cette passe recoud plus
    // de 230 000 lignes a chaque rechargement du catalogue. Seuls l'audit et
    // la resolution des sets en ont besoin ; eux appellent getAllGames().
    if (!with_roms) return games;

    // Step 2: load every ROM in a single query and dispatch to its game.
    // Replaces the previous N+1 pattern (one prepare/step/finalize per game).
    // Restreinte au catalogue demande : les 370 000 ROMs de MAME n'ont rien a
    // faire dans un audit FinalBurn Neo, meme ecartees une par une ensuite.
    const char* roms_sql = emulator.empty()
        ? "SELECT game_id, name, size, crc, merge FROM roms;"
        : "SELECT r.game_id, r.name, r.size, r.crc, r.merge FROM roms r "
          "JOIN games g ON g.id = r.game_id WHERE g.emulator = ?;";
    sqlite3_stmt* roms_stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, roms_sql, -1, &roms_stmt, nullptr) == SQLITE_OK) {
        if (!emulator.empty()) sqlite3_bind_text(roms_stmt, 1, emulator.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(roms_stmt) == SQLITE_ROW) {
            int64_t game_id = sqlite3_column_int64(roms_stmt, 0);
            auto it = id_to_index.find(game_id);
            if (it == id_to_index.end()) continue; // orphan row, skip

            Rom rom;
            rom.name = safe_column_text(roms_stmt, 1);
            rom.size = sqlite3_column_int64(roms_stmt, 2);
            rom.crc  = safe_column_text(roms_stmt, 3);
            rom.merge = safe_column_text(roms_stmt, 4);
            games[it->second].roms.push_back(std::move(rom));
        }
        sqlite3_finalize(roms_stmt);
    }

    // Les CHD, pour les sets qui en ont (MAME seulement) : quelques centaines
    // de lignes, lues de la meme facon.
    const char* disks_sql = emulator.empty()
        ? "SELECT game_id, name, sha1, merge FROM disks;"
        : "SELECT d.game_id, d.name, d.sha1, d.merge FROM disks d "
          "JOIN games g ON g.id = d.game_id WHERE g.emulator = ?;";
    sqlite3_stmt* disks_stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, disks_sql, -1, &disks_stmt, nullptr) == SQLITE_OK) {
        if (!emulator.empty()) sqlite3_bind_text(disks_stmt, 1, emulator.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(disks_stmt) == SQLITE_ROW) {
            auto it = id_to_index.find(sqlite3_column_int64(disks_stmt, 0));
            if (it == id_to_index.end()) continue;
            Disk d;
            d.name = safe_column_text(disks_stmt, 1);
            d.sha1 = safe_column_text(disks_stmt, 2);
            d.merge = safe_column_text(disks_stmt, 3);
            games[it->second].disks.push_back(std::move(d));
        }
        sqlite3_finalize(disks_stmt);
    }

    // Les devices des machines MAME, de la meme facon. Ordre d'insertion :
    // celui du DAT.
    const char* devices_sql = emulator.empty()
        ? "SELECT game_id, name FROM devices ORDER BY rowid;"
        : "SELECT d.game_id, d.name FROM devices d "
          "JOIN games g ON g.id = d.game_id WHERE g.emulator = ? ORDER BY d.rowid;";
    sqlite3_stmt* devices_stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, devices_sql, -1, &devices_stmt, nullptr) == SQLITE_OK) {
        if (!emulator.empty()) sqlite3_bind_text(devices_stmt, 1, emulator.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(devices_stmt) == SQLITE_ROW) {
            auto it = id_to_index.find(sqlite3_column_int64(devices_stmt, 0));
            if (it == id_to_index.end()) continue;
            games[it->second].devices.push_back(safe_column_text(devices_stmt, 1));
        }
        sqlite3_finalize(devices_stmt);
    }

    return games;
}

Game DatabaseManager::buildGameFromQuery(sqlite3_stmt* stmt) {
    // Column layout from SELECT * (col 0 = id):
    // 1:name 2:description 3:year 4:manufacturer 5:system 6:status
    // 7:video_type 8:orientation 9:width 10:height 11:aspect_x 12:aspect_y
    // 13:driver_status 14:comment 15:cloneof 16:romof 17:sourcefile
    // 18:snapshot_path 19:dat_source
    // 20:is_favorite 21:last_played 22:play_count 23:play_time_secs  (may be absent on old rows)
    Game game;
    game.name         = safe_column_text(stmt, 1);
    game.description  = safe_column_text(stmt, 2);
    game.year         = safe_column_text(stmt, 3);
    game.manufacturer = safe_column_text(stmt, 4);
    game.system       = safe_column_text(stmt, 5);
    game.status       = safe_column_text(stmt, 6);
    game.video_type   = safe_column_text(stmt, 7);
    game.orientation  = safe_column_text(stmt, 8);
    game.width        = safe_column_text(stmt, 9);
    game.height       = safe_column_text(stmt, 10);
    game.aspect_x     = safe_column_text(stmt, 11);
    game.aspect_y     = safe_column_text(stmt, 12);
    game.driver_status= safe_column_text(stmt, 13);
    game.comment      = safe_column_text(stmt, 14);
    game.cloneof      = safe_column_text(stmt, 15);
    game.romof        = safe_column_text(stmt, 16);
    game.sourcefile   = safe_column_text(stmt, 17);
    game.snapshot_path= safe_column_text(stmt, 18);
    game.dat_source   = safe_column_text(stmt, 19);

    int ncols = sqlite3_column_count(stmt);
    if (ncols > 20) game.is_favorite    = sqlite3_column_int(stmt, 20) != 0;
    if (ncols > 21) game.last_played    = safe_column_text(stmt, 21);
    if (ncols > 22) game.play_count     = sqlite3_column_int(stmt, 22);
    if (ncols > 23) game.play_time_secs = sqlite3_column_int(stmt, 23);

    // These land wherever ALTER TABLE appended them, which depends on how many
    // of the migrated columns a given DB was already missing : so resolve them
    // by name rather than by a hard-coded index.
    //
    // The loop runs to the end. It used to stop at dat_header, which was
    // correct while that was the only column resolved this way; once the
    // session counters were appended after it, that early exit meant they were
    // never read at all, and every session length silently came back as zero.
    for (int c = 24; c < ncols; ++c) {
        const char* cn = sqlite3_column_name(stmt, c);
        if (!cn) continue;
        if (std::strcmp(cn, "dat_header") == 0)
            game.dat_header = safe_column_text(stmt, c);
        else if (std::strcmp(cn, "last_session_secs") == 0)
            game.last_session_secs = sqlite3_column_int(stmt, c);
        else if (std::strcmp(cn, "longest_session_secs") == 0)
            game.longest_session_secs = sqlite3_column_int(stmt, c);
        // Meme raison, meme mecanique : selon l'anciennete de la base, ces
        // colonnes tombent a des indices differents.
        else if (std::strcmp(cn, "genre") == 0)
            game.genre = safe_column_text(stmt, c);
        else if (std::strcmp(cn, "family") == 0)
            game.family = safe_column_text(stmt, c);
        else if (std::strcmp(cn, "players") == 0)
            game.players = sqlite3_column_int(stmt, c);
        else if (std::strcmp(cn, "fb_hiscore") == 0)
            game.fb_hiscore = sqlite3_column_int(stmt, c) != 0;
        else if (std::strcmp(cn, "is_bios") == 0)
            game.is_bios = sqlite3_column_int(stmt, c) != 0;
        // Quel catalogue fournit ce jeu. Par nom comme les precedentes : selon
        // qu'une base est neuve ou migree, elle ne tombe pas au meme indice.
        else if (std::strcmp(cn, "emulator") == 0) {
            std::string e = safe_column_text(stmt, c);
            if (!e.empty()) game.emulator = e;
        }
    }
    return game;
}

std::vector<Game> DatabaseManager::getGamesBySystem(const std::string& system) {
    const char* sql = "SELECT * FROM games WHERE system = ? ORDER BY description;";
    
    sqlite3_stmt* stmt;
    std::vector<Game> games;
    
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return games;
    }
    
    sqlite3_bind_text(stmt, 1, system.c_str(), -1, SQLITE_STATIC);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        games.push_back(buildGameFromQuery(stmt));
    }
    
    sqlite3_finalize(stmt);
    return games;
}

std::vector<Game> DatabaseManager::getGamesByStatus(const std::string& status) {
    // Le catalogue FinalBurn Neo : son seul appelant (reevaluateGamesAvailability)
    // reecrit les statuts par updateGameStatus, qui vise FinalBurn Neo.
    const char* sql = "SELECT * FROM games WHERE status = ? AND emulator = 'fbneo' ORDER BY description;";
    
    sqlite3_stmt* stmt;
    std::vector<Game> games;
    
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return games;
    }
    
    sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_STATIC);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        games.push_back(buildGameFromQuery(stmt));
    }
    
    sqlite3_finalize(stmt);
    return games;
}

Game DatabaseManager::getGame(const std::string& game_name) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    // Cette surcharge ne peut pas prendre d'emulateur : un parametre par
    // defaut de plus rendrait getGame(nom, systeme) ambigu. Elle reste donc
    // sur le catalogue FinalBurn Neo, le seul que ses appelants connaissent ;
    // pour lever le doute il faut passer par getGame(nom, systeme, emulateur).
    const char* sql = "SELECT * FROM games WHERE name = ? AND emulator = 'fbneo';";
    
    sqlite3_stmt* stmt;
    Game game;
    
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return game;
    }
    
    sqlite3_bind_text(stmt, 1, game_name.c_str(), -1, SQLITE_STATIC);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        game = buildGameFromQuery(stmt);
        
        // Load ROMs for this game
        int64_t game_id = sqlite3_column_int64(stmt, 0); // Get game ID from main query
        const char* roms_sql = "SELECT name, size, crc, file_path, merge FROM roms WHERE game_id = ?;";
        sqlite3_stmt* roms_stmt;
        
        if (sqlite3_prepare_v2(m_db, roms_sql, -1, &roms_stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(roms_stmt, 1, game_id);
            
            while (sqlite3_step(roms_stmt) == SQLITE_ROW) {
                Rom rom;
                rom.name = safe_column_text(roms_stmt, 0);
                rom.size = sqlite3_column_int64(roms_stmt, 1);
                rom.crc = safe_column_text(roms_stmt, 2);
                rom.merge = safe_column_text(roms_stmt, 4);
                game.roms.push_back(rom);
            }
            
            sqlite3_finalize(roms_stmt);
        }
    }
    
    sqlite3_finalize(stmt);
    return game;
}

Game DatabaseManager::getPlayerStats(const std::string& game_name, const std::string& system,
                                     const std::string& emulator) {
    if (emulator.empty() || emulator == "fbneo") return getGame(game_name, system, "fbneo");
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    Game g;
    g.name = game_name;
    g.system = system;
    g.emulator = emulator;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db,
            "SELECT is_favorite, last_played, play_count, play_time_secs,"
            " last_session_secs, longest_session_secs FROM player_stats"
            " WHERE emulator = ? AND name = ? AND system = ?;", -1, &st, nullptr) != SQLITE_OK)
        return g;
    sqlite3_bind_text(st, 1, emulator.c_str(),  -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, game_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, system.c_str(),    -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        g.is_favorite          = sqlite3_column_int(st, 0) != 0;
        g.last_played          = safe_column_text(st, 1);
        g.play_count           = sqlite3_column_int(st, 2);
        g.play_time_secs       = sqlite3_column_int(st, 3);
        g.last_session_secs    = sqlite3_column_int(st, 4);
        g.longest_session_secs = sqlite3_column_int(st, 5);
    }
    sqlite3_finalize(st);
    return g;
}

Game DatabaseManager::getGame(const std::string& game_name, const std::string& system,
                             const std::string& emulator) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    // (name, system) ne designe plus une ligne unique : `mslug` / 'Arcade'
    // existe chez FinalBurn Neo comme chez MAME.
    const char* sql = "SELECT * FROM games WHERE name = ? AND system = ? AND emulator = ?;";
    
    sqlite3_stmt* stmt;
    Game game;
    
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return game;
    }
    
    sqlite3_bind_text(stmt, 1, game_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, system.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, emulator.c_str(), -1, SQLITE_STATIC);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        game = buildGameFromQuery(stmt);
        
        // Load ROMs for this game
        int64_t game_id = sqlite3_column_int64(stmt, 0); // Get game ID from main query
        const char* roms_sql = "SELECT name, size, crc, file_path, merge FROM roms WHERE game_id = ?;";
        sqlite3_stmt* roms_stmt;
        
        if (sqlite3_prepare_v2(m_db, roms_sql, -1, &roms_stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(roms_stmt, 1, game_id);
            
            while (sqlite3_step(roms_stmt) == SQLITE_ROW) {
                Rom rom;
                rom.name = safe_column_text(roms_stmt, 0);
                rom.size = sqlite3_column_int64(roms_stmt, 1);
                rom.crc = safe_column_text(roms_stmt, 2);
                rom.merge = safe_column_text(roms_stmt, 4);
                game.roms.push_back(rom);
            }
            
            sqlite3_finalize(roms_stmt);
        }
    }
    
    sqlite3_finalize(stmt);
    return game;
}

std::vector<Game> DatabaseManager::getAllGamesWithName(const std::string& game_name,
                                                      const std::string& emulator) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::vector<Game> games;
    // Cette fonction sert a lever l'ambiguite d'un nom de set porte par
    // plusieurs systemes. Depuis MAME, le meme nom existe aussi d'un
    // emulateur a l'autre : un emulateur vide rend les deux catalogues,
    // sinon la reponse s'y limite.
    const char* sql = "SELECT id, name, description, year, manufacturer, system, cloneof, romof, sourcefile, comment, video_type, orientation, width, height, aspect_x, aspect_y, driver_status, status, snapshot_path, dat_source, dat_header, emulator FROM games WHERE name = ? AND (? = '' OR emulator = ?);";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return games;
    }
    
    sqlite3_bind_text(stmt, 1, game_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, emulator.c_str(),  -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, emulator.c_str(),  -1, SQLITE_STATIC);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Game game;
        int64_t game_id = sqlite3_column_int64(stmt, 0);
        game.name = safe_column_text(stmt, 1);
        game.description = safe_column_text(stmt, 2);
        game.year = safe_column_text(stmt, 3);
        game.manufacturer = safe_column_text(stmt, 4);
        game.system = safe_column_text(stmt, 5);
        game.cloneof = safe_column_text(stmt, 6);
        game.romof = safe_column_text(stmt, 7);
        game.sourcefile = safe_column_text(stmt, 8);
        game.comment = safe_column_text(stmt, 9);
        game.video_type = safe_column_text(stmt, 10);
        game.orientation = safe_column_text(stmt, 11);
        game.width = safe_column_text(stmt, 12);
        game.height = safe_column_text(stmt, 13);
        game.aspect_x = safe_column_text(stmt, 14);
        game.aspect_y = safe_column_text(stmt, 15);
        game.driver_status = safe_column_text(stmt, 16);
        game.status = safe_column_text(stmt, 17);
        game.snapshot_path = safe_column_text(stmt, 18);
        game.dat_source = safe_column_text(stmt, 19);
        game.dat_header = safe_column_text(stmt, 20);
        // Lue PAR INDICE, comme tout ce bloc : la liste de colonnes est
        // explicite juste au-dessus.
        game.emulator = safe_column_text(stmt, 21);
        if (game.emulator.empty()) game.emulator = "fbneo";

        // Load ROMs for this game
        const char* roms_sql = "SELECT name, size, crc, file_path, merge FROM roms WHERE game_id = ?;";
        sqlite3_stmt* roms_stmt;
        if (sqlite3_prepare_v2(m_db, roms_sql, -1, &roms_stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(roms_stmt, 1, game_id);
            while (sqlite3_step(roms_stmt) == SQLITE_ROW) {
                Rom rom;
                rom.name = safe_column_text(roms_stmt, 0);
                rom.size = sqlite3_column_int64(roms_stmt, 1);
                rom.crc = safe_column_text(roms_stmt, 2);
                rom.merge = safe_column_text(roms_stmt, 4);
                game.roms.push_back(rom);
            }
            
            sqlite3_finalize(roms_stmt);
        }
        
        games.push_back(game);
    }
    
    sqlite3_finalize(stmt);
    return games;
}

bool DatabaseManager::clearGames(const std::string& emulator) {
    if (emulator.empty()) return clearAllData();
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    // Children first : foreign_keys is ON, and disks has no key of its own
    // to cascade from.
    const char* sqls[] = {
        "DELETE FROM disks WHERE game_id IN (SELECT id FROM games WHERE emulator = ?);",
        "DELETE FROM devices WHERE game_id IN (SELECT id FROM games WHERE emulator = ?);",
        "DELETE FROM roms WHERE game_id IN (SELECT id FROM games WHERE emulator = ?);",
        "DELETE FROM games WHERE emulator = ?;",
    };
    bool own_tx = beginTransaction();
    for (const char* sql : sqls) {
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            std::cerr << "[ERROR] clearGames: " << sqlite3_errmsg(m_db) << std::endl;
            if (own_tx) rollbackTransaction();
            return false;
        }
        sqlite3_bind_text(stmt, 1, emulator.c_str(), -1, SQLITE_TRANSIENT);
        const int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            std::cerr << "[ERROR] clearGames: " << sqlite3_errmsg(m_db) << std::endl;
            if (own_tx) rollbackTransaction();
            return false;
        }
    }
    if (own_tx) commitTransaction();
    return true;
}

bool DatabaseManager::clearAllData() {
    const char* sql = "DELETE FROM disks; DELETE FROM devices; DELETE FROM roms; DELETE FROM games;";
    char* err_msg = nullptr;
    
    int rc = sqlite3_exec(m_db, sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "Erreur suppression données: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    
    return true;
}

bool DatabaseManager::gameExists(const std::string& game_name) {
    const char* sql = "SELECT COUNT(*) FROM games WHERE name = ?;";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, game_name.c_str(), -1, SQLITE_STATIC);

    bool exists = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        exists = sqlite3_column_int(stmt, 0) > 0;
    }

    sqlite3_finalize(stmt);
    return exists;
}

size_t DatabaseManager::getGameCount(const std::string& emulator) {
    const char* sql = "SELECT COUNT(*) FROM games WHERE (? = '' OR emulator = ?);";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(stmt, 1, emulator.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, emulator.c_str(), -1, SQLITE_TRANSIENT);

    size_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return count;
}

bool DatabaseManager::beginTransaction() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    char* err_msg = nullptr;
    int rc = sqlite3_exec(m_db, "BEGIN TRANSACTION;", 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "Erreur début transaction: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

bool DatabaseManager::commitTransaction() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    char* err_msg = nullptr;
    int rc = sqlite3_exec(m_db, "COMMIT;", 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "Erreur commit transaction: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

bool DatabaseManager::rollbackTransaction() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    char* err_msg = nullptr;
    int rc = sqlite3_exec(m_db, "ROLLBACK;", 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "Erreur rollback transaction: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

bool DatabaseManager::createSavepoint(const std::string& name) {
    std::string sql = "SAVEPOINT " + name + ";";
    char* err_msg = nullptr;
    int rc = sqlite3_exec(m_db, sql.c_str(), 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "[ERROR] Failed to create savepoint " << name << ": " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

bool DatabaseManager::releaseSavepoint(const std::string& name) {
    std::string sql = "RELEASE SAVEPOINT " + name + ";";
    char* err_msg = nullptr;
    int rc = sqlite3_exec(m_db, sql.c_str(), 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "[ERROR] Failed to release savepoint " << name << ": " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

bool DatabaseManager::rollbackToSavepoint(const std::string& name) {
    std::string sql = "ROLLBACK TO SAVEPOINT " + name + ";";
    char* err_msg = nullptr;
    int rc = sqlite3_exec(m_db, sql.c_str(), 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "[ERROR] Failed to rollback to savepoint " << name << ": " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

size_t DatabaseManager::getGameCountByStatus(const std::string& status, const std::string& emulator) {
    const char* sql = "SELECT COUNT(*) FROM games WHERE status = ? AND (? = '' OR emulator = ?);";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }

    sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, emulator.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, emulator.c_str(), -1, SQLITE_TRANSIENT);

    size_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return count;
}

// ── Favourites ───────────────────────────────────────────────────────────────

// Seul FinalBurn Neo a son catalogue dans `games` : pour les autres
// emulateurs, player_stats est la seule table qui porte le favori, et la ligne
// peut ne pas exister encore.
bool DatabaseManager::toggleFavorite(const std::string& game_name, const std::string& system,
                                     const std::string& emulator) {
    if (emulator != "fbneo") {
        const char* sql =
            "INSERT INTO player_stats(emulator, name, system, is_favorite) VALUES(?, ?, ?, 1) "
            "ON CONFLICT(emulator, name, system) DO UPDATE SET "
            "is_favorite = CASE WHEN is_favorite = 0 THEN 1 ELSE 0 END;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(stmt, 1, emulator.c_str(),  -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, game_name.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, system.c_str(),    -1, SQLITE_STATIC);
        bool ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        return ok;
    }
    // `games` porte les deux catalogues : sans l'emulateur, mettre `mslug`
    // FBNeo en favori basculerait aussi le `mslug` de MAME.
    const char* sql =
        "UPDATE games SET is_favorite = CASE WHEN is_favorite = 0 THEN 1 ELSE 0 END "
        "WHERE name = ? AND system = ? AND emulator = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, game_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, system.c_str(),    -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, emulator.c_str(),  -1, SQLITE_STATIC);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool DatabaseManager::isFavorite(const std::string& game_name, const std::string& system,
                                 const std::string& emulator) {
    if (emulator != "fbneo") {
        const char* sql = "SELECT is_favorite FROM player_stats "
                          "WHERE emulator = ? AND name = ? AND system = ?;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(stmt, 1, emulator.c_str(),  -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, game_name.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, system.c_str(),    -1, SQLITE_STATIC);
        bool fav = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) != 0;
        sqlite3_finalize(stmt);
        return fav;
    }
    const char* sql = "SELECT is_favorite FROM games WHERE name = ? AND system = ? AND emulator = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, game_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, system.c_str(),    -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, emulator.c_str(),  -1, SQLITE_STATIC);
    bool fav = false;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        fav = sqlite3_column_int(stmt, 0) != 0;
    sqlite3_finalize(stmt);
    return fav;
}

bool DatabaseManager::ignoreSet(const std::string& game_name, const std::string& system,
                                const std::string& note, const std::string& emulator) {
    const char* sql = "INSERT OR REPLACE INTO ignored_sets (emulator, name, system, note, added_at) "
                      "VALUES (?, ?, ?, ?, strftime('%Y-%m-%dT%H:%M:%SZ', 'now'));";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, emulator.c_str(),  -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, game_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, system.c_str(),    -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, note.c_str(),      -1, SQLITE_STATIC);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool DatabaseManager::unignoreSet(const std::string& game_name, const std::string& system,
                                  const std::string& emulator) {
    const char* sql = "DELETE FROM ignored_sets WHERE emulator = ? AND name = ? AND system = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, emulator.c_str(),  -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, game_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, system.c_str(),    -1, SQLITE_STATIC);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool DatabaseManager::isIgnored(const std::string& game_name, const std::string& system,
                                const std::string& emulator) {
    const char* sql = "SELECT 1 FROM ignored_sets WHERE emulator = ? AND name = ? AND system = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, emulator.c_str(),  -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, game_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, system.c_str(),    -1, SQLITE_STATIC);
    bool found = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return found;
}

// Un emulateur vide rend la liste des deux catalogues : un ecran qui affiche
// tout ce que le joueur a mis de cote n'a pas a les separer.
std::vector<DatabaseManager::IgnoredSet> DatabaseManager::getIgnoredSets(const std::string& emulator) {
    std::vector<IgnoredSet> out;
    const char* sql = emulator.empty()
        ? "SELECT name, system, note, added_at, emulator FROM ignored_sets ORDER BY system, name;"
        : "SELECT name, system, note, added_at, emulator FROM ignored_sets"
          " WHERE emulator = ? ORDER BY system, name;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    if (!emulator.empty()) sqlite3_bind_text(stmt, 1, emulator.c_str(), -1, SQLITE_STATIC);
    while (sqlite3_step(stmt) == SQLITE_ROW)
        out.push_back({safe_column_text(stmt, 0), safe_column_text(stmt, 1),
                       safe_column_text(stmt, 2), safe_column_text(stmt, 3),
                       safe_column_text(stmt, 4)});
    sqlite3_finalize(stmt);
    return out;
}

std::vector<Game> DatabaseManager::getFavorites() {
    // FinalBurn Neo : les favoris MAME ne vivent que dans player_stats.
    const char* sql = "SELECT * FROM games WHERE is_favorite = 1 AND emulator = 'fbneo' ORDER BY name;";
    sqlite3_stmt* stmt;
    std::vector<Game> result;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    while (sqlite3_step(stmt) == SQLITE_ROW)
        result.push_back(buildGameFromQuery(stmt));
    sqlite3_finalize(stmt);
    return result;
}

// ── Play tracking ─────────────────────────────────────────────────────────────

bool DatabaseManager::recordLaunch(const std::string& game_name, const std::string& system,
                                   const std::string& emulator) {
    // Get ISO-8601 timestamp
    time_t now = time(nullptr);
    char ts[32];
    struct tm* t = localtime(&now);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", t);

    if (emulator != "fbneo") {
        const char* sql =
            "INSERT INTO player_stats(emulator, name, system, last_played, play_count)"
            " VALUES(?, ?, ?, ?, 1)"
            " ON CONFLICT(emulator, name, system) DO UPDATE SET"
            " last_played = excluded.last_played, play_count = play_count + 1;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(stmt, 1, emulator.c_str(),  -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, game_name.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, system.c_str(),    -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, ts,                -1, SQLITE_TRANSIENT);
        bool ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        return ok;
    }

    // Idem : le compteur de lancements appartient a un seul des deux `mslug`.
    const char* sql =
        "UPDATE games SET last_played = ?, play_count = play_count + 1 "
        "WHERE name = ? AND system = ? AND emulator = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, ts,             -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, game_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, system.c_str(),    -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, emulator.c_str(),  -1, SQLITE_STATIC);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool DatabaseManager::addPlayTime(const std::string& game_name, const std::string& system,
                                  int seconds, const std::string& emulator) {
    // The three figures are written in one statement so they can never drift
    // apart: a longest session that no cumulative total accounts for would be
    // impossible to explain to the player.
    if (emulator != "fbneo") {
        const char* sql =
            "INSERT INTO player_stats(emulator, name, system, play_time_secs,"
            " last_session_secs, longest_session_secs) VALUES(?, ?, ?, ?, ?, ?)"
            " ON CONFLICT(emulator, name, system) DO UPDATE SET"
            " play_time_secs = play_time_secs + excluded.play_time_secs,"
            " last_session_secs = excluded.last_session_secs,"
            " longest_session_secs = MAX(longest_session_secs, excluded.longest_session_secs);";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(stmt, 1, emulator.c_str(),  -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, game_name.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, system.c_str(),    -1, SQLITE_STATIC);
        sqlite3_bind_int (stmt, 4, seconds);
        sqlite3_bind_int (stmt, 5, seconds);
        sqlite3_bind_int (stmt, 6, seconds);
        bool ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        return ok;
    }

    const char* sql =
        "UPDATE games SET play_time_secs = play_time_secs + ?,"
        " last_session_secs = ?,"
        " longest_session_secs = MAX(COALESCE(longest_session_secs, 0), ?)"
        " WHERE name = ? AND system = ? AND emulator = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int (stmt, 1, seconds);
    sqlite3_bind_int (stmt, 2, seconds);
    sqlite3_bind_int (stmt, 3, seconds);
    sqlite3_bind_text(stmt, 4, game_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, system.c_str(),    -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, emulator.c_str(),  -1, SQLITE_STATIC);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<Game> DatabaseManager::getRecentlyPlayed(int limit) {
    const char* sql =
        "SELECT * FROM games WHERE last_played IS NOT NULL AND emulator = 'fbneo' "
        "ORDER BY last_played DESC LIMIT ?;";
    sqlite3_stmt* stmt;
    std::vector<Game> result;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    sqlite3_bind_int(stmt, 1, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW)
        result.push_back(buildGameFromQuery(stmt));
    sqlite3_finalize(stmt);
    return result;
}

bool DatabaseManager::registerDatFile(const std::string& filename, const std::string& filepath, time_t last_modified, size_t file_size, int games_count) {
    const char* sql = "INSERT OR REPLACE INTO dat_files (filename, filepath, last_modified, file_size, games_count) VALUES (?, ?, ?, ?, ?);";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, filename.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, filepath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, last_modified);
    sqlite3_bind_int64(stmt, 4, file_size);
    sqlite3_bind_int(stmt, 5, games_count);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return rc == SQLITE_DONE;
}

bool DatabaseManager::isDatFileUpToDate(const std::string& filename, time_t last_modified, size_t file_size) {
    const char* sql = "SELECT last_modified, file_size FROM dat_files WHERE filename = ?;";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, filename.c_str(), -1, SQLITE_STATIC);
    
    bool up_to_date = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        time_t stored_modified = sqlite3_column_int64(stmt, 0);
        size_t stored_size = sqlite3_column_int64(stmt, 1);
        up_to_date = (stored_modified == last_modified && stored_size == file_size);
    }
    
    sqlite3_finalize(stmt);
    return up_to_date;
}

std::vector<std::string> DatabaseManager::getOutdatedDatFiles(const std::string& dat_directory) {
    std::vector<std::string> outdated_files;
    
    if (!std::filesystem::exists(dat_directory)) {
        return outdated_files;
    }
    
    for (const auto& entry : std::filesystem::directory_iterator(dat_directory)) {
        if (entry.is_regular_file() && DatParser::datKind(entry.path().string()) != DatParser::DatKind::None) {
            std::string filename = entry.path().filename().string();
            auto ftime = std::filesystem::last_write_time(entry);
            time_t last_modified = std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch()).count();
            size_t file_size = std::filesystem::file_size(entry);
            
            if (!isDatFileUpToDate(filename, last_modified, file_size)) {
                outdated_files.push_back(filename);
            }
        }
    }
    
    return outdated_files;
}

bool DatabaseManager::removeGamesFromDat(const std::string& filename) {
    // Delete the child ROM rows BEFORE the parent games: foreign_keys is ON, so
    // deleting games first would violate roms.game_id -> games.id and fail.
    // (roms is keyed by game_id, not game_name : the old game_name predicate also
    // referenced a non-existent column.)
    const char* del_roms  = "DELETE FROM roms WHERE game_id IN (SELECT id FROM games WHERE dat_source = ?);";
    const char* del_games = "DELETE FROM games WHERE dat_source = ?;";

    sqlite3_stmt* stmt = nullptr;

    for (const char* del_children : {
             "DELETE FROM disks WHERE game_id IN (SELECT id FROM games WHERE dat_source = ?);",
             "DELETE FROM devices WHERE game_id IN (SELECT id FROM games WHERE dat_source = ?);"}) {
        if (sqlite3_prepare_v2(m_db, del_children, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, filename.c_str(), -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            stmt = nullptr;
        }
    }

    if (sqlite3_prepare_v2(m_db, del_roms, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Erreur préparation removeGamesFromDat (roms): " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }
    sqlite3_bind_text(stmt, 1, filename.c_str(), -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (sqlite3_prepare_v2(m_db, del_games, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Erreur préparation removeGamesFromDat (games): " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }
    sqlite3_bind_text(stmt, 1, filename.c_str(), -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(m_db);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_DONE) {
        std::cout << "[INFO] Removed " << changes << " games from DAT: " << filename << std::endl;
        return true;
    }
    return false;
}

bool DatabaseManager::removeUnreferencedDatFiles(const std::vector<std::string>& existing_files) {
    // First, get list of DAT files that will be removed
    std::vector<std::string> removed_dat_files;
    
    if (existing_files.empty()) {
        // All DAT files will be removed - get all current ones
        const char* get_all_sql = "SELECT filename FROM dat_files;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(m_db, get_all_sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                removed_dat_files.push_back(safe_column_text(stmt, 0));
            }
            sqlite3_finalize(stmt);
        }
    } else {
        // Get DAT files not in existing_files list
        std::string query = "SELECT filename FROM dat_files WHERE filename NOT IN (";
        for (size_t i = 0; i < existing_files.size(); ++i) {
            if (i > 0) query += ",";
            query += "?";
        }
        query += ");";
        
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            for (size_t i = 0; i < existing_files.size(); ++i) {
                sqlite3_bind_text(stmt, i + 1, existing_files[i].c_str(), -1, SQLITE_STATIC);
            }
            
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                removed_dat_files.push_back(safe_column_text(stmt, 0));
            }
            sqlite3_finalize(stmt);
        }
    }
    
    // Remove games from the DAT files that will be deleted
    for (const auto& dat_filename : removed_dat_files) {
        std::cout << "[SYNC] Suppression des jeux du DAT supprimé: " << dat_filename << std::endl;
        removeGamesFromDat(dat_filename);
    }
    
    // Remove DAT files from database that no longer exist on disk
    const char* sql = "DELETE FROM dat_files WHERE filename NOT IN (";
    
    if (existing_files.empty()) {
        const char* delete_all = "DELETE FROM dat_files;";
        return sqlite3_exec(m_db, delete_all, 0, 0, nullptr) == SQLITE_OK;
    }
    
    std::string query = sql;
    for (size_t i = 0; i < existing_files.size(); ++i) {
        if (i > 0) query += ",";
        query += "?";
    }
    query += ");";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    
    for (size_t i = 0; i < existing_files.size(); ++i) {
        sqlite3_bind_text(stmt, i + 1, existing_files[i].c_str(), -1, SQLITE_STATIC);
    }

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

// ==================== INCREMENTAL DAT UPDATE ("DIFF") ====================

// Separator byte used to pack composite keys/values. 0x1F (US) never appears in
// game names, systems, ROM names or hex CRCs, so it is a safe delimiter.
static const char kSep = '\x1f';

int DatabaseManager::protectedPlayerStats() {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, "SELECT COUNT(*) FROM player_stats;", -1, &stmt, nullptr)
            != SQLITE_OK)
        return 0;
    int n = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : 0;
    sqlite3_finalize(stmt);
    return n;
}

// Total volontairement tous emulateurs confondus : l'ecran des statistiques
// parle de la bibliotheque du joueur, pas d'un catalogue en particulier.
int DatabaseManager::countFavorites() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db,
            "SELECT COUNT(*) FROM player_stats WHERE is_favorite = 1;", -1, &stmt, nullptr)
            != SQLITE_OK)
        return 0;
    int n = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : 0;
    sqlite3_finalize(stmt);
    return n;
}

// Un jeu « joue » est un jeu lance au moins une fois, pas un jeu dont la ligne
// existe : une ligne peut n'avoir ete creee que par une mise en favori.
int DatabaseManager::countPlayedGames() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db,
            "SELECT COUNT(*) FROM player_stats WHERE play_count > 0;", -1, &stmt, nullptr)
            != SQLITE_OK)
        return 0;
    int n = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : 0;
    sqlite3_finalize(stmt);
    return n;
}

std::unordered_map<std::string, std::string> DatabaseManager::snapshotStatusSignatures(const std::string& emulator) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::unordered_map<std::string, std::string> out;

    // One pass over games joined with their ROMs, ordered so each game's ROM rows
    // are contiguous and deterministically ordered. The signature is the ordered
    // concatenation of "name:size:crc|" over the game's ROMs.
    // La cle porte l'emulateur : `mslug` FBNeo et `mslug` MAME sont deux jeux
    // differents, avec deux definitions de ROMs differentes. Confondus, le
    // statut de l'un serait restaure sur l'autre a chaque mise a jour de DAT.
    const char* sql =
        "SELECT g.name, g.system, g.status, r.name, r.size, r.crc, g.emulator "
        "FROM games g LEFT JOIN roms r ON r.game_id = g.id "
        "WHERE (? = '' OR g.emulator = ?) "
        "ORDER BY g.emulator, g.name, g.system, r.name, r.size, r.crc;";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[ERROR] snapshotStatusSignatures prepare failed: " << sqlite3_errmsg(m_db) << std::endl;
        return out;
    }
    sqlite3_bind_text(stmt, 1, emulator.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, emulator.c_str(), -1, SQLITE_TRANSIENT);

    std::string cur_key, cur_status, cur_sig;
    bool have_game = false;

    auto flush = [&]() {
        if (have_game) out[cur_key] = cur_status + kSep + cur_sig;
    };

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string gname = safe_column_text(stmt, 0);
        std::string gsys  = safe_column_text(stmt, 1);
        std::string gemu  = safe_column_text(stmt, 6);
        if (gemu.empty()) gemu = "fbneo";
        // "name\x1fsystem\x1femulator" : trois parties depuis MAME.
        std::string key = gname + kSep + gsys + kSep + gemu;

        if (!have_game || key != cur_key) {
            flush();
            cur_key = key;
            cur_status = safe_column_text(stmt, 2);
            cur_sig.clear();
            have_game = true;
        }

        // r.name is NULL for games with no ROMs (LEFT JOIN produced a single row).
        if (sqlite3_column_type(stmt, 3) != SQLITE_NULL) {
            cur_sig += safe_column_text(stmt, 3);
            cur_sig += ':';
            cur_sig += std::to_string(sqlite3_column_int64(stmt, 4));
            cur_sig += ':';
            cur_sig += safe_column_text(stmt, 5);
            cur_sig += '|';
        }
    }
    flush();

    sqlite3_finalize(stmt);

    // The devices a machine needs are part of its definition : a set that
    // gains one is judged again.
    const char* dev_sql =
        "SELECT g.name, g.system, g.emulator, d.name FROM devices d "
        "JOIN games g ON g.id = d.game_id WHERE (? = '' OR g.emulator = ?) ORDER BY d.game_id, d.name;";
    if (sqlite3_prepare_v2(m_db, dev_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, emulator.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, emulator.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string gemu = safe_column_text(stmt, 2);
            if (gemu.empty()) gemu = "fbneo";
            auto it = out.find(safe_column_text(stmt, 0) + kSep + safe_column_text(stmt, 1) + kSep + gemu);
            if (it != out.end()) it->second += "dev:" + safe_column_text(stmt, 3) + '|';
        }
        sqlite3_finalize(stmt);
    }
    return out;
}

int DatabaseManager::applyPreservedStatuses(
        const std::unordered_map<std::string, std::string>& old_snapshot,
        std::vector<std::string>& changed_zip_names,
        const std::string& emulator,
        std::vector<std::string>* changed_keys) {
    // Signatures of the freshly reloaded games (statuses are all 'missing' here).
    // Only the emulator that was reloaded : the other one was not touched and
    // still holds its own statuses.
    std::unordered_map<std::string, std::string> current = snapshotStatusSignatures(emulator);

    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    int restored = 0;
    bool own_tx = beginTransaction();

    for (const auto& [key, cur_val] : current) {
        size_t cur_sep = cur_val.find(kSep);
        std::string cur_sig = (cur_sep == std::string::npos) ? std::string() : cur_val.substr(cur_sep + 1);

        // Decoupage symetrique de la cle a trois parties construite par
        // snapshotStatusSignatures().
        size_t key_sep = key.find(kSep);
        std::string gname = (key_sep == std::string::npos) ? key : key.substr(0, key_sep);
        std::string rest  = (key_sep == std::string::npos) ? std::string() : key.substr(key_sep + 1);
        size_t sys_sep = rest.find(kSep);
        std::string gsys = (sys_sep == std::string::npos) ? rest : rest.substr(0, sys_sep);
        std::string gemu = (sys_sep == std::string::npos) ? std::string("fbneo") : rest.substr(sys_sep + 1);
        if (gemu.empty()) gemu = "fbneo";

        bool preserved = false;
        auto it = old_snapshot.find(key);
        if (it != old_snapshot.end()) {
            size_t old_sep = it->second.find(kSep);
            std::string old_status = (old_sep == std::string::npos) ? it->second : it->second.substr(0, old_sep);
            std::string old_sig    = (old_sep == std::string::npos) ? std::string() : it->second.substr(old_sep + 1);
            if (old_sig == cur_sig) {
                // Identical ROM definition => the previously computed status is still valid.
                if (old_status != "missing") {
                    updateGameStatus(gname, old_status, gsys, gemu);
                    ++restored;
                }
                preserved = true;
            }
        }

        // New game, or ROM definition changed: force its ZIP to be re-checked.
        // FinalBurn Neo only : rom_cache is the per-file bookkeeping of ITS
        // scan. A MAME set's status is re-derived from zip_contents alone
        // (RomResolve::resolve_all_from_cache), and invalidating "mslug.zip"
        // for it would make the next FinalBurn Neo scan re-read a file that
        // did not change : for a first MAME import, 28 000 of them.
        if (!preserved && gemu == "fbneo") {
            changed_zip_names.push_back(gname + ".zip");
        }
        if (!preserved && changed_keys) changed_keys->push_back(key);
    }

    if (own_tx) commitTransaction();
    return restored;
}

bool DatabaseManager::invalidateRomCacheForFiles(const std::vector<std::string>& zip_filenames) {
    if (zip_filenames.empty()) return true;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    const char* sql = "DELETE FROM rom_cache WHERE filename = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[ERROR] invalidateRomCacheForFiles prepare failed: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    bool own_tx = beginTransaction();
    for (const auto& fn : zip_filenames) {
        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, fn.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    if (own_tx) commitTransaction();
    return true;
}

bool DatabaseManager::storeZipContents(const std::string& filepath,
        const std::vector<std::pair<std::string, unsigned long>>& entries) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    // Store under the same canonical/sanitized path used by registerRomFile so the
    // two caches agree on the key.
    std::string store_path = filepath;
    try { store_path = std::filesystem::canonical(filepath).string(); } catch (...) {}
    store_path = sanitize_path(store_path);

    // Drop any previous contents for this file (they may have changed on disk).
    {
        sqlite3_stmt* del;
        if (sqlite3_prepare_v2(m_db, "DELETE FROM zip_contents WHERE filepath = ?;", -1, &del, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(del, 1, store_path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(del);
            sqlite3_finalize(del);
        }
    }

    const char* sql = "INSERT OR REPLACE INTO zip_contents (filepath, entry_name, crc) VALUES (?, ?, ?);";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[ERROR] storeZipContents prepare failed: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }
    for (const auto& e : entries) {
        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, store_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, e.first.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, (sqlite3_int64)e.second);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    return true;
}

bool DatabaseManager::stampZipContents(const std::string& filepath, long long file_size, long long last_modified) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::string store_path = filepath;
    try { store_path = std::filesystem::canonical(filepath).string(); } catch (...) {}
    store_path = sanitize_path(store_path);
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db,
            "INSERT OR REPLACE INTO zip_contents_stamp (filepath, file_size, last_modified) VALUES (?, ?, ?);",
            -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, store_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)file_size);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)last_modified);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::unordered_map<std::string, std::pair<long long, long long>> DatabaseManager::getZipContentStamps() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::unordered_map<std::string, std::pair<long long, long long>> out;
    // Every archive zip_contents describes, with the freshest evidence of the
    // file it was read from : our own stamp, else the FinalBurn Neo scan's
    // rom_cache row, written in the same transaction as the contents.
    const char* sql =
        "SELECT z.filepath,"
        " COALESCE(s.file_size, c.file_size), COALESCE(s.last_modified, c.last_modified)"
        " FROM (SELECT DISTINCT filepath FROM zip_contents) z"
        " LEFT JOIN zip_contents_stamp s ON s.filepath = z.filepath"
        " LEFT JOIN rom_cache c ON c.filepath = z.filepath;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (sqlite3_column_type(stmt, 1) == SQLITE_NULL) continue;   // contents with no provenance : re-read
        out[safe_column_text(stmt, 0)] = { sqlite3_column_int64(stmt, 1), sqlite3_column_int64(stmt, 2) };
    }
    sqlite3_finalize(stmt);
    return out;
}

bool DatabaseManager::getAllZipContents(std::vector<ZipContentRow>& out) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    // Ordered by filepath so callers can group a file's entries in a single pass.
    const char* sql = "SELECT filepath, entry_name, crc FROM zip_contents ORDER BY filepath;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ZipContentRow r;
        r.filepath   = safe_column_text(stmt, 0);
        r.entry_name = safe_column_text(stmt, 1);
        r.crc        = (unsigned long)sqlite3_column_int64(stmt, 2);
        out.push_back(std::move(r));
    }
    sqlite3_finalize(stmt);
    return true;
}

std::vector<std::string> DatabaseManager::getZipContentPaths() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::vector<std::string> out;
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, "SELECT DISTINCT filepath FROM zip_contents;", -1, &stmt, nullptr) != SQLITE_OK)
        return out;
    while (sqlite3_step(stmt) == SQLITE_ROW) out.push_back(safe_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    return out;
}

int DatabaseManager::forgetZipContents(const std::vector<std::string>& filepaths) {
    if (filepaths.empty()) return 0;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    sqlite3_stmt* del_rows;
    sqlite3_stmt* del_stamp;
    if (sqlite3_prepare_v2(m_db, "DELETE FROM zip_contents WHERE filepath = ?;", -1, &del_rows, nullptr) != SQLITE_OK)
        return 0;
    if (sqlite3_prepare_v2(m_db, "DELETE FROM zip_contents_stamp WHERE filepath = ?;", -1, &del_stamp, nullptr) != SQLITE_OK) {
        sqlite3_finalize(del_rows);
        return 0;
    }
    bool own_tx = beginTransaction();
    int forgotten = 0;
    for (const auto& path : filepaths) {
        for (sqlite3_stmt* st : {del_rows, del_stamp}) {
            sqlite3_reset(st);
            sqlite3_bind_text(st, 1, path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(st);
        }
        ++forgotten;
    }
    if (own_tx) commitTransaction();
    sqlite3_finalize(del_rows);
    sqlite3_finalize(del_stamp);
    return forgotten;
}

// ==================== ROM CACHE MANAGEMENT ====================

bool DatabaseManager::registerRomFile(const std::string& filename, const std::string& filepath, time_t last_modified, size_t file_size, time_t dat_timestamp) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    // Prepare once per call : use registerRomFilesBulk for high-volume insertion.
    const char* sql = "INSERT OR REPLACE INTO rom_cache "
                      "(filename, filepath, last_modified, file_size, last_scan_time, dat_timestamp, file_crc) "
                      "VALUES (?, ?, ?, ?, ?, ?, 0);";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[ERROR] registerRomFile prepare failed: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    // Store canonical path to avoid mismatch between runs (symlinks/relative paths).
    // The file was just scanned so it exists : skip the exists() check.
    std::string store_path = filepath;
    try { store_path = std::filesystem::canonical(filepath).string(); } catch (...) {}
    store_path = sanitize_path(store_path);

    time_t now = std::time(nullptr);
    sqlite3_bind_text(stmt, 1, filename.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, store_path.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, last_modified);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)file_size);
    sqlite3_bind_int64(stmt, 5, now);
    sqlite3_bind_int64(stmt, 6, dat_timestamp);
    // file_crc is 0: we rely on (size + mtime) for cache validity.
    // Whole-file CRC is only computed on demand in isRomFileCached() as a fallback.

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        std::cerr << "[ERROR] registerRomFile insert failed for " << filename << ": " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    return true;
}

bool DatabaseManager::isRomFileCached(const std::string& filepath, time_t last_modified, size_t file_size, time_t current_dat_timestamp) {
    const char* sql = "SELECT last_modified, file_size, dat_timestamp, file_crc FROM rom_cache WHERE filepath = ? ORDER BY id DESC LIMIT 1;";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    bool is_cached = isRomFileCachedStmt(stmt, filepath, last_modified, file_size, current_dat_timestamp);
    sqlite3_finalize(stmt);
    return is_cached;
}

bool DatabaseManager::isRomFileCachedStmt(sqlite3_stmt* stmt, const std::string& filepath, time_t last_modified, size_t file_size, time_t current_dat_timestamp) {
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);

    // Use canonical path for lookup to match stored values. Every caller of this
    // (isRomFileCached and getOutdatedRomFiles) already has this path from a
    // directory listing, so it's known to exist : canonical() requires that
    // anyway, and the try/catch below already covers the case where it doesn't
    // (e.g. a symlink that broke between listing and here), so a separate
    // exists() pre-check was just one more stat per file for nothing.
    std::string lookup_path = filepath;
    try {
        lookup_path = std::filesystem::canonical(filepath).string();
    } catch (...) { lookup_path = filepath; }
    sqlite3_bind_text(stmt, 1, lookup_path.c_str(), -1, SQLITE_TRANSIENT);

    bool is_cached = false;

    int step_result = sqlite3_step(stmt);
    if (step_result == SQLITE_ROW) {
        time_t cached_modified = sqlite3_column_int64(stmt, 0);
        size_t cached_size = sqlite3_column_int64(stmt, 1);
        time_t cached_dat_timestamp = sqlite3_column_int64(stmt, 2);
        long cached_file_crc = static_cast<long>(sqlite3_column_int64(stmt, 3));

        // If DAT timestamp changed, we must re-scan
            const long mtime_tolerance_seconds = 2; // tolerate small timestamp differences
            if (cached_dat_timestamp != current_dat_timestamp) {
                is_cached = false;
            } else if (cached_size == file_size && std::llabs(static_cast<long long>(cached_modified) - static_cast<long long>(last_modified)) <= mtime_tolerance_seconds) {
                // Size matches and mtime within tolerance => consider cached
                is_cached = true;
            } else {
            // Metadata differs (timestamps or size). As a fallback, compute file CRC and compare
            long current_crc = 0;
            try {
                current_crc = compute_file_crc32(lookup_path);
            } catch (...) {
                current_crc = 0;
            }

            if (current_crc != 0 && cached_file_crc != 0 && current_crc == cached_file_crc) {
                // File contents identical despite metadata differences - treat as cached
                is_cached = true;
            } else {
                is_cached = false;
            }
        }
    }

    return is_cached;
}

bool DatabaseManager::clearRomCache() {
    // Clear both the metadata cache and the content-addressed cache : they describe
    // the same physical files.
    const char* sql = "DELETE FROM rom_cache; DELETE FROM zip_contents; DELETE FROM zip_contents_stamp;";
    char* err_msg = nullptr;

    int rc = sqlite3_exec(m_db, sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "Erreur suppression cache ROM: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    return true;
}

bool DatabaseManager::clearDirectorySnapshots() {
    const char* sql = "DELETE FROM directory_snapshots;";
    char* err_msg = nullptr;

    int rc = sqlite3_exec(m_db, sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "Erreur suppression directory_snapshots: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    return true;
}

bool DatabaseManager::clearDirectoryFiles() {
    const char* sql = "DELETE FROM directory_files;";
    char* err_msg = nullptr;

    int rc = sqlite3_exec(m_db, sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "Erreur suppression directory_files: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

bool DatabaseManager::getDirectoryFileList(const std::string& path, std::vector<DirFileInfo>& files) {
    const char* sql = "SELECT filename, file_size, last_modified FROM directory_files "
                      "WHERE path = ? ORDER BY filename;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);

    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        found = true;
        DirFileInfo f;
        f.filename = safe_column_text(stmt, 0);
        f.size     = sqlite3_column_int64(stmt, 1);
        f.mtime    = sqlite3_column_int64(stmt, 2);
        files.push_back(std::move(f));
    }
    sqlite3_finalize(stmt);
    return found;
}

bool DatabaseManager::updateDirectoryFileList(const std::string& path, const std::vector<DirFileInfo>& files) {
    // Delete existing entries for this path and insert the new list
    const char* delete_sql = "DELETE FROM directory_files WHERE path = ?;";
    sqlite3_stmt* del_stmt;
    if (sqlite3_prepare_v2(m_db, delete_sql, -1, &del_stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(del_stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(del_stmt);
    sqlite3_finalize(del_stmt);

    const char* insert_sql = "INSERT OR REPLACE INTO directory_files (path, filename, file_size, last_modified) VALUES (?, ?, ?, ?);";
    sqlite3_stmt* ins_stmt;
    if (sqlite3_prepare_v2(m_db, insert_sql, -1, &ins_stmt, nullptr) != SQLITE_OK) return false;

    for (const auto& f : files) {
        sqlite3_reset(ins_stmt);
        sqlite3_bind_text(ins_stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins_stmt, 2, f.filename.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(ins_stmt, 3, f.size);
        sqlite3_bind_int64(ins_stmt, 4, f.mtime);
        sqlite3_step(ins_stmt);
    }
    sqlite3_finalize(ins_stmt);
    return true;
}

bool DatabaseManager::getSavedRomRoots(std::vector<std::string>& roots) {
    const char* sql = "SELECT path FROM saved_roots ORDER BY path;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        roots.push_back(safe_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return true;
}

bool DatabaseManager::updateSavedRomRoots(const std::vector<std::string>& roots) {
    // Replace current saved_roots with provided list
    const char* delete_sql = "DELETE FROM saved_roots;";
    char* err_msg = nullptr;
    if (sqlite3_exec(m_db, delete_sql, 0, 0, &err_msg) != SQLITE_OK) {
        sqlite3_free(err_msg);
        return false;
    }

    const char* insert_sql = "INSERT OR REPLACE INTO saved_roots (path) VALUES (?);";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, insert_sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    for (const auto& p : roots) {
        std::string store = p;
        try {
            if (!p.empty() && std::filesystem::exists(p)) {
                store = std::filesystem::canonical(p).string();
            }
        } catch (...) { store = p; }
        // normalize: remove trailing slash
        if (!store.empty() && store.back() == '/') store.pop_back();
        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, store.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    return true;
}

bool DatabaseManager::removeEntriesForRoot(const std::string& root_path) {
    // Compute canonical root with trailing slash
    std::string canon_root = root_path;
    try {
        if (!root_path.empty() && std::filesystem::exists(root_path)) {
            canon_root = std::filesystem::canonical(root_path).string();
        }
    } catch (...) { canon_root = root_path; }
    if (!canon_root.empty() && canon_root.back() != '/') canon_root.push_back('/');

    // Collect matching filepaths
    const char* select_sql = "SELECT filepath FROM rom_cache;";
    sqlite3_stmt* sel_stmt;
    if (sqlite3_prepare_v2(m_db, select_sql, -1, &sel_stmt, nullptr) != SQLITE_OK) return false;
    std::vector<std::string> to_delete;
    while (sqlite3_step(sel_stmt) == SQLITE_ROW) {
        std::string fp = safe_column_text(sel_stmt, 0);
        std::string fp_canon;
        try { fp_canon = std::filesystem::canonical(fp).string(); } catch (...) { fp_canon = fp; }
        std::string check = fp_canon;
        if (!check.empty() && check.back() != '/') check.push_back('/');
        if (check.rfind(canon_root, 0) == 0) {
            to_delete.push_back(fp);
        }
    }
    sqlite3_finalize(sel_stmt);

    // Safety checks: avoid accidental deletion of entire cache.
    if (canon_root.empty() || canon_root == "/") {
        std::cerr << "[WARN] removeEntriesForRoot: invalid canonical root '" << canon_root << "' - aborting" << std::endl;
        return false;
    }
        // Use SQL to delete rom_cache rows whose filepath starts with the canonical root + '/'
        int total_cache = 0;
        const char* count_sql = "SELECT COUNT(*) FROM rom_cache;";
        sqlite3_stmt* count_stmt;
        if (sqlite3_prepare_v2(m_db, count_sql, -1, &count_stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(count_stmt) == SQLITE_ROW) total_cache = sqlite3_column_int(count_stmt, 0);
            sqlite3_finalize(count_stmt);
        }

        // Prepare pattern like '/canonical/root/%'
        std::string pattern = canon_root;
        if (!pattern.empty() && pattern.back() == '/') pattern.pop_back();
        pattern += "/%";

        int delete_count = 0;
        const char* count_del_sql = "SELECT COUNT(*) FROM rom_cache WHERE filepath LIKE ?;";
        sqlite3_stmt* count_del_stmt;
        if (sqlite3_prepare_v2(m_db, count_del_sql, -1, &count_del_stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(count_del_stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(count_del_stmt) == SQLITE_ROW) delete_count = sqlite3_column_int(count_del_stmt, 0);
            sqlite3_finalize(count_del_stmt);
        }

        std::cerr << "[DBG removeEntriesForRoot] canon_root='" << canon_root << "' pattern='" << pattern << "' total_cache=" << total_cache << " delete_count=" << delete_count << std::endl;

        // Log sample filepaths that would be deleted (up to 100) to help debugging
        if (delete_count > 0) {
            const char* sample_sql = "SELECT filepath FROM rom_cache WHERE filepath LIKE ? LIMIT 100;";
            sqlite3_stmt* samp_stmt;
            if (sqlite3_prepare_v2(m_db, sample_sql, -1, &samp_stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(samp_stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
                int idx = 0;
                while (sqlite3_step(samp_stmt) == SQLITE_ROW) {
                    std::string p = safe_column_text(samp_stmt, 0);
                    std::cerr << "[DBG removeEntriesForRoot] candidate[" << idx << "] = '" << p << "'" << std::endl;
                    idx++;
                }
                sqlite3_finalize(samp_stmt);
            }
        }

        // Safety: never allow deleting entire cache
        if (delete_count > 0 && total_cache > 0 && delete_count == total_cache) {
            std::cerr << "[WARN] removeEntriesForRoot: would remove all " << total_cache << " cache entries for root '" << canon_root << "' - aborting to avoid catastrophic removal" << std::endl;
            return false;
        }

        // Additional safety guard: refuse deletion that would remove a large portion
        const double DELETE_THRESHOLD = 0.50; // 50%
        if (delete_count > 0 && total_cache > 0 && delete_count >= static_cast<int>(std::ceil(total_cache * DELETE_THRESHOLD))) {
            std::cerr << "[WARN] removeEntriesForRoot: delete_count=" << delete_count << " exceeds threshold (" << (DELETE_THRESHOLD*100) << "%) of total_cache=" << total_cache << " - aborting" << std::endl;
            return false;
        }

        if (delete_count > 0) {
            const char* delete_sql = "DELETE FROM rom_cache WHERE filepath LIKE ?;";
            sqlite3_stmt* del_stmt;
            if (sqlite3_prepare_v2(m_db, delete_sql, -1, &del_stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(del_stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(del_stmt);
                sqlite3_finalize(del_stmt);
                std::cout << "[CACHE CLEANUP] Removed " << delete_count << " rom_cache entries under root " << canon_root << std::endl;
            }
        }

    // Remove directory_snapshots and directory_files for paths under root
    const char* select_snap_sql = "SELECT path FROM directory_snapshots;";
    sqlite3_stmt* snap_stmt;
    if (sqlite3_prepare_v2(m_db, select_snap_sql, -1, &snap_stmt, nullptr) == SQLITE_OK) {
        std::vector<std::string> snap_delete;
        while (sqlite3_step(snap_stmt) == SQLITE_ROW) {
            std::string p = safe_column_text(snap_stmt, 0);
            std::string p_canon;
            try { p_canon = std::filesystem::canonical(p).string(); } catch (...) { p_canon = p; }
            std::string check = p_canon;
            if (!check.empty() && check.back() != '/') check.push_back('/');
            if (check.rfind(canon_root, 0) == 0) snap_delete.push_back(p);
        }
        sqlite3_finalize(snap_stmt);

        const char* del_snap_sql = "DELETE FROM directory_snapshots WHERE path = ?;";
        for (const auto& p : snap_delete) {
            sqlite3_stmt* ds;
            if (sqlite3_prepare_v2(m_db, del_snap_sql, -1, &ds, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(ds, 1, p.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(ds);
                sqlite3_finalize(ds);
            }
        }
    }

    const char* select_df_sql = "SELECT DISTINCT path FROM directory_files;";
    sqlite3_stmt* df_stmt;
    if (sqlite3_prepare_v2(m_db, select_df_sql, -1, &df_stmt, nullptr) == SQLITE_OK) {
        std::vector<std::string> df_delete;
        while (sqlite3_step(df_stmt) == SQLITE_ROW) {
            std::string p = safe_column_text(df_stmt, 0);
            std::string p_canon;
            try { p_canon = std::filesystem::canonical(p).string(); } catch (...) { p_canon = p; }
            std::string check = p_canon;
            if (!check.empty() && check.back() != '/') check.push_back('/');
            if (check.rfind(canon_root, 0) == 0) df_delete.push_back(p);
        }
        sqlite3_finalize(df_stmt);

        const char* del_df_sql = "DELETE FROM directory_files WHERE path = ?;";
        for (const auto& p : df_delete) {
            sqlite3_stmt* dd;
            if (sqlite3_prepare_v2(m_db, del_df_sql, -1, &dd, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(dd, 1, p.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(dd);
                sqlite3_finalize(dd);
            }
        }
    }

    return true;
}

// Unconditionally purge all rom_cache, directory_snapshot and directory_files
// entries that live under root_path. No safety threshold : this is called only
// when the user explicitly removes a configured ROM root from settings, so the
// deletion is intentional.
bool DatabaseManager::purgeCacheForRoot(const std::string& root_path) {
    std::string canon_root = root_path;
    try {
        if (!root_path.empty()) {
            // The directory may no longer exist on disk after removal : don't require it to exist.
            if (std::filesystem::exists(root_path))
                canon_root = std::filesystem::canonical(root_path).string();
        }
    } catch (...) { canon_root = root_path; }
    if (!canon_root.empty() && canon_root.back() == '/') canon_root.pop_back();
    if (canon_root.empty() || canon_root == "/") return false; // safety: never nuke everything

    std::string prefix = canon_root + "/";

    // Delete from rom_cache
    {
        const char* sql = "DELETE FROM rom_cache WHERE filepath LIKE ?;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            std::string pattern = prefix + "%";
            sqlite3_bind_text(stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
            int rc = sqlite3_step(stmt);
            int deleted = sqlite3_changes(m_db);
            sqlite3_finalize(stmt);
            std::cerr << "[purgeCacheForRoot] rom_cache: deleted " << deleted << " entries under " << canon_root << std::endl;
        }
    }

    // Delete from directory_snapshots
    {
        const char* sql = "DELETE FROM directory_snapshots WHERE path = ? OR path LIKE ?;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            std::string pattern = prefix + "%";
            sqlite3_bind_text(stmt, 1, canon_root.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, pattern.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    // Delete from directory_files
    {
        const char* sql = "DELETE FROM directory_files WHERE path = ? OR path LIKE ?;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            std::string pattern = prefix + "%";
            sqlite3_bind_text(stmt, 1, canon_root.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, pattern.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    return true;
}

bool DatabaseManager::reevaluateGamesAvailability(const std::vector<std::string>& roms_paths) {
    // Re-evaluate all games currently marked as available or incorrect
    std::vector<Game> avail = getGamesByStatus("available");
    std::vector<Game> incorrect = getGamesByStatus("incorrect");

    std::vector<Game> to_check;
    to_check.reserve(avail.size() + incorrect.size());
    to_check.insert(to_check.end(), avail.begin(), avail.end());
    to_check.insert(to_check.end(), incorrect.begin(), incorrect.end());

    for (auto& game : to_check) {
        RomScanner::check_availability(game, roms_paths);
        updateGameStatus(game.name, game.status, game.system);
    }

    return true;
}

int DatabaseManager::getRomCacheCount() {
    const char* sql = "SELECT COUNT(*) FROM rom_cache;";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[ERROR] getRomCacheCount prepare failed: " << sqlite3_errmsg(m_db) << std::endl;
        return 0;
    }

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return count;
}

bool DatabaseManager::cleanupRomCache(const std::vector<std::string>& rom_paths) {
    // Get all cached file paths
    const char* sql = "SELECT filepath FROM rom_cache;";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    std::vector<std::string> files_to_remove;
    // The subset that is gone from the disk : their contents leave
    // zip_contents too. A file merely outside FinalBurn Neo's directories may
    // still be another emulator's, and RomScanner::prune_zip_cache decides
    // for those.
    std::vector<std::string> files_gone;

    // Precompute canonical rom_paths so we can check whether a cached filepath
    // is still under any of the configured ROM directories.
    std::vector<std::string> canon_rom_paths;
    for (const auto& p : rom_paths) {
        try {
            if (!p.empty() && std::filesystem::exists(p)) {
                std::string c = std::filesystem::canonical(p).string();
                if (!c.empty() && c.back() != '/') c.push_back('/');
                canon_rom_paths.push_back(c);
            } else {
                std::string c = p;
                if (!c.empty() && c.back() != '/') c.push_back('/');
                canon_rom_paths.push_back(c);
            }
        } catch (...) {
            std::string c = p;
            if (!c.empty() && c.back() != '/') c.push_back('/');
            canon_rom_paths.push_back(c);
        }
    }

    // If no configured rom paths (or none could be canonicalized), do not remove anything here.
    // This prevents accidental full-cache deletion when the application has no configured ROM roots.
    std::vector<std::string> filtered_roots;
    for (auto &rp : canon_rom_paths) {
        if (!rp.empty() && rp != "/") filtered_roots.push_back(rp);
    }
    canon_rom_paths.swap(filtered_roots);
    if (canon_rom_paths.empty()) {
        std::cerr << "[CACHE CLEANUP] No valid configured ROM roots provided : skipping cleanup to avoid mass deletion" << std::endl;
        sqlite3_finalize(stmt);
        return false;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* filepath_cstr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (!filepath_cstr) continue;

        std::string filepath = filepath_cstr;

        bool remove_entry = false;

        // If file no longer exists on disk -> remove
        bool exists_on_disk = false;
        try {
            exists_on_disk = std::filesystem::exists(filepath);
        } catch (...) { exists_on_disk = false; }
        if (!exists_on_disk) {
            remove_entry = true;
            files_to_remove.push_back(filepath);
            files_gone.push_back(filepath);
            continue;
        }

        // Compute canonical filepath; if we cannot canonicalize, skip removing this entry.
        std::string fp_canon;
        try {
            fp_canon = std::filesystem::canonical(filepath).string();
        } catch (...) {
            // Skip this file - we can't reliably compare
            continue;
        }
        if (!fp_canon.empty() && fp_canon.back() != '/') fp_canon.push_back('/');

        // If canonical filepath is not under any configured canonical root, mark for removal
        bool under_any = false;
        for (const auto& rp : canon_rom_paths) {
            if (!rp.empty() && fp_canon.rfind(rp, 0) == 0) { under_any = true; break; }
        }
        if (!under_any) files_to_remove.push_back(filepath);
    }

    sqlite3_finalize(stmt);

    // Remove entries that are no longer valid
    if (!files_to_remove.empty()) {
        // Get current total cache count for safety checks
        int total_cache_now = getRomCacheCount();
        const double DELETE_THRESHOLD = 0.50; // 50%
        if (total_cache_now > 0 && static_cast<int>(files_to_remove.size()) >= static_cast<int>(std::ceil(total_cache_now * DELETE_THRESHOLD))) {
            std::cerr << "[WARN] cleanupRomCache: would remove " << files_to_remove.size() << " entries (>= " << (DELETE_THRESHOLD*100) << "% of current cache " << total_cache_now << ") - aborting to avoid catastrophic removal" << std::endl;
            return false;
        }

        std::cout << "[CACHE CLEANUP] Removing " << files_to_remove.size() << " files from cache (not under configured paths or deleted)" << std::endl;

        // Print sample of files to remove (first 100) to help debugging
        for (size_t i = 0; i < files_to_remove.size() && i < 100; ++i) {
            std::cerr << "[DBG cleanupRomCache] to_remove[" << i << "] = '" << files_to_remove[i] << "'" << std::endl;
        }

        const char* delete_sql = "DELETE FROM rom_cache WHERE filepath = ?;";
        for (const auto& filepath : files_to_remove) {
            sqlite3_stmt* delete_stmt;
            if (sqlite3_prepare_v2(m_db, delete_sql, -1, &delete_stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(delete_stmt, 1, filepath.c_str(), -1, SQLITE_STATIC);
                sqlite3_step(delete_stmt);
                sqlite3_finalize(delete_stmt);
            }
        }
        forgetZipContents(files_gone);
    }

    return true;
}

void DatabaseManager::startCacheCleanupThread(const std::vector<std::string>& rom_paths) {
    if (m_cache_cleanup_running.load()) return;
    m_cache_cleanup_paths = rom_paths;
    m_cache_cleanup_running.store(true);
    m_cache_cleanup_thread = std::thread(cache_cleanup_thread_func, this, m_cache_cleanup_paths, &m_cache_cleanup_running);
}

void DatabaseManager::stopCacheCleanupThread() {
    if (!m_cache_cleanup_running.load()) return;
    m_cache_cleanup_running.store(false);
    if (m_cache_cleanup_thread.joinable()) m_cache_cleanup_thread.join();
}

time_t DatabaseManager::getLastDatTimestamp() {
    const char* sql = "SELECT value FROM scan_metadata WHERE key = 'last_dat_timestamp';";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }

    time_t timestamp = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        timestamp = sqlite3_column_int64(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return timestamp;
}

bool DatabaseManager::needsDatResync() {
    const char* sql = "SELECT value FROM scan_metadata WHERE key = 'needs_dat_resync';";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    bool needed = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) != 0;
    sqlite3_finalize(stmt);
    return needed;
}

bool DatabaseManager::needsMameDatResync() {
    return getScanMetadata("needs_mame_dat_resync", 0) != 0;
}

bool DatabaseManager::clearMameDatResyncFlag() {
    return sqlite3_exec(m_db, "DELETE FROM scan_metadata WHERE key = 'needs_mame_dat_resync';",
                        0, 0, nullptr) == SQLITE_OK;
}

bool DatabaseManager::clearDatResyncFlag() {
    char* err = nullptr;
    if (sqlite3_exec(m_db, "DELETE FROM scan_metadata WHERE key = 'needs_dat_resync';",
                     0, 0, &err) != SQLITE_OK) {
        std::cerr << "[WARN] Could not clear the DAT resync flag: " << err << std::endl;
        sqlite3_free(err);
        return false;
    }
    return true;
}

int64_t DatabaseManager::getScanMetadata(const std::string& key, int64_t fallback) {
    const char* sql = "SELECT value FROM scan_metadata WHERE key = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return fallback;
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_STATIC);
    int64_t v = fallback;
    if (sqlite3_step(stmt) == SQLITE_ROW) v = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return v;
}

bool DatabaseManager::setScanMetadata(const std::string& key, int64_t value) {
    const char* sql = "INSERT OR REPLACE INTO scan_metadata (key, value) VALUES (?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, value);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<std::string> DatabaseManager::getDatHeaders() {
    std::vector<std::string> out;
    const char* sql = "SELECT DISTINCT dat_header FROM games WHERE dat_header IS NOT NULL AND dat_header != '' ORDER BY dat_header;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    while (sqlite3_step(stmt) == SQLITE_ROW) out.push_back(safe_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    return out;
}

std::map<std::string, DatabaseManager::DatFileStats> DatabaseManager::getDatFileStats() {
    std::map<std::string, DatFileStats> out;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db,
            "SELECT dat_source, COUNT(*), MAX(dat_header) FROM games WHERE dat_source IS NOT NULL GROUP BY dat_source;",
            -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            auto& st = out[safe_column_text(stmt, 0)];
            st.games  = sqlite3_column_int(stmt, 1);
            st.header = safe_column_text(stmt, 2);
        }
        sqlite3_finalize(stmt);
    }
    if (sqlite3_prepare_v2(m_db,
            "SELECT g.dat_source, COUNT(r.id) FROM roms r JOIN games g ON g.id = r.game_id "
            "WHERE g.dat_source IS NOT NULL GROUP BY g.dat_source;",
            -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW)
            out[safe_column_text(stmt, 0)].roms = sqlite3_column_int(stmt, 1);
        sqlite3_finalize(stmt);
    }
    return out;
}

int DatabaseManager::countDatFiles() {
    const char* sql = "SELECT COUNT(DISTINCT dat_source) FROM games;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    int n = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

bool DatabaseManager::setLastDatTimestamp(time_t timestamp) {
    const char* sql = "INSERT OR REPLACE INTO scan_metadata (key, value) VALUES ('last_dat_timestamp', ?);";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(stmt, 1, timestamp);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

std::vector<std::string> DatabaseManager::getOutdatedRomFiles(const std::vector<std::string>& rom_paths, time_t current_dat_timestamp, bool recursive, bool include_loose_files,
                                                               std::function<bool(size_t, const std::string&)> progress_cb) {
    std::vector<std::string> outdated_files;

    // Prepared once and reused for every file below via isRomFileCachedStmt().
    // This loop can run against tens of thousands of files (a large or slow // e.g. external/network : drive included), and re-preparing the same query
    // per file was previously the dominant cost.
    const char* sql = "SELECT last_modified, file_size, dat_timestamp, file_crc FROM rom_cache WHERE filepath = ? ORDER BY id DESC LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return outdated_files;

    size_t checked = 0;
    bool cancelled = false;
    // Dispatching to the UI is throttled downstream (by time, not by this
    // count), so this can be small enough to make Cancel feel responsive
    // without itself becoming a meaningful per-file cost.
    constexpr size_t kProgressEvery = 40;

    for (const auto& rom_path : rom_paths) {
        if (cancelled) break;
        if (!std::filesystem::exists(rom_path)) {
            continue;
        }

        if (recursive) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(rom_path)) {
                if (!entry.is_regular_file()) continue;

                std::string filepath = entry.path().string();

                // Skip DAT files (handled elsewhere)
                if (entry.path().extension() == ".dat") continue;

                // If not including loose files, only consider .zip
                if (!include_loose_files && entry.path().extension() != ".zip") continue;

                auto ftime = std::filesystem::last_write_time(entry);
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
                );
                time_t last_modified = std::chrono::system_clock::to_time_t(sctp);
                size_t file_size = std::filesystem::file_size(entry);

                if (!isRomFileCachedStmt(stmt, filepath, last_modified, file_size, current_dat_timestamp)) {
                    outdated_files.push_back(filepath);
                }
                if (progress_cb && (++checked % kProgressEvery) == 0 && !progress_cb(checked, rom_path)) {
                    cancelled = true;
                    break;
                }
            }
        } else {
            for (const auto& entry : std::filesystem::directory_iterator(rom_path)) {
                if (!entry.is_regular_file()) continue;

                std::string filepath = entry.path().string();
                if (entry.path().extension() == ".dat") continue;
                if (!include_loose_files && entry.path().extension() != ".zip") continue;

                auto ftime = std::filesystem::last_write_time(entry);
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
                );
                time_t last_modified = std::chrono::system_clock::to_time_t(sctp);
                size_t file_size = std::filesystem::file_size(entry);

                if (!isRomFileCachedStmt(stmt, filepath, last_modified, file_size, current_dat_timestamp)) {
                    outdated_files.push_back(filepath);
                }
                if (progress_cb && (++checked % kProgressEvery) == 0 && !progress_cb(checked, rom_path)) {
                    cancelled = true;
                    break;
                }
            }
        }
    }

    sqlite3_finalize(stmt);
    if (progress_cb) progress_cb(checked, rom_paths.empty() ? std::string() : rom_paths.back());
    return outdated_files;
}

static std::string crc_to_hex8(unsigned long crc) {
    std::ostringstream ss;
    ss << std::hex << std::noshowbase << std::setw(8) << std::setfill('0') << (crc & 0xFFFFFFFFUL);
    std::string s = ss.str();
    // normalize to lowercase
    for (auto &c : s) c = static_cast<char>(std::tolower((unsigned char)c));
    return s;
}

std::vector<Game> DatabaseManager::getGamesByRomCrc(unsigned long crc, const std::string& emulator) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::vector<Game> results;
    // Un meme CRC se retrouve des deux cotes : l'emulateur borne la recherche
    // au catalogue qui interesse l'appelant, et un emulateur vide les rend
    // tous les deux.
    const char* sql = "SELECT g.id, g.name, g.system, g.emulator FROM roms r JOIN games g ON r.game_id = g.id WHERE LOWER(r.crc) = ? AND (? = '' OR g.emulator = ?);";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return results;

    std::string hex = crc_to_hex8(crc);
    sqlite3_bind_text(stmt, 1, hex.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, emulator.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, emulator.c_str(), -1, SQLITE_STATIC);

    // Le doublon se juge maintenant sur les trois parties de la cle.
    std::set<std::tuple<std::string,std::string,std::string>> seen;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string name = safe_column_text(stmt, 1);
        std::string system = safe_column_text(stmt, 2);
        std::string emu = safe_column_text(stmt, 3);
        if (emu.empty()) emu = "fbneo";
        if (seen.insert({name, system, emu}).second) {
            Game g = getGame(name, system, emu);
            if (!g.name.empty()) results.push_back(g);
        }
    }
    sqlite3_finalize(stmt);
    return results;
}

bool DatabaseManager::getDirectorySnapshot(const std::string& path, int& file_count, time_t& last_modified) {
    const char* sql = "SELECT file_count, last_modified FROM directory_snapshots WHERE path = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        file_count = static_cast<int>(sqlite3_column_int(stmt, 0));
        last_modified = static_cast<time_t>(sqlite3_column_int64(stmt, 1));
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

bool DatabaseManager::updateDirectorySnapshot(const std::string& path, int file_count, time_t last_modified) {
    const char* sql = "INSERT OR REPLACE INTO directory_snapshots (path, file_count, last_modified) VALUES (?, ?, ?);";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, file_count);
    sqlite3_bind_int64(stmt, 3, static_cast<long long>(last_modified));

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}
// ── Metadonnees texte ───────────────────────────────────────────────────────
//
// scan_metadata ne porte que des entiers ; cette table-ci prend le reste.

static const char* kMetaTextSql = R"(
    CREATE TABLE IF NOT EXISTS app_metadata (
        key   TEXT PRIMARY KEY,
        value TEXT
    );
)";

std::string DatabaseManager::getMetaString(const std::string& key) {
    if (!m_db) return {};
    sqlite3_exec(m_db, kMetaTextSql, nullptr, nullptr, nullptr);

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db, "SELECT value FROM app_metadata WHERE key = ?;",
                           -1, &st, nullptr) != SQLITE_OK)
        return {};
    sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    std::string out;
    if (sqlite3_step(st) == SQLITE_ROW) out = safe_column_text(st, 0);
    sqlite3_finalize(st);
    return out;
}

bool DatabaseManager::setMetaString(const std::string& key, const std::string& value) {
    if (!m_db) return false;
    sqlite3_exec(m_db, kMetaTextSql, nullptr, nullptr, nullptr);

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db,
            "INSERT OR REPLACE INTO app_metadata (key, value) VALUES (?, ?);",
            -1, &st, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, key.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

// ── Catalogue MAME ──────────────────────────────────────────────────────────

static const char* kMameCatalogSql = R"(
    CREATE TABLE IF NOT EXISTS mame_catalog (
        name          TEXT PRIMARY KEY,
        description   TEXT,
        year          TEXT,
        manufacturer  TEXT,
        cloneof       TEXT,
        romof         TEXT,
        sourcefile    TEXT,
        driver_status TEXT,
        is_bios       INTEGER DEFAULT 0,
        is_device     INTEGER DEFAULT 0,
        is_mechanical INTEGER DEFAULT 0,
        runnable      INTEGER DEFAULT 1,
        status        TEXT DEFAULT 'missing',
        genre         TEXT,
        players       INTEGER DEFAULT 0,
        orientation   TEXT,
        width         TEXT,
        height        TEXT
    );
)";

/* CREATE TABLE IF NOT EXISTS ne rattrape rien.
 *
 * Une base construite avant ces colonnes garde sa table telle quelle : le
 * CREATE ne fait rien, et le premier SELECT qui les demande echoue, ce qui
 * rendait un catalogue vide au lieu de 27 199 machines. Les colonnes
 * s'ajoutent donc une a une. Aucune contrainte ne change, la table n'est pas
 * reecrite, et les lignes deja presentes gardent leur contenu.
 */
static void ensure_mame_catalog(sqlite3* db) {
    if (!db) return;
    sqlite3_exec(db, kMameCatalogSql, nullptr, nullptr, nullptr);

    struct ColDef { const char* name; const char* ddl; };
    static const ColDef cols[] = {
        { "genre",       "ALTER TABLE mame_catalog ADD COLUMN genre TEXT;" },
        { "players",     "ALTER TABLE mame_catalog ADD COLUMN players INTEGER DEFAULT 0;" },
        { "orientation", "ALTER TABLE mame_catalog ADD COLUMN orientation TEXT;" },
        { "width",       "ALTER TABLE mame_catalog ADD COLUMN width TEXT;" },
        { "height",      "ALTER TABLE mame_catalog ADD COLUMN height TEXT;" },
    };

    std::set<std::string> have;
    sqlite3_stmt* pi = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(mame_catalog);", -1, &pi, nullptr) == SQLITE_OK) {
        while (sqlite3_step(pi) == SQLITE_ROW) have.insert(safe_column_text(pi, 1));
        sqlite3_finalize(pi);
    }
    for (const auto& c : cols)
        if (!have.count(c.name)) sqlite3_exec(db, c.ddl, nullptr, nullptr, nullptr);
}

void DatabaseManager::beginMameCatalogRebuild() {
    if (!m_db) return;
    ensure_mame_catalog(m_db);
    // La version n'est effacee qu'ici : tant que le remplacement n'est pas
    // valide, un demarrage interrompu redeclenchera la regeneration au lieu
    // de faire confiance a une table a moitie remplie.
    setMetaString("mame_build", "");
    sqlite3_exec(m_db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
    sqlite3_exec(m_db, "DELETE FROM mame_catalog;", nullptr, nullptr, nullptr);
}

bool DatabaseManager::insertMameMachines(const std::vector<MameMachine>& machines) {
    if (!m_db || machines.empty()) return false;

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db,
            "INSERT OR REPLACE INTO mame_catalog "
            "(name, description, year, manufacturer, cloneof, romof, sourcefile, "
            " driver_status, is_bios, is_device, is_mechanical, runnable, "
            " players, orientation, width, height) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
            -1, &st, nullptr) != SQLITE_OK)
        return false;

    // Un seul prepare pour tout le lot : a 50 000 machines, en refaire un par
    // ligne domine largement le cout de l'insertion.
    for (const auto& m : machines) {
        sqlite3_bind_text(st, 1, m.name.c_str(),          -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, m.description.c_str(),   -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, m.year.c_str(),          -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, m.manufacturer.c_str(),  -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 5, m.cloneof.c_str(),       -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 6, m.romof.c_str(),         -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 7, m.sourcefile.c_str(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 8, m.driver_status.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int (st, 9,  m.is_bios       ? 1 : 0);
        sqlite3_bind_int (st, 10, m.is_device     ? 1 : 0);
        sqlite3_bind_int (st, 11, m.is_mechanical ? 1 : 0);
        sqlite3_bind_int (st, 12, m.runnable      ? 1 : 0);
        sqlite3_bind_int (st, 13, m.players);
        sqlite3_bind_text(st, 14, m.orientation.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 15, m.width.c_str(),       -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 16, m.height.c_str(),      -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    return true;
}

void DatabaseManager::commitMameCatalogRebuild(const std::string& build) {
    if (!m_db) return;
    sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr);
    sqlite3_exec(m_db,
        "CREATE INDEX IF NOT EXISTS idx_mame_catalog_desc ON mame_catalog(description);",
        nullptr, nullptr, nullptr);
    setMetaString("mame_build", build);
}

void DatabaseManager::abortMameCatalogRebuild() {
    if (!m_db) return;
    sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
}

int DatabaseManager::countMameMachines() {
    if (!m_db) return 0;
    ensure_mame_catalog(m_db);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db, "SELECT COUNT(*) FROM mame_catalog;", -1, &st, nullptr)
            != SQLITE_OK)
        return 0;
    int n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

std::vector<Game> DatabaseManager::getMameCatalog(bool include_mechanical) {
    std::vector<Game> games;
    if (!m_db) return games;
    ensure_mame_catalog(m_db);

    // Les machines internes (isdevice) restent en base parce que l'audit d'un
    // set split en a besoin, mais elles n'ont rien a faire dans une liste de
    // jeux : personne ne « joue » a une puce sonore.
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db,
            // mame_catalog est regenere en entier a chaque changement de
            // version de MAME : les favoris et le temps de jeu ne peuvent donc
            // pas y vivre, ils sont lus depuis player_stats, dont c'est le
            // role. Le systeme joint est celui que la boucle reconstruit plus
            // bas, sans quoi la ligne ecrite par toggleFavorite ne serait
            // jamais retrouvee.
            "SELECT m.name, m.description, m.year, m.manufacturer, m.cloneof, m.romof, "
            "       m.sourcefile, m.driver_status, m.is_bios, m.is_mechanical, m.status, "
            "       COALESCE(p.is_favorite, 0), p.last_played, COALESCE(p.play_count, 0), "
            "       COALESCE(p.play_time_secs, 0), COALESCE(p.last_session_secs, 0), "
            "       COALESCE(p.longest_session_secs, 0), "
            "       m.genre, COALESCE(m.players, 0), m.orientation, m.width, m.height "
            "FROM mame_catalog m "
            "LEFT JOIN player_stats p ON p.emulator = 'mame' AND p.name = m.name "
            "  AND p.system = CASE WHEN m.is_mechanical != 0 THEN 'Mechanical' ELSE 'Arcade' END "
            "WHERE m.is_device = 0 "
            "  AND (? = 1 OR m.is_mechanical = 0) ORDER BY m.description;",
            -1, &st, nullptr) != SQLITE_OK)
        return games;
    sqlite3_bind_int(st, 1, include_mechanical ? 1 : 0);

    while (sqlite3_step(st) == SQLITE_ROW) {
        Game g;
        g.emulator      = "mame";
        g.name          = safe_column_text(st, 0);
        g.description   = safe_column_text(st, 1);
        g.year          = safe_column_text(st, 2);
        g.manufacturer  = safe_column_text(st, 3);
        g.cloneof       = safe_column_text(st, 4);
        g.romof         = safe_column_text(st, 5);
        g.sourcefile    = safe_column_text(st, 6);
        g.driver_status = safe_column_text(st, 7);
        g.is_bios       = sqlite3_column_int(st, 8) != 0;
        // MAME ne classe pas ses machines par « systeme » : on reprend la
        // distinction qui parle a l'utilisateur, jouable ou mecanique.
        g.system        = sqlite3_column_int(st, 9) != 0 ? "Mechanical" : "Arcade";
        g.status        = safe_column_text(st, 10);
        if (g.status.empty()) g.status = "missing";
        g.is_favorite          = sqlite3_column_int(st, 11) != 0;
        g.last_played          = safe_column_text(st, 12);
        g.play_count           = sqlite3_column_int(st, 13);
        g.play_time_secs       = sqlite3_column_int(st, 14);
        g.last_session_secs    = sqlite3_column_int(st, 15);
        g.longest_session_secs = sqlite3_column_int(st, 16);
        /* Le genre ne vient pas de MAME : il vient de catver.ini, le fichier
         * communautaire que l'ecran des reglages sait telecharger. Tant que
         * personne ne l'a charge, la colonne est vide et les machines MAME
         * n'apparaissent dans aucun genre : c'est un fait, pas une panne. */
        g.genre         = safe_column_text(st, 17);
        g.players       = sqlite3_column_int(st, 18);
        g.orientation   = safe_column_text(st, 19);
        g.width         = safe_column_text(st, 20);
        g.height        = safe_column_text(st, 21);
        g.dat_source    = "mame";
        g.dat_header    = "MAME";
        games.push_back(std::move(g));
    }
    sqlite3_finalize(st);
    return games;
}

void DatabaseManager::resetMameStatuses() {
    if (!m_db) return;
    ensure_mame_catalog(m_db);
    sqlite3_exec(m_db, "UPDATE mame_catalog SET status = 'missing';",
                 nullptr, nullptr, nullptr);
}

/* Les genres de catver.ini, poses sur un catalogue deja construit.
 *
 * Une mise a jour, pas un remplacement : le catalogue vient de MAME et ne
 * doit rien a ce fichier. Une machine que catver.ini ne connait pas garde
 * simplement un genre vide. Tout passe dans UNE transaction, sinon les
 * 50 000 UPDATE deviennent 50 000 ecritures disque.
 *
 * Rend le nombre de lignes du catalogue effectivement classees : catver.ini
 * decrit beaucoup plus de machines que celles installees ici, et compter les
 * lignes lues dirait n'importe quoi.
 */
int DatabaseManager::setMameGenres(
        const std::vector<std::pair<std::string, std::string>>& genres) {
    if (!m_db || genres.empty()) return 0;
    ensure_mame_catalog(m_db);

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db, "UPDATE mame_catalog SET genre = ? WHERE name = ?;",
                           -1, &st, nullptr) != SQLITE_OK)
        return -1;

    int applied = 0;
    sqlite3_exec(m_db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
    for (const auto& [name, genre] : genres) {
        sqlite3_bind_text(st, 1, genre.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, name.c_str(),  -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_DONE) applied += sqlite3_changes(m_db);
        sqlite3_reset(st);
    }
    sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr);
    sqlite3_finalize(st);
    return applied;
}

int DatabaseManager::countMameGenres() {
    if (!m_db) return 0;
    ensure_mame_catalog(m_db);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db,
            "SELECT COUNT(*) FROM mame_catalog "
            "WHERE is_device = 0 AND genre IS NOT NULL AND genre != '';",
            -1, &st, nullptr) != SQLITE_OK)
        return 0;
    int n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

bool DatabaseManager::setMameStatuses(
        const std::vector<std::pair<std::string, std::string>>& verdicts) {
    if (!m_db || verdicts.empty()) return false;

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db, "UPDATE mame_catalog SET status = ? WHERE name = ?;",
                           -1, &st, nullptr) != SQLITE_OK)
        return false;

    sqlite3_exec(m_db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
    for (const auto& [name, status] : verdicts) {
        sqlite3_bind_text(st, 1, status.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, name.c_str(),   -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_reset(st);
    }
    sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr);
    sqlite3_finalize(st);
    return true;
}
