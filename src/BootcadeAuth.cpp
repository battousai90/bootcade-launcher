// src/BootcadeAuth.cpp
#include "BootcadeAuth.h"
#include "Platform.h"
#include <iostream>
#include <functional>
#include <algorithm>
#include "AppContext.h"

#include "CurlInit.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sys/stat.h>

namespace BootcadeAuth {
namespace {

const char* ISSUER =
    "https://auth.bootcade.duckdns.org/realms/bootcade";
const char* CLIENT_ID = "bootcade-launcher";

std::mutex g_mutex;              // les jetons sont lus depuis le fil de l'UI
                                 // et écrits depuis le fil d'interrogation
std::string g_access, g_refresh, g_verifier;
std::function<void()> g_session_lost;   // voir set_session_lost_handler
std::time_t g_expires_at = 0;

// ── HTTP ────────────────────────────────────────────────────────────────
size_t sink(void* data, size_t size, size_t n, void* out) {
    static_cast<std::string*>(out)->append(static_cast<char*>(data), size * n);
    return size * n;
}

// -> le corps de la réponse, et le code HTTP par `status`. Une chaîne vide
// avec status 0 signifie que la requête n'est même pas partie.
std::string post(const std::string& url, const std::string& body, long& status) {
    status = 0;
    CURL* curl = curl_easy_init();
    if (!curl) return {};

    std::string out;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sink);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    // Un jeton n'a de valeur que si personne ne peut l'intercepter : on ne
    // désactive donc JAMAIS la vérification du certificat, même pour
    // déboguer.
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    if (curl_easy_perform(curl) == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    return out;
}

std::string escape(const std::string& s) {
    CURL* curl = curl_easy_init();
    if (!curl) return s;
    char* e = curl_easy_escape(curl, s.c_str(), int(s.size()));
    std::string out = e ? e : s;
    if (e) curl_free(e);
    curl_easy_cleanup(curl);
    return out;
}

// ── PKCE ────────────────────────────────────────────────────────────────
std::string base64url(const unsigned char* data, size_t len) {
    static const char* A =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = data[i] << 16;
        if (i + 1 < len) v |= data[i + 1] << 8;
        if (i + 2 < len) v |= data[i + 2];
        out += A[(v >> 18) & 63];
        out += A[(v >> 12) & 63];
        if (i + 1 < len) out += A[(v >> 6) & 63];
        if (i + 2 < len) out += A[v & 63];
    }
    return out;                      // sans '=' : la spec PKCE les proscrit
}

std::string make_verifier() {
    unsigned char raw[48];
    // RAND_bytes et pas rand() : un vérifieur prévisible ne protège de rien,
    // et c'est précisément ce que PKCE est censé empêcher.
    if (RAND_bytes(raw, sizeof raw) != 1) return {};
    return base64url(raw, sizeof raw);
}

std::string challenge_of(const std::string& verifier) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return {};
    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1
           && EVP_DigestUpdate(ctx, verifier.data(), verifier.size()) == 1
           && EVP_DigestFinal_ex(ctx, digest, &len) == 1;
    EVP_MD_CTX_free(ctx);
    return ok ? base64url(digest, len) : std::string{};
}

// ── Session sur disque ──────────────────────────────────────────────────
// Le jeton de rafraîchissement vaut une session : il vit dans son propre
// fichier en 0600, et non dans config.json, qu'on ouvre et recopie souvent.
std::string path() {
    return AppContext::get_user_config_dir() + "/session.json";
}

void persist() {
    nlohmann::json j;
    j["refresh_token"] = g_refresh;
    std::ofstream f(path());
    if (!f) return;
    f << j.dump();
    f.close();
    ::chmod(path().c_str(), S_IRUSR | S_IWUSR);
}

void forget() {
    g_access.clear();
    g_refresh.clear();
    g_expires_at = 0;
    std::error_code ec;
    std::filesystem::remove(path(), ec);
}

// Enregistre ce que le serveur vient de rendre. Appelé sous g_mutex.
bool accept_tokens(const std::string& body) {
    try {
        auto j = nlohmann::json::parse(body);
        if (!j.contains("access_token")) return false;
        g_access  = j["access_token"].get<std::string>();
        g_refresh = j.value("refresh_token", g_refresh);
        // Une marge de trente secondes : un jeton qui expire pendant le trajet
        // de la requête produirait un 401 incompréhensible pour le joueur.
        /* On renouvelle AVANT l'echeance, jamais apres.
         *
         * Avec trente secondes de marge, un jeton encore valable deux
         * secondes etait remis tel quel : le temps de construire la requete
         * et de televerser, le serveur le recevait perime et repondait 401.
         * Le joueur lisait alors << serveur injoignable >> alors qu'il etait
         * connecte et le serveur debout. La marge est bornee a la moitie de
         * la duree de vie, sans quoi un jeton court se renouvellerait a
         * chaque appel. */
        const long long life   = j.value("expires_in", 60);
        const long long margin = std::min<long long>(90, life / 2);
        g_expires_at = std::time(nullptr) + life - margin;
        persist();
        return true;
    } catch (...) {
        return false;
    }
}

// Décode la charge utile du jeton. UNIQUEMENT pour l'affichage : c'est le
// serveur de scores qui vérifie la signature, et lui seul. Un jeton lu ici
// pourrait avoir été fabriqué de toutes pièces.
nlohmann::json claims() {
    auto first = g_access.find('.');
    if (first == std::string::npos) return {};
    auto second = g_access.find('.', first + 1);
    if (second == std::string::npos) return {};

    std::string b64 = g_access.substr(first + 1, second - first - 1);
    for (auto& c : b64) { if (c == '-') c = '+'; else if (c == '_') c = '/'; }
    while (b64.size() % 4) b64 += '=';

    std::string out(b64.size(), '\0');
    int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                            reinterpret_cast<const unsigned char*>(b64.data()),
                            int(b64.size()));
    if (n <= 0) return {};
    out.resize(n);
    while (!out.empty() && out.back() == '\0') out.pop_back();
    try { return nlohmann::json::parse(out); } catch (...) { return {}; }
}

// Un utilitaire commun : les trois accesseurs lisent le meme jeton de la meme
// facon, et l'ecrire trois fois inviterait a les faire diverger. Doit rester
// AVANT ses appelants : une fonction d'espace anonyme n'est pas visible plus
// haut dans le fichier.
std::string claim_string(const char* name) {
    auto c = claims();
    return c.contains(name) && c[name].is_string()
         ? c[name].get<std::string>() : std::string{};
}

bool refresh_locked() {
    if (g_refresh.empty()) return false;
    const std::string before = g_refresh;
    long status = 0;
    std::string body = post(std::string(ISSUER) + "/protocol/openid-connect/token",
                            "grant_type=refresh_token&client_id=" + std::string(CLIENT_ID)
                          + "&refresh_token=" + escape(g_refresh),
                            status);
    if (status == 200 && accept_tokens(body)) {
        /* Trace deliberee, pas un reste de mise au point : c'est le seul
         * moyen de VOIR le cycle de renouvellement sur une vraie session,
         * sans attendre qu'un score se perde pour s'apercevoir qu'il ne
         * tourne pas. Sur std::cerr, que main.cpp redirige vers debug.log :
         * std::cout part dans un terminal que personne n'ouvre depuis une
         * icone, la trace y aurait ete parfaitement inutile. Elle dit aussi si le service a fait tourner le jeton
         * de rafraichissement, ce qu'il faut avoir persiste. */
        const bool rotated = (g_refresh != before);
        std::cerr << "[AUTH] jeton renouvele, valable "
                  << (g_expires_at - std::time(nullptr)) << " s"
                  << (rotated ? ", refresh_token renouvele et enregistre"
                              : ", refresh_token inchange")
                  << std::endl;
        return true;
    }
    // 400 sur un rafraîchissement veut dire que la session est morte côté
    // serveur : la garder ferait échouer chaque requête en silence.
    if (status == 400 || status == 401) {
        std::cerr << "[AUTH] renouvellement refuse (HTTP " << status
                  << "), session abandonnee" << std::endl;
        forget();
        // Le joueur n'a rien demande : il doit l'apprendre tout de suite.
        if (g_session_lost) g_session_lost();
    }
    return false;
}

}  // namespace

// ── Interface ───────────────────────────────────────────────────────────
DeviceCode begin() {
    DeviceCode dc;

    std::string verifier = make_verifier();
    std::string challenge = challenge_of(verifier);
    if (verifier.empty() || challenge.empty()) {
        dc.error = "impossible de generer le verifieur PKCE";
        return dc;
    }

    long status = 0;
    std::string body = post(
        std::string(ISSUER) + "/protocol/openid-connect/auth/device",
        /* offline_access : une session qui survit a la fermeture du launcher.
         *
         * Avec << openid >> seul, Keycloak rattache le jeton de
         * rafraichissement a sa session en ligne, qui expire apres une
         * demi-heure sans renouvellement. Or le launcher ne renouvelle que
         * quand il tourne : ferme le soir, il trouvait le lendemain un
         * << HTTP 400, session abandonnee >> et redemandait au joueur de se
         * connecter, a chaque fois. Un jeton hors-ligne n'a pas cette
         * echeance : il reste valable des semaines sans activite, ce qui est
         * le rythme normal d'une application de bureau. */
        "client_id=" + std::string(CLIENT_ID) + "&scope=openid%20offline_access"
      + "&code_challenge=" + challenge + "&code_challenge_method=S256",
        status);

    if (status != 200) {
        dc.error = status ? ("le serveur a repondu " + std::to_string(status))
                          : "serveur injoignable";
        return dc;
    }
    try {
        auto j = nlohmann::json::parse(body);
        dc.device_code = j.at("device_code").get<std::string>();
        dc.user_code   = j.at("user_code").get<std::string>();
        dc.verification_uri = j.value("verification_uri", "");
        dc.verification_uri_complete =
            j.value("verification_uri_complete", dc.verification_uri);
        dc.interval   = j.value("interval", 5);
        dc.expires_in = j.value("expires_in", 600);
        dc.ok = true;
    } catch (...) {
        dc.error = "reponse du serveur incomprehensible";
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_verifier = verifier;      // gardé pour l'échange final
    }
    return dc;
}

Poll poll(const DeviceCode& dc) {
    std::string verifier;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        verifier = g_verifier;
    }

    long status = 0;
    std::string body = post(
        std::string(ISSUER) + "/protocol/openid-connect/token",
        "grant_type=urn:ietf:params:oauth:grant-type:device_code"
        "&client_id=" + std::string(CLIENT_ID)
      + "&device_code=" + escape(dc.device_code)
      + "&code_verifier=" + escape(verifier),
        status);

    if (status == 200) {
        std::lock_guard<std::mutex> lock(g_mutex);
        return accept_tokens(body) ? Poll::Granted : Poll::Failed;
    }
    if (!status) return Poll::Failed;

    // Les erreurs de ce flux sont nommées par la RFC 8628 : les traiter par
    // leur nom plutôt que par le code HTTP, qui vaut 400 pour toutes.
    try {
        auto j = nlohmann::json::parse(body);
        std::string e = j.value("error", "");
        if (e == "authorization_pending") return Poll::Pending;
        if (e == "slow_down")             return Poll::SlowDown;
        if (e == "access_denied")         return Poll::Denied;
        if (e == "expired_token")         return Poll::Expired;
        // Tout le reste est un refus qu'on ne saurait pas expliquer au
        // joueur : au moins le journal dira lequel. Un serveur qui n'autorise
        // pas les sessions hors-ligne repond ici << not_allowed >>, et sans
        // cette ligne ce refus ressemblait a une panne quelconque.
        std::cerr << "[AUTH] connexion refusee (HTTP " << status << ") : " << e
                  << " : " << j.value("error_description", "") << std::endl;
    } catch (...) {}
    return Poll::Failed;
}

bool restore() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_refresh.empty()) return refresh_locked();

    std::ifstream f(path());
    if (!f) return false;
    try {
        nlohmann::json j;
        f >> j;
        g_refresh = j.value("refresh_token", "");
    } catch (...) {
        return false;
    }
    return refresh_locked();
}

std::string access_token() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_access.empty() && !g_refresh.empty()) refresh_locked();
    // g_expires_at porte deja la marge de renouvellement, voir accept_tokens.
    if (!g_access.empty() && std::time(nullptr) >= g_expires_at) refresh_locked();
    return g_access;
}

std::string username() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_access.empty()) return {};
    return claim_string("preferred_username");
}

std::string avatar_id() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_access.empty() ? std::string{} : claim_string("avatar");
}

std::string country() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_access.empty() ? std::string{} : claim_string("country");
}

// Keycloak sert sa console de compte sous <realm>/account. Derivee de
// l'emetteur plutot que recopiee : une seule adresse a changer le jour ou le
// domaine bouge.
std::string account_console_url() {
    return std::string(ISSUER) + "/account";
}

bool signed_in() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return !g_refresh.empty();
}

void sign_out() {
    std::lock_guard<std::mutex> lock(g_mutex);
    forget();   // voulu par le joueur : rien a signaler
}

void set_session_lost_handler(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_session_lost = std::move(fn);
}

std::string session_path() { return path(); }

}  // namespace BootcadeAuth
