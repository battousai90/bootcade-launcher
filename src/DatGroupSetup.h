// src/DatGroupSetup.h
//
// Apres l'installation d'un emulateur : le groupe de DAT de cette version
// (EmulatorVariants) est cree s'il n'existe pas, ses DAT sont generes par son
// executable, et il devient le groupe actif de cet emulateur. Un groupe deja
// la garde son dossier et son nom : le joueur a pu les choisir.
#pragma once

#include <gtkmm/window.h>
#include <string>

namespace DatGroupSetup {

// Vrai si les DAT ont ete generes. Montre ses propres fenetres de progression.
bool install_and_generate(Gtk::Window& parent, const std::string& emulator, const std::string& variant);

}
