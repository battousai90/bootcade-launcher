// src/RomManifest.cpp
#include "RomManifest.h"
#include "Platform.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;

namespace RomManifest {
namespace {

nlohmann::json to_json(const Entry& e) {
    nlohmann::json j;
    j["file"]       = e.file;
    j["game"]       = e.game;
    j["system"]     = e.system;
    j["dat_header"] = e.dat_header;
    j["reason"]     = e.reason;
    j["origin"]     = e.origin;
    j["action"]     = e.action;
    j["details"]    = e.details;
    j["added_at"]   = e.added_at;
    if (!e.removed_at.empty()) j["removed_at"] = e.removed_at;
    if (!e.outcome.empty())    j["outcome"]    = e.outcome;
    return j;
}

std::string str(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

Entry from_json(const nlohmann::json& j) {
    Entry e;
    e.file       = str(j, "file");
    e.game       = str(j, "game");
    e.system     = str(j, "system");
    e.dat_header = str(j, "dat_header");
    e.reason     = str(j, "reason");
    e.origin     = str(j, "origin");
    e.action     = str(j, "action");
    e.added_at   = str(j, "added_at");
    e.removed_at = str(j, "removed_at");
    e.outcome    = str(j, "outcome");
    auto d = j.find("details");
    if (d != j.end() && d->is_array())
        for (const auto& line : *d)
            if (line.is_string()) e.details.push_back(line.get<std::string>());
    return e;
}

// Relative paths are stored '/'-separated whatever the platform, so a
// manifest copied along with its folder still reads.
std::string generic(const fs::path& p) { return p.generic_string(); }

} // namespace

std::string local_time(const std::string& iso) {
    std::tm tm{};
    if (!strptime(iso.c_str(), "%Y-%m-%dT%H:%M:%SZ", &tm)) return iso;
    std::time_t t = timegm(&tm);
    std::tm lt{};
    localtime_r(&t, &lt);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &lt);
    return buf;
}

std::string now_iso() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

bool Manifest::is_manifest_file(const std::string& filename) {
    return filename == kFileName || filename == std::string(kFileName) + ".tmp";
}

Manifest Manifest::load(const std::string& root) {
    Manifest m;
    m.m_root = root;
    if (root.empty()) return m;

    std::ifstream in(fs::path(root) / kFileName);
    if (!in) return m;

    nlohmann::json j;
    try { in >> j; } catch (const std::exception& ex) {
        std::cerr << "[MANIFEST] unreadable " << root << ": " << ex.what() << " : starting empty" << std::endl;
        return m;
    }
    if (!j.is_object()) return m;

    // A newer schema is not something to guess at : read what can be read,
    // and the next save writes the current version. Entries are self-
    // describing enough that a missing field is just an empty one.
    int version = j.value("schema_version", 0);
    if (version > kSchemaVersion)
        std::cerr << "[MANIFEST] " << root << " has schema " << version
                  << ", this build knows " << kSchemaVersion << " : fields it does not know are dropped" << std::endl;

    auto read_list = [&](const char* key, std::vector<Entry>& out) {
        auto it = j.find(key);
        if (it == j.end() || !it->is_array()) return;
        for (const auto& item : *it)
            if (item.is_object()) {
                Entry e = from_json(item);
                if (!e.file.empty()) out.push_back(std::move(e));
            }
    };
    read_list("entries", m.m_entries);
    read_list("history", m.m_history);
    return m;
}

bool Manifest::save() const {
    if (m_root.empty()) return false;
    std::error_code ec;
    fs::create_directories(m_root, ec);

    nlohmann::json j;
    j["schema_version"] = kSchemaVersion;
    j["updated_at"]     = now_iso();
    j["entries"]        = nlohmann::json::array();
    j["history"]        = nlohmann::json::array();
    for (const auto& e : m_entries) j["entries"].push_back(to_json(e));
    for (const auto& e : m_history) j["history"].push_back(to_json(e));

    // Written whole to a sibling, then renamed over the old file: a crash
    // mid-write leaves the previous manifest intact rather than half a JSON.
    const fs::path final_path = fs::path(m_root) / kFileName;
    const fs::path tmp_path   = fs::path(m_root) / (std::string(kFileName) + ".tmp");
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::cerr << "[MANIFEST] cannot write " << tmp_path << std::endl;
            return false;
        }
        out << j.dump(2) << '\n';
        if (!out) return false;
    }
    fs::rename(tmp_path, final_path, ec);
    if (ec) {
        std::cerr << "[MANIFEST] cannot replace " << final_path << ": " << ec.message() << std::endl;
        fs::remove(tmp_path, ec);
        return false;
    }
    return true;
}

void Manifest::add(Entry e) {
    if (e.added_at.empty()) e.added_at = now_iso();
    for (auto& existing : m_entries) {
        if (existing.file == e.file) { existing = std::move(e); return; }
    }
    m_entries.push_back(std::move(e));
}

const Entry* Manifest::find(const std::string& rel_file) const {
    for (const auto& e : m_entries)
        if (e.file == rel_file) return &e;
    return nullptr;
}

void Manifest::remove(const std::string& rel_file, const std::string& outcome) {
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if (it->file != rel_file) continue;
        Entry gone = std::move(*it);
        m_entries.erase(it);
        gone.removed_at = now_iso();
        gone.outcome    = outcome;
        m_history.push_back(std::move(gone));
        if (m_history.size() > kMaxHistory)
            m_history.erase(m_history.begin(), m_history.begin() + (m_history.size() - kMaxHistory));
        return;
    }
}

int Manifest::reconcile() {
    std::error_code ec;
    std::vector<std::string> gone;
    for (const auto& e : m_entries)
        if (!fs::exists(absolute(e.file), ec)) gone.push_back(e.file);
    for (const auto& f : gone) remove(f, outcome::Vanished);
    return (int)gone.size();
}

void Manifest::retire_all(const std::string& outcome_value) {
    std::vector<std::string> all;
    all.reserve(m_entries.size());
    for (const auto& e : m_entries) all.push_back(e.file);
    for (const auto& f : all) remove(f, outcome_value);
}

std::string Manifest::relative(const std::string& absolute_path) const {
    std::error_code ec;
    fs::path rel = fs::relative(absolute_path, m_root, ec);
    if (ec || rel.empty() || *rel.begin() == "..")
        return generic(fs::path(absolute_path).filename());
    return generic(rel);
}

std::string Manifest::absolute(const std::string& rel_file) const {
    return (fs::path(m_root) / fs::path(rel_file)).string();
}

namespace {

bool same_bytes(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::file_size(a, ec) != fs::file_size(b, ec) || ec) return false;
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    std::vector<char> ba(1 << 16), bb(1 << 16);
    while (fa && fb) {
        fa.read(ba.data(), (std::streamsize)ba.size());
        fb.read(bb.data(), (std::streamsize)bb.size());
        if (fa.gcount() != fb.gcount() || !std::equal(ba.begin(), ba.begin() + fa.gcount(), bb.begin()))
            return false;
    }
    return true;
}

} // namespace

std::string free_destination(const std::string& dir, const std::string& src, bool& same) {
    same = false;
    const fs::path from(src);
    const std::string stem = from.stem().string(), ext = from.extension().string();
    std::error_code ec;
    for (int n = 1;; ++n) {
        const fs::path dest = fs::path(dir) / (n == 1 ? from.filename().string()
                                                       : stem + " (" + std::to_string(n) + ")" + ext);
        if (!fs::exists(dest, ec)) return dest.string();
        if (same_bytes(from, dest)) { same = true; return dest.string(); }
    }
}

} // namespace RomManifest
