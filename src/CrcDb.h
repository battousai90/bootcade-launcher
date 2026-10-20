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
//       "b": "full" | "read" | "pending", "c": ["crc", ...] } } }
// b = « full » : CRC deduits du DAT de la version complete ; « read » : lus
// dans le fichier ; « pending » : modifie depuis, pas encore lu (jamais propose).
//
// Appels bloquants (reseau) : hors du fil GTK.
#pragma once

#include <string>
#include <unordered_map>
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
    std::string generated() const { return m_generated; }

private:
    struct Entry { char basis = 'p'; std::vector<unsigned long> crcs; };   // trie
    std::unordered_map<std::string, Entry> m_entries;
    std::string m_generated;
    bool m_loaded = false;
};

// Le fichier de la base pour une source et un emulateur, ou "" s'il n'y en a pas.
std::string file_for(const std::string& source_kind, const std::string& emulator);
// La cle d'un jeu dans ce fichier.
std::string key_for(const std::string& source_kind, const std::string& lol_folder, const std::string& name);

}
