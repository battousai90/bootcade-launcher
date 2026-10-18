// src/DownloadSources.h
//
// Les sources de telechargement du ROM Manager : d'ou l'onglet Downloads va
// chercher les jeux manquants.
//
// Trois sont predefinies et ne se suppriment pas :
//   bootcade   le serveur Bootcade, avec le compte du joueur et son quota
//   lolroms    lolroms.com, sans compte
//   retroroms  bda.retroroms.info, avec un compte retroroms et 100 jeux / 24 h
// Le joueur peut en ajouter d'autres : une adresse, et au besoin un
// identifiant et un mot de passe (authentification HTTP). Chacune s'active ou
// se desactive pour le telechargement.
//
// Le tout vit dans config.json (rom_manager.download_sources). Les mots de
// passe des sources ajoutees vont dans un fichier a part, lisible du seul
// joueur (download-secrets.json) ; pour RetroRoms, aucun mot de passe n'est
// garde : seulement la session, comme un navigateur.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace DownloadSources {

struct Source {
    std::string id;           // "bootcade", "lolroms", "retroroms", ou "custom-<n>"
    std::string kind;         // "bootcade" | "lolroms" | "retroroms" | "custom"
    std::string name;
    std::string url;          // custom : modele d'adresse ({name}, {system}, {ext})
    std::string user;         // custom : identifiant HTTP
    std::string emulators = "fbneo";   // custom : "fbneo", "mame" ou "fbneo,mame"
    bool        enabled = true;
    int         daily_quota = 0;       // 0 : pas de quota connu
    bool builtin() const { return kind != "custom"; }
    bool serves(const std::string& emulator) const;
};

std::vector<Source> load();
void save(const std::vector<Source>& sources);
Source find(const std::string& id);   // .id vide si inconnue

// Mots de passe des sources ajoutees (fichier a part, droits 600).
std::string password(const std::string& id);
void set_password(const std::string& id, const std::string& pw);

// Quota compte par Bootcade : les telechargements reussis des dernieres 24 h.
void record_download(const std::string& id);
int  used_last_24h(const std::string& id);

// Un jeu recu dans une autre version que celle du DAT (CRC differents), et la
// date du fichier sur la source a ce moment-la. Tant que la source garde ce
// fichier (meme date), il n'est plus propose ; s'il change, il l'est de nouveau.
void remember_wrong(const std::string& id, const std::string& key, const std::string& site_date);
bool known_wrong(const std::string& id, const std::string& key, const std::string& site_date);

// Ou vont les jeux : "inbox" (Import les verifie et les range), "outbox"
// (prets a passer dans la bibliotheque), "library" (directement), ou
// "folder" (download_folder).
std::string download_target();
void set_download_target(const std::string& target);

// Le dossier des telechargements (rom_manager.download_folder) ; vide : le
// dossier d'import.
std::string download_folder();
void set_download_folder(const std::string& folder);
std::string chosen_source();
void set_chosen_source(const std::string& id);

// Une source ajoutee : son adresse pour un jeu, puis le telechargement.
// {name} le jeu, {system} le systeme de la base, {ext} zip ou 7z. Sans
// {name}, « /{name}.zip » est ajoute a l'adresse.
std::string custom_url(const Source& s, const std::string& system, const std::string& name);

struct Result {
    bool        ok = false;
    std::string path;
    std::string reason;
    long        http_status = 0;
    bool        site_fault = false;   // le site ne repond pas : inutile de continuer
    bool        transferred = false;  // un fichier est arrive (meme refuse ensuite) : il compte au quota
    std::string quota_header;         // ce que la reponse dit du quota, s'il le dit
};
// Un fichier vers `target` (par un .part, qu'Import ne voit jamais). Une page
// HTML recue a la place d'un fichier est un refus, pas un telechargement.
// `cookie_jar` : fichier de session (RetroRoms) ; `userpwd` : « user:mot ».
struct Fetch {
    std::string cookie_jar;
    std::string userpwd;
};
bool fetch_file(const std::string& url, const std::string& target, const Fetch& opt,
                const std::atomic<bool>& cancelled, const std::function<void(double)>& progress,
                Result& r);

Result download_custom(const Source& s, const std::string& system, const std::string& name,
                       const std::string& dest_dir, const std::atomic<bool>& cancelled,
                       const std::function<void(double)>& progress);

}
