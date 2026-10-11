// src/DatLayout.cpp
#include "DatLayout.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <unordered_set>

namespace DatLayout {

namespace {

unsigned long crc_of(const std::string& hex) {
    return hex.empty() ? 0 : std::strtoul(hex.c_str(), nullptr, 16);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

} // namespace

bool natural_less(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const bool da = std::isdigit((unsigned char)a[i]), db = std::isdigit((unsigned char)b[j]);
        if (da && db) {
            size_t ie = i, je = j;
            while (ie < a.size() && std::isdigit((unsigned char)a[ie])) ++ie;
            while (je < b.size() && std::isdigit((unsigned char)b[je])) ++je;
            // Compared by value : leading zeros aside, the longer run is the
            // larger number.
            size_t is = i, js = j;
            while (is + 1 < ie && a[is] == '0') ++is;
            while (js + 1 < je && b[js] == '0') ++js;
            if (ie - is != je - js) return ie - is < je - js;
            const int c = a.compare(is, ie - is, b, js, je - js);
            if (c != 0) return c < 0;
            i = ie;
            j = je;
        } else if (da != db) {
            return da;   // a number sorts before a letter, as in the reference
        } else {
            if (a[i] != b[j]) return (unsigned char)a[i] < (unsigned char)b[j];
            ++i;
            ++j;
        }
    }
    return a.size() - i < b.size() - j;
}

std::string entry_path(const std::string& name) {
    std::string out = name;
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}

Layout::Layout(const std::vector<const Game*>& sets, const std::string& mode) : m_mode(mode) {
    std::unordered_map<std::string, const Game*> by_name;
    for (const Game* g : sets) by_name.emplace(g->name, g);
    auto find = [&](const std::string& name) -> const Game* {
        auto it = by_name.find(name);
        return it == by_name.end() ? nullptr : it->second;
    };

    // A set's own ROMs : those the DAT does not mark merge=, each once.
    auto own_roms = [](const Game& g, std::vector<Entry>& out) {
        std::unordered_set<std::string> seen;
        for (const auto& r : g.roms) {
            if (r.crc.empty() || r.is_inherited()) continue;
            if (!seen.insert(r.name + '\x1f' + lower(r.crc)).second) continue;
            out.push_back({entry_path(r.name), crc_of(r.crc), (uint64_t)r.size, ""});
        }
    };
    auto own_disks = [&](const Game& g, std::vector<DiskEntry>& out) {
        std::unordered_set<std::string> seen;
        for (const auto& d : g.disks) {
            if (!d.merge.empty() || !seen.insert(d.name).second) continue;
            if (disk_is_parents(g, d, find(g.cloneof))) continue;   // the parent's one copy
            out.push_back({d.name, lower(d.sha1), ""});
        }
    };
    auto add_archive = [&](Archive&& a, const std::string& set) {
        if (a.entries.empty() && a.disks.empty()) return;
        const size_t idx = m_archives.size();
        m_archive_of[set] = idx;
        std::vector<size_t> e(a.entries.size()), d(a.disks.size());
        for (size_t i = 0; i < e.size(); ++i) e[i] = i;
        for (size_t i = 0; i < d.size(); ++i) d[i] = i;
        m_entries_of[set] = std::move(e);
        m_disks_of[set] = std::move(d);
        m_archives.push_back(std::move(a));
    };

    if (mode == "merged") {
        // Each parent's archive, and every clone attached to the parent at
        // the top of its cloneof chain (within this DAT).
        auto top = [&](const Game* g) {
            for (int depth = 0; depth < 8 && !g->cloneof.empty(); ++depth) {
                const Game* up = find(g->cloneof);
                if (!up || up == g) break;
                g = up;
            }
            return g;
        };
        std::unordered_map<std::string, std::vector<const Game*>> clones;
        std::vector<const Game*> parents;
        for (const Game* g : sets) {
            const Game* t = top(g);
            if (t == g) parents.push_back(g);
            else clones[t->name].push_back(g);
        }
        for (const Game* p : parents) {
            Archive a;
            a.name = p->name;
            own_roms(*p, a.entries);
            own_disks(*p, a.disks);
            std::vector<size_t> p_entries(a.entries.size()), p_disks(a.disks.size());
            for (size_t i = 0; i < p_entries.size(); ++i) p_entries[i] = i;
            for (size_t i = 0; i < p_disks.size(); ++i) p_disks[i] = i;

            std::unordered_map<unsigned long, size_t> by_crc;
            for (size_t i = 0; i < a.entries.size(); ++i) by_crc.emplace(a.entries[i].crc, i);
            std::unordered_map<std::string, size_t> by_disk;
            for (size_t i = 0; i < a.disks.size(); ++i) by_disk.emplace(a.disks[i].name, i);

            auto& cl = clones[p->name];
            std::sort(cl.begin(), cl.end(), [](const Game* x, const Game* y) { return natural_less(x->name, y->name); });
            std::vector<std::pair<std::string, std::pair<std::vector<size_t>, std::vector<size_t>>>> clone_parts;
            for (const Game* c : cl) {
                std::vector<Entry> roms;
                own_roms(*c, roms);
                std::sort(roms.begin(), roms.end(), [](const Entry& x, const Entry& y) { return natural_less(x.name, y.name); });
                std::vector<size_t> need;
                for (auto& r : roms) {
                    auto hit = by_crc.find(r.crc);
                    if (hit != by_crc.end()) { need.push_back(hit->second); continue; }
                    r.name = c->name + "/" + r.name;
                    r.owner = c->name;
                    by_crc.emplace(r.crc, a.entries.size());
                    need.push_back(a.entries.size());
                    a.entries.push_back(std::move(r));
                }
                std::vector<DiskEntry> disks;
                own_disks(*c, disks);
                std::sort(disks.begin(), disks.end(), [](const DiskEntry& x, const DiskEntry& y) { return natural_less(x.name, y.name); });
                std::vector<size_t> need_disks;
                for (auto& d : disks) {
                    auto hit = by_disk.find(d.name);
                    if (hit != by_disk.end()) { need_disks.push_back(hit->second); continue; }
                    d.owner = c->name;
                    by_disk.emplace(d.name, a.disks.size());
                    need_disks.push_back(a.disks.size());
                    a.disks.push_back(std::move(d));
                }
                clone_parts.push_back({c->name, {std::move(need), std::move(need_disks)}});
            }
            if (a.entries.empty() && a.disks.empty()) continue;
            const size_t idx = m_archives.size();
            m_archive_of[p->name] = idx;
            m_entries_of[p->name] = std::move(p_entries);
            m_disks_of[p->name] = std::move(p_disks);
            for (auto& [name, parts] : clone_parts) {
                if (parts.first.empty() && parts.second.empty()) continue;
                m_archive_of[name] = idx;
                m_entries_of[name] = std::move(parts.first);
                m_disks_of[name] = std::move(parts.second);
            }
            m_archives.push_back(std::move(a));
        }
        return;
    }

    for (const Game* g : sets) {
        Archive a;
        a.name = g->name;
        if (mode == "split") {
            own_roms(*g, a.entries);
            own_disks(*g, a.disks);
        } else {
            // As it is, or non-merged : every ROM and disk the set lists.
            std::unordered_set<std::string> seen, names;
            for (const auto& r : g->roms) {
                if (r.crc.empty() || !seen.insert(r.name + '\x1f' + lower(r.crc)).second) continue;
                names.insert(entry_path(r.name));
                a.entries.push_back({entry_path(r.name), crc_of(r.crc), (uint64_t)r.size, ""});
            }
            std::unordered_set<std::string> disk_names;
            for (const auto& d : g->disks) {
                if (disk_is_parents(*g, d, find(g->cloneof))) continue;   // the parent's one copy
                if (disk_names.insert(d.name).second) a.disks.push_back({d.name, lower(d.sha1), ""});
            }
            if (mode == "non-merged") {
                // Plus the ROMs of the devices it uses, followed through the
                // devices' own. A name the set already uses keeps its own ROM.
                std::unordered_set<std::string> visited{g->name};
                std::vector<std::string> todo(g->devices.begin(), g->devices.end());
                while (!todo.empty()) {
                    const std::string name = todo.back();
                    todo.pop_back();
                    if (!visited.insert(name).second) continue;
                    const Game* dev = find(name);
                    if (!dev) continue;
                    for (const auto& r : dev->roms) {
                        if (r.crc.empty()) continue;
                        const std::string path = entry_path(r.name);
                        if (!names.insert(path).second) continue;
                        a.entries.push_back({path, crc_of(r.crc), (uint64_t)r.size, dev->name});
                    }
                    todo.insert(todo.end(), dev->devices.begin(), dev->devices.end());
                }
            }
        }
        add_archive(std::move(a), g->name);
    }
}

const Archive* Layout::archive_of(const std::string& set) const {
    auto it = m_archive_of.find(set);
    return it == m_archive_of.end() ? nullptr : &m_archives[it->second];
}

const std::vector<size_t>& Layout::entries_of(const std::string& set) const {
    static const std::vector<size_t> none;
    auto it = m_entries_of.find(set);
    return it == m_entries_of.end() ? none : it->second;
}

const std::vector<size_t>& Layout::disks_of(const std::string& set) const {
    static const std::vector<size_t> none;
    auto it = m_disks_of.find(set);
    return it == m_disks_of.end() ? none : it->second;
}

} // namespace DatLayout
