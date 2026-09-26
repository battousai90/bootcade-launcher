// src/DatParser.h
#pragma once
#include <vector>
#include <memory>
#include "Game.h"
#include "DatabaseManager.h"

class DatParser {
public:
    static std::vector<Game> parse(const std::string& filepath);
    static std::vector<Game> parseAllDats(const std::string& directory);
    
    // New database-based methods
    // Imports one DAT file ; returns the number of games, -1 on error. A raw
    // MAME -listxml file is converted first (MameCatalog::convert_listxml_file)
    // and its three resolved DATs imported under its own file name. `note`,
    // when given, receives one line worth logging about what was done (a
    // conversion, a software list skipped), or stays empty.
    static int parseToDatabase(const std::string& filepath, std::shared_ptr<DatabaseManager> db,
                               std::string* note = nullptr);

    // What a file in a DAT folder is. A .dat is a Logiqx datafile (as it
    // always was) unless its root is <mame> ; a .xml counts only when its
    // root is <datafile> or <mame>, so unrelated XML files are left alone.
    enum class DatKind { None, Datafile, MameListxml };
    static DatKind datKind(const std::string& filepath);

    // "MAME Software List …" : media lists, which the ROM Manager does not
    // import (yet).
    static bool isSoftwareListHeader(const std::string& headerName);
    static bool parseAllDatsToDatabase(const std::string& directory, std::shared_ptr<DatabaseManager> db);
    static bool synchronizeDatsToDatabase(const std::string& directory, std::shared_ptr<DatabaseManager> db);
    // L'emulateur dont vient un DAT, deduit de son en-tete.
    //
    // Un fichier DAT ne porte aucune autre marque d'origine : ni le nom du
    // fichier (l'utilisateur le renomme), ni le dossier (les deux emulateurs
    // peuvent partager le meme) ne sont fiables. L'en-tete <header><name>,
    // lui, est ecrit par le producteur du DAT et voyage avec le fichier.
    // Rend "mame" ou "fbneo" ; un en-tete inconnu vaut "fbneo", le seul
    // catalogue qui existait avant.
    static std::string emulatorFromHeader(const std::string& headerName);

    // Le systeme d'un DAT, tel que la colonne games.system le range :
    // « FinalBurn Neo - Arcade Games » → « Arcade », « MAME ROMs (split) » →
    // « ROMs (split) ». « Unknown » si l'en-tete ne suit aucun des deux usages.
    static std::string extractSystemFromHeader(const std::string& headerName);

private:
    // One transaction for a whole DAT's sets. Returns how many, -1 on error.
    static int insertGames(const std::vector<Game>& games, std::shared_ptr<DatabaseManager> db);
    // One Logiqx datafile into the database, its games recorded under
    // `dat_source` ; no dat_files registration.
    static int importDatafile(const std::string& filepath, std::shared_ptr<DatabaseManager> db,
                              const std::string& dat_source, std::string* note);
};