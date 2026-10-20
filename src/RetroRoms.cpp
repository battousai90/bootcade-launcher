// src/RetroRoms.cpp
#include "RetroRoms.h"

#include "AppContext.h"
#include "CurlInit.h"   // sous Windows : les certificats livres avec Bootcade
#include "i18n.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <regex>

namespace fs = std::filesystem;

namespace RetroRoms {
namespace {

constexpr const char* kSite  = "https://bda.retroroms.info/";
constexpr const char* kForum = "https://www.retroroms.info/";
constexpr const char* kUserAgent = "Mozilla/5.0 (Bootcade)";

std::string jar_path() {
    return (fs::path(AppContext::get_user_config_dir()) / "retroroms-session.txt").string();
}
void protect_jar() {
    std::error_code ec;
    if (fs::exists(jar_path(), ec))
        fs::permissions(jar_path(), fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
}

size_t to_string(char* p, size_t size, size_t n, void* out) {
    static_cast<std::string*>(out)->append(p, size * n);
    return size * n;
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

// Une page, avec la session. `post` non vide : envoi d'un formulaire.
struct Page { bool ok = false; long status = 0; std::string body, url; };
Page get(const std::string& url, const std::string& post = std::string(), bool session = true) {
    Page p;
    CURL* curl = curl_easy_init();
    if (!curl) return p;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &p.body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    if (session) {
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, jar_path().c_str());
        curl_easy_setopt(curl, CURLOPT_COOKIEJAR, jar_path().c_str());
    }
    if (!post.empty()) curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, post.c_str());
    const CURLcode rc = curl_easy_perform(curl);
    char* eff = nullptr;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &p.status);
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff);
    if (eff) p.url = eff;
    curl_easy_cleanup(curl);   // ecrit le fichier de cookies
    protect_jar();
    p.ok = rc == CURLE_OK;
    return p;
}

std::string html_decode(std::string s) {
    static const std::pair<const char*, const char*> ents[] = {
        {"&amp;", "&"}, {"&middot;", "·"}, {"&nbsp;", " "}, {"&quot;", "\""}, {"&#039;", "'"},
        {"&lt;", "<"}, {"&gt;", ">"}};
    for (const auto& e : ents)
        for (size_t p = s.find(e.first); p != std::string::npos; p = s.find(e.first, p + 1)) s.replace(p, std::strlen(e.first), e.second);
    return s;
}
std::string strip_tags(const std::string& s) {
    std::string out;
    bool in = false;
    for (char c : s) {
        if (c == '<') in = true;
        else if (c == '>') in = false;
        else if (!in) out += c;
    }
    return html_decode(out);
}
std::string trim(std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
    return s.substr(i);
}
// La valeur d'un attribut dans une balise.
std::string attr(const std::string& tag, const std::string& name) {
    const std::string key = name + "=\"";
    size_t p = tag.find(key);
    if (p == std::string::npos) return {};
    p += key.size();
    const size_t e = tag.find('"', p);
    return e == std::string::npos ? std::string() : html_decode(tag.substr(p, e - p));
}
// Les champs <input> d'un formulaire : nom -> valeur.
std::vector<std::pair<std::string, std::string>> inputs_of(const std::string& form) {
    std::vector<std::pair<std::string, std::string>> out;
    for (size_t p = form.find("<input"); p != std::string::npos; p = form.find("<input", p + 1)) {
        const size_t e = form.find('>', p);
        if (e == std::string::npos) break;
        const std::string tag = form.substr(p, e - p);
        const std::string name = attr(tag, "name");
        if (name.empty()) continue;
        const std::string type = attr(tag, "type");
        if ((type == "checkbox" || type == "radio") && tag.find("checked") == std::string::npos) continue;
        out.emplace_back(name, attr(tag, "value"));
    }
    return out;
}
// Le formulaire dont l'action contient `needle`, balises comprises.
std::string form_with(const std::string& body, const std::string& needle, std::string* action = nullptr) {
    for (size_t p = body.find("<form"); p != std::string::npos; p = body.find("<form", p + 1)) {
        const size_t te = body.find('>', p);
        const size_t fe = body.find("</form>", p);
        if (te == std::string::npos || fe == std::string::npos) break;
        const std::string tag = body.substr(p, te - p);
        const std::string act = attr(tag, "action");
        if (act.find(needle) == std::string::npos && tag.find(needle) == std::string::npos) continue;
        if (action) *action = act;
        return body.substr(p, fe - p);
    }
    return {};
}
std::string encode_form(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string out;
    for (const auto& f : fields) {
        if (!out.empty()) out += '&';
        out += url_encode(f.first) + "=" + url_encode(f.second);
    }
    return out;
}
std::string title_of(const std::string& body) {
    const size_t p = body.find("<title>");
    if (p == std::string::npos) return {};
    const size_t e = body.find("</title>", p);
    return trim(strip_tags(body.substr(p + 7, e == std::string::npos ? 80 : e - p - 7)));
}

// « 2026-May-25 22:05 » -> « 2026-05-25 ».
std::string iso_day(const std::string& d) {
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (d.size() < 11) return {};
    for (int i = 0; i < 12; ++i)
        if (d.compare(5, 3, months[i]) == 0) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%s-%02d-%s", d.substr(0, 4).c_str(), i + 1, d.substr(9, 2).c_str());
            return buf;
        }
    return {};
}

// Les entrees d'un dossier : nom (« x.zip » ou « dossier/ ») -> date, et sa taille.
bool list(const std::string& dir, std::map<std::string, std::string>& out,
          std::map<std::string, std::string>* sizes = nullptr) {
    Page p = get(std::string(kSite) + dir + "/", std::string(), false);
    if (!p.ok || p.status != 200) return false;
    for (size_t r = p.body.find("<td class=\"link\">"); r != std::string::npos; r = p.body.find("<td class=\"link\">", r + 1)) {
        const size_t s = r + 17;
        const size_t e = p.body.find("</td>", s);
        if (e == std::string::npos) break;
        const std::string name = trim(strip_tags(p.body.substr(s, e - s)));
        if (name.empty() || name == "Parent directory/" || name.find(".info/") != std::string::npos) continue;
        std::string day;
        const size_t d = p.body.find("<td class=\"date\">", e);
        const size_t next = p.body.find("<td class=\"link\">", e);
        if (d != std::string::npos && (next == std::string::npos || d < next)) day = iso_day(p.body.substr(d + 17, 17));
        out[name] = day;
        const size_t z = p.body.find("<td class=\"size\">", e);
        if (sizes && z != std::string::npos && (next == std::string::npos || z < next)) {
            const size_t ze = p.body.find("</td>", z);
            std::string sz = trim(p.body.substr(z + 17, ze - z - 17));
            (*sizes)[name] = sz == "-" ? std::string() : sz;
        }
    }
    return true;
}

} // namespace

static Status status_once() {
    Status s;
    Page p = get(std::string(kSite) + "downloads/");
    if (!p.ok || p.status != 200) return s;
    s.answered = true;
    const size_t n = p.body.find("<p class=\"nav\">");
    if (n == std::string::npos) return s;
    const size_t e = p.body.find("</p>", n);
    s.banner = trim(strip_tags(p.body.substr(n + 15, e == std::string::npos ? 200 : e - n - 15)));
    s.signed_in = s.banner.find("Anonymous") == std::string::npos && s.banner.find("Log in") == std::string::npos;
    if (s.signed_in) {
        const size_t dot = s.banner.find("·");
        s.user = trim(s.banner.substr(0, dot));
        // « 5 of 100 files in the last 24 hours », dans le bandeau « allowance ».
        static const std::regex allowance(R"re((\d+) of (\d+) files in the last 24 hours)re");
        std::smatch m;
        if (std::regex_search(p.body, m, allowance)) { s.used = std::stoi(m[1]); s.limit = std::stoi(m[2]); }
    }
    std::cerr << "[RETROROMS] banner: " << s.banner << std::endl;
    return s;
}

// La session du site expire en quelques heures, la connexion du forum est retenue bien plus
// longtemps : quand le site ne nous reconnait plus, on lui fait relire celle du forum (sans
// mot de passe), et il ouvre une nouvelle session.
Status status() {
    Status s = status_once();
    std::error_code ec;
    if (s.answered && !s.signed_in && fs::exists(jar_path(), ec)) {
        get(std::string(kSite) + "login/start?go=%2Fdownloads%2F");
        s = status_once();
    }
    return s;
}

bool sign_in(const std::string& id, const std::string& password, std::string& why) {
    sign_out();
    // 1. Le site renvoie vers le forum, qui montre son formulaire de connexion.
    Page p = get(std::string(kSite) + "login/start?go=%2Fdownloads%2F");
    if (!p.ok) { why = _("RetroRoms does not answer."); return false; }
    std::cerr << "[RETROROMS] login start -> " << p.url << " (" << title_of(p.body) << ")" << std::endl;
    std::string action, relocate;
    std::string form = form_with(p.body, "member.php", &action);
    if (!form.empty()) {
        auto fields = inputs_of(form);
        for (const auto& f : fields) if (f.first == "relocate") relocate = f.second;
        std::vector<std::pair<std::string, std::string>> post;
        for (auto& f : fields) {
            if (f.first == "ID") f.second = id;
            else if (f.first == "Password") f.second = password;
            else if (f.first == "rememberMe") f.second = "1";
            post.push_back(f);
        }
        bool has_id = false, has_pw = false, has_remember = false;
        for (const auto& f : post) { has_id |= f.first == "ID"; has_pw |= f.first == "Password"; has_remember |= f.first == "rememberMe"; }
        if (!has_id) post.emplace_back("ID", id);
        if (!has_pw) post.emplace_back("Password", password);
        if (!has_remember) post.emplace_back("rememberMe", "1");
        // 2. Le formulaire du forum.
        p = get(action.empty() ? std::string(kForum) + "member.php" : action, encode_form(post));
        if (!p.ok) { why = _("The RetroRoms forum does not answer."); return false; }
        std::cerr << "[RETROROMS] after login -> " << p.url << " (" << title_of(p.body) << ")" << std::endl;
        if (!form_with(p.body, "member.php").empty() && p.url.find("bda.retroroms.info") == std::string::npos) {
            why = _("The forum refused this login : check the e-mail or user name and the password.");
            return false;
        }
    }
    // 3. Le forum repond « Please Wait » et renvoie en JavaScript vers
    // l'autorisation OAuth ; la premiere fois, celle-ci demande d'autoriser le
    // site de telechargement ; puis retour sur bda. Chaque etape est suivie.
    for (int step = 0; step < 5 && p.url.find("bda.retroroms.info") == std::string::npos; ++step) {
        std::string consent_action;
        const std::string consent = form_with(p.body, "oauth2", &consent_action);
        static const std::regex meta(R"re(http-equiv="refresh"[^>]*content="[^"]*url=([^"]+)")re", std::regex::icase);
        static const std::regex js(R"re(location(?:\.href)?\s*=\s*['"]([^'"]+)['"])re");
        std::smatch m;
        if (!consent.empty() && p.url.find("/m/oauth2/") != std::string::npos) {
            auto fields = inputs_of(consent);
            // Le bouton « autoriser » : un <button name=... value=...> n'est pas un input.
            static const std::regex button(R"re(<button[^>]*name="([^"]+)"[^>]*value="([^"]*)")re");
            if (std::regex_search(consent, m, button)) fields.emplace_back(m[1], m[2]);
            p = get(consent_action.empty() ? p.url : consent_action, encode_form(fields));
            std::cerr << "[RETROROMS] consent -> ";
        } else if (std::regex_search(p.body, m, meta) || std::regex_search(p.body, m, js)) {
            std::string next = html_decode(m[1]);
            if (next.rfind("http", 0) != 0) next = std::string(kForum) + (next[0] == '/' ? next.substr(1) : next);
            p = get(next);
            std::cerr << "[RETROROMS] redirect -> ";
        } else if (!relocate.empty()) {
            p = get(relocate);
            relocate.clear();
            std::cerr << "[RETROROMS] relocate -> ";
        } else {
            break;
        }
        std::cerr << p.url << " (" << title_of(p.body) << ")" << std::endl;
        if (!p.ok) break;
    }
    const Status s = status();
    if (s.signed_in) return true;
    why = s.answered ? _("Signed in on the forum, but the download site did not open the session. See the log.")
                     : _("RetroRoms does not answer.");
    return false;
}

void sign_out() {
    std::error_code ec;
    fs::remove(jar_path(), ec);
}

std::string folder_for(const std::string& emulator, const std::string& system) {
    if (emulator == "mame") return "downloads/mame/currentroms";
    if (emulator != "fbneo") return {};
    static const std::map<std::string, std::string> fbneo = {
        {"Arcade", "arcade"}, {"Neo Geo", "arcade"},
        {"Astrocade Home Computer", "astrocade"}, {"ColecoVision", "coleco"},
        {"FDS", "fds"}, {"Fairchild Channel F", "channelf"},
        {"Game Gear", "gamegear"}, {"MSX 1", "msx"}, {"Master System", "sms"},
        {"Megadrive", "megadrive"}, {"Neo Geo Pocket", "ngp"},
        {"PC-Engine", "pce"}, {"Sega SG-1000", "sg1000"},
        {"SuprGrafx", "sgx"}, {"TurboGrafx 16", "tg16"}, {"ZX Spectrum", "spectrum"},
    };
    auto it = fbneo.find(system);
    return it == fbneo.end() ? std::string() : "downloads/FBneo/ROMs_Nightly/" + it->second;
}

bool Catalog::has(const std::string& emulator, const std::string& system, const std::string& name,
                  bool is_chd, bool& known, std::string* date, std::string* size) {
    known = false;
    std::string folder = folder_for(emulator, system);
    if (folder.empty()) { known = true; return false; }
    if (is_chd) folder = "downloads/mame/CHDs";
    auto it = m_dirs.find(folder);
    if (it == m_dirs.end()) {
        Listing l;
        l.ok = list(folder, l.names, &l.sizes);
        it = m_dirs.emplace(folder, std::move(l)).first;
    }
    if (!it->second.ok) return false;
    known = true;
    auto n = it->second.names.find(is_chd ? name + "/" : name + ".zip");
    if (n == it->second.names.end()) return false;
    if (date) *date = n->second;
    if (size) { auto z = it->second.sizes.find(n->first); if (z != it->second.sizes.end()) *size = z->second; }
    return true;
}

DownloadSources::Result download(const std::string& emulator, const std::string& system, const std::string& name,
                                 bool is_chd, const std::string& dest_dir, const std::atomic<bool>& cancelled,
                                 const std::function<void(double)>& progress) {
    DownloadSources::Result r;
    DownloadSources::Fetch opt;
    opt.cookie_jar = jar_path();
    const std::string folder = folder_for(emulator, system);
    if (folder.empty()) { r.reason = _("not available on RetroRoms"); return r; }
    if (is_chd) {
        std::map<std::string, std::string> files;
        const std::string dir = "downloads/mame/CHDs/" + name;
        if (!list(dir, files)) { r.reason = _("not available on RetroRoms"); return r; }
        std::vector<std::string> disks;
        for (const auto& f : files)
            if (f.first.size() > 4 && f.first.compare(f.first.size() - 4, 4, ".chd") == 0) disks.push_back(f.first);
        if (disks.empty()) { r.reason = _("not available on RetroRoms"); return r; }
        const std::string out_dir = (fs::path(dest_dir) / name).string();
        std::error_code ec;
        fs::create_directories(out_dir, ec);
        for (size_t i = 0; i < disks.size(); ++i) {
            DownloadSources::Result one;
            if (!DownloadSources::fetch_file(std::string(kSite) + dir + "/" + url_encode(disks[i]),
                                             (fs::path(out_dir) / disks[i]).string(), opt, cancelled,
                                             [&](double f) { if (progress) progress((i + f) / disks.size()); }, one))
                return one;
        }
        r.ok = true;
        r.path = out_dir;
        return r;
    }
    DownloadSources::fetch_file(std::string(kSite) + folder + "/" + url_encode(name) + ".zip",
                                (fs::path(dest_dir) / (name + ".zip")).string(), opt, cancelled, progress, r);
    return r;
}

}
