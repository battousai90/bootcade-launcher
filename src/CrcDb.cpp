// src/CrcDb.cpp
#include "CrcDb.h"

#include "AppContext.h"
#include "CurlInit.h"
#include "DatSource.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

namespace CrcDb {
namespace {

constexpr const char* kBase = "https://files.bootcade.duckdns.org/crcdb/";

size_t to_string(char* p, size_t size, size_t n, void* out) {
    static_cast<std::string*>(out)->append(p, size * n);
    return size * n;
}

// Une adresse vers une chaine ; "" si le serveur n'a pas repondu 200.
bool get(const std::string& url, std::string& body, long timeout) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Bootcade");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");   // gzip : nginx le sert compresse
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK && status == 200;
}

// Encode un morceau d'adresse comme le homelab l'attend (tout sauf A-Z a-z 0-9 - _ . ~).
std::string url_part(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

// Un modele d'adresse de la base, ses {champs} remplaces (encodes).
std::string fill(std::string tpl, const std::vector<std::pair<std::string, std::string>>& fields) {
    for (const auto& f : fields)
        for (size_t p = tpl.find(f.first); p != std::string::npos; p = tpl.find(f.first, p)) {
            const std::string v = url_part(f.second);
            tpl.replace(p, f.first.size(), v);
            p += v.size();
        }
    return tpl;
}

std::string cache_path(const std::string& file) {
    const fs::path dir = fs::path(AppContext::get_user_config_dir()) / "crcdb";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return (dir / file).string();
}

} // namespace

bool Db::load(const std::string& file, std::string* error) {
    m_entries.clear();
    m_chd_bytes.clear();
    m_chd_disks.clear();
    m_systems.clear();
    m_urls.clear();
    m_chd_url.clear();
    m_loaded = false;
    const std::string local = cache_path(file);

    // Ce que le serveur annonce : l'empreinte du fichier a jour.
    std::string want_sha, manifest;
    if (get(std::string(kBase) + "crcdb-manifest.json", manifest, 30)) {
        try {
            const auto j = nlohmann::json::parse(manifest);
            if (j.value("schema_version", 0) == 1)
                for (const auto& f : j.value("files", nlohmann::json::array()))
                    if (f.value("name", std::string()) == file) want_sha = f.value("sha256", std::string());
        } catch (...) {}
    }

    std::error_code ec;
    const bool have_local = fs::exists(local, ec);
    if (!want_sha.empty() && (!have_local || DatSource::sha256_of(local) != want_sha)) {
        std::string body;
        if (get(std::string(kBase) + file, body, 300)) {
            const std::string part = local + ".part";
            { std::ofstream out(part, std::ios::binary); out << body; }
            if (DatSource::sha256_of(part) == want_sha) fs::rename(part, local, ec);
            else { fs::remove(part, ec); if (error) *error = "crcdb : empreinte inattendue"; }
        }
    }
    if (!fs::exists(local, ec)) { if (error && error->empty()) *error = "crcdb : base indisponible"; return false; }

    try {
        std::ifstream in(local);
        nlohmann::json j = nlohmann::json::parse(in);
        if (j.value("schema_version", 0) != 1) { if (error) *error = "crcdb : schema inconnu"; return false; }
        m_generated = j.value("generated", std::string());
        const auto& files = j["files"];
        m_entries.reserve(files.size());
        for (auto it = files.begin(); it != files.end(); ++it) {
            Entry e;
            const std::string b = it->value("b", std::string("pending"));
            e.basis = b == "pending" ? 'p' : b == "full" ? 'f' : 'r';
            if (it->contains("c"))
                for (const auto& c : (*it)["c"]) e.crcs.push_back(std::stoul(c.get<std::string>(), nullptr, 16));
            std::sort(e.crcs.begin(), e.crcs.end());
            if (it->contains("z") && (*it)["z"].is_number_unsigned()) e.bytes = (*it)["z"].get<unsigned long long>();
            e.date = it->value("d", std::string());
            m_entries.emplace(it.key(), std::move(e));
        }
        if (j.contains("chds") && j["chds"].is_object())
            for (auto it = j["chds"].begin(); it != j["chds"].end(); ++it) {
                if (it->contains("z") && (*it)["z"].is_number_unsigned())
                    m_chd_bytes[it.key()] = (*it)["z"].get<unsigned long long>();
                if (it->contains("f") && (*it)["f"].is_object())
                    for (auto d = (*it)["f"].begin(); d != (*it)["f"].end(); ++d) m_chd_disks[it.key()].push_back(d.key());
            }
        if (j.contains("systems") && j["systems"].is_object())
            for (auto e = j["systems"].begin(); e != j["systems"].end(); ++e)
                if (e->is_object())
                    for (auto s = e->begin(); s != e->end(); ++s)
                        if (s->is_string()) m_systems[e.key()][s.key()] = s->get<std::string>();
        if (j.contains("urls") && j["urls"].is_object())
            for (auto u = j["urls"].begin(); u != j["urls"].end(); ++u)
                if (u->is_string()) m_urls[u.key()] = u->get<std::string>();
        m_chd_url = j.value("chd_url", std::string());
    } catch (const std::exception& ex) {
        if (error) *error = std::string("crcdb : ") + ex.what();
        return false;
    }
    m_loaded = true;
    return true;
}

Verdict Db::check(const std::string& key, const std::vector<unsigned long>& crcs, int* have) const {
    if (have) *have = 0;
    auto it = m_entries.find(key);
    if (it == m_entries.end()) return Verdict::Unknown;
    if (it->second.basis == 'p') return Verdict::Pending;
    if (crcs.empty()) return Verdict::Unknown;   // rien a comparer (un CHD, par exemple)
    int found = 0;
    for (unsigned long c : crcs)
        if (std::binary_search(it->second.crcs.begin(), it->second.crcs.end(), c)) ++found;
    if (have) *have = found;
    return found == (int)crcs.size() ? Verdict::Match : Verdict::Mismatch;
}

unsigned long long Db::bytes_of(const std::string& key) const {
    auto it = m_entries.find(key);
    return it == m_entries.end() ? 0 : it->second.bytes;
}

unsigned long long Db::chd_bytes(const std::string& name) const {
    auto it = m_chd_bytes.find(name);
    return it == m_chd_bytes.end() ? 0 : it->second;
}

std::string Db::key_for(const std::string& emulator, const std::string& system, const std::string& name) const {
    auto e = m_systems.find(emulator);
    if (e == m_systems.end()) return {};
    auto s = e->second.find(system);
    if (s == e->second.end()) s = e->second.find("*");
    if (s == e->second.end()) return {};
    return s->second.empty() ? name : s->second + "/" + name;
}

bool Db::has(const std::string& key, std::string* date) const {
    auto it = m_entries.find(key);
    if (it == m_entries.end()) return false;
    if (date) *date = it->second.date;
    return true;
}

std::string Db::url_for(const std::string& key) const {
    // Le prefixe est tout ce qui precede le nom du jeu : « Arcade/FBNeo/megadrive/sonic ».
    const size_t slash = key.rfind('/');
    const std::string prefix = slash == std::string::npos ? std::string() : key.substr(0, slash);
    const std::string name = slash == std::string::npos ? key : key.substr(slash + 1);
    auto u = m_urls.find(prefix);
    if (u == m_urls.end()) return {};
    return fill(u->second, {{"{name}", name}});
}

std::vector<std::pair<std::string, std::string>> Db::chd_files(const std::string& name) const {
    std::vector<std::pair<std::string, std::string>> out;
    auto it = m_chd_disks.find(name);
    if (it == m_chd_disks.end() || m_chd_url.empty()) return out;
    for (const auto& disk : it->second) out.emplace_back(fill(m_chd_url, {{"{name}", name}, {"{disk}", disk}}), disk);
    return out;
}

std::string file_for(const std::string& source_kind, const std::string& emulator) {
    if (source_kind == "lolroms") return "lolroms.json";
    if (source_kind == "retroroms" && emulator == "mame") return "retroroms-mame.json";
    if (source_kind == "retroroms" && emulator == "fbneo") return "retroroms-fbneo.json";
    return {};
}

std::string key_for(const std::string& source_kind, const std::string& lol_folder, const std::string& name) {
    // LolRoms et RetroRoms FinalBurn Neo rangent par dossier (arcade, megadrive...) ; MAME n'en a qu'un.
    return lol_folder.empty() ? name : lol_folder + "/" + name;
}

}
