// src/FbneoVideo.cpp
#include "FbneoVideo.h"

#include <poll.h>
#include <cerrno>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <mutex>
#include <utility>

#include "AppContext.h"

namespace FbneoVideo {

namespace {

/* La sortie standard d'une commande, avec un delai au-dela duquel on tue.
 *
 * Un FBNeo plus ancien ne connait pas -list-video-json : faute de nom de
 * jeu, il ouvrirait son menu et ne rendrait jamais la main. Les pilotes
 * « dummy » de la SDL l'empechent d'ouvrir une fenetre sous les yeux du
 * joueur, et le delai l'empeche de bloquer Bootcade. `env` plutot que des
 * variables posees ici : flatpak-spawn ne transmet pas notre environnement,
 * alors que la commande, elle, part telle quelle sur l'hote.
 */
std::string capture(const std::string& exe, int timeout_ms) {
    const std::vector<std::string> cmd = AppContext::host_command(
        {"env", "SDL_VIDEODRIVER=dummy", "SDL_AUDIODRIVER=dummy", exe, "-list-video-json"});

    int fds[2];
    if (pipe(fds) != 0) return {};
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return {}; }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, STDIN_FILENO); dup2(null, STDERR_FILENO); close(null); }
        std::vector<char*> argv;
        for (const auto& a : cmd) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(fds[1]);

    std::string out;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool timed_out = false;
    std::array<char, 1 << 14> buf;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) { timed_out = true; break; }
        pollfd p{fds[0], POLLIN, 0};
        const int r = poll(&p, 1, static_cast<int>(left));
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) { timed_out = true; break; }
        const ssize_t n = read(fds[0], buf.data(), buf.size());
        if (n <= 0) break;
        out.append(buf.data(), static_cast<size_t>(n));
        if (out.size() > (1u << 20)) { timed_out = true; break; }   // ce n'est plus un JSON de reglages
    }
    close(fds[0]);
    if (timed_out) {
        kill(pid, SIGTERM);
        for (int i = 0; i < 20 && waitpid(pid, nullptr, WNOHANG) == 0; ++i) usleep(25000);
        kill(pid, SIGKILL);
        out.clear();
    }
    waitpid(pid, nullptr, 0);
    return out;
}

Range read_range(const nlohmann::json& ranges, const char* key, Range fallback) {
    if (!ranges.contains(key) || !ranges[key].is_object()) return fallback;
    const auto& r = ranges[key];
    Range out = fallback;
    out.min = r.value("min", fallback.min);
    out.max = r.value("max", fallback.max);
    out.def = r.value("default", fallback.def);
    return out;
}

}  // namespace

const Filter* Capabilities::filter(int index) const {
    for (const auto& f : softfx)
        if (f.index == index) return &f;
    return nullptr;
}

bool Capabilities::has_mask(int index) const {
    return std::any_of(rgbmask.begin(), rgbmask.end(),
                       [index](const Mask& m) { return m.index == index; });
}

bool Capabilities::has_renderer(const std::string& name) const {
    return std::find(renderers.begin(), renderers.end(), name) != renderers.end();
}

Capabilities parse(const std::string& text) {
    Capabilities caps;
    // Le JSON est ecrit avant toute autre impression ; on tolere tout de meme
    // un prefixe, qu'une bibliotheque bavarde pourrait glisser devant.
    const auto start = text.find('{');
    if (start == std::string::npos) return caps;
    try {
        const auto j = nlohmann::json::parse(text.begin() + static_cast<std::ptrdiff_t>(start), text.end());
        if (!j.is_object() || !j.contains("softfx") || !j["softfx"].is_array()) return caps;
        for (const auto& f : j["softfx"]) {
            if (!f.is_object()) continue;
            Filter one;
            one.index     = f.value("index", -1);
            one.name      = f.value("name", std::string());
            one.zoom      = f.value("zoom", 1);
            one.available = f.value("available", false);
            if (f.contains("depths") && f["depths"].is_array())
                for (const auto& d : f["depths"]) if (d.is_number_integer()) one.depths.push_back(d.get<int>());
            if (one.index >= 0 && !one.name.empty()) caps.softfx.push_back(std::move(one));
        }
        if (j.contains("rgbmask") && j["rgbmask"].is_array())
            for (const auto& m : j["rgbmask"]) {
                if (!m.is_object()) continue;
                Mask one{m.value("index", 0), m.value("name", std::string())};
                if (one.index > 0 && !one.name.empty()) caps.rgbmask.push_back(std::move(one));
            }
        if (j.contains("renderers") && j["renderers"].is_array())
            for (const auto& r : j["renderers"])
                if (r.is_string() && !r.get<std::string>().empty()) caps.renderers.push_back(r.get<std::string>());
        if (j.contains("ranges") && j["ranges"].is_object()) {
            const auto& r = j["ranges"];
            caps.softfx_range        = read_range(r, "softfx",        caps.softfx_range);
            caps.rgbmask_range       = read_range(r, "rgbmask",       caps.rgbmask_range);
            caps.internalres_range   = read_range(r, "internalres",   caps.internalres_range);
            caps.scanintensity_range = read_range(r, "scanintensity", caps.scanintensity_range);
            if (r.contains("renderer") && r["renderer"].is_object())
                caps.renderer_default = r["renderer"].value("default", std::string());
        }
        caps.ok = true;
    } catch (...) {
        return Capabilities{};
    }
    return caps;
}

Capabilities probe(const std::string& exe) {
    static std::mutex m;
    static std::map<std::pair<std::string, long long>, Capabilities> cache;
    if (exe.empty() || ::access(exe.c_str(), X_OK) != 0) return {};
    struct stat st{};
    const long long stamp = ::stat(exe.c_str(), &st) == 0 ? static_cast<long long>(st.st_mtime) : 0;

    // Le verrou couvre la sonde elle-meme : un lancement qui arrive pendant
    // celle des reglages attend sa reponse au lieu d'en relancer une.
    std::lock_guard<std::mutex> lock(m);
    const auto key = std::make_pair(exe, stamp);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    Capabilities caps = parse(capture(exe, 3000));
    cache[key] = caps;
    return caps;
}

void Options::load(const nlohmann::json& o) {
    if (!o.is_object()) { *this = Options{}; return; }
    const Options d;
    softfx_on      = o.value("softfx_on",      d.softfx_on);
    softfx         = o.value("softfx",         d.softfx);
    scanlines      = o.value("scanlines",      d.scanlines);
    scanintensity  = std::clamp(o.value("scanintensity", d.scanintensity), 0, 255);
    rgbmask_on     = o.value("rgbmask_on",     d.rgbmask_on);
    rgbmask        = o.value("rgbmask",        d.rgbmask);
    stretch        = o.value("stretch",        d.stretch);
    internalres_on = o.value("internalres_on", d.internalres_on);
    internalres    = o.value("internalres",    d.internalres);
    renderer_on    = o.value("renderer_on",    d.renderer_on);
    renderer       = o.value("renderer",       d.renderer);
}

void Options::save(nlohmann::json& o) const {
    o = nlohmann::json::object();
    o["softfx_on"]      = softfx_on;
    o["softfx"]         = softfx;
    o["scanlines"]      = scanlines;
    o["scanintensity"]  = scanintensity;
    o["rgbmask_on"]     = rgbmask_on;
    o["rgbmask"]        = rgbmask;
    o["stretch"]        = stretch;
    o["internalres_on"] = internalres_on;
    o["internalres"]    = internalres;
    o["renderer_on"]    = renderer_on;
    o["renderer"]       = renderer;
}

/* Rien n'est passe quand la valeur est celle par defaut : FBNeo garde alors
 * ce que dit son fbneo.ini, et une ligne de commande courte reste lisible
 * dans le journal. Une valeur que le binaire n'accepte pas (filtre retire
 * d'un build, backend absent de cette machine) est ignoree de la meme facon
 * plutot que transmise. */
std::vector<std::string> Options::launch_args(const Capabilities& caps) const {
    std::vector<std::string> args;
    if (!caps.ok) return args;
    auto valued = [&args](const char* option, const std::string& value) {
        args.emplace_back(option);
        args.push_back(value);
    };
    if (softfx_on && softfx != caps.softfx_range.def) {
        const Filter* f = caps.filter(softfx);
        if (f && f->available) valued("-softfx", std::to_string(softfx));
    }
    if (scanlines) {
        args.emplace_back("-scanlines");
        if (scanintensity != caps.scanintensity_range.def &&
            caps.scanintensity_range.contains(scanintensity))
            valued("-scanintensity", std::to_string(scanintensity));
    }
    if (rgbmask_on && rgbmask != caps.rgbmask_range.def && caps.has_mask(rgbmask))
        valued("-rgbmask", std::to_string(rgbmask));
    if (stretch) args.emplace_back("-stretch");
    if (internalres_on && internalres != caps.internalres_range.def &&
        caps.internalres_range.contains(internalres))
        valued("-internalres", std::to_string(internalres));
    if (renderer_on && renderer != caps.renderer_default && caps.has_renderer(renderer))
        valued("-renderer", renderer);
    return args;
}

int Options::active_count(const Capabilities& caps) const {
    if (!caps.ok) return 0;
    int n = 0;
    const auto args = launch_args(caps);
    for (const auto& a : args)
        if (a == "-softfx" || a == "-scanlines" || a == "-rgbmask" || a == "-stretch" ||
            a == "-internalres" || a == "-renderer")
            ++n;
    return n;
}

}  // namespace FbneoVideo
