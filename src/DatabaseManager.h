// src/DatabaseManager.h
#pragma once
#include <map>
#include <string>
#include <vector>
#include <memory>
#include <ctime>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <functional>
#include "Game.h"

struct sqlite3;
struct sqlite3_stmt;

class DatabaseManager {
public:
    DatabaseManager(const std::string& db_path);
    ~DatabaseManager();
    
    bool initialize();
    bool insertGame(const Game& game);
    bool insertRom(int64_t game_id, const Rom& rom);
    bool updateRomPath(const std::string& game_name, const std::string& rom_name, const std::string& file_path);
    bool updateGameStatus(const std::string& game_name, const std::string& status);
    // L'emulateur complete la cle : `mslug` existe chez FinalBurn Neo comme
    // chez MAME, tous deux en system 'Arcade'. Defaut 'fbneo' pour que les
    // appelants ecrits avant MAME continuent de designer le meme jeu.
    bool updateGameStatus(const std::string& game_name, const std::string& status, const std::string& system,
                          const std::string& emulator = "fbneo");
    bool updateGameStatusWithSource(const std::string& game_name, const std::string& status, const std::string& system, const std::string& source_directory,
                                    const std::string& emulator = "fbneo");
    bool resetGamesFromDirectory(const std::string& directory, const std::string& emulator = "fbneo");
    bool updateGameSnapshot(const std::string& game_name, const std::string& snapshot_path);
    bool resetAllGamesToMissing();
    
    // Un seul catalogue a la fois, FinalBurn Neo par defaut : la table porte
    // aussi les sets MAME que le gestionnaire de ROMs importe de ses DAT, et
    // ni la fenetre principale (qui lit MAME dans mame_catalog) ni un audit
    // FinalBurn Neo ne doivent les voir. Un emulateur vide rend tout.
    std::vector<Game> getAllGames(const std::string& emulator = "fbneo");
    // Sans les ROMs : ce dont l'interface a besoin, et rien de plus.
    std::vector<Game> getAllGamesLight(const std::string& emulator = "fbneo");
    std::vector<Game> getGamesBySystem(const std::string& system);
    std::vector<Game> getGamesByStatus(const std::string& status);
    Game getGame(const std::string& game_name);
    // Favori, lancements et temps de jeu d'un set, quel que soit l'emulateur :
    // ceux de MAME ne vivent que dans player_stats, pas dans `games`.
    Game getPlayerStats(const std::string& game_name, const std::string& system,
                        const std::string& emulator);
    Game getGame(const std::string& game_name, const std::string& system,
                 const std::string& emulator = "fbneo");
    // Un emulateur vide rend le jeu des deux catalogues.
    std::vector<Game> getAllGamesWithName(const std::string& game_name,
                                          const std::string& emulator = "fbneo");
    // Meme convention : FinalBurn Neo par defaut, vide pour les deux.
    size_t getGameCount(const std::string& emulator = "fbneo");
    size_t getGameCountByStatus(const std::string& status, const std::string& emulator = "fbneo");

    // Favourites
    // L'emulateur fait partie de l'identite d'un set : `mslug` existe chez
    // FinalBurn Neo comme chez MAME, tous deux en system 'Arcade', et sans lui
    // les deux jeux partageraient favori et temps de jeu. Valeur par defaut
    // 'fbneo' : c'est le seul catalogue qui existait avant.
    bool toggleFavorite(const std::string& game_name, const std::string& system,
                        const std::string& emulator = "fbneo");

    // ── Ignored sets ───────────────────────────────────────────────────────
    // "Do not report this set as a problem again" : a nodump-only set, a
    // prototype, anything the user knowingly does not maintain. Keyed by
    // (name, system) in its own table, with no foreign key to games, for the
    // same reason as player_stats: the games table is wiped on every DAT
    // reload and a decision the user took must survive that.
    struct IgnoredSet { std::string name, system, note, added_at, emulator; };
    bool ignoreSet(const std::string& game_name, const std::string& system,
                   const std::string& note = "", const std::string& emulator = "fbneo");
    bool unignoreSet(const std::string& game_name, const std::string& system,
                     const std::string& emulator = "fbneo");
    bool isIgnored(const std::string& game_name, const std::string& system,
                   const std::string& emulator = "fbneo");
    // Un emulateur vide rend les sets ignores des deux catalogues.
    std::vector<IgnoredSet> getIgnoredSets(const std::string& emulator = "fbneo");
    bool isFavorite(const std::string& game_name, const std::string& system,
                    const std::string& emulator = "fbneo");
    std::vector<Game> getFavorites();

    // Play tracking
    bool recordLaunch(const std::string& game_name, const std::string& system,
                      const std::string& emulator = "fbneo");
    bool addPlayTime(const std::string& game_name, const std::string& system, int seconds,
                     const std::string& emulator = "fbneo");
    std::vector<Game> getRecentlyPlayed(int limit = 20);

    bool clearAllData();
    // One emulator's games, with their ROMs and disks ; empty = every game.
    // A DAT update of FinalBurn Neo leaves MAME's sets and statuses alone.
    bool clearGames(const std::string& emulator);
    bool gameExists(const std::string& game_name);

    // Transaction management
    bool beginTransaction();
    bool commitTransaction();
    bool rollbackTransaction();

    // Savepoint management (for nested transactions)
    bool createSavepoint(const std::string& name);
    bool releaseSavepoint(const std::string& name);
    bool rollbackToSavepoint(const std::string& name);
    
    // Incremental DAT update ("diff") support.
    // A snapshot maps "name\x1fsystem\x1femulator" -> "status\x1fromsignature", where the ROM
    // signature is a content fingerprint built from the game's ROM set (name/size/crc).
    // Two games with the same signature have an identical ROM definition, so a game
    // whose signature is unchanged across a DAT reload keeps its previously computed
    // availability status without needing to re-read its ROM files from disk.
    // How many games carry favourites or play history. That data survives a
    // DAT rebuild through triggers on the games table (see the constructor):
    // four separate paths delete from it, so the guarantee lives in the
    // database rather than in each of them.
    int protectedPlayerStats();

    // Deux compteurs pour la carte de profil des reglages. Comptes en SQL
    // plutot qu'en chargeant getFavorites() : la reponse tient dans un entier,
    // et rapatrier des centaines de Game complets pour appeler .size() dessus
    // serait payer une liste pour n'en lire que la longueur.
    int countFavorites();
    int countPlayedGames();

    // `emulator` narrows the snapshot to one emulator's games ; empty = all.
    std::unordered_map<std::string, std::string> snapshotStatusSignatures(const std::string& emulator = "");
    // After a DAT reload, restore statuses for every game whose (name, system) and
    // ROM signature are unchanged versus the given snapshot. Games that are new or
    // whose signature changed keep the default 'missing' status and have their ZIP
    // filename ("<name>.zip") appended to changed_zip_names so the caller can
    // invalidate exactly those cache entries. Returns the number of restored games.
    // `emulator` limits the pass to the games that were reloaded (empty = all);
    // `changed_keys`, when given, receives the snapshot key of every game that
    // is new or whose ROM definition changed : the only ones left to judge.
    int applyPreservedStatuses(const std::unordered_map<std::string, std::string>& old_snapshot,
                               std::vector<std::string>& changed_zip_names,
                               const std::string& emulator = "",
                               std::vector<std::string>* changed_keys = nullptr);
    // Remove rom_cache rows whose filename matches any of the given ZIP filenames,
    // forcing those (and only those) ZIPs to be re-read on the next scan.
    bool invalidateRomCacheForFiles(const std::vector<std::string>& zip_filenames);

    // Content-addressed ROM cache (Phase 2): persist and read back the {entry, crc}
    // contents of scanned ZIPs so game availability can be re-derived without disk I/O.
    struct ZipContentRow { std::string filepath; std::string entry_name; unsigned long crc; };
    bool storeZipContents(const std::string& filepath,
                          const std::vector<std::pair<std::string, unsigned long>>& entries);
    bool getAllZipContents(std::vector<ZipContentRow>& out);
    // Freshness of zip_contents, for a scan that reads archives into the cache
    // without FinalBurn Neo's per-file bookkeeping (the MAME scan, see
    // RomScanner::scan_into_cache). A stamp records the size and mtime of the
    // file the contents were read from; getZipContentStamps() returns, for
    // every archive the cache describes, that stamp or failing it the
    // rom_cache row of the FinalBurn Neo scan that read it. Keys are the
    // canonical paths zip_contents stores.
    bool stampZipContents(const std::string& filepath, long long file_size, long long last_modified);
    std::unordered_map<std::string, std::pair<long long, long long>> getZipContentStamps();
    // Every archive zip_contents describes (canonical paths), and forgetting
    // some : their contents and their stamp. Scans call this for files gone
    // from the disk or out of every library, so that the cache only ever
    // describes files that are there and whoever reads it (a DAT update) need
    // not check the disk file by file. Returns how many paths were forgotten.
    std::vector<std::string> getZipContentPaths();
    int forgetZipContents(const std::vector<std::string>& filepaths);

    // DAT file management
    bool registerDatFile(const std::string& filename, const std::string& filepath, time_t last_modified, size_t file_size, int games_count);
    bool isDatFileUpToDate(const std::string& filename, time_t last_modified, size_t file_size);
    std::vector<std::string> getOutdatedDatFiles(const std::string& dat_directory);
    bool removeGamesFromDat(const std::string& filename);
    bool removeUnreferencedDatFiles(const std::vector<std::string>& existing_files);

    // ROM cache management
    bool registerRomFile(const std::string& filename, const std::string& filepath, time_t last_modified, size_t file_size, time_t dat_timestamp);
    bool isRomFileCached(const std::string& filepath, time_t last_modified, size_t file_size, time_t current_dat_timestamp);
    bool clearRomCache();
    bool clearDirectorySnapshots();
    bool cleanupRomCache(const std::vector<std::string>& rom_paths);
    // Background cache cleanup thread
    void startCacheCleanupThread(const std::vector<std::string>& rom_paths);
    void stopCacheCleanupThread();
    int getRomCacheCount();
    time_t getLastDatTimestamp();
    bool setLastDatTimestamp(time_t timestamp);
    // Set by a schema migration that left a freshly added column empty for
    // every game already loaded (roms.merge). The main window offers the DAT
    // re-read once, then clears it.
    bool needsDatResync();
    bool clearDatResyncFlag();
    // Set when the devices table first appears in a database that already
    // holds MAME sets : their <device_ref> are only known once MAME's DATs
    // are read again. The main window offers that reload once, then clears it.
    bool needsMameDatResync();
    bool clearMameDatResyncFlag();
    // Small named integers in scan_metadata (timestamps, counters), for
    // whatever a screen needs to remember between two launches.
    int64_t getScanMetadata(const std::string& key, int64_t fallback = 0);
    bool    setScanMetadata(const std::string& key, int64_t value);

    // Idem pour du texte : scan_metadata ne stocke que des entiers, et la
    // version de MAME ("0.289 (unknown)") n'y tient pas.
    std::string getMetaString(const std::string& key);
    bool        setMetaString(const std::string& key, const std::string& value);

    // --- Catalogue MAME -------------------------------------------------
    // Remplacement en bloc : on repart d'une table vide et on ne publie le
    // resultat qu'a la fin, pour qu'une regeneration interrompue ne laisse
    // pas un demi-catalogue derriere elle.
    void beginMameCatalogRebuild();
    bool insertMameMachines(const std::vector<MameMachine>& machines);
    void commitMameCatalogRebuild(const std::string& build);
    void abortMameCatalogRebuild();
    int  countMameMachines();
    // Les flippers et machines a sous forment un tiers du catalogue MAME et
    // ne se jouent pas comme le reste : ecartes par defaut, y compris du
    // compte de jeux et de la liste des systemes.
    std::vector<Game> getMameCatalog(bool include_mechanical = false);

    // Verdict de MAME sur la collection. On repart de « absent » partout :
    // `mame -verifyroms` n'ecrit aucune ligne pour ce qu'il ne trouve pas, si
    // bien que les manquants ne se deduisent que par difference.
    void resetMameStatuses();
    bool setMameStatuses(const std::vector<std::pair<std::string, std::string>>& verdicts);

    /* Le genre des machines MAME, qui ne vient PAS de MAME.
     *
     * `mame -listxml` n'expose ni genre, ni famille, ni nombre de joueurs :
     * ces champs sont une extension de notre fork FinalBurn Neo. Le genre se
     * lit donc dans catver.ini, le fichier communautaire de progetto-SNAPS,
     * et se pose ici par-dessus un catalogue deja construit.
     *
     * Rend le nombre de machines du catalogue effectivement classees.
     */
    int setMameGenres(const std::vector<std::pair<std::string, std::string>>& genres);
    // Combien de machines jouables portent un genre : ce que la carte des
    // reglages affiche pour dire si le fichier a servi a quelque chose.
    int countMameGenres();
    // How many DAT files the games table was built from.
    int countDatFiles();
    // Every distinct DAT header ("FinalBurn Neo - Arcade Games"), sorted : the
    // system folders a library laid out from these DATs is made of.
    std::vector<std::string> getDatHeaders();
    // Per DAT file (games.dat_source): how many sets and ROM entries it
    // contributed, and its header name.
    struct DatFileStats { int games = 0, roms = 0; std::string header; };
    std::map<std::string, DatFileStats> getDatFileStats();
    // progress_cb, when set, is invoked periodically (not per file) with the
    // number of files checked so far and the root currently being walked : this
    // step does a filesystem stat + DB lookup per file with no other feedback,
    // so on a large or slow (e.g. external/network) drive it can otherwise look
    // hung for minutes with nothing on screen to prove it isn't. Returning false
    // aborts the walk (cooperative cancellation : checked at the same cadence
    // the callback fires, not per file).
    std::vector<std::string> getOutdatedRomFiles(const std::vector<std::string>& rom_paths, time_t current_dat_timestamp, bool recursive = true, bool include_loose_files = true,
                                                  std::function<bool(size_t, const std::string&)> progress_cb = nullptr);
    // Find candidate games that reference a ROM with the given CRC (crc as uLong)
    // Un emulateur vide cherche dans les deux catalogues.
    std::vector<Game> getGamesByRomCrc(unsigned long crc, const std::string& emulator = "fbneo");
    // Directory snapshot helpers (used for light pre-scan to detect folder changes)
    bool getDirectorySnapshot(const std::string& path, int& file_count, time_t& last_modified);
    bool updateDirectorySnapshot(const std::string& path, int file_count, time_t last_modified);
    // Store/inspect directory file lists to detect adds/removes/renames : and
    // rewrites. Size + mtime matter: a rebuilt ROM set (RomVault, torrentzip)
    // keeps every filename identical, so a name-only comparison would report
    // "no change" and leave the old, wrong statuses in place.
    struct DirFileInfo {
        std::string filename;
        long long   size  = 0;
        long long   mtime = 0;
        bool operator==(const DirFileInfo& o) const {
            return filename == o.filename && size == o.size && mtime == o.mtime;
        }
        bool operator!=(const DirFileInfo& o) const { return !(*this == o); }
        bool operator<(const DirFileInfo& o) const { return filename < o.filename; }
    };
    bool getDirectoryFileList(const std::string& path, std::vector<DirFileInfo>& files);
    bool updateDirectoryFileList(const std::string& path, const std::vector<DirFileInfo>& files);
    bool clearDirectoryFiles();
    // Persist configured ROM roots so we can detect removed roots between scans
    bool getSavedRomRoots(std::vector<std::string>& roots);
    bool updateSavedRomRoots(const std::vector<std::string>& roots);
    // Remove all cache/snapshot entries under a given root path
    bool removeEntriesForRoot(const std::string& root_path);
    // Unconditional purge of cache entries for an explicitly-removed root (no safety threshold)
    bool purgeCacheForRoot(const std::string& root_path);
    // Re-evaluate availability statuses for games after cache changes
    bool reevaluateGamesAvailability(const std::vector<std::string>& roms_paths);
    
private:
    // Implementation commune de getAllGames / getAllGamesLight : la passe
    // de recouture des ROMs est ce qui les distingue.
    std::vector<Game> loadAllGames(bool with_roms, const std::string& emulator);

    sqlite3* m_db;
    std::string m_db_path;
    // Guards methods called from the parallel scan workers (read-only) so that
    // they don't interleave with concurrent transactions on the same handle.
    // Recursive because transactional callers may invoke other helpers that
    // also lock (e.g. updateGameStatusWithSource from within a BEGIN...COMMIT).
    mutable std::recursive_mutex m_mutex;
    // Background cache cleanup
    std::thread m_cache_cleanup_thread;
    std::atomic<bool> m_cache_cleanup_running{false};
    std::vector<std::string> m_cache_cleanup_paths;
    
    bool createTables();
    // Passe player_stats et ignored_sets a la cle (emulator, name, system).
    // Jouee une seule fois, sous garde de version dans scan_metadata.
    bool migratePlayerDataToEmulatorKey();
    // Passe `games` a UNIQUE(emulator, name, system), par reconstruction de la
    // table : SQLite ne sait pas remplacer une contrainte UNIQUE en place.
    // Jouee une seule fois, sous sa propre garde de version.
    bool migrateGamesToEmulatorKey();
    Game buildGameFromQuery(sqlite3_stmt* stmt);
    // Same check as isRomFileCached(), but against a statement the caller already
    // prepared : getOutdatedRomFiles() walks potentially tens of thousands of
    // files, and re-preparing this query per file was the single biggest cost
    // in that loop.
    bool isRomFileCachedStmt(sqlite3_stmt* stmt, const std::string& filepath, time_t last_modified, size_t file_size, time_t current_dat_timestamp);
};