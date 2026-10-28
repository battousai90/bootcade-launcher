// src/CrcDb.h
//
// La base de CRC des sources de telechargement (LolRoms, RetroRoms), publiee
// par CT 105 (generate-crcdb.py) dans https://files.bootcade.duckdns.org/crcdb/ :
// ce que chaque fichier du site contient, pour ne proposer que les jeux dont
// tous les CRC du DAT y sont. Un fichier a une autre version coute un
// telechargement (et, chez RetroRoms, un du quota) pour rien : la base le dit
// avant.
//
// Contrat v1 : crcdb-manifest.json (sha256 de chaque fichier de la base), puis
// <source>.json :
//   { "schema_version": 1, "files": { "<cle>": { "d": "2026-05-28",
//       "b": "full" | "read" | "pending", "c": ["crc", ...], "z": 211968 } },
//     "chds": { "<jeu>": { "z": 3328599654, "f": { "<disque>.chd": 3328599654 } } } }
// b = « full » : CRC deduits du DAT de la version complete ; « read » : lus
// dans le fichier ; « pending » : modifie depuis, pas encore lu (jamais propose).
// z = taille du fichier du site en octets (d'apres la taille affichee, arrondie) ;
// « chds » : la taille de ce qu'un CHD fait telecharger (tous les disques du
// dossier du jeu), que RetroRoms n'affiche pas. Les deux sont facultatifs.
//
// Depuis le 2026-10-27, la base dit aussi OU telecharger, pour que le launcher
// ne lise plus les pages des sites (qui changent sans prevenir) :
//   "systems": { "<emulateur>": { "<systeme du DAT>" | "*": "<prefixe de cle>" } },
//   "urls":    { "<prefixe de cle>": "https://...{name}.7z" },
//   "chd_url": "https://...{name}...{disk}"
// La cle d'un jeu est « prefixe/jeu » (« jeu » si le prefixe est vide). Une
// base sans ces champs (homelab pas encore a jour) : routed() est faux, le
// launcher lit le site comme avant.
//
// Appels bloquants (reseau) : hors du fil GTK.
#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace CrcDb {

enum class Verdict {
    Match,      // tous les CRC du DAT sont dans le fichier du site
    Mismatch,   // le fichier existe, mais une autre version que le DAT
    Pending,    // modifie depuis la version complete, pas encore lu
    Unknown,    // pas dans la base
};

class Db {
public:
    // Charge la base d'une source (« retroroms-mame.json », « lolroms.json ») :
    // celle du cache si elle est a jour, sinon telechargee et verifiee. Faux si
    // ni le reseau ni le cache n'ont rien donne.
    bool load(const std::string& file, std::string* error = nullptr);
    bool loaded() const { return m_loaded; }
    // Verifie le jeu `key` contre ses CRC (ceux de ses propres ROMs dans le DAT).
    // `have` recoit le nombre de ces CRC que le fichier contient.
    Verdict check(const std::string& key, const std::vector<unsigned long>& crcs, int* have = nullptr) const;
    size_t size() const { return m_entries.size(); }
    // Ce que le joueur va telecharger, en octets ; 0 si la base ne le sait pas.
    unsigned long long bytes_of(const std::string& key) const;
    unsigned long long chd_bytes(const std::string& name) const;

    // La base dit ou telecharger (voir plus haut) : le site ne se lit plus.
    bool routed() const { return !m_systems.empty() && !m_urls.empty(); }
    // La cle d'un jeu sur cette source, ou "" si elle ne propose pas ce systeme.
    std::string key_for(const std::string& emulator, const std::string& system, const std::string& name) const;
    // Le site a-t-il ce fichier ? `date` recoit sa date (AAAA-MM-JJ).
    bool has(const std::string& key, std::string* date = nullptr) const;
    // L'adresse du fichier d'une cle, ou "".
    std::string url_for(const std::string& key) const;
    // Les disques d'un CHD : (adresse, nom du disque). Vide si le site n'a pas ce CHD.
    std::vector<std::pair<std::string, std::string>> chd_files(const std::string& name) const;
    std::string generated() const { return m_generated; }

private:
    struct Entry { char basis = 'p'; std::vector<unsigned long> crcs; unsigned long long bytes = 0; std::string date; };   // crcs trie
    std::unordered_map<std::string, Entry> m_entries;
    std::unordered_map<std::string, unsigned long long> m_chd_bytes;
    std::unordered_map<std::string, std::vector<std::string>> m_chd_disks;   // jeu -> ses disques .chd
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> m_systems;   // emulateur -> systeme -> prefixe
    std::unordered_map<std::string, std::string> m_urls;   // prefixe -> adresse avec {name}
    std::string m_chd_url;
    std::string m_generated;
    bool m_loaded = false;
};

// Le fichier de la base pour une source et un emulateur, ou "" s'il n'y en a pas.
std::string file_for(const std::string& source_kind, const std::string& emulator);
// La cle d'un jeu dans ce fichier.
std::string key_for(const std::string& source_kind, const std::string& lol_folder, const std::string& name);

}
