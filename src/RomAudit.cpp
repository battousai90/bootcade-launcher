// src/RomAudit.cpp
#include "RomAudit.h"
#include "i18n.h"

#include <algorithm>
#include <filesystem>
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
    rep.style = RomResolve::load_style(emulator);

    // ── 1. Index the scan cache ──────────────────────────────────────────────
    // The same index the scanner's split pass reads, so the two pick the same
    // archive for a set and judge it by the same rule.
    report(cb, 2.0, _("Reading the ROM cache…"));
    RomResolve::CacheIndex index(db, roms_paths);

    // Which archives hold a given CRC : the repair-source axis ("a good copy
    // exists in mslug2.zip") and the orphan report's "copy elsewhere" flag.
    std::unordered_multimap<unsigned long, std::string> crc_to_archive;
    for (const auto& [path, a] : index.all())
        for (const auto& [crc, name] : a.name_by_crc)
            crc_to_archive.emplace(crc, path);

    rep.pool_empty = index.empty();
    log(cb, "Indexed " + std::to_string(index.size()) + " archive(s) from the scan cache; collection style: "
            + RomResolve::to_string(rep.style) + ".");
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
    // A set with no ROM to verify (a MAME device or BIOS whose zip is only
    // kept so it is not an orphan, a CHD-only machine) gets no verdict below :
    // it is not counted either, or the total would never add up.
    rep.total = 0;
    for (const auto& g : games) if (in_group(g) && (!g.roms.empty() || !g.disks.empty())) rep.total++;

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

    // The romof chain is walked through this in-memory list, never the database.
    RomResolve::GameLookup game_for = [&](const std::string& name, const std::string& system) -> Game {
        auto it = by_key.find(name + '\x1f' + system);
        return it == by_key.end() ? Game{} : games[it->second];
    };
    RomResolve::ArchiveLookup archive_for = [&](const Game& g) { return index.for_game(g); };

    // Archives named after no game that a set nevertheless claimed by content
    // (see below) : real games, not orphans.
    std::unordered_set<std::string> claimed_by_content;

    // Every set's verdict, for the BIOS dependency count below.
    std::unordered_map<std::string, std::string> status_by_key;
    status_by_key.reserve(games.size());

    for (size_t gi = 0; gi < games.size(); ++gi) {
        if (cancelled(cb)) { rep.cancelled = true; return rep; }
        if ((gi % 512) == 0)
            report(cb, 15.0 + 84.0 * (double)gi / (double)games.size(),
                   _("Auditing ") + std::to_string(gi) + " / " + std::to_string(games.size()));

        const Game& g = games[gi];
        if (!in_group(g)) continue;

        // A set of CHDs : its disk files are judged by their headers, where
        // they are expected (RomResolve::evaluate_disks). The cache knows
        // nothing of them, no archive holds them, and Fix leaves them alone.
        if (g.roms.empty() && !g.disks.empty()) {
            GameEntry e;
            e.name        = g.name;
            e.system      = g.system;
            e.description = g.description;
            e.cloneof     = g.cloneof;
            e.is_bios     = g.is_bios;
            e.is_chd      = true;
            e.dat_header  = RomResolve::expected_folder(g);
            const RomResolve::DiskResult d = RomResolve::evaluate_disks(g, roms_paths);
            e.archive       = d.folder;
            e.archive_found = !d.folder.empty();
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
            e.status  = d.status;
            e.ignored = ignored.count(g.name + '\x1f' + g.system) > 0;
            status_by_key[g.name + '\x1f' + g.system] = e.status;
            if (e.ignored) {
                rep.ignored++;
                if (!problems_only || e.status != "available") rep.games.push_back(std::move(e));
                continue;
            }
            if (e.status == "available")      rep.available++;
            else if (e.status == "incorrect") rep.incorrect++;
            else                              rep.missing++;
            if (!problems_only || e.status != "available") rep.games.push_back(std::move(e));
            continue;
        }
        if (g.roms.empty()) continue;

        GameEntry e;
        e.name        = g.name;
        e.system      = g.system;
        e.description = g.description;
        e.cloneof     = g.cloneof;
        e.is_bios     = g.is_bios;
        e.dat_header  = RomResolve::expected_folder(g);

        // Which archive should hold this set? By name first (the index scores
        // same-named candidates and breaks ties by folder, see CacheIndex).
        const RomResolve::Archive* own = index.for_game(g);
        if (own) {
            e.archive = own->path;
            e.archive_found = true;
        }

        // No archive carries this set's name. The scanner falls back to matching on
        // content alone, so a correctly-dumped set inside a differently-named ZIP
        // still counts as available : mirror that, or the audit would invent
        // "missing" sets the scanner is happy with. Mirror it exactly : the
        // scanner only ever tries archives whose name matches NO game, and
        // only upgrades a set every ROM of which is in there. An archive that
        // carries another game's name is that game's, whatever it shares :
        // a clone pinned on its parent's zip by the two ROMs they have in
        // common would report the parent's own data as "extra", and Fix
        // would then pull a correct set apart.
        if (!own) {
            // A CRC shared across many archives (a BIOS, a common expansion ROM)
            // cannot tell one archive from another and must not count as
            // evidence. Same reasoning, same threshold, as RomInbox's
            // content-discovery axis.
            constexpr size_t kMaxArchivesPerDiscriminatingCrc = 32;
            std::unordered_map<std::string, int> hits;
            int wanted = 0;   // the set's own ROMs with a CRC : what must all be there
            for (const auto& rom : g.roms) {
                if (rom.crc.empty()) continue;
                if (rom.merge.empty()) ++wanted;
                unsigned long want = strtoul(rom.crc.c_str(), nullptr, 16);
                std::unordered_set<std::string> seen;
                auto range = crc_to_archive.equal_range(want);
                for (auto it = range.first; it != range.second; ++it) {
                    const std::string stem = lower(fs::path(it->second).stem().string());
                    if (known_stems.count(stem)) continue;   // named after a game : that game's
                    seen.insert(it->second);
                }
                if (seen.size() > kMaxArchivesPerDiscriminatingCrc) continue;
                for (const auto& path : seen) if (rom.merge.empty()) hits[path]++;
            }
            int best_hits = 0;
            for (const auto& [path, n] : hits)
                if (n > best_hits) { best_hits = n; e.archive = path; }
            if (best_hits > 0 && best_hits >= wanted) {
                e.archive_found = true;
                own = index.by_path(e.archive);
                claimed_by_content.insert(e.archive);
            } else {
                e.archive.clear();
            }
        }

        // The verdict, ROM by ROM, by the one shared rule. In a split
        // collection an inherited ROM the set's own zip lacks is looked for in
        // the parent's, then the BIOS's, and the verdict says which one had it.
        RomResolve::Verdict verdict = RomResolve::evaluate(g, own, rep.style, archive_for, game_for);
        if (verdict.roms.empty()) continue;

        for (const auto& v : verdict.roms) {
            RomEntry r;
            r.name           = v.name;
            r.crc            = v.crc;
            r.size           = v.size;
            r.state          = v.state;
            r.found_as       = v.found_as;
            r.found_crc      = v.found_crc;
            r.found_in       = v.found_in;
            r.inherited      = v.inherited;
            r.inherited_from = v.inherited_from;

            if (r.state == RomState::Corrupt) {
                e.corrupt++;
                // Does a good copy exist anywhere else in the library? If so the
                // set can be repaired locally instead of re-downloaded.
                auto other = crc_to_archive.find(r.crc);
                if (other != crc_to_archive.end()) r.found_in = other->second;
            } else if (r.state == RomState::Absent) {
                auto other = crc_to_archive.find(r.crc);
                if (other != crc_to_archive.end()) r.found_in = other->second;
                e.absent++;
            } else if (r.state == RomState::WrongName) {
                e.wrong++;
            }
            e.roms.push_back(std::move(r));
        }

        // Entries physically present in the archive that no rom above needs at
        // all (RomVault calls these Purple/Brown: "not needed here"). Harmless
        // for FBNeo : it only ever reads what it asks for by name : but worth
        // surfacing so they can be swept into quarantine like anything else.
        // An inherited ROM a split zip still carries is required-but-optional,
        // not extra : it is in e.roms, so it never lands here.
        // Does the archive hold anything of this set at all? A same-named zip
        // of another system (Arcade's circus.zip next to ColecoVision's
        // "circus") answers to the name and to nothing else : it is not this
        // set's archive, and its content is not this set's "extra files".
        // Reporting it as such would let Fix pull a correct set apart.
        bool belongs = false;
        for (const auto& r : e.roms)
            if (r.state != RomState::Absent && r.inherited_from.empty()) { belongs = true; break; }
        if (own && !belongs) {
            e.archive_found = false;
            e.archive.clear();
            own = nullptr;
        }

        if (own && !e.archive.empty()) {
            // DAT rom names are already canonical (e.g. "Spider-Man: Return…").
            // Archive entry names are what needs normalizing here : many were
            // saved with '-' where the DAT has ':' (filesystem-safe substitution)
            // : same direction the cache itself normalizes in when built.
            // An entry that answered a ROM under another name (found_as) is
            // that ROM, misnamed : not an extra.
            std::unordered_set<std::string> required;
            for (const auto& r : e.roms) {
                required.insert(r.name);
                if (!r.found_as.empty() && r.inherited_from.empty()) required.insert(r.found_as);
            }
            for (const auto& name : own->entries)
                if (!required.count(name) && !required.count(RomScanner::normalize_name(name)))
                    e.extra_entries.push_back(name);
        }

        e.status = verdict.status;
        // The set's CHDs, when it has some besides its zip (single-folder MAME
        // DAT) : listed after the ROMs, judged by their headers where the set
        // keeps them (<root>/<set>/), and one verdict for the whole set.
        if (!g.disks.empty()) {
            e.has_disks  = true;
            e.zip_status = verdict.status;
            const RomResolve::DiskResult d = RomResolve::evaluate_disks(g, roms_paths);
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
            e.status = RomResolve::combine_status(verdict.status, d.status);
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
        if (claimed_by_content.count(path)) continue;
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
