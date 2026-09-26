// src/RomResolve.cpp
#include "RomResolve.h"

#include "AppContext.h"
#include "DatSource.h"
#include "RomScanner.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace RomResolve {
namespace {

unsigned long parse_crc_hex(const std::string& hex) {
    if (hex.empty()) return 0;
    unsigned long crc = 0;
    std::stringstream ss;
    ss << std::hex << hex;
    ss >> crc;
    return crc;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// One archive, one ROM: the scanner's historical rule, unchanged. Name first
// (raw, then normalized), CRC as the fallback that turns a stray name into
// "wrong name" rather than "absent".
struct Probe {
    RomState      state = RomState::Absent;
    std::string   entry;   // the entry that matched, when one did
    unsigned long crc = 0; // its CRC (the wrong one, for Corrupt)
};

Probe probe(const Archive* a, const std::string& wanted_name, unsigned long crc) {
    Probe p;
    if (!a) return p;
    auto it = a->crc_by_name.find(wanted_name);
    if (it == a->crc_by_name.end())
        it = a->crc_by_name.find(RomScanner::normalize_name(wanted_name));
    bool name_present = (it != a->crc_by_name.end());
    if (name_present && it->second == crc) {
        p.state = RomState::Present;
        p.entry = wanted_name;
        p.crc   = crc;
        return p;
    }
    auto by_crc = a->name_by_crc.find(crc);
    if (by_crc != a->name_by_crc.end()) {
        p.state = RomState::WrongName;
        p.entry = by_crc->second;
        p.crc   = crc;
        return p;
    }
    p.state = name_present ? RomState::Corrupt : RomState::Absent;
    if (name_present) { p.entry = wanted_name; p.crc = it->second; }
    return p;
}

bool has_data(RomState s) { return s == RomState::Present || s == RomState::WrongName; }

} // namespace

// ── Folders ─────────────────────────────────────────────────────────────────

std::string combine_status(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a == "missing" || b == "missing")     return "missing";
    if (a == "incorrect" || b == "incorrect") return "incorrect";
    return "available";
}

std::string expected_folder(const Game& g) {
    if (!g.dat_header.empty()) return g.dat_header;
    // Same naming as the producer's own headers : generate_dats writes
    // "MAME ROMs (split)" (system "ROMs (split)"), FinalBurn Neo
    // "FinalBurn Neo - Arcade Games" (system "Arcade").
    if (g.emulator == "mame") return "MAME " + g.system;
    return "FinalBurn Neo - " + g.system + " Games";
}

// ── Style ───────────────────────────────────────────────────────────────────

SetStyle style_from_string(const std::string& s) {
    std::string v = lower(s);
    if (v == "split") return SetStyle::Split;
    return SetStyle::NonMerged;
}

std::string to_string(SetStyle s) {
    switch (s) {
        case SetStyle::Split: return "split";
        default:              return "non-merged";
    }
}

SetStyle load_style() {
    nlohmann::json j;
    std::ifstream fi(AppContext::get_config_path());
    if (fi) { try { fi >> j; } catch (...) { return SetStyle::NonMerged; } }
    if (!j.contains("rom_manager") || !j["rom_manager"].is_object()) return SetStyle::NonMerged;
    const auto& rm = j["rom_manager"];
    std::string wanted = (rm.contains("library_group") && rm["library_group"].is_string()) ? rm["library_group"].get<std::string>() : "";
    if (rm.contains("dat_groups") && rm["dat_groups"].is_array()) {
        std::string first_style, chosen_style;
        for (const auto& g : rm["dat_groups"]) {
            if (!g.is_object()) continue;
            std::string style = (g.contains("set_style") && g["set_style"].is_string()) ? g["set_style"].get<std::string>() : "";
            if (first_style.empty()) first_style = style;
            if (!wanted.empty() && g.contains("id") && g["id"].is_string() && g["id"].get<std::string>() == wanted) chosen_style = style;
        }
        if (!chosen_style.empty()) return style_from_string(chosen_style);
        if (!first_style.empty())  return style_from_string(first_style);
    }
    // Before groups existed the style was one key for the whole library.
    if (rm.contains("set_style") && rm["set_style"].is_string())
        return style_from_string(rm["set_style"].get<std::string>());
    return SetStyle::NonMerged;
}

SetStyle load_style(const std::string& emulator) {
    const auto groups = DatSource::load_groups();
    if (const DatSource::Group* g = DatSource::group_for(groups, emulator))
        return style_from_string(g->set_style);
    // No group describes this emulator : FinalBurn Neo keeps the rule it
    // always had (the library group's, else the legacy key), anything else
    // is judged non-merged.
    return emulator == "fbneo" ? load_style() : SetStyle::NonMerged;
}

// ── Archive ─────────────────────────────────────────────────────────────────

void Archive::add(const std::string& entry_name, unsigned long crc) {
    crc_by_name[entry_name] = crc;
    crc_by_name[RomScanner::normalize_name(entry_name)] = crc;
    name_by_crc.emplace(crc, entry_name);
    entries.push_back(entry_name);
}

// ── The rule ────────────────────────────────────────────────────────────────

Verdict evaluate(const Game& game, const Archive* own, SetStyle style,
                 const ArchiveLookup& archive_for, const GameLookup& game_for) {
    Verdict v;
    if (game.roms.empty()) return v;

    bool all_present = true, all_correct = true;
    for (const auto& rom : game.roms) {
        if (rom.crc.empty()) continue;   // nodump: nothing to verify against

        RomVerdict r;
        r.name      = rom.name;
        r.crc       = parse_crc_hex(rom.crc);
        r.size      = (uint64_t)rom.size;
        r.inherited = rom.is_inherited();

        // The set's own archive is always consulted first, whatever the style:
        // a split collection may still carry a copy of an inherited ROM, and
        // FBNeo is happy either way.
        Probe p = probe(own, rom.name, r.crc);
        r.state = p.state;
        r.found_crc = p.crc;
        if (has_data(p.state) && p.entry != rom.name) r.found_as = p.entry;

        // Split: an inherited ROM the set's own zip cannot vouch for is looked
        // for up the romof chain (parent, then the parent's parent, then the
        // BIOS), under the name the DAT says it carries there. A ROM the DAT
        // marks as the set's own never takes this path: "it exists in some
        // other zip" is precisely what must not count as present.
        if (style == SetStyle::Split && r.inherited && !has_data(r.state) && archive_for && game_for) {
            std::string name = game.romof, system = game.system;
            for (int depth = 0; depth < 8 && !name.empty(); ++depth) {
                Game ancestor = game_for(name, system);
                if (ancestor.name.empty()) break;
                Probe q = probe(archive_for(ancestor), rom.merge, r.crc);
                if (has_data(q.state)) {
                    r.state          = q.state;
                    r.found_crc      = q.crc;
                    r.inherited_from = ancestor.name;
                    const Archive* a = archive_for(ancestor);
                    if (a) r.found_in = a->path;
                    if (q.entry != rom.name) r.found_as = q.entry;
                    break;
                }
                if (ancestor.romof == name) break;   // a set naming itself as parent
                name = ancestor.romof;
            }
        }

        if (r.state == RomState::Absent)                                    all_present = false;
        else if (r.state == RomState::WrongName || r.state == RomState::Corrupt) all_correct = false;
        v.roms.push_back(std::move(r));
    }

    if (v.roms.empty()) return v;
    if (!all_present)      v.status = "missing";
    else if (!all_correct) v.status = "incorrect";
    else                   v.status = "available";
    return v;
}

std::string status_of(const Game& game, const Archive* own, SetStyle style,
                      const ArchiveLookup& archive_for, const GameLookup& game_for) {
    return evaluate(game, own, style, archive_for, game_for).status;
}

// ── CHDs ────────────────────────────────────────────────────────────────────

std::string chd_header_sha1(const std::string& path) {
    // Every CHD starts with "MComprHD", the header length and the version,
    // big-endian. Where the content SHA1 (raw data + metadata : the one MAME
    // lists) sits depends on the version : checked against real v5 files
    // (offset 84) ; v4 (48) and v3 (80) follow MAME's chd.cpp.
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    unsigned char h[124] = {};
    in.read(reinterpret_cast<char*>(h), sizeof(h));
    const std::streamsize got = in.gcount();
    if (got < 16 || std::memcmp(h, "MComprHD", 8) != 0) return "";
    auto be32 = [&](int off) {
        return (uint32_t(h[off]) << 24) | (uint32_t(h[off + 1]) << 16) | (uint32_t(h[off + 2]) << 8) | uint32_t(h[off + 3]);
    };
    const uint32_t length = be32(8), version = be32(12);
    int offset = -1;
    if (version == 5 && length >= 124) offset = 84;
    else if (version == 4 && length >= 108) offset = 48;
    else if (version == 3 && length >= 120) offset = 80;
    if (offset < 0 || got < offset + 20) return "";
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(40);
    for (int i = 0; i < 20; ++i) {
        out += hex[h[offset + i] >> 4];
        out += hex[h[offset + i] & 15];
    }
    return out;
}

DiskResult evaluate_disks(const Game& game, const std::vector<std::string>& roots,
                          const GameLookup& game_for) {
    DiskResult res;
    if (game.disks.empty()) return res;
    std::error_code ec;

    // MAME CHDs are kept merged, whatever the style of the ROMs : every disk
    // of a clone sits in its parent's folder, its own ones included, and a
    // disk the BIOS provides (lindbios) stays in the BIOS's folder. So a
    // disk is looked for in the set's own folder first (a split copy), then
    // in each ancestor's, up the cloneof / romof chain. Each ancestor's
    // disks are kept : a file there with the right name and the wrong SHA1
    // is that ancestor's own disk, not a bad copy of this one.
    struct Home { std::string set; std::unordered_set<std::string> own_sha1; };
    std::vector<Home> homes{{game.name, {}}};
    {
        std::unordered_set<std::string> seen{game.name};
        std::vector<std::string> next;
        for (const std::string& up : {game.cloneof, game.romof})
            if (!up.empty()) next.push_back(up);
        for (int depth = 0; depth < 8 && !next.empty(); ++depth) {
            std::vector<std::string> after;
            for (const auto& name : next) {
                if (!seen.insert(name).second) continue;
                Home h{name, {}};
                if (game_for) {
                    const Game up = game_for(name, game.system);
                    for (const auto& d : up.disks) h.own_sha1.insert(lower(d.sha1));
                    for (const std::string& above : {up.cloneof, up.romof})
                        if (!above.empty()) after.push_back(above);
                }
                homes.push_back(std::move(h));
            }
            next = std::move(after);
        }
    }

    bool all_present = true, all_correct = true;
    for (const auto& d : game.disks) {
        DiskVerdict v;
        v.name = d.name;
        v.sha1 = lower(d.sha1);
        // Its name in the set, and, for a disk the DAT marks merge=, the
        // name its owner gives it.
        std::vector<std::string> names{d.name};
        if (!d.merge.empty() && d.merge != d.name) names.push_back(d.merge);
        const std::string folder = expected_folder(game);
        for (const auto& root : roots) {
            if (root.empty()) continue;
            for (size_t hi = 0; hi < homes.size() && v.state != RomState::Present; ++hi) {
                const Home& home = homes[hi];
                std::vector<fs::path> candidates;
                for (const auto& disk : names) {
                    candidates.push_back(fs::path(root) / home.set / (disk + ".chd"));
                    if (!folder.empty()) candidates.push_back(fs::path(root) / folder / home.set / (disk + ".chd"));
                }
                for (const fs::path& p : candidates) {
                    if (!fs::is_regular_file(p, ec)) continue;
                    const std::string sha1 = chd_header_sha1(p.string());
                    if (sha1 == v.sha1) {
                        v.state = RomState::Present;
                        v.path = p.string();
                        v.found_sha1 = sha1;
                        break;
                    }
                    // In an ancestor's folder, a wrong file only says
                    // something when it is not one of that ancestor's own
                    // disks ; without the ancestor's description, nothing.
                    const bool ancestors_own = hi > 0 && (!game_for || home.own_sha1.count(sha1));
                    if (v.state == RomState::Absent && !ancestors_own) {   // the first wrong copy, until a good one turns up
                        v.state = RomState::Corrupt;
                        v.path = p.string();
                        v.found_sha1 = sha1;
                    }
                }
            }
            if (v.state == RomState::Present) break;
        }
        if (v.state == RomState::Absent)       all_present = false;
        else if (v.state != RomState::Present) all_correct = false;
        if (res.folder.empty() && !v.path.empty()) res.folder = fs::path(v.path).parent_path().string();
        res.disks.push_back(std::move(v));
    }
    if (!all_present)      res.status = "missing";
    else if (!all_correct) res.status = "incorrect";
    else                   res.status = "available";
    return res;
}

// ── CacheIndex ──────────────────────────────────────────────────────────────

bool under_any_root(const std::string& path, const std::vector<std::string>& roots) {
    for (std::string root : roots) {
        while (root.size() > 1 && root.back() == '/') root.pop_back();
        if (root.empty()) continue;
        if (path.size() > root.size() && path.compare(0, root.size(), root) == 0 && path[root.size()] == '/')
            return true;
    }
    return false;
}

CacheIndex::CacheIndex(std::shared_ptr<DatabaseManager> db, const std::vector<std::string>& roots_in,
                       OnDisk on_disk) {
    std::error_code ec;
    std::vector<DatabaseManager::ZipContentRow> rows;
    db->getAllZipContents(rows);

    // The roots are canonicalised once ; the paths of the cache already are,
    // so they are compared as strings : no disk access per archive.
    std::vector<std::string> roots;
    for (const auto& r : roots_in)
        if (!r.empty()) roots.push_back(fs::weakly_canonical(fs::path(r), ec).string());

    // One DAT, one folder named after it. Compared without case : RomVault
    // and the user name folders as they like ("Mame" for the DAT "MAME").
    for (const auto& h : db->getDatHeaders()) m_headers.insert(lower(h));

    std::unordered_map<std::string, bool> usable;
    for (const auto& r : rows) {
        auto u = usable.find(r.filepath);
        if (u == usable.end()) {
            // The root first : a string compare, where exists() is a disk
            // access, and on a USB drive a slow one.
            bool ok = roots.empty() || under_any_root(r.filepath, roots);
            if (ok && on_disk == OnDisk::Verify) ok = fs::exists(fs::path(r.filepath), ec);
            u = usable.emplace(r.filepath, ok).first;
            if (ok) {
                m_by_stem[lower(fs::path(r.filepath).stem().string())].push_back(r.filepath);
                m_archives[r.filepath].path = r.filepath;
            }
        }
        if (!u->second) continue;
        m_archives[r.filepath].add(r.entry_name, r.crc);
    }
}

const Archive* CacheIndex::by_path(const std::string& path) const {
    auto it = m_archives.find(path);
    return it == m_archives.end() ? nullptr : &it->second;
}

const Archive* CacheIndex::for_game(const Game& game) const {
    auto cand = m_by_stem.find(lower(game.name));
    if (cand == m_by_stem.end() || cand->second.empty()) return nullptr;

    // Several same-named archives can coexist (same short name under two
    // systems; an outbox added to roms_paths mirroring the library's folder
    // names). Folder name alone cannot break the tie : a broken duplicate in an
    // outbox sits under the very same folder name as the good copy. Score each
    // candidate by how many of the set's ROMs it satisfies by name and CRC; the
    // folder name only settles a genuine tie.
    const std::string folder = expected_folder(game);
    const Archive* best = nullptr;
    int  best_score = -1;
    bool best_dir_match = false;
    for (const auto& path : cand->second) {
        const Archive* a = by_path(path);
        if (!a) continue;
        const std::string parent_dir = lower(fs::path(path).parent_path().filename().string());
        const bool dir_match = !folder.empty() && parent_dir == lower(folder);
        // One DAT, one folder. Two MAME DATs may define the same name on
        // purpose (Pleasuredome's neogeo in "ROMs (split)" holds its own ROMs,
        // in "ROMs (bios-devices)" everything it needs) : the zip in the other
        // DAT's folder is that DAT's set, however well it scores. A folder
        // named after no DAT stays open to every set.
        if (game.emulator == "mame" && !dir_match && m_headers.count(parent_dir))
            continue;
        int score = 0;
        for (const auto& rom : game.roms) {
            if (rom.crc.empty()) continue;
            unsigned long want = parse_crc_hex(rom.crc);
            auto it = a->crc_by_name.find(rom.name);
            if (it == a->crc_by_name.end())
                it = a->crc_by_name.find(RomScanner::normalize_name(rom.name));
            if (it != a->crc_by_name.end() && it->second == want) ++score;
        }
        if (score > best_score || (score == best_score && dir_match && !best_dir_match)) {
            best = a;
            best_score = score;
            best_dir_match = dir_match;
        }
    }
    return best;
}

// ── Cache pass for inherited ROMs ───────────────────────────────────────────

int resolve_inherited_from_cache(std::shared_ptr<DatabaseManager> db,
                                 const std::vector<std::string>& roots,
                                 SetStyle style,
                                 const std::unordered_set<std::string>& touched,
                                 const std::string& emulator) {
    if (style != SetStyle::Split) return 0;

    CacheIndex index(db, roots);
    if (index.empty()) return 0;

    // One emulator's sets : the romof chain of a FinalBurn Neo clone must
    // never land on MAME's parent of the same name, nor the reverse.
    std::vector<Game> games = db->getAllGames(emulator);
    std::unordered_map<std::string, size_t> by_key;
    by_key.reserve(games.size());
    for (size_t i = 0; i < games.size(); ++i)
        by_key[games[i].name + '\x1f' + games[i].system] = i;

    GameLookup game_for = [&](const std::string& name, const std::string& system) -> Game {
        auto it = by_key.find(name + '\x1f' + system);
        return it == by_key.end() ? Game{} : games[it->second];
    };
    ArchiveLookup archive_for = [&](const Game& g) { return index.for_game(g); };

    // "Affected by what was just scanned": the set itself, or any of its
    // ancestors. Walked through the in-memory list, never the database.
    auto is_touched = [&](const Game& g) {
        if (touched.empty()) return true;
        if (touched.count(lower(g.name))) return true;
        std::string name = g.romof;
        for (int depth = 0; depth < 8 && !name.empty(); ++depth) {
            if (touched.count(lower(name))) return true;
            auto it = by_key.find(name + '\x1f' + g.system);
            if (it == by_key.end()) break;
            const std::string& next = games[it->second].romof;
            if (next == name) break;
            name = next;
        }
        return false;
    };

    int changed = 0;
    db->beginTransaction();
    for (const auto& g : games) {
        bool inherits = std::any_of(g.roms.begin(), g.roms.end(),
                                    [](const Rom& r) { return r.is_inherited(); });
        if (!inherits || !is_touched(g)) continue;

        const Archive* own = index.for_game(g);
        if (!own) continue;   // the cache has nothing to say about this set

        std::string status = status_of(g, own, style, archive_for, game_for);
        if (status.empty() || status == g.status) continue;
        // With the folder the set's own archive sits in, like the live scan
        // records it : that is what lets a removed ROM directory take its
        // sets' statuses away with it.
        db->updateGameStatusWithSource(g.name, status, g.system,
                                       fs::path(own->path).parent_path().string(), emulator);
        ++changed;
    }
    db->commitTransaction();
    return changed;
}

// ── Every status of one emulator, from the cache ────────────────────────────

namespace {

// What resolve_all_from_cache and resolve_changed_from_cache share : one
// emulator's sets in memory, the cache index, and the verdict of one set.
struct CachePass {
    CacheIndex index;
    std::vector<Game> games;
    std::unordered_map<std::string, size_t> by_key;   // "name\x1fsystem" → games[i]
    GameLookup    game_for;
    ArchiveLookup archive_for;

    CachePass(std::shared_ptr<DatabaseManager> db, const std::vector<std::string>& roots,
              const std::string& emulator, CacheIndex::OnDisk on_disk)
        : index(db, roots, on_disk), games(db->getAllGames(emulator)) {
        by_key.reserve(games.size());
        for (size_t i = 0; i < games.size(); ++i)
            by_key[games[i].name + '\x1f' + games[i].system] = i;
        game_for = [this](const std::string& name, const std::string& system) -> Game {
            auto it = by_key.find(name + '\x1f' + system);
            return it == by_key.end() ? Game{} : games[it->second];
        };
        archive_for = [this](const Game& g) { return index.for_game(g); };
    }

    // Judges `g` and writes its status when it changed. `need_own` : leave the
    // set alone when the cache holds no archive of its own.
    void judge(std::shared_ptr<DatabaseManager> db, const Game& g, const std::vector<std::string>& roots,
               SetStyle style, const std::string& emulator, bool need_own, CacheResolveResult& out) {
        // A CHD-only set (MAME's "CHDs (merged)" DAT, or a machine of the
        // single-folder DAT with no ROM of its own) : judged by the headers of
        // its disk files, which no cache holds. A few hundred small reads.
        if (g.roms.empty() && !g.disks.empty()) {
            const DiskResult d = evaluate_disks(g, roots, game_for);
            ++out.evaluated;
            if (d.status == "available")      ++out.available;
            else if (d.status == "incorrect") ++out.incorrect;
            else                              ++out.missing;
            if (d.status == g.status) return;
            db->updateGameStatusWithSource(g.name, d.status, g.system, d.folder, emulator);
            ++out.changed;
            return;
        }
        // A set with no ROM to verify : nothing to say, and no status is
        // invented for it.
        if (g.roms.empty()) return;

        const Archive* own = index.for_game(g);
        if (!own && need_own) return;
        Verdict v = evaluate(g, own, style, archive_for, game_for);
        // A zip and CHDs (single-folder MAME DAT : kinst) : one verdict for
        // the set, the zip's and the disks' together.
        if (!g.disks.empty()) v.status = combine_status(v.status, evaluate_disks(g, roots, game_for).status);
        if (v.status.empty()) return;    // only nodumps
        ++out.evaluated;
        if (v.status == "available")      ++out.available;
        else if (v.status == "incorrect") ++out.incorrect;
        else                              ++out.missing;
        if (v.status == g.status) return;
        // The folder of the set's own archive, like the live scan records it :
        // what lets a ROM directory removed from Settings take its sets'
        // statuses away with it (resetGamesFromDirectory).
        const std::string where = own ? fs::path(own->path).parent_path().string() : std::string();
        db->updateGameStatusWithSource(g.name, v.status, g.system, where, emulator);
        ++out.changed;
    }
};

} // namespace

CacheResolveResult resolve_all_from_cache(std::shared_ptr<DatabaseManager> db,
                                          const std::vector<std::string>& roots,
                                          SetStyle style,
                                          const std::string& emulator,
                                          const std::function<bool(size_t, size_t)>& progress) {
    CacheResolveResult out;
    CachePass pass(db, roots, emulator, CacheIndex::OnDisk::Verify);
    const auto& games = pass.games;

    db->beginTransaction();
    for (size_t i = 0; i < games.size(); ++i) {
        if (progress && (i % 1024) == 0 && !progress(i, games.size())) { out.cancelled = true; break; }
        pass.judge(db, games[i], roots, style, emulator, /*need_own=*/false, out);
    }
    db->commitTransaction();
    if (progress && !out.cancelled) progress(games.size(), games.size());
    return out;
}

CacheResolveResult resolve_changed_from_cache(std::shared_ptr<DatabaseManager> db,
                                              const std::vector<std::string>& roots,
                                              SetStyle style,
                                              const std::string& emulator,
                                              const std::unordered_set<std::string>& changed,
                                              const std::function<bool(size_t, size_t)>& progress) {
    CacheResolveResult out;
    if (changed.empty()) return out;
    CachePass pass(db, roots, emulator, CacheIndex::OnDisk::Trust);
    const auto& games = pass.games;

    // A set is to judge when it changed itself (whatever the cache says), or
    // when one of its ancestors did (only if its own archive is known).
    // Walked through the in-memory list, never the database.
    std::vector<std::pair<size_t, bool>> todo;   // index, need_own
    for (size_t i = 0; i < games.size(); ++i) {
        const Game& g = games[i];
        if (changed.count(g.name + '\x1f' + g.system)) { todo.emplace_back(i, false); continue; }
        std::string name = g.romof;
        for (int depth = 0; depth < 8 && !name.empty(); ++depth) {
            if (changed.count(name + '\x1f' + g.system)) { todo.emplace_back(i, true); break; }
            auto it = pass.by_key.find(name + '\x1f' + g.system);
            if (it == pass.by_key.end()) break;
            const std::string& next = games[it->second].romof;
            if (next == name) break;
            name = next;
        }
    }

    db->beginTransaction();
    for (size_t k = 0; k < todo.size(); ++k) {
        if (progress && (k % 256) == 0 && !progress(k, todo.size())) { out.cancelled = true; break; }
        pass.judge(db, games[todo[k].first], roots, style, emulator, todo[k].second, out);
    }
    db->commitTransaction();
    if (progress && !out.cancelled) progress(todo.size(), todo.size());
    return out;
}

} // namespace RomResolve
