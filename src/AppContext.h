// src/AppContext.h
#pragma once
#include <string>
#include <vector>

class AppContext {
public:
    static std::string get_executable_dir();
    static std::string get_user_config_dir();
    static std::string get_config_path();
    // config.json exists and holds a JSON object once this has run : created
    // empty on a first start, set aside (config.json.unreadable-<time>) and
    // recreated when it cannot be read. Called once, at the very start : many
    // readers take its keys with json::value(), which throws on a missing file.
    static void ensure_config();

    // Root holding assets/ and locale/. Resolved once, by probing the layouts the
    // app can actually be run from: the build tree, a portable/AppImage bundle, and
    // an FHS install where the binary lands in /usr/bin while its data goes to
    // /usr/share/bootcade.
    static std::string get_data_dir();
    static std::string get_asset_path(const std::string& subpath);
    static std::string get_locale_dir();

    /* Rend au systeme la memoire liberee mais gardee par l'allocateur.
     *
     * Un balayage, un audit ou un rechargement de DAT allouent des centaines
     * de Mo de temporaires puis les liberent ; glibc les garde dans ses
     * arenes (mesure : 334 Mo utilises pour 708 Mo detenus apres un
     * balayage) et le moniteur du bureau les compte a l'application.
     * A appeler a la FIN de ces operations, jamais au milieu. */
    static void trim_heap();

    // True when running inside a Flatpak sandbox.
    static bool in_flatpak();

    // Prefix a command so it runs on the host when sandboxed, unchanged otherwise.
    //
    // FinalBurn Neo lives on the host and links against the host's libraries (SDL2
    // and friends), which do not exist inside the sandbox : executing it directly
    // from a Flatpak fails with "error while loading shared libraries". flatpak-spawn
    // hands it to the host instead; that is what the --talk-name=org.freedesktop.Flatpak
    // permission in the manifest exists for.
    static std::vector<std::string> host_command(const std::vector<std::string>& args);

private:
    static std::string s_executable_dir;
    static std::string s_data_dir;
};