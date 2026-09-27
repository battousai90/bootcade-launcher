// src/GenerateDAT.h
#pragma once

#include <gtkmm.h>
#include <string>

class GenerateDAT {
public:
    // dat_path: where the DAT files should land : written into FBNeo's own
    // fbneo.ini (szAppDatListsPath) before running it, so the launcher tells
    // FBNeo where to write instead of guessing where it might have decided to
    // write on its own. Pass empty to fall back to FBNeo's own default
    // (~/.local/share/fbneo/support/lists/dat) for a first-ever run with
    // nothing configured yet.
    static void execute(Gtk::Window& parent, const std::string& fbneo_executable,
                         const std::string& dat_path, Gtk::Entry* dat_entry = nullptr);

    // Le meme geste, pour MAME. Rien n'est lance ici : MAME n'ecrit pas de
    // DAT, c'est MameCatalog::generate_dats qui convertit sa sortie -listxml
    // en Logiqx. La conversion dure une dizaine de secondes et tourne donc
    // dans un fil, la fenetre de progression restant vivante sur le fil
    // principal. Le bouton Annuler interrompt la conversion en cours.
    static void execute_mame(Gtk::Window& parent, const std::string& mame_executable,
                             const std::string& dat_path, Gtk::Entry* dat_entry = nullptr);

private:
    static void show_success_dialog(Gtk::Window& parent, const std::string& dat_path, Gtk::Entry* dat_entry);
};