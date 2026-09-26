// src/RomAudit.cpp
#include "RomAudit.h"
#include "i18n.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace RomAudit {
namespace {

void report(const RomInbox::Callbacks& cb, double pct, const std::string& msg) {
    if (cb.progress) cb.progress(pct, msg);
}
void log(const RomInbox::Callbacks& cb, const std::string& msg) {
    if (cb.log) cb.log(msg);
}
bool cancelled(const RomInbox::Callbacks& cb) { return cb.cancelled && cb.cancelled(); }

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

} // namespace

Report audit(std::shared_ptr<DatabaseManager> db,
             const std::vector<std::string>& roms_paths,
             bool problems_only,
             const RomInbox::Callbacks& cb,
             const std::set<std::string>& dat_sources,
             const std::string& emulator) {
    Report rep;
    rep.emulator = emulator;

    // ── 1. Index the scan cache ──────────────────────────────────────────────
    report(cb, 2.0, _("Reading the ROM cache…"));
    RomResolve::CacheIndex index(db, roms_paths);

    // Which archives hold a given CRC : the repair-source axis ("a good copy
    // exists in mslug2.zip") and the orphan report's "copy elsewhere" flag.
    std::unordered_multimap<unsigned long, std::string> crc_to_archive;
    for (const auto& [path, a] : index.all())
        for (const auto& [crc, name] : a.name_by_crc)
            crc_to_archive.emplace(crc, path);

    rep.pool_empty = index.empty();
    log(cb, "Indexed " + std::to_string(index.size()) + " archive(s) from the scan cache.");
    if (rep.pool_empty) {
        log(cb, "  WARNING: the cache is empty : run a ROM scan first.");
        report(cb, 100.0, _("Nothing to audit."));
        return rep;
    }

    // ── 2. Walk every game in the database ───────────────────────────────────
    report(cb, 12.0, _("Loading the game list…"));
    // One emulator's sets, those of the audited group. The romof chain and the
    // orphan check still see every set of that emulator : a parent outside
    // the group is still a parent. They never see the other emulator's :
    // MAME's mslug is not the parent of a FinalBurn Neo clone, and a MAME zip
    // is not "a known game" to a FinalBurn Neo library, nor the reverse. Only
    // the verdicts reported are the group's.
    std::vector<Game> games = db->getAllGames(emulator);
    auto in_group = [&](const Game& g) { return dat_sources.empty() || dat_sources.count(g.dat_source) > 0; };

    // Sets the user asked not to hear about again (see DatabaseManager::ignoreSet).
    std::unordered_set<std::string> ignored;
    for (const auto& ig : db->getIgnoredSets(emulator)) ignored.insert(ig.name + '\x1f' + ig.system);

    // Every short name the current DAT knows about, regardless of which exact
    // archive ends up "claimed" for it. The same short name legitimately exists
    // under several systems (mslug is Arcade, Neo Geo *and* GBA's "Metal Slug
    // Advance"), and if two roots both happen to have a folder with the same
    // name : e.g. the library's own "…GBA Games" and an outbox someone also
    // added to roms_paths : the per-game archive picker can only claim one of
    // the look-alikes. The other must not be reported as an orphan just
    // because it lost that coin flip; it is exactly as real a game.
    std::unordered_set<std::string> known_stems;
    std::unordered_map<std::string, std::vector<size_t>> games_by_stem;
    std::unordered_map<std::string, size_t> by_key;
    by_key.reserve(games.size());
    for (size_t i = 0; i < games.size(); ++i) {
        known_stems.insert(lower(games[i].name));
        games_by_stem[lower(games[i].name)].push_back(i);
        by_key[games[i].name + '\x1f' + games[i].system] = i;
    }

    // Every set's verdict, for the BIOS dependency count below.
    std::unordered_map<std::string, std::string> status_by_key;
    status_by_key.reserve(games.size());

    // What each DAT of the group says every archive of its folder holds :
    // its sets, read with ITS merge mode (DatLayout). A set is judged against
    // that list and nothing else : never against a parent's or a device's
    // archive.
    const auto readings = RomResolve::dat_readings(db);
    std::map<std::string, std::vector<const Game*>> sets_by_dat;
    for (const auto& g : games) if (in_group(g)) sets_by_dat[g.dat_source].push_back(&g);
    std::map<std::string, std::unique_ptr<DatLayout::Layout>> layouts;
    std::map<std::string, std::string> folder_of;   // DAT file → its folder
    for (const auto& [dat, sets] : sets_by_dat) {
        auto rd = readings.find(dat);
        const std::string mode = rd == readings.end() ? std::string() : rd->second.mode;
        folder_of[dat] = rd == readings.end() ? std::string() : rd->second.folder;
        layouts[dat] = std::make_unique<DatLayout::Layout>(sets, mode);
        log(cb, "  " + dat + " : " + (mode.empty() ? std::string("read as it is") : mode + " sets")
                + ", folder \"" + folder_of[dat] + "\"");
    }
    // Only a set the DAT expects something for is counted.
    rep.total = 0;
    for (const auto& [dat, sets] : sets_by_dat)
        for (const Game* g : sets) {
            const auto& lay = *layouts[dat];
            if (!lay.entries_of(g->name).empty() || !lay.disks_of(g->name).empty()) rep.total++;
        }

    for (size_t gi = 0; gi < games.size(); ++gi) {
        if (cancelled(cb)) { rep.cancelled = true; return rep; }
        if ((gi % 512) == 0)
            report(cb, 15.0 + 84.0 * (double)gi / (double)games.size(),
                   _("Auditing ") + std::to_string(gi) + " / " + std::to_string(games.size()));

        const Game& g = games[gi];
        if (!in_group(g)) continue;
        const DatLayout::Layout& lay = *layouts[g.dat_source];
        const DatLayout::Archive* arc = lay.archive_of(g.name);
        const auto& need       = lay.entries_of(g.name);
        const auto& need_disks = lay.disks_of(g.name);
        if (!arc || (need.empty() && need_disks.empty())) continue;   // the DAT expects nothing for it
        const std::string& folder = folder_of[g.dat_source];

        GameEntry e;
        e.name           = g.name;
        e.system         = g.system;
        e.description    = g.description;
        e.cloneof        = g.cloneof;
        e.is_bios        = g.is_bios;
        e.dat_header     = folder;
        e.archive_is_own = arc->name == g.name;

        // The archive : named after `arc` (the set's own, or its parent's in
        // merged), in the DAT's folder. Several candidates (the same folder
        // name under two roots) : the one that holds most of what is needed.
        const RomResolve::Archive* own = nullptr;
        if (!arc->entries.empty()) {
            int best = -1;
            for (const RomResolve::Archive* cand : index.in_folder(folder, arc->name)) {
                int score = 0;
                for (size_t i : need)
                    if (RomResolve::probe_rom(cand, arc->entries[i].name, arc->entries[i].crc) == RomState::Present) ++score;
                if (score > best) { best = score; own = cand; }
            }
            if (own) {
                e.archive = own->path;
                e.archive_found = true;
            }
        }

        std::string zip_status;
        if (!need.empty()) {
            bool all_present = true, all_correct = true;
            for (size_t i : need) {
                const DatLayout::Entry& x = arc->entries[i];
                RomEntry r;
                r.name      = x.name;
                r.crc       = x.crc;
                r.size      = x.size;
                r.inherited = !x.owner.empty() && x.owner != g.name;
                r.state     = RomResolve::probe_rom(own, x.name, x.crc, &r.found_as, &r.found_crc);
                if (r.found_as == x.name) r.found_as.clear();
                if (r.state == RomState::Corrupt || r.state == RomState::Absent) {
                    // A good copy anywhere else in the library : the set can be
                    // rebuilt locally instead of re-downloaded.
                    auto range = crc_to_archive.equal_range(r.crc);
                    for (auto it = range.first; it != range.second; ++it)
                        if (!own || it->second != own->path) { r.found_in = it->second; break; }
                }
                if (r.state == RomState::Corrupt)        { e.corrupt++; all_correct = false; }
                else if (r.state == RomState::Absent)    { e.absent++;  all_present = false; }
                else if (r.state == RomState::WrongName) { e.wrong++;   all_correct = false; }
                e.roms.push_back(std::move(r));
            }
            zip_status = !all_present ? "missing" : !all_correct ? "incorrect" : "available";

            // Entries the archive holds and its DAT does not list for it
            // (RomVault's "not needed here"). Only on the archive's own set :
            // a merged clone's row speaks for its part of its parent's archive.
            if (own && e.archive_is_own) {
                std::unordered_set<std::string> required;
                for (const auto& x : arc->entries) required.insert(x.name);
                for (const auto& r : e.roms) if (!r.found_as.empty()) required.insert(r.found_as);
                for (const auto& name : own->entries)
                    if (!required.count(name) && !required.count(RomScanner::normalize_name(name))
                        && !required.count(DatLayout::entry_path(name)))
                        e.extra_entries.push_back(name);
            }
        }
        e.status = zip_status;

        // Its CHDs, in the folder of its archive's set.
        if (!need_disks.empty()) {
            std::vector<DatLayout::DiskEntry> disks;
            for (size_t i : need_disks) disks.push_back(arc->disks[i]);
            const RomResolve::DiskResult d = RomResolve::evaluate_layout_disks(arc->name, disks, roms_paths, folder);
            for (const auto& v : d.disks) {
                RomEntry r;
                r.name       = v.name + ".chd";
                r.state      = v.state;
                r.is_disk    = true;
                r.sha1       = v.sha1;
                r.found_sha1 = v.found_sha1;
                r.found_in   = v.path;
                if (r.state == RomState::Absent)       e.absent++;
                else if (r.state == RomState::Corrupt) e.corrupt++;
                e.roms.push_back(std::move(r));
            }
            if (need.empty()) {
                // A set of CHDs only : its "archive" is its folder.
                e.is_chd        = true;
                e.archive       = d.folder;
                e.archive_found = !d.folder.empty();
                e.status        = d.status;
            } else {
                e.has_disks  = true;
                e.zip_status = zip_status;
                e.status     = RomResolve::combine_status(zip_status, d.status);
            }
        }
        e.ignored = ignored.count(g.name + '\x1f' + g.system) > 0;
        status_by_key[g.name + '\x1f' + g.system] = e.status;

        // Repairable = nothing is truly gone. Every broken piece : absent or
        // corrupt : has a good copy in another library archive, so the set can be
        // rebuilt locally instead of re-downloaded. Computed for an ignored set
        // too : its row still says what it is, it just is not counted.
        e.repairable = (e.status != "available");
        for (const auto& r : e.roms) {
            // A CHD is never rebuilt : one that is not right keeps the set
            // from being repaired, whatever `found_in` says of it.
            if (r.is_disk ? r.state != RomState::Present
                          : ((r.state == RomState::Absent || r.state == RomState::Corrupt) && r.found_in.empty())) {
                e.repairable = false;
                break;
            }
        }

        // An ignored set keeps its real status (so "why did I ignore this?"
        // still has an answer) but is a bucket of its own: not a problem to
        // count, not a repair to offer.
        if (e.ignored) {
            rep.ignored++;
            if (!problems_only || e.status != "available" || !e.extra_entries.empty())
                rep.games.push_back(std::move(e));
            continue;
        }

        if (e.status == "available")      rep.available++;
        else if (e.status == "incorrect") rep.incorrect++;
        else                              rep.missing++;
        if (e.repairable) rep.repairable++;

        if (!problems_only || e.status != "available" || !e.extra_entries.empty())
            rep.games.push_back(std::move(e));
    }

    // ── 2b. BIOS sets that are not there, and what they take down with them ─
    // Walked through the in-memory list: a set depends on a BIOS when its
    // romof chain ends on it.
    {
        std::unordered_map<std::string, int> dependents;   // bios key → count
        for (const auto& g : games) {
            std::string name = g.romof, system = g.system;
            for (int depth = 0; depth < 8 && !name.empty(); ++depth) {
                auto it = by_key.find(name + '\x1f' + system);
                if (it == by_key.end()) break;
                const Game& anc = games[it->second];
                if (anc.is_bios) { dependents[name + '\x1f' + system]++; break; }
                if (anc.romof == name) break;
                name = anc.romof;
            }
        }
        for (const auto& g : games) {
            if (!g.is_bios || !in_group(g)) continue;
            auto st = status_by_key.find(g.name + '\x1f' + g.system);
            if (st == status_by_key.end() || st->second == "available") continue;
            auto dep = dependents.find(g.name + '\x1f' + g.system);
            // MAME's DATs are resolved (no romof : ROMs (split), ROMs
            // (bios-devices), CHDs) : no set runs off another's archive, so a
            // missing BIOS takes nothing down with it. It is one more missing
            // set, already counted, not a line of its own.
            if (emulator == "mame" && dep == dependents.end()) continue;
            rep.missing_bios.push_back({g.name, g.system, st->second,
                                        dep == dependents.end() ? 0 : dep->second});
        }
        std::sort(rep.missing_bios.begin(), rep.missing_bios.end(),
                  [](const BiosGap& a, const BiosGap& b) { return a.dependents > b.dependents; });
    }

    std::sort(rep.games.begin(), rep.games.end(), [](const GameEntry& a, const GameEntry& b) {
        if (a.system != b.system) return a.system < b.system;
        return a.name < b.name;
    });

    // ── 3. Whole archives matching no known game name at all ─────────────────
    // Same RomVault behaviour: an unrecognized zip is still opened and each of
    // its entries checked by CRC against the whole library, regardless of
    // whether the zip's own name matches anything. Checked against known_stems
    // rather than "did some GameEntry end up claiming this exact path" : a
    // duplicate copy of a real game (e.g. sitting in an outbox someone also
    // scans) is still a real game, just not the one instance a same-named
    // system folder in another root happened to win for its GameEntry.
    report(cb, 96.0, _("Checking for orphan archives…"));
    for (const auto& [path, a] : index.all()) {
        if (cancelled(cb)) { rep.cancelled = true; return rep; }
        // A name is not enough on its own : the archive must also hold some ROM
        // of a set so named. A zip left under a set's former name (the DAT
        // renamed MSX berserk to berzerk, and NES has a berserk of its own)
        // carries none of the NES set's data, and hiding it for its name
        // alone would leave it in the library for good.
        auto named_set_holds_it = [&](const RomResolve::Archive& arc) {
            auto it = games_by_stem.find(lower(fs::path(path).stem().string()));
            if (it == games_by_stem.end()) return false;
            for (size_t gi : it->second) {
                bool any_crc = false;
                for (const auto& rom : games[gi].roms) {
                    if (rom.crc.empty()) continue;
                    any_crc = true;
                    if (arc.name_by_crc.count(strtoul(rom.crc.c_str(), nullptr, 16))) return true;
                }
                if (!any_crc) return true;   // nothing to check it against : trust the name
            }
            return false;
        };
        if (named_set_holds_it(a)) continue;
        if (a.entries.empty()) continue;

        OrphanArchive orphan;
        orphan.path = path;
        for (const auto& name : a.entries) {
            auto crc_it = a.crc_by_name.find(name);
            if (crc_it == a.crc_by_name.end()) continue;

            OrphanEntry oe;
            oe.name = name;
            oe.crc  = crc_it->second;
            auto range = crc_to_archive.equal_range(oe.crc);
            for (auto it = range.first; it != range.second && !oe.copy_elsewhere; ++it)
                if (it->second != path) oe.copy_elsewhere = true;
            orphan.entries.push_back(std::move(oe));
        }
        rep.orphans.push_back(std::move(orphan));
    }
    std::sort(rep.orphans.begin(), rep.orphans.end(),
              [](const OrphanArchive& a, const OrphanArchive& b) { return a.path < b.path; });

    report(cb, 100.0, _("Audit complete."));
    log(cb, "Library: " + std::to_string(rep.available) + " available, " +
                std::to_string(rep.incorrect) + " incorrect, " +
                std::to_string(rep.missing) + " missing (of " +
                std::to_string(rep.total) + " sets); " +
                std::to_string(rep.repairable) + " repairable from the library itself; " +
                std::to_string(rep.ignored) + " ignored; " +
                std::to_string(rep.orphans.size()) + " orphan archive(s) matching no current DAT entry.");
    return rep;
}

} // namespace RomAudit
