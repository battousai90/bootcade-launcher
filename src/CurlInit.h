// src/CurlInit.h
//
// <curl/curl.h>, et sous Windows une chose de plus : les certificats.
//
// Le libcurl de MSYS2 cherche ses autorites de certification a l'endroit ou
// MSYS2 les range (C:/msys64/mingw64/...), qui n'existe pas sur la machine d'un
// joueur : tout HTTPS echoue alors sur « SSL CA ». On lui donne le fichier livre
// avec Bootcade (ssl/certs/ca-bundle.crt, a cote de l'exe), et le magasin de
// certificats de Windows en complement, pour les reseaux d'entreprise qui
// signent avec leur propre autorite.
//
// curl_easy_init() et curl_easy_reset() sont redefinis pour que les quarante
// appels du programme en profitent sans changer. Sous Linux : curl.h, rien d'autre.
#pragma once
#include <curl/curl.h>

#ifdef _WIN32

#include "Platform.h"

#include <filesystem>
#include <string>

namespace BcCurl {

inline void setup_ca(CURL* h) {
    if (!h) return;
    static const std::string bundle = [] {
        std::string exe = Platform::executable_path();
        exe = exe.substr(0, exe.find_last_of("\\/"));
        const std::string p = exe + "\\ssl\\certs\\ca-bundle.crt";
        std::error_code ec;
        return std::filesystem::exists(p, ec) ? p : std::string();
    }();
    if (!bundle.empty()) curl_easy_setopt(h, CURLOPT_CAINFO, bundle.c_str());
#ifdef CURLSSLOPT_NATIVE_CA
    curl_easy_setopt(h, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif
}

inline CURL* init() {
    CURL* h = curl_easy_init();
    setup_ca(h);
    return h;
}

inline void reset(CURL* h) {
    curl_easy_reset(h);
    setup_ca(h);
}

}  // namespace BcCurl

#define curl_easy_init()     BcCurl::init()
#define curl_easy_reset(h)   BcCurl::reset(h)

#endif
