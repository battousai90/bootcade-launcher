// src/RomRebuild.cpp
#include "RomRebuild.h"
#include "DatParser.h"
#include "DatSource.h"
#include "FileCopy.h"
#include "MameCatalog.h"
#include "RomArchive.h"
#include "RomManifest.h"
#include "RomResolve.h"
#include "TorrentZip.h"
#include "i18n.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace RomRebuild {
namespace {

bool is_cancelled(const Callbacks& cb) { return cb.cancelled && cb.cancelled(); }

void report(const Callbacks& cb, double pct, const std::string& msg) {
    if (cb.progress) cb.progress(pct, msg);
}

void log(const Callbacks& cb, const std::string& msg) {
    if (cb.log) cb.log(msg);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string crc_hex(unsigned long crc) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%08lx", crc);
    return buf;
}

// A ROM is known by its CRC and its size together : the DAT gives both.
std::string key_of(unsigned long crc, uint64_t size) {
    return crc_hex(crc) + ':' + std::to_string(size);
}

bool is_chd(const std::string& path) {
    return lower(fs::path(path).extension().string()) == ".chd";
}

// What a written archive of `format` looks like on disk.
bool has_format(const std::string& path, RomBuild::Format format) {
    return format == RomBuild::Format::Zip ? RomArchive::is_zip(path)
                                           : lower(fs::path(path).extension().string()) == ".7z";
}

// An archive of `format` that needs nothing more : a ZIP must be a TorrentZip.
bool conforms(const std::string& path, RomBuild::Format format) {
    return has_format(path, format) && (format != RomBuild::Format::Zip || TorrentZip::is_torrentzip(path));
}

// What a run leaves next to an archive while writing it : "<archive>.tmp",
// its TorrentZip pass "<archive>.tmp.tz", its staging folder
// "<archive>.tmp.staging". Nothing else is ever taken for one.
bool is_leftover(const std::string& name) {
    auto ends = [&](const char* tail) {
        const size_t n = std::char_traits<char>::length(tail);
        return name.size() > n && name.compare(name.size() - n, n, tail) == 0;
    };
    return ends(".tmp") || ends(".tmp.tz") || ends(".tmp.staging");
}

// Every file read, and what it holds.
struct SourceFile {
    std::string path;
    std::vector<RomArchive::Entry> entries;
    bool in_dest = false;   // the destination's own
};

struct Index {
    std::vector<SourceFile> files;
    std::unordered_map<std::string, size_t> file_of;                                  // path → files[i]
    std::unordered_map<std::string, std::vector<std::pair<size_t, size_t>>> by_key;   // CRC:size → (file, entry)
    std::unordered_map<std::string, std::string> chd_by_sha1;
};

// The files under `root`, sorted : the same folder gives the same plan.
// Folders in `skip` are not entered, nor what an interrupted run left.
std::vector<std::string> list_files(const fs::path& root, bool recursive, const std::vector<fs::path>& skip) {
    std::vector<std::string> out;
    std::error_code ec;
    auto skipped = [&](const fs::path& p) {
        const fs::path c = RomBuild::weak_canonical(p.string());
        for (const auto& s : skip) if (RomBuild::is_under(c, s)) return true;
        return false;
    };
    auto take = [&](const fs::directory_entry& e) {
        std::error_code fec;
        if (!e.is_regular_file(fec) || is_leftover(e.path().filename().string())) return;
        out.push_back(e.path().string());
    };
    if (recursive) {
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            std::error_code dec;
            if (it->is_directory(dec)) {
                if (skipped(it->path()) || is_leftover(it->path().filename().string())) it.disable_recursion_pending();
                continue;
            }
            take(*it);
        }
    } else {
        for (const auto& e : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) take(e);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The file and entry a piece is read from.
struct Choice { long file = -1; size_t entry = 0; };

// ── The DATs, whoever holds them ─────────────────────────────────────────────

// A temporary folder for the DATs pulled out of an archive, removed with it.
struct TempDir {
    fs::path dir;
    TempDir() {
        std::error_code ec;
        dir = fs::temp_directory_path(ec) / ("bootcade-rebuild-dats-" + std::to_string(std::time(nullptr)) + "-"
                                             + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(dir, ec);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(dir, ec); }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

struct Dats {
    std::vector<Game> owned;                        // DATs read from files
    std::unique_ptr<RomResolve::LayoutBook> book;   // a group's, from the database
    std::vector<std::string> all;                   // every DAT read, in order
    std::unordered_map<std::string, std::vector<const Game*>> sets_of;
    std::unordered_map<std::string, std::string> folder_of, label_of;
    std::unordered_map<std::string, DatParser::DatInfo> info_of;
};

bool is_dat_name(const std::string& name) {
    const std::string ext = lower(fs::path(name).extension().string());
    return ext == ".dat" || ext == ".xml";
}

// One DAT file read in memory : a Logiqx datafile or a MAME -listxml.
bool read_dat_file(const std::string& path, const std::string& key, std::vector<Game>& out,
                   DatParser::DatInfo& info, std::string& error, const Callbacks& cb) {
    info = DatParser::inspect(path);
    std::vector<Game> games;
    if (info.kind == DatParser::DatKind::MameListxml) {
        std::string version;
        const int n = MameCatalog::read_listxml_file(path, games, &version, [&](int done) {
            if (done % 5000 == 0) report(cb, 0.0, _("Reading the DAT…") + std::string(" ") + std::to_string(done));
            return !is_cancelled(cb);
        });
        if (n < 0) { error = is_cancelled(cb) ? "cancelled" : "cannot read " + path; return false; }
        if (info.version.empty()) info.version = version;
    } else if (info.kind == DatParser::DatKind::Datafile) {
        games = DatParser::parse(path);
        if (games.empty()) { error = "no set in " + path; return false; }
    } else {
        error = path + " is not a DAT";
        return false;
    }
    for (auto& g : games) g.dat_source = key;
    out.insert(out.end(), std::make_move_iterator(games.begin()), std::make_move_iterator(games.end()));
    return true;
}

bool load_dats(std::shared_ptr<DatabaseManager> db, const Options& o, Dats& d, std::string& error, const Callbacks& cb) {
    std::set<std::string> folders_taken;
    auto folder_for = [&](const std::string& wanted) {
        const std::string base = RomBuild::sanitize_component(wanted);
        std::string f = base;
        for (int n = 2; !folders_taken.insert(f).second; ++n) f = base + " (" + std::to_string(n) + ")";
        return f;
    };

    if (o.origin == DatOrigin::Group) {
        const auto groups = DatSource::load_groups();
        const DatSource::Group* group = nullptr;
        for (const auto& g : groups) if (g.id == o.group_id) group = &g;
        if (!group) { error = "the DAT group no longer exists"; return false; }
        if (!group->active) {
            // Not in the database (one active group per emulator) : its DATs
            // read from its folder in memory, like DAT files, the group left
            // as it is.
            for (const auto& name : dats_of_group(db, o.group_id)) {
                if (is_cancelled(cb)) { error = "cancelled"; return false; }
                report(cb, 0.0, _("Reading the DAT…") + std::string(" ") + name);
                DatParser::DatInfo info;
                if (!read_dat_file((fs::path(group->folder) / name).string(), name, d.owned, info, error, cb)) return false;
                d.info_of[name] = info;
                d.all.push_back(name);
                const std::string label = !info.name.empty()
                    ? info.name + (info.kind == DatParser::DatKind::MameListxml && !info.version.empty() ? " " + info.version : std::string())
                    : fs::path(name).stem().string();
                d.label_of[name]  = label;
                d.folder_of[name] = folder_for(label);
            }
            for (const auto& g : d.owned) d.sets_of[g.dat_source].push_back(&g);
            return true;
        }
        d.all = dats_of_group(db, o.group_id);
        d.book = std::make_unique<RomResolve::LayoutBook>(db, group->emulator);
        for (const auto& g : d.book->games())
            if (std::find(d.all.begin(), d.all.end(), g.dat_source) != d.all.end()) d.sets_of[g.dat_source].push_back(&g);
        for (const auto& dat : d.all) {
            d.info_of[dat] = DatParser::inspect((fs::path(group->folder) / dat).string());
            const auto& sets = d.sets_of[dat];
            const std::string label = sets.empty() || sets.front()->dat_header.empty() ? fs::path(dat).stem().string()
                                                                                         : sets.front()->dat_header;
            d.label_of[dat] = label;
            const std::string folder = d.book->reading_of(dat).folder;
            d.folder_of[dat] = folder_for(folder.empty() ? label : folder);
        }
        return true;
    }

    // Files : read in memory, and forgotten after the run. An archive's DAT
    // ("<archive>|<entry>") is pulled out into a temporary folder first.
    TempDir tmp;
    size_t slot = 0;
    for (const auto& key : o.files) {
        if (is_cancelled(cb)) { error = "cancelled"; return false; }
        const size_t bar = key.find('|');
        report(cb, 0.0, _("Reading the DAT…") + std::string(" ") + fs::path(key.substr(bar == std::string::npos ? 0 : bar + 1)).filename().string());
        std::string path = key;
        if (bar != std::string::npos) {
            const std::string archive = key.substr(0, bar), entry = key.substr(bar + 1);
            std::unordered_map<std::string, std::string> files;
            const std::string dir = (tmp.dir / std::to_string(slot++)).string();
            if (!RomArchive::extract(archive, {entry}, dir, files, error) || !files.count(entry)) {
                error = "cannot read " + entry + " in " + archive + (error.empty() ? "" : ": " + error);
                return false;
            }
            // Under its own name : datKind looks at the extension.
            const fs::path named = fs::path(dir) / fs::path(entry).filename();
            std::error_code ec;
            fs::rename(files[entry], named, ec);
            path = ec ? files[entry] : named.string();
        }
        DatParser::DatInfo info;
        if (!read_dat_file(path, key, d.owned, info, error, cb)) return false;
        d.info_of[key] = info;
        d.all.push_back(key);
        const std::string label = !info.name.empty()
            ? info.name + (info.kind == DatParser::DatKind::MameListxml && !info.version.empty() ? " " + info.version : std::string())
            : fs::path(key.substr(bar == std::string::npos ? 0 : bar + 1)).stem().string();
        d.label_of[key] = label;
        d.folder_of[key] = folder_for(label);
    }
    // The vector no longer grows : pointers into it are stable now.
    for (const auto& g : d.owned) d.sets_of[g.dat_source].push_back(&g);
    return true;
}

// What would give the mode a DAT cannot : said in plain words, for the
// report and the log.
std::string advice_for(const DatReport& r, const std::string& mode) {
    if (r.problems.empty()) return {};
    std::map<DatLayout::Missing, size_t> count;
    for (const auto& p : r.problems) ++count[p.kind];
    std::string out;
    auto add = [&](const std::string& line) { out += (out.empty() ? "" : " ") + line; };
    if (count.count(DatLayout::Missing::NoLinks))
        add("This DAT names no parent, clone, BIOS nor device : it lists fixed archives, which can only be rebuilt as it lists them.");
    if (count.count(DatLayout::Missing::NoDeviceRefs))
        add("It describes devices, but no set names the devices it uses (no device_ref) : their ROMs cannot be put in a non-merged set.");
    if (size_t n = count[DatLayout::Missing::MergeNotMarked])
        add(std::to_string(n) + " set(s) list a ROM of their parent or BIOS without merge= : which ROM is shared cannot be told.");
    if (size_t n = count[DatLayout::Missing::ParentRomsNotListed])
        add(std::to_string(n) + " set(s) inherit from a parent or BIOS, and the DAT marks no ROM as inherited (merge=) : which of those ROMs they use cannot be told.");
    if (size_t n = count[DatLayout::Missing::LinkTargetMissing])
        add(std::to_string(n) + " link(s) name a set no DAT read holds : add the DAT that holds it.");
    const bool mame = !r.build.empty() || lower(r.label).rfind("mame", 0) == 0;
    if (mame)
        add("A DAT with every link gives " + std::string(mode.empty() ? "it" : mode) + " : the -listxml of the same MAME build"
            + (r.build.empty() ? std::string() : " (" + r.build + ")") + ".");
    else
        add("A DAT that keeps parent/clone links and merge= marks would give " + std::string(mode.empty() ? "it" : mode) + ".");
    return out;
}

} // namespace

std::vector<std::string> dats_of_group(std::shared_ptr<DatabaseManager> db, const std::string& group_id) {
    std::vector<std::string> out;
    const auto groups = DatSource::load_groups();
    for (const auto& g : groups)
        if (g.id == group_id && !g.active) {
            for (const auto& f : DatSource::selected_in_folder(g)) out.push_back(f);
            return out;
        }
    for (const auto& [file, st] : db->getDatFileStats()) {
        const DatSource::Group* g = DatSource::group_of(groups, file);
        if (g && g->id == group_id) out.push_back(file);
    }
    return out;
}

std::vector<std::string> dats_of_folder(const std::string& folder) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(folder, fs::directory_options::skip_permission_denied, ec)) {
        std::error_code fec;
        if (!e.is_regular_file(fec)) continue;
        const std::string path = e.path().string();
        if (is_dat_name(path) && DatParser::datKind(path) != DatParser::DatKind::None) {
            out.push_back(path);
        } else if (RomArchive::looks_like_archive(path)) {
            std::vector<std::string> names;
            if (!RomArchive::list_names(path, names)) continue;
            for (const auto& n : names) if (is_dat_name(n)) out.push_back(path + '|' + n);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ── analyze ──────────────────────────────────────────────────────────────────

Report analyze(std::shared_ptr<DatabaseManager> db, const Options& options, const Callbacks& cb) {
    Report rep;
    rep.options = options;
    std::error_code ec;

    // ── The DATs, and what each can give ────────────────────────────────────
    Dats d;
    if (!load_dats(db, options, d, rep.error, cb)) {
        if (rep.error == "cancelled") { rep.cancelled = true; rep.error.clear(); }
        return rep;
    }
    std::vector<std::string> chosen;
    for (const auto& dat : d.all)
        if (std::find(options.dats.begin(), options.dats.end(), dat) != options.dats.end()) chosen.push_back(dat);
    if (chosen.empty()) { rep.error = "none of the chosen DATs could be read"; return rep; }

    // Which DAT each set belongs to, for the dependencies another DAT holds.
    std::unordered_map<std::string, std::string> dat_of_set;
    for (const auto& dat : d.all) for (const Game* g : d.sets_of[dat]) dat_of_set.emplace(g->name, dat);
    // Devices described, and device_ref links, over every DAT read : a
    // device may live in a DAT of its own.
    size_t isdevice = 0, device_refs = 0;
    for (const auto& dat : d.all) { isdevice += d.info_of[dat].isdevice; device_refs += d.info_of[dat].device_refs; }

    std::unordered_map<std::string, std::vector<const Game*>> linked_of;
    std::unordered_map<std::string, std::unordered_set<std::string>> undeterminable_of;   // DAT → sets
    for (const auto& dat : chosen) {
        auto& linked = linked_of[dat];
        for (const auto& other : d.all) if (other != dat) linked.insert(linked.end(), d.sets_of[other].begin(), d.sets_of[other].end());

        DatReport r;
        r.dat   = dat;
        r.label = d.label_of[dat];
        const auto& info = d.info_of[dat];
        r.version = info.version;
        r.build   = info.build;
        r.isdevice    = info.isdevice;
        r.device_refs = info.device_refs;
        const auto& sets = d.sets_of[dat];
        r.sets = sets.size();
        for (const Game* g : sets) {
            r.clones += !g->cloneof.empty();
            r.romofs += !g->romof.empty();
            r.disks  += g->disks.size();
            for (const auto& rom : g->roms) r.merge_roms += rom.is_inherited();
        }
        for (int m = 0; m < 4; ++m) {
            DatLayout::CheckResult c = DatLayout::check(sets, linked, kModes[m]);
            if (std::string(kModes[m]) == DatSource::kMergeNonMerged && !c.whole_dat && isdevice > 0 && device_refs == 0) {
                c.whole_dat = true;
                c.problems.insert(c.problems.begin(), {DatLayout::Missing::NoDeviceRefs, "", ""});
            }
            r.possible[m] = !c.whole_dat;
            if (options.mode == kModes[m]) {
                r.whole = c.whole_dat;
                r.problems = std::move(c.problems);
            }
        }
        r.advice = advice_for(r, options.mode);
        if (r.whole) {
            rep.refused = true;
            log(cb, "REFUSED " + r.label + " : " + r.advice);
        } else {
            for (const auto& p : r.problems) if (!p.set.empty()) undeterminable_of[dat].insert(p.set);
            if (!r.problems.empty()) log(cb, "WARNING: " + r.label + " : " + r.advice);
        }
        rep.dats.push_back(std::move(r));
    }

    // ── Every source file, and the destination's own ─────────────────────
    const fs::path dest_c = RomBuild::weak_canonical(options.dest_dir);
    const fs::path replaced_c = dest_c / kReplacedSubdir;
    std::vector<std::pair<std::string, bool>> files;   // path, in the destination
    report(cb, 0.0, _("Listing the source folder…"));
    // The destination inside the source is read once, as the destination.
    for (auto& f : list_files(options.source_dir, options.recursive, {dest_c})) files.push_back({std::move(f), false});
    if (fs::is_directory(options.dest_dir, ec))
        for (auto& f : list_files(options.dest_dir, true, {replaced_c})) files.push_back({std::move(f), true});

    Index idx;
    for (size_t i = 0; i < files.size(); ++i) {
        if (is_cancelled(cb)) { rep.cancelled = true; return rep; }
        const auto& [path, in_dest] = files[i];
        report(cb, 80.0 * (double)i / (double)files.size(), fs::path(path).filename().string());
        if (!in_dest) ++rep.source_files;
        if (is_chd(path)) {
            const std::string sha1 = RomResolve::chd_header_sha1(path);
            if (!sha1.empty()) idx.chd_by_sha1.emplace(sha1, path);
            else if (!in_dest) rep.unreadable.push_back(path);
            continue;
        }
        // A readme, a cue sheet, a picture : never ROM data.
        if (RomArchive::is_sidecar(path) || fs::path(path).filename() == kReportFile) continue;
        SourceFile sf;
        sf.path = path;
        sf.in_dest = in_dest;
        // What each ZIP's central directory states : a whole collection read in
        // minutes, not hours. A ROM whose data does not match is caught when it
        // is read to be written (another copy taken, or the set written without
        // it), and every archive written is read back in full before it is kept.
        if (!RomArchive::read_listed(path, sf.entries)) {
            if (!in_dest) rep.unreadable.push_back(path);
            continue;
        }
        const size_t at = idx.files.size();
        for (size_t e = 0; e < sf.entries.size(); ++e)
            idx.by_key[key_of(sf.entries[e].crc, sf.entries[e].size)].push_back({at, e});
        idx.file_of.emplace(path, at);
        idx.files.push_back(std::move(sf));
    }
    log(cb, "read " + std::to_string(rep.source_files) + " source file(s), " + std::to_string(rep.unreadable.size()) + " unreadable");

    // ── What each chosen DAT lays out ───────────────────────────────────────
    report(cb, 80.0, _("Laying out the DATs…"));
    const char* ext = RomBuild::extension(options.format);
    size_t dat_no = 0;
    for (const auto& r : rep.dats) {
        if (is_cancelled(cb)) { rep.cancelled = true; return rep; }
        report(cb, 80.0 + 20.0 * (double)dat_no++ / (double)rep.dats.size(), r.label);
        if (r.whole) continue;   // refused : nothing of it is laid out
        const std::string& dat = r.dat;
        const auto& sets = d.sets_of[dat];
        // The DAT alone : no rule taken from an emulator.
        const DatLayout::Layout lay(sets, options.mode, DatLayout::Options{linked_of[dat], true});
        const std::string folder = d.folder_of[dat];
        std::unordered_map<std::string, const Game*> game_of;
        for (const Game* g : sets) game_of.emplace(g->name, g);

        // The archives holding a set the DAT cannot tell.
        std::unordered_map<std::string, std::string> why_of;
        for (const auto& p : r.problems) {
            if (p.set.empty()) continue;
            const DatLayout::Archive* a = lay.archive_of(p.set);
            const std::string at = a ? a->name : p.set;
            std::string& why = why_of[at];
            if (!why.empty()) continue;
            switch (p.kind) {
                case DatLayout::Missing::MergeNotMarked:      why = p.set + " : " + p.detail + " not marked merge="; break;
                case DatLayout::Missing::ParentRomsNotListed: why = p.set + " : which ROMs of " + p.detail + " it uses is not said"; break;
                case DatLayout::Missing::LinkTargetMissing:   why = p.set + " : " + p.detail + " is in no DAT read"; break;
                default:                                      why = p.set; break;
            }
        }

        // Split and merged sets lean on their BIOS and devices : said when
        // a DAT left out holds them.
        std::unordered_map<std::string, std::set<std::string>> needs_of;   // archive set → needs
        if (options.mode == DatSource::kMergeSplit || options.mode == DatSource::kMergeMerged) {
            for (const Game* g : sets) {
                const DatLayout::Archive* a = lay.archive_of(g->name);
                if (!a) continue;
                std::vector<std::string> deps(g->devices.begin(), g->devices.end());
                const Game* cur = g;
                for (int depth = 0; depth < 8 && cur && !cur->romof.empty(); ++depth) {
                    deps.push_back(cur->romof);
                    auto in = game_of.find(cur->romof);
                    cur = in == game_of.end() ? nullptr : in->second;
                }
                for (const auto& dep : deps) {
                    if (game_of.count(dep)) continue;
                    auto other = dat_of_set.find(dep);
                    if (other == dat_of_set.end() || std::find(chosen.begin(), chosen.end(), other->second) != chosen.end()) continue;
                    needs_of[a->name].insert(dep + " (" + d.label_of[other->second] + ")");
                }
            }
        }

        for (const auto& a : lay.archives()) {
            ArchivePlan ap;
            ap.set    = a.name;
            ap.dat    = dat;
            ap.folder = folder;
            if (auto g = game_of.find(a.name); g != game_of.end()) ap.description = g->second->description;
            const fs::path base = fs::path(options.dest_dir) / folder;
            if (!a.entries.empty()) ap.dest_path = (base / (RomBuild::sanitize_component(a.name) + ext)).string();
            if (!a.disks.empty())   ap.disk_dir  = (base / RomBuild::sanitize_component(a.name)).string();
            if (auto n = needs_of.find(a.name); n != needs_of.end()) ap.needs.assign(n->second.begin(), n->second.end());

            // Each piece from the file that gives this archive the most :
            // the fewer the sources, the fewer files to open. A ZIP first,
            // the one format an entry streams out of.
            std::unordered_map<size_t, int> gives;
            for (const auto& e : a.entries) {
                auto hit = idx.by_key.find(key_of(e.crc, e.size));
                if (hit == idx.by_key.end()) continue;
                std::unordered_set<size_t> once;
                for (const auto& [f, _] : hit->second) if (once.insert(f).second) ++gives[f];
            }
            std::set<size_t> used;
            int from_sources = 0;   // found in a source, an empty ROM written from nothing aside
            for (const auto& e : a.entries) {
                Piece p;
                p.name  = e.name;
                p.crc   = e.crc;
                p.size  = e.size;
                p.owner = e.owner;
                ap.bytes += e.size;
                auto hit = idx.by_key.find(key_of(e.crc, e.size));
                if (hit == idx.by_key.end() && e.size == 0) {
                    // An empty ROM no source holds : written from nothing.
                    p.found = true;
                } else if (hit != idx.by_key.end()) {
                    Choice best;
                    long best_score = -1;
                    for (const auto& [f, en] : hit->second) {
                        const long score = gives[f] * 4L + (RomArchive::is_zip(idx.files[f].path) ? 2
                                         : !RomArchive::looks_like_archive(idx.files[f].path) ? 1 : 0);
                        if (score > best_score) { best_score = score; best = {(long)f, en}; }
                    }
                    p.found     = true;
                    p.container = idx.files[best.file].path;
                    p.entry     = idx.files[best.file].entries[best.entry].name;
                    used.insert((size_t)best.file);
                    ++from_sources;
                    const std::string at_dest = ap.dest_path;
                    for (int pass = 0; pass < 2; ++pass)
                        for (const auto& [f, en] : hit->second) {
                            if ((long)f == best.file && en == best.entry) continue;
                            if ((idx.files[f].path == at_dest) != (pass == 0)) continue;
                            if (pass == 1 && p.others.size() >= 4) break;
                            p.others.emplace_back(idx.files[f].path, idx.files[f].entries[en].name);
                        }
                }
                p.found ? ++ap.found : ++ap.missing;
                ap.pieces.push_back(std::move(p));
            }
            for (const auto& dk : a.disks) {
                DiskPiece dp;
                dp.name  = dk.name;
                dp.sha1  = dk.sha1;
                dp.owner = dk.owner;
                const fs::path there = fs::path(ap.disk_dir) / (dk.name + ".chd");
                if (fs::exists(there, ec) && RomResolve::chd_header_sha1(there.string()) == dk.sha1) {
                    dp.present = dp.found = true;
                } else if (auto hit = idx.chd_by_sha1.find(dk.sha1); hit != idx.chd_by_sha1.end()) {
                    dp.found = true;
                    dp.file  = hit->second;
                }
                if (dp.found) ++from_sources;
                dp.found ? ++ap.found : ++ap.missing;
                ap.disks.push_back(std::move(dp));
            }
            std::set<std::string> distinct;
            for (size_t f : used) distinct.insert(idx.files[f].path);
            for (const auto& dp : ap.disks) if (!dp.file.empty()) distinct.insert(dp.file);
            ap.sources = (int)distinct.size();

            // An archive holding exactly the pieces, nothing else.
            auto exact = [&](const SourceFile& sf) {
                if (sf.entries.size() != ap.pieces.size()) return false;
                std::unordered_map<std::string, std::string> want;
                for (const auto& p : ap.pieces) want.emplace(p.name, key_of(p.crc, p.size));
                for (const auto& e : sf.entries) {
                    auto w = want.find(e.name);
                    if (w == want.end() || w->second != key_of(e.crc, e.size)) return false;
                }
                return true;
            };
            const SourceFile* at_dest = nullptr;
            if (!ap.dest_path.empty())
                if (auto at = idx.file_of.find(ap.dest_path); at != idx.file_of.end() && exact(idx.files[at->second]))
                    at_dest = &idx.files[at->second];
            const bool dest_conforms = at_dest && conforms(ap.dest_path, options.format);
            if (!ap.dest_path.empty())
                if (auto at = idx.file_of.find(ap.dest_path); at != idx.file_of.end()) {
                    std::unordered_set<std::string> wanted;
                    for (const auto& p : ap.pieces) wanted.insert(key_of(p.crc, p.size));
                    std::set<std::string> held;
                    for (const auto& e : idx.files[at->second].entries)
                        if (wanted.count(key_of(e.crc, e.size))) held.insert(key_of(e.crc, e.size));
                    ap.dest_keys.assign(held.begin(), held.end());
                }
            ap.replaces = !ap.dest_path.empty() && fs::exists(ap.dest_path, ec) && (!dest_conforms || options.recompress);
            const bool disks_there = std::all_of(ap.disks.begin(), ap.disks.end(), [](const DiskPiece& dk) { return dk.present; });

            if (why_of.count(a.name)) {
                ap.status = Status::Undeterminable;
                ap.why = why_of[a.name];
            } else if (ap.missing > 0) {
                // Nothing found in any source : no archive, not even one of
                // empty ROMs written from nothing.
                ap.status = from_sources > 0 ? Status::Incomplete : Status::Impossible;
                if (ap.status == Status::Incomplete && options.write_incomplete) {
                    // Written with the pieces found, unless the destination
                    // already holds exactly those : nothing new to give it.
                    std::vector<const Piece*> got;
                    for (const auto& p : ap.pieces) if (p.found) got.push_back(&p);
                    auto holds_got = [&](const SourceFile& sf) {
                        if (sf.entries.size() != got.size()) return false;
                        std::unordered_map<std::string, std::string> want;
                        for (const auto* p : got) want.emplace(p->name, key_of(p->crc, p->size));
                        for (const auto& e : sf.entries) {
                            auto w = want.find(e.name);
                            if (w == want.end() || w->second != key_of(e.crc, e.size)) return false;
                        }
                        return true;
                    };
                    bool archive_there = got.empty();
                    if (!archive_there && !options.recompress && !ap.dest_path.empty())
                        if (auto at = idx.file_of.find(ap.dest_path); at != idx.file_of.end())
                            archive_there = holds_got(idx.files[at->second]) && conforms(ap.dest_path, options.format);
                    const bool disks_to_add = std::any_of(ap.disks.begin(), ap.disks.end(),
                                                          [](const DiskPiece& dk) { return dk.found && !dk.present; });
                    ap.archive_ok = archive_there;
                    ap.partial    = !archive_there || disks_to_add;
                    ap.replaces   = ap.replaces && !archive_there;
                }
            } else if (options.recompress) {
                ap.status = Status::Build;
            } else if ((ap.dest_path.empty() || dest_conforms) && disks_there) {
                ap.status = Status::AlreadyThere;
            } else if (ap.dest_path.empty() || dest_conforms) {
                ap.status = Status::Build;     // only CHDs to add
                ap.archive_ok = true;
            } else if (at_dest && options.format == RomBuild::Format::Zip && RomArchive::is_zip(ap.dest_path)) {
                ap.status = Status::Normalise;
                ap.from = ap.dest_path;
            } else {
                ap.status = Status::Build;
                // Already that very archive somewhere : copied as it is when
                // it conforms, normalised when it is a ZIP that does not.
                for (size_t f : used) {
                    const SourceFile& sf = idx.files[f];
                    if (sf.path == ap.dest_path || !exact(sf)) continue;
                    if (conforms(sf.path, options.format)) { ap.status = Status::Copy; ap.from = sf.path; break; }
                    if (options.format == RomBuild::Format::Zip && RomArchive::is_zip(sf.path) && ap.status == Status::Build) {
                        ap.status = Status::Normalise;
                        ap.from = sf.path;
                    }
                }
            }
            switch (ap.status) {
                case Status::Build:          ++rep.build;          break;
                case Status::Copy:           ++rep.copy;           break;
                case Status::Normalise:      ++rep.normalise;      break;
                case Status::AlreadyThere:   ++rep.already;        break;
                case Status::Incomplete:     ++rep.incomplete; rep.partial += ap.partial; break;
                case Status::Impossible:     ++rep.impossible;     break;
                case Status::Undeterminable: ++rep.undeterminable; break;
            }
            rep.archives.push_back(std::move(ap));
        }
    }
    log(cb, std::to_string(rep.archives.size()) + " archive(s) in the DATs : "
            + std::to_string(rep.build + rep.copy + rep.normalise) + " to write, " + std::to_string(rep.already) + " already there, "
            + std::to_string(rep.incomplete) + " incomplete (" + std::to_string(rep.partial) + " written with what was found), " + std::to_string(rep.impossible) + " not possible, "
            + std::to_string(rep.undeterminable) + " not determinable");
    report(cb, 100.0, "");
    return rep;
}

// ── apply ────────────────────────────────────────────────────────────────────

bool writes(const ArchivePlan& a) {
    return a.status == Status::Build || a.status == Status::Copy || a.status == Status::Normalise || a.partial;
}

bool chosen(const Report& report, const ArchivePlan& a) {
    if (!writes(a)) return false;
    if (a.selected) return true;
    return std::none_of(report.archives.begin(), report.archives.end(),
                        [](const ArchivePlan& b) { return b.selected && writes(b); });
}

namespace {

const char* status_word(Status s) {
    switch (s) {
        case Status::Build:          return "build";
        case Status::Copy:           return "copy";
        case Status::Normalise:      return "normalise";
        case Status::AlreadyThere:   return "already there";
        case Status::Incomplete:     return "incomplete";
        case Status::Impossible:     return "not possible";
        case Status::Undeterminable: return "not determinable";
    }
    return "?";
}

const char* mode_word(const std::string& m) {
    if (m.empty())                     return "as the DAT lists";
    if (m == DatSource::kMergeSplit)  return "split";
    if (m == DatSource::kMergeMerged) return "merged";
    return "non-merged";
}

std::string now_text() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return buf;
}

} // namespace

ApplyResult apply(const Report& rep, const Callbacks& cb) {
    ApplyResult res;
    const Options& o = rep.options;
    std::error_code ec;
    fs::create_directories(o.dest_dir, ec);

    // What an interrupted run left behind : only our own leftovers, never
    // an archive.
    {
        std::vector<fs::path> leftovers;
        for (auto it = fs::recursive_directory_iterator(o.dest_dir, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            if (is_leftover(it->path().filename().string())) {
                leftovers.push_back(it->path());
                std::error_code dec;
                if (it->is_directory(dec)) it.disable_recursion_pending();
            }
        for (const auto& p : leftovers) {
            std::error_code rec;
            fs::remove_all(p, rec);
            if (!rec) { ++res.cleaned; log(cb, "cleaned " + p.filename().string() + " (left by an interrupted run)"); }
        }
        ec.clear();
    }

    std::vector<const ArchivePlan*> todo;
    for (const auto& a : rep.archives)
        if (chosen(rep, a)) todo.push_back(&a);
    const bool some_checked = todo.size() < (size_t)std::count_if(rep.archives.begin(), rep.archives.end(), writes);

    // A destination archive replaced : its previous content is where it was
    // kept, for an archive later in the run that takes pieces from it.
    std::unordered_map<std::string, std::string> relocated;
    auto now_at = [&](const std::string& p) {
        auto it = relocated.find(p);
        return it == relocated.end() ? p : it->second;
    };
    // Where the archive at `path` is kept before being replaced ; "" when
    // _replaced/ already holds these very bytes.
    auto keep_path = [&](const std::string& path, std::string& existing) {
        fs::path rel = fs::relative(path, o.dest_dir, ec);
        if (ec || rel.empty() || *rel.begin() == "..") rel = fs::path(path).filename();
        ec.clear();
        const fs::path dir = fs::path(o.dest_dir) / kReplacedSubdir / rel.parent_path();
        bool same = false;
        const std::string to = RomManifest::free_destination(dir.string(), path, same);
        existing = to;
        return same ? std::string() : to;
    };
    // `tmp` in place of `dest`, the previous one kept.
    auto put_in_place = [&](const std::string& tmp, const std::string& dest, std::string& error) {
        std::string existing;
        const bool had = fs::exists(dest, ec);
        const std::string keep = had ? keep_path(dest, existing) : std::string();
        if (!RomBuild::publish(tmp, dest, keep, error)) return false;
        if (had) {
            relocated[dest] = existing;
            ++res.replaced;
            log(cb, "kept the previous " + fs::path(dest).filename().string() + " in " + kReplacedSubdir + "/");
        }
        return true;
    };
    const std::function<bool()> cancelled = [&] { return is_cancelled(cb); };

    // What became of each archive written, and the ROMs it was written without.
    std::unordered_map<const ArchivePlan*, std::string> outcome;
    std::unordered_map<const ArchivePlan*, std::vector<std::string>> lost_of;
    for (size_t i = 0; i < todo.size(); ++i) {
        if (is_cancelled(cb)) { res.cancelled = true; break; }
        const ArchivePlan& ap = *todo[i];
        report(cb, 100.0 * (double)i / (double)todo.size(), ap.set);
        std::string error;
        bool ok = true;

        std::vector<std::string> lost;   // keys of the ROMs no copy of which reads back
        bool kept = false;
        if (!ap.dest_path.empty() && !ap.archive_ok) {
            fs::create_directories(fs::path(ap.dest_path).parent_path(), ec);
            const std::string tmp = ap.dest_path + ".tmp";
            std::vector<RomBuild::Piece> pieces;
            std::vector<const Piece*> plan_of;
            for (const auto& p : ap.pieces)
                if (p.found) {
                    pieces.push_back({p.name, p.crc, p.size, p.container.empty() ? std::string() : now_at(p.container), p.entry, ""});
                    plan_of.push_back(&p);
                }
            if (ap.status == Status::Copy) {
                copy_file_robust(now_at(ap.from), tmp, true, ec);
                if (ec) { error = "cannot copy " + ap.from + ": " + ec.message(); ok = false; }
                else if (!RomBuild::verify(tmp, o.format, pieces, error)) ok = false;
                else if (o.format == RomBuild::Format::Zip && !TorrentZip::is_torrentzip(tmp)) { error = "the copy is not a TorrentZip"; ok = false; }
                if (!ok) fs::remove(tmp, ec);
            } else if (ap.status == Status::Normalise) {
                ok = RomBuild::normalise_zip(now_at(ap.from), tmp, pieces, error);
            } else {
                ok = RomBuild::build_archive(tmp, pieces, o.format, cancelled, error);
            }
            // A source whose data does not read back as its CRC fails the
            // whole archive : each piece is read and checked, another copy of
            // it taken when there is one, and the archive written again
            // without the ROMs no copy of which reads back.
            if (!ok && !is_cancelled(cb) && !RomBuild::disk_full(o.dest_dir)) {
                auto reads_back = [](const std::string& container, const std::string& entry, unsigned long crc, uint64_t size) {
                    RomBuild::Bytes data;
                    std::string why;
                    return RomBuild::read_bytes(container, entry, data, why) && data.size() == size && RomBuild::crc_of(data) == crc;
                };
                std::vector<RomBuild::Piece> good;
                bool swapped = false;   // a piece taken from another copy
                for (size_t k = 0; k < pieces.size(); ++k) {
                    RomBuild::Piece q = pieces[k];
                    bool readable = q.container.empty() || reads_back(q.container, q.entry, q.crc, q.size);
                    for (size_t alt = 0; !readable && alt < plan_of[k]->others.size(); ++alt) {
                        const auto& [c, e] = plan_of[k]->others[alt];
                        if (reads_back(now_at(c), e, q.crc, q.size)) { q.container = now_at(c); q.entry = e; readable = swapped = true; }
                    }
                    if (readable) good.push_back(std::move(q));
                    else lost.push_back(key_of(q.crc, q.size));
                    if (is_cancelled(cb)) break;
                }
                if ((swapped || !lost.empty()) && !is_cancelled(cb)) {
                    for (size_t k = 0; k < pieces.size(); ++k)
                        if (std::find(lost.begin(), lost.end(), key_of(pieces[k].crc, pieces[k].size)) != lost.end())
                            res.unreadable.push_back(ap.folder + "/" + ap.set + " : " + pieces[k].name + " (" + crc_hex(pieces[k].crc) + ")");
                    const bool any = std::any_of(good.begin(), good.end(), [](const RomBuild::Piece& q) { return !q.container.empty(); });
                    if (!o.write_incomplete) error = std::to_string(lost.size()) + " ROM(s) do not read back as their CRC ; incomplete sets are not written";
                    else if (!any) error = "no ROM of it reads back as its CRC";
                    else {
                        error.clear();
                        ok = RomBuild::build_archive(tmp, good, o.format, cancelled, error);
                        if (ok) pieces = std::move(good);
                    }
                }
            }
            // Never an archive in place traded for one that lacks some of its
            // ROMs : those the destination holds are kept, whatever happens.
            if (ok && fs::exists(ap.dest_path, ec) && !ap.dest_keys.empty()) {
                std::unordered_set<std::string> written;
                for (const auto& q : pieces) written.insert(key_of(q.crc, q.size));
                int lose = 0;
                for (const auto& k : ap.dest_keys)
                    if (!written.count(k) && std::find(lost.begin(), lost.end(), k) == lost.end()) ++lose;
                if (lose) {
                    fs::remove(tmp, ec);
                    kept = true;
                    ++res.kept;
                    log(cb, "kept    " + ap.folder + "/" + ap.set + " as it is : the new archive would lack "
                            + std::to_string(lose) + " ROM(s) it holds");
                }
            }
            if (ok && !kept) ok = put_in_place(tmp, ap.dest_path, error);
        }

        for (const auto& dk : ap.disks) {
            if (!ok) break;
            if (dk.present || !dk.found) continue;
            const fs::path to = fs::path(ap.disk_dir) / (dk.name + ".chd");
            fs::create_directories(to.parent_path(), ec);
            const std::string tmp = to.string() + ".tmp";
            copy_file_robust(dk.file, tmp, true, ec);
            if (ec) { error = "cannot copy " + dk.file + ": " + ec.message(); ok = false; }
            else if (RomResolve::chd_header_sha1(tmp) != dk.sha1) { error = "the copy of " + dk.name + ".chd does not read back"; ok = false; }
            if (!ok) { fs::remove(tmp, ec); break; }
            ok = put_in_place(tmp, to.string(), error);
            if (!ok) break;
            ++res.disks;
            res.bytes_written += fs::file_size(to, ec);
        }

        if (!lost.empty()) lost_of[&ap] = lost;
        if (ok && kept) {
            // Left in place : its CHDs are still added above.
            outcome[&ap] = "kept as it is : the new archive would lack some of its ROMs";
        } else if (ok) {
            outcome[&ap] = ap.archive_ok ? "CHDs added"
                         : lost.empty() ? "written"
                                        : "written without " + std::to_string(lost.size()) + " ROM(s) that do not read back";
            if (!ap.dest_path.empty() && !ap.archive_ok) {
                res.bytes_written += fs::file_size(ap.dest_path, ec);
                if (!lost.empty() && ap.status != Status::Incomplete) ++res.partial;
                else switch (ap.status) {
                    case Status::Copy:       ++res.copied;     break;
                    case Status::Normalise:  ++res.normalised; break;
                    case Status::Incomplete: ++res.partial;    break;
                    default:                 ++res.built;      break;
                }
            }
            log(cb, std::string(ap.status == Status::Copy ? "copied  " : ap.status == Status::Normalise ? "normalised "
                                : ap.status == Status::Incomplete ? "incomplete, written with what was found " : "rebuilt ")
                    + ap.folder + "/" + ap.set);
        } else if (is_cancelled(cb)) {
            res.cancelled = true;
            break;
        } else {
            ++res.failed;
            outcome[&ap] = "FAILED : " + error;
            res.errors.push_back(ap.set + ": " + error);
            log(cb, "FAILED  " + ap.set + ": " + error);
            // A full disk fails every archive after this one : stopped here,
            // with nothing replaced.
            if (RomBuild::disk_full(o.dest_dir)) {
                res.disk_full = true;
                log(cb, "FAILED  the destination is full : the rebuild stops here");
                break;
            }
        }
    }

    // ── The report, next to what it describes ───────────────────────────────
    res.report_path = (fs::path(o.dest_dir) / kReportFile).string();
    const std::string report_tmp = res.report_path + ".tmp";
    {
        std::ofstream out(report_tmp);
        out << "Bootcade rebuild report, " << now_text() << "\n\n"
            << "Source       : " << o.source_dir << (o.recursive ? " (with subfolders)" : "") << "\n"
            << "Destination  : " << o.dest_dir << "\n"
            << "DATs         : " << (o.origin == DatOrigin::Group ? "DAT group " + o.group_id : std::string("DAT files")) << "\n";
        for (const auto& r : rep.dats) {
            out << "  " << r.label << (r.build.empty() ? (r.version.empty() ? "" : " " + r.version) : " (" + r.build + ")") << "\n"
                << "    " << r.dat << "\n"
                << "    " << r.sets << " sets, " << r.clones << " clones, " << r.romofs << " with romof, " << r.merge_roms
                << " ROMs marked merge=, " << r.device_refs << " device_ref, " << r.isdevice << " devices, " << r.disks << " disks\n"
                << "    gives :";
            for (int m = 0; m < 4; ++m) out << " " << mode_word(kModes[m]) << (r.possible[m] ? " yes" : " no") << (m < 3 ? "," : "");
            out << "\n";
            if (!r.advice.empty()) out << "    " << (r.whole ? "REFUSED : " : "") << r.advice << "\n";
        }
        out << "Set type     : " << mode_word(o.mode) << ", " << RomBuild::extension(o.format)
            << (o.recompress ? ", every archive rewritten" : "")
            << (o.write_incomplete ? ", incomplete sets written with what was found" : ", incomplete sets not written") << "\n\n"
            << "Rebuilt " << res.built << ", copied " << res.copied << ", normalised " << res.normalised << ", CHDs " << res.disks
            << ", already there " << rep.already << ", incomplete " << rep.incomplete << " (written " << res.partial << ")"
            << ", left in place " << res.kept
            << ", not possible " << rep.impossible
            << ", not determinable " << rep.undeterminable << ", failed " << res.failed
            << (res.cancelled ? " (cancelled)" : "") << (res.disk_full ? " (stopped : destination full)" : "") << "\n"
            << "Kept in " << kReplacedSubdir << "/ before being replaced : " << res.replaced
            << ", leftovers of an interrupted run removed : " << res.cleaned << "\n\n"
            << "These sets match their DAT. Whether the emulator runs them is another matter : it may need\n"
            << "BIOS or device sets of another DAT, and a game can be listed as not working.\n";
        for (const auto& e : res.errors) out << "\nFAILED " << e;
        if (!res.errors.empty()) out << "\n";
        // Every archive : complete or not, what was recovered, what is missing.
        long expected = 0, recovered = 0;
        for (const auto& a : rep.archives) {
            if (a.status == Status::Undeterminable) continue;
            expected += a.found + a.missing;
            recovered += a.found;
            if (auto l = lost_of.find(&a); l != lost_of.end()) recovered -= (long)l->second.size();
        }
        out << "\nROMs and CHDs the DATs list : " << expected << ", recovered " << recovered << ", missing " << expected - recovered << "\n"
            << "\nArchives (complete or incomplete, recovered / listed, what was done) :\n";
        for (const auto& a : rep.archives) {
            const auto l = lost_of.find(&a);
            const int lost = l == lost_of.end() ? 0 : (int)l->second.size();
            const int got = a.found - lost, all = a.found + a.missing;
            const char* state = a.status == Status::Undeterminable ? "not determinable"
                              : got == 0                           ? "not possible"
                              : got < all                          ? "incomplete"
                                                                   : "complete";
            std::string done;
            if (auto oc = outcome.find(&a); oc != outcome.end()) done = oc->second;
            else if (a.status == Status::AlreadyThere) done = "already there";
            else if (a.status == Status::Incomplete && a.archive_ok) done = "the destination already holds what was found";
            else if (a.status == Status::Undeterminable) done = "not written (" + a.why + ")";
            else if (writes(a) && some_checked) done = "not written (not checked)";
            else if (res.cancelled || res.disk_full) done = "not written (stopped)";
            else done = "not written";
            out << "\n" << a.folder << "/" << a.set << " : " << state << " " << got << "/" << all << " ; " << done;
            for (const auto& n : a.needs) out << "\n  needs " << n;
            if (a.status != Status::Undeterminable) {
                for (const auto& p : a.pieces)
                    if (!p.found) out << "\n  missing " << p.name << "  crc " << crc_hex(p.crc) << "  size " << p.size;
                    else if (lost && std::find(l->second.begin(), l->second.end(), key_of(p.crc, p.size)) != l->second.end())
                        out << "\n  unreadable " << p.name << "  crc " << crc_hex(p.crc) << "  size " << p.size << " (no copy reads back)";
                for (const auto& dk : a.disks)
                    if (!dk.found) out << "\n  missing " << dk.name << ".chd  sha1 " << dk.sha1;
            }
        }
        out << "\n";
        for (const auto& u : rep.unreadable) out << "\nunreadable " << u;
        out << "\n";
        out.close();
        if (!out) { log(cb, "could not write " + res.report_path); res.report_path.clear(); }
    }
    if (!res.report_path.empty()) {
        fs::rename(report_tmp, res.report_path, ec);
        if (ec) { log(cb, "could not write " + res.report_path); res.report_path.clear(); }
    }
    fs::remove(report_tmp, ec);
    report(cb, 100.0, "");
    return res;
}

} // namespace RomRebuild
