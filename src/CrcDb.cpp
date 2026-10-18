// src/CrcDb.cpp
#include "CrcDb.h"

#include "AppContext.h"
#include "CurlInit.h"
#include "DatSource.h"

#include <nlohmann/json.hpp>

#include <algorithm>
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

std::string cache_path(const std::string& file) {
    const fs::path dir = fs::path(AppContext::get_user_config_dir()) / "crcdb";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return (dir / file).string();
}

} // namespace

bool Db::load(const std::string& file, std::string* error) {
    m_entries.clear();
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
            m_entries.emplace(it.key(), std::move(e));
        }
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

std::string file_for(const std::string& source_kind, const std::string& emulator) {
    if (source_kind == "lolroms") return "lolroms.json";
    if (source_kind == "retroroms" && emulator == "mame") return "retroroms-mame.json";
    return {};
}

std::string key_for(const std::string& source_kind, const std::string& lol_folder, const std::string& name) {
    return source_kind == "lolroms" ? lol_folder + "/" + name : name;
}

}
