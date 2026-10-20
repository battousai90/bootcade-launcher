// src/FbneoUpdateCheck.cpp
#include "FbneoUpdateCheck.h"
#include "Platform.h"
#include "FbneoChannel.h"

#include "CurlInit.h"
#include <nlohmann/json.hpp>
#include <ctime>
#include <cctype>

namespace FbneoUpdateCheck {
namespace {

size_t write_to_string(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

} // namespace

namespace {
Result fetch_release(const char* url) {
    Result r;

    CURL* curl = curl_easy_init();
    if (!curl) { r.error = "curl_easy_init failed"; return r; }

    std::string body;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
    headers = curl_slist_append(headers, "User-Agent: bootcade");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) { r.error = curl_easy_strerror(res); return r; }
    if (status != 200)   { r.error = "HTTP " + std::to_string(status); return r; }

    try {
        auto j = nlohmann::json::parse(body);
        r.sha          = j.value("target_commitish", "");
        r.tag          = j.value("tag_name", "");
        r.name         = j.value("name", "");
        r.published_at = j.value("published_at", "");
        if (j.contains("assets") && j["assets"].is_array())
            for (const auto& a : j["assets"])
                if (a.is_object()) {
                    std::string digest = a.contains("digest") && a["digest"].is_string()
                                             ? a["digest"].get<std::string>() : std::string();
                    if (digest.rfind("sha256:", 0) == 0) digest.erase(0, 7);
                    else digest.clear();
                    r.assets.push_back({a.value("name", ""),
                                        a.value("browser_download_url", ""), digest});
                }
        // Le tag suffit : c'est lui qui porte la version. Le commit n'a de
        // sens que pour FBNeo, dont on compare la revision construite.
        r.ok = !r.tag.empty();
        if (!r.ok) r.error = "response had no tag_name";
    } catch (const std::exception& e) {
        r.error = std::string("JSON parse error: ") + e.what();
    }
    return r;
}
} // namespace

Result fetch_latest() {
    // La version choisie dans Reglages > Emulateur : Bootcade ou officielle.
    return fetch_release(FbneoChannel::releases_url().c_str());
}

std::string asset_name() {
#ifdef _WIN32
    // L'officiel n'a pas de port SDL2 pour Windows : son zip est fbneo64.exe.
    if (FbneoChannel::official()) return "windows-x86_64.zip";
    return "windows-sdl2-x86_64.zip";
#else
    return "linux-sdl2-x86_64.zip";
#endif
}

std::string download_url() {
    return "https://github.com/battousai90/FBNeo/releases/download/latest/" + asset_name();
}

std::string executable_name() {
#ifdef _WIN32
    if (FbneoChannel::official()) return "fbneo64.exe";
    return "fbneo.exe";
#else
    return "fbneo";
#endif
}

bool has_platform_asset(const Result& release) {
    for (const auto& a : release.assets)
        if (a.name == asset_name()) return true;
    return false;
}

Result fetch_launcher_latest() {
    return fetch_release("https://api.github.com/repos/battousai90/bootcade-launcher/releases/latest");
}

std::string upstream_commit(const Result& release) {
    const std::string key = "upstream ";
    const size_t at = release.name.find(key);
    if (at != std::string::npos) {
        std::string hex;
        for (size_t i = at + key.size(); i < release.name.size() && std::isxdigit((unsigned char)release.name[i]); ++i)
            hex += release.name[i];
        if (hex.size() >= 7) return hex.substr(0, 7);
    }
    return release.sha.substr(0, 7);
}

std::time_t parse_iso8601(const std::string& s) {
    std::tm tm{};
    if (strptime(s.c_str(), "%Y-%m-%dT%H:%M:%S", &tm) == nullptr) return -1;
    return timegm(&tm); // GitHub timestamps are always UTC ("Z" suffix)
}

}
