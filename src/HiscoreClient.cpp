// src/HiscoreClient.cpp
#include "HiscoreClient.h"
#include "BootcadeAuth.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <ctime>
#include <cctype>
#include <filesystem>
#include <fstream>

// Version dans le User-Agent : c'est le seul moyen, cote serveur, de savoir
// quelle build parle. Sans elle, les logs du proxy montraient des launchers
// qui n'ouvraient jamais de session sans pouvoir dire s'ils avaient
// seulement la fonctionnalite.
#ifdef BOOTCADE_VERSION
static const char* kUserAgent = "bootcade-launcher/" BOOTCADE_VERSION;
#else
static const char* kUserAgent = "bootcade-launcher";
#endif
#include <map>
#include <mutex>
#include <iostream>
#include <system_error>

namespace HiscoreClient {
namespace {

std::mutex  g_mutex;
std::string g_base_url;
std::set<std::string> g_saveram;
std::map<std::string, std::string> g_metrics;
std::mutex g_saveram_mutex;

// Le magasin hors ligne est défini plus bas, avec le reste du cache.
nlohmann::json read_json_file(const std::string& path);
void write_json_file(const std::string& path, const nlohmann::json& j);
std::string cache_file();

size_t write_to_string(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

// URL-escape a path segment. ROM names are tame, but system names are not:
// "Neo Geo Pocket" and "Fairchild Channel F" both carry spaces, and a raw
// space in a request line is a malformed request.
std::string escape_segment(CURL* curl, const std::string& s) {
    char* enc = curl_easy_escape(curl, s.c_str(), (int)s.size());
    if (!enc) return s;
    std::string out(enc);
    curl_free(enc);
    return out;
}

// GET returning the parsed body, or a null json on any failure.
/* Ce qu'une requete a REELLEMENT donne.
 *
 * L'ancienne version rendait nullptr pour six situations differentes :
 * adresse absente, curl indisponible, echec de transport, statut non-200,
 * corps illisible, et reponse vide. L'appelant ne pouvait donc pas
 * distinguer << personne n'a repondu >> de << le serveur a repondu ceci >>,
 * et finissait par lire l'etat du reseau dans le contenu metier.
 */
struct JsonReply {
    bool           answered = false;
    long           status   = 0;
    bool           ok       = false;
    std::string    error;
    nlohmann::json body = nullptr;
};

JsonReply get_json(const std::string& path, long timeout_secs, bool with_account = false) {
    JsonReply r;
    std::string base;
    { std::lock_guard<std::mutex> lock(g_mutex); base = g_base_url; }
    if (base.empty()) { r.error = "adresse du service non configuree"; return r; }

    CURL* curl = curl_easy_init();
    if (!curl) { r.error = "curl indisponible"; return r; }

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, (base + path).c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_secs);
    // Short connect timeout on purpose: when the service is simply not there
    // : the common case away from the homelab : we want to give up quickly
    // and leave the interface alone, not hold a worker thread for the full
    // transfer timeout on every game the player clicks.
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    struct curl_slist* headers = nullptr;
    if (with_account) {
        const std::string token = BootcadeAuth::access_token();
        if (!token.empty())
            headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());
        if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (headers) curl_slist_free_all(headers);

    // Seul un echec de transport signifie que rien n'a repondu.
    if (res != CURLE_OK) { r.error = curl_easy_strerror(res); return r; }
    r.answered = true;
    r.status   = status;
    if (status != 200) {
        r.error = "HTTP " + std::to_string(status);
        return r;
    }
    try {
        r.body = nlohmann::json::parse(body);
        r.ok   = true;
    } catch (const std::exception& e) {
        r.error = std::string("reponse illisible : ") + e.what();
    }
    return r;
}

} // namespace

std::string base_url() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_base_url;
}

void set_base_url(const std::string& url) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_base_url = url;
    // A trailing slash would produce "//api/..." : harmless on most servers,
    // but it defeats any cache keyed on the exact URL.
    while (!g_base_url.empty() && g_base_url.back() == '/') g_base_url.pop_back();
}

/* Le verdict de transport voyage avec le resultat metier, sans jamais s'y
 * melanger : c'est tout l'objet de Fetched. */
template <typename T>
static void carry(Fetched<T>& out, const JsonReply& r) {
    out.answered    = r.answered;
    out.http_status = r.status;
    out.error       = r.error;
}

Fetched<std::set<std::string>> fetch_supported() {
    Fetched<std::set<std::string>> res;
    auto& out = res.value;
    // Longer than a per-game lookup: this one is fetched once at startup and
    // the list spans every ranked game.
    auto rep = get_json("/api/supported", 15L);
    carry(res, rep);
    if (!rep.ok) return res;
    // Une reponse 200 qui n'est pas un tableau reste une reponse : le service
    // est joignable, c'est son contenu qui ne convient pas.
    if (!rep.body.is_array()) { res.error = "reponse inattendue"; return res; }
    res.ok = true;
    for (const auto& item : rep.body) {
        if (!item.is_object()) continue;
        std::string system = item.value("system", "");
        std::string game   = item.value("game", "");
        if (system.empty() || game.empty()) continue;
        out.insert(key(system, game));
        {
            std::lock_guard<std::mutex> lock(g_saveram_mutex);
            if (item.value("file", std::string()) == "saveram")
                g_saveram.insert(key(system, game));
            const std::string metric = item.value("metric", std::string());
            if (!metric.empty() && metric != "score")
                g_metrics[key(system, game)] = metric;
        }
    }
    {   // Persisté avec la liste, sinon un démarrage hors ligne relirait des
        // jeux supportés sans savoir quel fichier leur correspond.
        std::lock_guard<std::mutex> lock(g_saveram_mutex);
        if (!cache_file().empty() && (!g_saveram.empty() || !g_metrics.empty())) {
            auto j = read_json_file(cache_file());
            nlohmann::json list = nlohmann::json::array();
            for (const auto& k : g_saveram) list.push_back(k);
            j["saveram"] = list;
            nlohmann::json metrics = nlohmann::json::object();
            for (const auto& entry : g_metrics) metrics[entry.first] = entry.second;
            j["metrics"] = metrics;
            write_json_file(cache_file(), j);
        }
    }
    return res;
}

bool sync_hiscore_dat(const std::string& fbneo_hiscores_dir) {
    std::string base;
    { std::lock_guard<std::mutex> lock(g_mutex); base = g_base_url; }
    if (base.empty() || fbneo_hiscores_dir.empty()) return false;

    // L'ETag connu evite de retransferer 130 Ko a chaque demarrage : le
    // service repond 304 quand le contenu n'a pas bouge.
    std::string known;
    if (!cache_file().empty()) {
        auto j = read_json_file(cache_file());
        known = j.value("hiscore_dat_etag", std::string());
    }

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string body, etag;
    struct curl_slist* headers = nullptr;
    if (!known.empty())
        headers = curl_slist_append(headers, ("If-None-Match: " + known).c_str());

    curl_easy_setopt(curl, CURLOPT_URL, (base + "/api/hiscore-dat").c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, +[](char* buffer, size_t size,
                                                       size_t items, void* out) -> size_t {
        const size_t total = size * items;
        std::string line(buffer, total);
        const std::string key = "etag:";
        if (line.size() > key.size()) {
            std::string head = line.substr(0, key.size());
            for (auto& c : head) c = (char)std::tolower((unsigned char)c);
            if (head == key) {
                std::string value = line.substr(key.size());
                while (!value.empty() && (value.front() == ' ' || value.front() == '"'))
                    value.erase(value.begin());
                while (!value.empty() && (value.back() == '\r' || value.back() == '\n' ||
                                          value.back() == '"' || value.back() == ' '))
                    value.pop_back();
                *static_cast<std::string*>(out) = value;
            }
        }
        return total;
    });
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &etag);
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (headers) curl_slist_free_all(headers);

    if (res != CURLE_OK || status != 200 || body.empty()) return false;

    // Ecrit a cote puis renomme : une coupure en plein transfert laisserait
    // sinon l'emulateur avec un hiscore.dat tronque, donc pire qu'avant.
    try {
        std::filesystem::create_directories(fbneo_hiscores_dir);
    } catch (const std::exception&) {
        return false;
    }
    const std::string target = fbneo_hiscores_dir + "/hiscore.dat";
    const std::string temp = target + ".part";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(body.data(), (std::streamsize)body.size());
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return false;
    }

    if (!etag.empty() && !cache_file().empty()) {
        auto j = read_json_file(cache_file());
        j["hiscore_dat_etag"] = etag;
        write_json_file(cache_file(), j);
    }
    return true;
}

std::string metric_of(const std::string& system, const std::string& game) {
    std::lock_guard<std::mutex> lock(g_saveram_mutex);
    if (g_metrics.empty() && !cache_file().empty()) {
        auto j = read_json_file(cache_file());
        if (j.contains("metrics") && j["metrics"].is_object())
            for (auto it = j["metrics"].begin(); it != j["metrics"].end(); ++it)
                if (it.value().is_string())
                    g_metrics[it.key()] = it.value().get<std::string>();
    }
    auto found = g_metrics.find(key(system, game));
    return found == g_metrics.end() ? std::string("score") : found->second;
}

bool is_saveram(const std::string& system, const std::string& game) {
    std::lock_guard<std::mutex> lock(g_saveram_mutex);
    if (g_saveram.empty() && !cache_file().empty()) {
        auto j = read_json_file(cache_file());
        if (j.contains("saveram") && j["saveram"].is_array())
            for (const auto& v : j["saveram"])
                if (v.is_string()) g_saveram.insert(v.get<std::string>());
    }
    return g_saveram.count(key(system, game)) > 0;
}

namespace {
// Shared by fetch_top and fetch_boards: the two endpoints return the same
// row shape, so they parse the same way.
Entry parse_entry(const nlohmann::json& item) {
    Entry e;
    e.player = item.value("player", "");
    if (item.contains("country") && item["country"].is_string())
        e.country = item["country"].get<std::string>();
    if (item.contains("score") && item["score"].is_number())
        e.score = item["score"].get<long long>();
    if (item.contains("since") && item["since"].is_string())
        e.since = item["since"].get<std::string>();
    return e;
}
} // namespace

Fetched<std::vector<Board>> fetch_boards(int limit) {
    Fetched<std::vector<Board>> res;
    auto& out = res.value;
    // Longer than a per-game lookup: this covers every ranked game at once.
    auto rep = get_json("/api/boards?limit=" + std::to_string(limit), 20L);
    carry(res, rep);
    if (!rep.ok) return res;
    if (!rep.body.is_array()) { res.error = "reponse inattendue"; return res; }
    res.ok = true;
    for (const auto& b : rep.body) {
        if (!b.is_object() || !b.contains("rows")) continue;
        Board board;
        board.system = b.value("system", "");
        board.game   = b.value("game", "");
        if (board.system.empty() || board.game.empty()) continue;
        for (const auto& item : b["rows"]) {
            Entry e = parse_entry(item);
            if (!e.player.empty()) board.rows.push_back(e);
        }
        out.push_back(std::move(board));
    }
    return res;
}

Fetched<std::vector<Entry>> fetch_top(const std::string& system,
                                      const std::string& game,
                                      int limit) {
    Fetched<std::vector<Entry>> res;
    auto& out = res.value;
    if (system.empty() || game.empty()) { res.error = "jeu non designe"; return res; }

    CURL* esc = curl_easy_init();
    if (!esc) { res.error = "curl indisponible"; return res; }
    std::string path = "/api/scores/" + escape_segment(esc, system) + "/" +
                       escape_segment(esc, game) + "/top?limit=" +
                       std::to_string(limit);
    curl_easy_cleanup(esc);

    auto rep = get_json(path, 8L);
    carry(res, rep);
    if (!rep.ok) return res;
    if (!rep.body.is_array()) { res.error = "reponse inattendue"; return res; }
    res.ok = true;
    for (const auto& item : rep.body) {
        if (!item.is_object()) continue;
        Entry e = parse_entry(item);
        if (!e.player.empty()) out.push_back(e);
    }
    return res;
}

SubmitResult submit(const std::string& system,
                    const std::string& game,
                    const std::string& player,
                    const std::string& country,
                    const Playtime&    playtime,
                    const std::string& hi_before,
                    const std::string& hi_after) {
    SubmitResult r;
    std::string base;
    { std::lock_guard<std::mutex> lock(g_mutex); base = g_base_url; }
    if (base.empty() || system.empty() || game.empty() || player.empty()) {
        r.error = "not configured";
        return r;
    }

    CURL* curl = curl_easy_init();
    if (!curl) { r.error = "curl_easy_init failed"; return r; }

    curl_mime* mime = curl_mime_init(curl);
    auto add_field = [&](const char* name, const std::string& value) {
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, name);
        curl_mime_data(part, value.c_str(), value.size());
    };
    auto add_file = [&](const char* name, const std::string& bytes) {
        if (bytes.empty()) return;
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, name);
        curl_mime_filename(part, name);
        curl_mime_data(part, bytes.data(), bytes.size());
    };
    add_field("system", system);
    add_field("game",   game);
    add_field("player", player);
    if (!country.empty()) add_field("country", country);
    // Sent even when the session set no record: a game that was played for
    // twenty minutes was played, whether or not the table moved.
    if (playtime.total > 0) {
        add_field("play_last",    std::to_string(playtime.last));
        add_field("play_longest", std::to_string(playtime.longest));
        add_field("play_total",   std::to_string(playtime.total));
    }
    add_file("hi",        hi_after);
    add_file("hi_before", hi_before);

    // Le jeton du compte : sans lui le serveur repond 401, et c'est voulu.
    // Depuis que les scores sont rattaches a un compte, un score anonyme n'a
    // plus de proprietaire, donc plus de place au classement. Le champ
    // `player` reste envoye pour les serveurs anterieurs, mais le serveur
    // actuel l'ignore et prend le nom dans le jeton : un champ texte fourni
    // par le client se falsifiait en une ligne.
    struct curl_slist* auth_headers = nullptr;
    std::string token = BootcadeAuth::access_token();
    if (!token.empty())
        auth_headers = curl_slist_append(auth_headers,
                                         ("Authorization: Bearer " + token).c_str());

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, (base + "/api/submit").c_str());
    if (auth_headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, auth_headers);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_mime_free(mime);
    if (auth_headers) curl_slist_free_all(auth_headers);
    curl_easy_cleanup(curl);

    // Seul un echec de transport signifie que rien n'a repondu. Passe cette
    // ligne, le serveur a parle, quel que soit ce qu'il a dit.
    if (res != CURLE_OK) { r.error = curl_easy_strerror(res); return r; }
    r.answered    = true;
    r.http_status = status;

    // 401 : le joueur n'est pas connecte, ou sa session a expire. Le dire en
    // clair plutot que de laisser une erreur muette : sans ca il jouerait des
    // heures en croyant publier ses scores.
    if (status == 401) {
        r.error = "connexion requise : ouvre les reglages et connecte-toi";
        return r;
    }
    r.reached = true;

    try {
        auto j = nlohmann::json::parse(body);
        // 409 carries a real explanation (a clone has no leaderboard) that the
        // player deserves to read, so it is parsed like any other answer.
        if (j.contains("error"))  r.reason = j.value("detail", j.value("error", ""));
        if (j.contains("reason")) r.reason = j.value("reason", "");
        // A pending answer may explain itself through `detail` alone (a first
        // game with no starting state). Kept for the log : without it, a
        // score held for review left no trace of why.
        if (r.reason.empty() && j.contains("detail")) r.reason = j.value("detail", "");

        /* Seul le SERVICE peut declarer un score irrecuperable. Le launcher
         * ne le deduit d'aucun code HTTP. */
        if (j.contains("retryable") && j["retryable"].is_boolean())
            r.retryable = j["retryable"].get<bool>();
        else {
            const std::string code = j.value("error", std::string());
            if (code == "invalid_score" || code == "unsupported_game"
                || code == "rejected_score" || code == "forbidden_account")
                r.retryable = false;
        }
        r.code   = j.value("code", std::string());
        r.detail = j.value("detail", std::string());
        if (j.contains("params") && j["params"].is_object()) {
            const auto& p = j["params"];
            if (p.contains("rows") && p["rows"].is_number()) r.rows = p["rows"].get<int>();
            if (p.contains("to_beat") && p["to_beat"].is_number()) {
                r.to_beat = p["to_beat"].get<long long>();
                r.has_to_beat = true;
            }
        }
        if (j.contains("submission") && j["submission"].is_number())
            r.submission = j["submission"].get<long long>();
        if (j.contains("report") && j["report"].is_boolean())
            r.report = j["report"].get<bool>();
        std::string st = j.value("status", "");
        r.accepted = (st == "accepted");
        r.pending  = (st == "pending");
        r.ignored  = (st == "ignored" || st == "playtime");
        if (j.contains("score") && j["score"].is_number()) {
            r.score = j["score"].get<long long>();
            r.has_score = true;
        }
    } catch (const std::exception& e) {
        r.error = std::string("bad reply: ") + e.what();
        r.reached = false;
    }
    if (status >= 500) { r.reached = false; r.error = "HTTP " + std::to_string(status); }
    return r;
}

// ── Signalements ──────────────────────────────────────────────────────────
ReportReply send_report(const ReportForm& form) {
    ReportReply r;
    std::string base;
    { std::lock_guard<std::mutex> lock(g_mutex); base = g_base_url; }
    if (base.empty()) { r.error = "adresse du service non configuree"; return r; }

    CURL* curl = curl_easy_init();
    if (!curl) { r.error = "curl indisponible"; return r; }

    curl_mime* mime = curl_mime_init(curl);
    auto add_field = [&](const char* name, const std::string& value) {
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, name);
        curl_mime_data(part, value.c_str(), value.size());
    };
    if (form.submission > 0) add_field("submission", std::to_string(form.submission));
    else {
        add_field("system", form.system);
        add_field("game",   form.game);
    }
    if (form.has_score) add_field("score", std::to_string(form.score));
    if (!form.message.empty()) add_field("message", form.message);
    if (!form.screenshot_path.empty()) {
        std::string ext = std::filesystem::path(form.screenshot_path).extension().string();
        for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
        const char* type = ext == ".png" ? "image/png" : ext == ".webp" ? "image/webp" : "image/jpeg";
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, "screenshot");
        curl_mime_filedata(part, form.screenshot_path.c_str());
        curl_mime_type(part, type);
    }

    struct curl_slist* headers = nullptr;
    const std::string token = BootcadeAuth::access_token();
    if (!token.empty())
        headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, (base + "/api/reports").c_str());
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_mime_free(mime);
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) { r.error = curl_easy_strerror(res); return r; }
    r.answered    = true;
    r.http_status = status;
    r.ok          = status == 201 || status == 200;
    try {
        auto j = nlohmann::json::parse(body);
        r.code   = j.value("code", std::string());
        r.detail = j.value("detail", std::string());
    } catch (const std::exception&) {}
    if (!r.ok && r.error.empty()) r.error = "HTTP " + std::to_string(status);
    return r;
}

Fetched<std::vector<PlayerReport>> fetch_my_reports() {
    Fetched<std::vector<PlayerReport>> res;
    if (BootcadeAuth::access_token().empty()) { res.error = "pas de compte"; return res; }
    auto rep = get_json("/api/me/reports", 10L, true);
    carry(res, rep);
    if (!rep.ok) return res;
    if (!rep.body.is_array()) { res.error = "reponse inattendue"; return res; }
    res.ok = true;
    auto text = [](const nlohmann::json& o, const char* k) {
        return o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : std::string();
    };
    for (const auto& o : rep.body) {
        if (!o.is_object()) continue;
        PlayerReport p;
        if (o.contains("id") && o["id"].is_number()) p.id = o["id"].get<long long>();
        p.system     = text(o, "system");
        p.game       = text(o, "game");
        p.status     = text(o, "status");
        p.reply      = text(o, "reply");
        p.replied_at = text(o, "replied_at");
        p.created_at = text(o, "created_at");
        if (o.contains("score_claimed") && o["score_claimed"].is_number()) {
            p.score_claimed = o["score_claimed"].get<long long>();
            p.has_claimed = true;
        }
        if (o.contains("published") && o["published"].is_number()) {
            p.published = o["published"].get<long long>();
            p.has_published = true;
        }
        res.value.push_back(std::move(p));
    }
    return res;
}

// ── Offline store ─────────────────────────────────────────────────────────
namespace {

std::string g_store_dir;

std::string store_path(const std::string& leaf) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_store_dir.empty() ? std::string() : g_store_dir + "/" + leaf;
}

std::string to_hex(const std::string& raw) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char c : raw) { out += digits[c >> 4]; out += digits[c & 0xF]; }
    return out;
}

std::string from_hex(const std::string& hex) {
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out += (char)((hi << 4) | lo);
    }
    return out;
}

std::string now_iso8601() {
    std::time_t t = std::time(nullptr);
    char buf[32] = {0};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

nlohmann::json read_json_file(const std::string& path) {
    std::ifstream fi(path);
    if (!fi) return nlohmann::json::object();
    try { nlohmann::json j; fi >> j; return j; }
    catch (const std::exception&) { return nlohmann::json::object(); }
}

void write_json_file(const std::string& path, const nlohmann::json& j) {
    // Written to a sibling then renamed: a launcher killed mid-write must not
    // leave a truncated cache that fails to parse on the next start.
    std::string tmp = path + ".tmp";
    { std::ofstream fo(tmp); if (!fo) return; fo << j.dump(); }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) std::filesystem::remove(tmp, ec);
}

std::string outbox_dir() { return store_path("hiscore-outbox"); }
std::string cache_file() { return store_path("hiscore-cache.json"); }
std::string personal_file() { return store_path("hiscore-personal.json"); }

} // namespace

void set_store_dir(const std::string& dir) {
    { std::lock_guard<std::mutex> lock(g_mutex); g_store_dir = dir; }
    std::error_code ec;
    std::filesystem::create_directories(outbox_dir(), ec);
}

void queue_submission(const std::string& system, const std::string& game,
                      const std::string& player, const std::string& country,
                      const Playtime& playtime,
                      const std::string& hi_before, const std::string& hi_after) {
    const std::string dir = outbox_dir();
    if (dir.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    nlohmann::json j;
    j["system"] = system; j["game"] = game;
    j["player"] = player; j["country"] = country;
    j["play_last"] = playtime.last;
    j["play_longest"] = playtime.longest;
    j["play_total"] = playtime.total;
    j["hi_before"] = to_hex(hi_before);
    j["hi_after"]  = to_hex(hi_after);
    j["queued_at"] = now_iso8601();

    // The name carries the clock and a counter so two sessions finishing in
    // the same second cannot overwrite one another.
    static std::atomic<unsigned> seq{0};
    std::string name = std::to_string((long long)std::time(nullptr)) + "-" +
                       std::to_string(seq++) + ".json";
    write_json_file(dir + "/" + name, j);
}

int outbox_size() {
    const std::string dir = outbox_dir();
    if (dir.empty()) return 0;
    std::error_code ec;
    int n = 0;
    for (auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.path().extension() == ".json") n++;
    return n;
}

FlushReport flush_outbox() {
    FlushReport rep;
    const std::string dir = outbox_dir();
    if (dir.empty() || base_url().empty()) return rep;

    /* Sans compte, inutile de frapper a la porte.
     *
     * L'envoi partait sans jeton et le service le refusait a coup sur : une
     * requete perdue a chaque demarrage, et un 401 dans les journaux qui
     * ressemblait a une panne. La file reste INTACTE et repartira d'elle-meme
     * a la reconnexion, ou refresh_hiscore_data_async est deja rappelee. */
    if (BootcadeAuth::access_token().empty()) return rep;

    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.path().extension() == ".json") files.push_back(e.path());
    std::sort(files.begin(), files.end());   // oldest first, so order is kept

    for (const auto& f : files) {
        auto j = read_json_file(f.string());
        if (!j.is_object() || !j.contains("game")) {
            std::filesystem::remove(f, ec);      // unreadable: drop it
            continue;
        }
        Playtime pt{ j.value("play_last", 0), j.value("play_longest", 0),
                     j.value("play_total", 0) };
        /* Le nom vient du COMPTE au moment de l'envoi, pas de la file.
         *
         * Une entree garee deconnecte n'en a pas : le serveur le prend dans le
         * jeton, ce champ ne sert qu'aux serveurs anterieurs. Mais submit()
         * refuse de partir sans nom, et la file gardait l'entree en silence,
         * a chaque demarrage, pour toujours. */
        std::string player = j.value("player", "");
        if (player.empty()) player = BootcadeAuth::username();
        const std::string system = j.value("system", ""), game = j.value("game", "");
        auto r = submit(system, game, player, j.value("country", ""), pt,
                        from_hex(j.value("hi_before", "")),
                        from_hex(j.value("hi_after", "")));
        /* Une ligne par entree, toujours : sans elle, une file qui ne se vide
         * pas ne laissait aucune trace de la raison. */
        std::cerr << "[HISCORE] file " << f.filename().string() << " " << system << "/" << game
                  << " : " << (r.answered ? "HTTP " + std::to_string(r.http_status) : "sans reponse")
                  << (r.accepted ? ", publie" : r.pending ? ", en attente" : r.ignored ? ", sans objet" : "")
                  << (!r.reason.empty() ? ", " + r.reason : !r.error.empty() ? ", " + r.error : "")
                  << std::endl;
        /* On ne SUPPRIME que sur un succes. Perdre un score legitime est
         * pire que le renvoyer dix fois.
         *
         *   rien ne repond   la file reste, et inutile d'insister ;
         *   401              conserve : le jeton se renouvellera ;
         *   403              conserve, avec la raison affichee : le service
         *                    repond, mais cela ne prouve pas que le score est
         *                    invalide ;
         *   5xx              conserve ;
         *   accepte, en attente, ou sans objet   le service l'a pris ;
         *   retryable faux   le service tranche lui-meme, la seule facon
         *                    d'ecarter une entree pour de bon.
         */
        if (!r.answered) { rep.kept++; break; }

        // Le service l'a pris : on retire de la file. Mais on distingue ce
        // qu'il en a fait, parce que le joueur ira le verifier au classement.
        if (r.accepted || r.pending || r.ignored) {
            std::filesystem::remove(f, ec);
            if (r.accepted)     rep.published++;
            else if (r.pending) rep.pending++;
            else                rep.ignored++;
            if (!r.reason.empty()) rep.reason = r.reason;
            continue;
        }
        if (!r.retryable) {
            std::filesystem::remove(f, ec);
            rep.dropped++;
            if (!r.reason.empty()) rep.reason = r.reason;
            continue;
        }
        rep.kept++;
        if (!r.reason.empty())     rep.reason = r.reason;
        else if (!r.error.empty()) rep.reason = r.error;
    }
    return rep;
}

std::string reports_seen_at() {
    if (cache_file().empty()) return {};
    return read_json_file(cache_file()).value("reports_seen_at", std::string());
}

void set_reports_seen_at(const std::string& iso) {
    if (cache_file().empty() || iso.empty()) return;
    auto j = read_json_file(cache_file());
    j["reports_seen_at"] = iso;
    write_json_file(cache_file(), j);
}

void cache_supported(const std::set<std::string>& supported) {
    if (cache_file().empty()) return;
    auto j = read_json_file(cache_file());
    nlohmann::json list = nlohmann::json::array();
    for (const auto& k : supported) list.push_back(k);
    j["supported"] = list;
    j["supported_at"] = now_iso8601();
    write_json_file(cache_file(), j);
}

std::set<std::string> cached_supported() {
    std::set<std::string> out;
    if (cache_file().empty()) return out;
    auto j = read_json_file(cache_file());
    if (!j.contains("supported") || !j["supported"].is_array()) return out;
    for (const auto& v : j["supported"])
        if (v.is_string()) out.insert(v.get<std::string>());
    return out;
}

void cache_top(const std::string& system, const std::string& game,
               const std::vector<Entry>& rows) {
    if (cache_file().empty()) return;
    auto j = read_json_file(cache_file());
    nlohmann::json entry;
    entry["at"] = now_iso8601();
    nlohmann::json list = nlohmann::json::array();
    for (const auto& e : rows)
        list.push_back({{"player", e.player}, {"country", e.country},
                        {"score", e.score}, {"since", e.since}});
    entry["rows"] = list;
    j["tops"][key(system, game)] = entry;
    write_json_file(cache_file(), j);
}

void cache_boards(const std::vector<Board>& boards) {
    if (cache_file().empty()) return;
    auto j = read_json_file(cache_file());
    const std::string at = now_iso8601();
    for (const auto& b : boards) {
        nlohmann::json list = nlohmann::json::array();
        for (const auto& e : b.rows)
            list.push_back({{"player", e.player}, {"country", e.country},
                            {"score", e.score}, {"since", e.since}});
        j["tops"][key(b.system, b.game)] = {{"at", at}, {"rows", list}};
    }
    j["boards_at"] = at;
    write_json_file(cache_file(), j);
}

bool probe_reachable(long timeout_secs) {
    // Requete HEAD sur la racine : aucun corps transfere, aucune donnee
    // personnelle en jeu. On ne veut qu'un verdict de joignabilite.
    if (base_url().empty()) return false;
    CURL* c = curl_easy_init();
    if (!c) return false;
    curl_easy_setopt(c, CURLOPT_URL, base_url().c_str());
    curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_secs);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, timeout_secs);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    const CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    return rc == CURLE_OK;
}

void cache_personal_ranks(const std::vector<Board>& boards, const std::string& user) {
    if (personal_file().empty() || user.empty()) return;
    nlohmann::json j;
    j["user"] = user;
    j["at"]   = now_iso8601();
    auto arr = nlohmann::json::array();
    for (const auto& b : boards) {
        // La MEILLEURE ligne du joueur sur ce jeu, jamais une autre : le
        // classement garde ses repetitions, un joueur present en 2e et en 4e
        // est 2e. Meme regle que partout ailleurs.
        for (size_t i = 0; i < b.rows.size(); ++i) {
            if (b.rows[i].player != user) continue;
            arr.push_back({{"system", b.system},
                           {"game", b.game},
                           {"personal_best", b.rows[i].score},
                           {"last_known_rank", static_cast<int>(i) + 1}});
            break;
        }
    }
    j["games"] = arr;
    write_json_file(personal_file(), j);
}

std::vector<PersonalRank> cached_personal_ranks(std::string* user_out) {
    std::vector<PersonalRank> out;
    if (user_out) user_out->clear();
    if (personal_file().empty()) return out;
    auto j = read_json_file(personal_file());
    if (user_out) *user_out = j.value("user", std::string());
    if (!j.contains("games") || !j["games"].is_array()) return out;
    for (const auto& v : j["games"]) {
        PersonalRank r;
        r.system          = v.value("system", "");
        r.game            = v.value("game", "");
        r.personal_best   = v.value("personal_best", 0LL);
        r.last_known_rank = v.value("last_known_rank", 0);
        if (!r.game.empty() && r.last_known_rank > 0) out.push_back(std::move(r));
    }
    return out;
}

std::vector<Board> cached_all_boards() {
    std::vector<Board> out;
    if (cache_file().empty()) return out;
    auto j = read_json_file(cache_file());
    if (!j.contains("tops")) return out;
    for (auto it = j["tops"].begin(); it != j["tops"].end(); ++it) {
        const std::string k = it.key();
        // La cle joint systeme et jeu par un SAUT DE LIGNE, pas par une
        // barre oblique : un nom de systeme peut contenir « / », un retour a
        // la ligne non. Verifie sur le fichier reel.
        const auto sep = k.find('\n');
        if (sep == std::string::npos) continue;
        Board b;
        b.system = k.substr(0, sep);
        b.game   = k.substr(sep + 1);
        const auto& entry = it.value();
        if (!entry.contains("rows") || !entry["rows"].is_array()) continue;
        for (const auto& v : entry["rows"]) {
            Entry e;
            e.player  = v.value("player", "");
            e.country = v.value("country", "");
            e.since   = v.value("since", "");
            if (v.contains("score") && v["score"].is_number())
                e.score = v["score"].get<long long>();
            if (!e.player.empty()) b.rows.push_back(e);
        }
        out.push_back(std::move(b));
    }
    return out;
}

std::vector<Entry> cached_top(const std::string& system, const std::string& game,
                              std::string* fetched_at) {
    std::vector<Entry> out;
    if (fetched_at) fetched_at->clear();
    if (cache_file().empty()) return out;
    auto j = read_json_file(cache_file());
    if (!j.contains("tops")) return out;
    const std::string k = key(system, game);
    if (!j["tops"].contains(k)) return out;
    const auto& entry = j["tops"][k];
    if (fetched_at) *fetched_at = entry.value("at", "");
    if (!entry.contains("rows") || !entry["rows"].is_array()) return out;
    for (const auto& v : entry["rows"]) {
        Entry e;
        e.player  = v.value("player", "");
        e.country = v.value("country", "");
        e.since   = v.value("since", "");
        if (v.contains("score") && v["score"].is_number())
            e.score = v["score"].get<long long>();
        if (!e.player.empty()) out.push_back(e);
    }
    return out;
}

} // namespace HiscoreClient
