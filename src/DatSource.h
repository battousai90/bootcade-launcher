// src/DatSource.h
//
// DAT groups, DAT sources, and how DAT files are kept current.
//
// A DAT GROUP is a named selection of DAT files the user organises as they
// please : "FinalBurn Neo" (every DAT), "FBNeo - GBA" (one file), "Special
// Arcade" (a hand-picked few). It is not an emulator and not a source. The
// Library audits against one group at a time, so a group is also how a
// player narrows what "complete" means for them.
//
// A DAT SOURCE is where files come from : the emulator itself (fbneo -dat),
// the Bootcade server (its dat-manifest.json, at an address this code knows),
// an address the user gives (a DAT, or an archive of DATs), one of the sites
// that publish DATs (sites()), or a folder filled by hand. The files of a source live in one folder on disk. Several groups may
// share the same source and folder : each keeps its own selection, the
// files are stored once, and the database loads the union of what active
// groups select, each file once.
//
// Groups live in config.json (rom_manager.dat_groups) : they are names,
// paths and URLs, like every other setting. The database keeps only what
// it derives from the files.
//
// The HTTP contract (dat-manifest.json, schema_version 1) is published by
// the file server next to the files it lists. The SHA256 is the only thing
// this code trusts to say "changed"; version and date are for the eye. A
// download lands in a temporary file, is checked for size and SHA256, and
// only then takes the DAT's place, atomically. A mismatch leaves the local
// DAT untouched and the temporary file gone.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace DatSource {

// Http is the Bootcade server : the key stays "http" for the groups already
// written in config.json.
enum class Kind { Emulator, Http, Url, Site, Folder };
const char* kind_key(Kind k);          // "emulator" | "http" | "url" | "site" | "folder"
Kind        kind_from_key(const std::string& s);

constexpr int kManifestSchema = 1;
// The Bootcade file server. Its addresses live here and nowhere else : the
// user never types them.
constexpr const char* kServer = "https://files.bootcade.duckdns.org";
constexpr const char* kDefaultManifestUrl = "https://files.bootcade.duckdns.org/dat/fbneo/dat-manifest.json";

// ── How one DAT is read ─────────────────────────────────────────────────────
//
// A DAT is the contract : each of its sets says what one archive (or one
// folder of CHDs) of the folder the DAT describes holds. A DAT whose sets
// are linked (cloneof / romof, merge= on a ROM or a disk, device_ref) needs
// one more thing to say that : its merge mode, as in RomVault's DAT rules.
//   split      : a set holds its own ROMs ; merge= ones stay with the
//                parent or the BIOS, devices in their own sets ;
//   non-merged : every set holds everything it runs on, parent's, BIOS's
//                and devices' ROMs included ;
//   merged     : a parent's archive holds its clones too ; clones have none.
// The mode the DAT's header declares (clrmamepro forcemerging, romcenter
// rommode) applies unless the user overrides it ; a linked DAT declaring
// nothing is split, as in RomVault. A DAT without links is read as it is :
// no mode at all. Nothing depends on where a DAT comes from or on its name.
constexpr const char* kMergeSplit     = "split";
constexpr const char* kMergeNonMerged = "non-merged";
constexpr const char* kMergeMerged    = "merged";

// The user's rule for one DAT file, kept in the group that selects it.
struct DatRule {
    std::string merge;              // "" = the DAT's own mode, else split
    bool        override_dat = false;  // use `merge` even when the DAT declares one
    std::string folder;             // where its sets go ; "" = named after the DAT's header
};

// What a DAT is, as far as reading it goes : whether its sets are linked,
// and the mode its header declares ("" when none).
struct DatTraits {
    bool        linked = false;
    std::string declared;
};

// The merge mode a DAT is read with : "" for a DAT without links.
std::string effective_merge(const DatTraits& traits, const DatRule* rule);

struct Group {
    std::string id;                 // "fbneo", "fbneo-gba", … : stable, never shown
    std::string name;               // "FinalBurn Neo", "FBNeo - GBA"
    std::string folder;             // where the source's .dat files live
    Kind        source = Kind::Http;
    // Quel emulateur ce groupe decrit. Kind::Emulator ne suffit plus a le
    // dire : FinalBurn Neo et MAME produisent tous deux leurs DAT, et le
    // gestionnaire doit savoir lequel appeler et lequel auditer. Defaut
    // 'fbneo' : les groupes deja ecrits dans config.json ne portent pas ce
    // champ et decrivent tous FinalBurn Neo.
    std::string emulator = "fbneo";  // "fbneo" | "mame"
    std::string url;                // Kind::Url : the DAT, or archive of DATs, to download
    bool        unzip = true;       // Kind::Url : unpack an archive's DATs into the folder
    std::string site;               // Kind::Site : the label of the chosen site (sites())
    // Before merge modes were per DAT, the group had one. Read only to give
    // each of the group's DATs that mode once (load_groups) ; never written.
    std::string set_style = "non-merged";
    std::map<std::string, DatRule> rules;   // DAT file name → its rule
    bool        active = true;      // an inactive group loads nothing and is not offered for audit
    // The selection. all_files: every DAT file the source provides, today
    // and tomorrow. Otherwise `files` names them one by one.
    bool        all_files = true;
    std::vector<std::string> files;
    std::string last_check;         // ISO-8601, Kind::Http
    std::string last_update;        // ISO-8601, any source: last time the folder changed through us

    // Is this DAT file part of the group?
    bool selects(const std::string& file) const;
    // Include or exclude one file. Leaving all_files for an explicit list
    // needs the list of what the source provides right now.
    void set_selected(const std::string& file, bool on, const std::vector<std::string>& provided);
};

// One active group per emulator, always : two active groups of the same
// emulator describe the same collection twice (the database keeps one row
// per set and refused the other's sets one by one). The group at `keep`
// (when given) becomes the active one of its emulator ; otherwise the first
// active one stays ; an emulator left with none gets its first group.
// True when anything changed.
constexpr size_t kNoGroup = (size_t)-1;
bool one_active_per_emulator(std::vector<Group>& groups, size_t keep = kNoGroup);

// config.json ⇄ groups. A config with no dat_groups yet gets one group built
// from the legacy keys (dat_path), so nothing changes for an existing user
// until they touch the screen. The previous shape of a group (inactive_files)
// is migrated into a selection.
std::vector<Group> load_groups();
bool               save_groups(const std::vector<Group>& groups);
// A new id from a name, unique among `taken`.
std::string        make_id(const std::string& name, const std::vector<Group>& taken);

// The DAT files present in a folder (*.dat, sorted by name).
std::vector<std::string> list_folder(const std::string& folder);

// The group a DAT file belongs to : the first active group selecting it
// whose folder holds it (the one files_to_load takes it from), null when
// none does. The group says which emulator the DAT describes : not the DAT's
// header, nor its name.
const Group* group_of(const std::vector<Group>& groups, const std::string& file);
// The rule of a DAT file : that of its group. Null when it has none.
const DatRule* rule_of(const std::vector<Group>& groups, const std::string& file);
// The emulator of the group whose folder holds the DAT at `path` ; empty
// when no group holds it.
std::string emulator_of_path(const std::string& path);

// Every DAT file the database must be built from : the union of what active
// groups select, one path per file name. Two groups pointing at different
// folders for the same file name would collide in the database (one row per
// game and system) : the first group wins and `conflicts` says so.
std::vector<std::string> files_to_load(const std::vector<Group>& groups,
                                       std::vector<std::string>* conflicts = nullptr);
// The file names one group selects among what its folder holds.
std::set<std::string> selected_in_folder(const Group& g);

// ── Which emulator the ROM manager works for ────────────────────────────────
//
// The Library audits against one group (rom_manager.library_group, else the
// first active group) and that group's emulator decides everything the
// manager does with files : which ROM directories it reads, which sets a
// file can be recognised as, where a rebuilt set belongs. Import and Outbox
// follow the same choice, so the whole window speaks for one emulator.
//
// The library group, or a default-constructed group (emulator "fbneo") when
// there is none : exactly what every job did before groups had an emulator.
Group library_group();
// The group that describes `emulator` : the library group when it is one of
// that emulator's, else the first active one, else the first one. Null when
// no group describes that emulator at all.
const Group* group_for(const std::vector<Group>& groups, const std::string& emulator);
// The ROM directories of one emulator's library, as Settings stores them
// (emulators.<id>.roms_paths). Files written before that key existed : the
// flat roms_paths / roms_path for FinalBurn Neo, mame_rompaths for MAME,
// the same fallbacks SettingsPanel::load_from_file applies.
std::vector<std::string> roms_paths_for(const std::string& emulator);

// ── Download from a site (Local folder) ─────────────────────────────────────
//
// The sites that publish MAME DATs, fetched from the author's own address :
// Bootcade never mirrors them, and every file keeps its source on screen.
// A pack per MAME version (progettosnaps), a zip per DAT and version
// (Pleasuredome), plain files always at the same address (AntoPISA).
struct Site {
    const char* emulator;   // whose DATs these are : "mame"
    const char* label;      // what the menu shows
    const char* source;     // who publishes it : "Pleasuredome"
    const char* homepage;   // their page
    const char* url;        // the file ; its version is only a starting point
};
const std::vector<Site>& sites();

// The newest address of the same file, when the address names a MAME
// version and the site lists its versions (progettosnaps, Pleasuredome) ;
// `url` itself otherwise, or on any network trouble (`error` says why).
std::string latest_url(const std::string& url, std::string& error);

// Downloads `url` into `folder`. An archive (.zip, .7z) is unpacked and its
// .dat / .xml files written there ; anything else is written under the file
// name the address carries. Every file goes through a hidden temporary name,
// so an interrupted transfer never leaves half a DAT. `written` receives the
// file names.
// With `unpack` false, an archive is kept as it is, under its own name.
bool fetch_direct(const std::string& url, const std::string& folder,
                  std::vector<std::string>& written, std::string& error,
                  const std::function<void(double pct, const std::string& message)>& progress = nullptr,
                  const std::function<bool()>& cancelled = nullptr,
                  bool unpack = true);

// Where each DAT of a folder came from, kept next to them in
// ".bootcade-sources.json" : the credit follows the files, whatever group
// uses them. source_of() is "" for a file with no recorded source.
void record_source(const std::string& folder, const std::vector<std::string>& files,
                   const Site& site, const std::string& url);
std::string source_of(const std::string& folder, const std::string& file);

// ── HTTP contract ───────────────────────────────────────────────────────────

struct RemoteFile {
    std::string name;
    uint64_t    size = 0;
    std::string sha256;
    std::string version;
    std::string date;
};

struct Manifest {
    int         schema_version = 0;
    std::string generated, group;
    std::vector<RemoteFile> files;
};

// The Bootcade server's MAME DATs : one folder per MAME channel (release,
// sooner, see MameSooner.h), each with its own dat-manifest.json.
std::string mame_manifest_url(const std::string& channel);
// The manifest a Bootcade-server group reads : FinalBurn Neo's, or the MAME
// one of the channel chosen in Settings (switching to Sooner switches its
// DATs too). Empty for an emulator the server has no DATs for.
std::string manifest_url(const Group& g);

// Fetches and parses a manifest. Refuses any schema this build does not
// know : `error` then says which one it met. Network work : call off the
// GTK thread.
bool fetch_manifest(const std::string& url, Manifest& out, std::string& error);

// Local file against the manifest. Sha256 of local files is computed here.
enum class State { UpToDate, Outdated, Missing, LocalOnly };
struct FileStatus {
    std::string name;
    State       state = State::LocalOnly;
    uint64_t    local_size = 0, remote_size = 0;
    std::string local_sha256, remote_sha256;
    std::string remote_version, remote_date;
};
std::vector<FileStatus> compare(const std::string& folder, const Manifest& manifest);

// Downloads one file of the manifest into `folder`, through a temporary
// name; size and SHA256 are checked before the rename. On any failure the
// current local file (if any) is untouched and the temporary file removed.
using Progress = std::function<void(double pct, const std::string& message)>;
bool download(const std::string& manifest_url, const RemoteFile& file,
              const std::string& folder, std::string& error,
              const Progress& progress = nullptr,
              const std::function<bool()>& cancelled = nullptr);

// SHA256 of a file, lower-case hex. Empty on error.
std::string sha256_of(const std::string& path);

// The header of a DAT file, without parsing its games.
struct Header {
    std::string name, description, version, date, author;
    std::vector<std::string> preview;   // first lines of the file, for the eye
};
Header read_header(const std::string& path, int preview_lines = 14);

std::string now_iso();

} // namespace DatSource
