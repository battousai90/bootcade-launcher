// src/SourcesDialog.h
//
// « Manage » de l'onglet Downloads : les sources de telechargement, leur
// compte, et celles que le joueur ajoute (adresse + authentification).
// Chacune s'active ou se desactive pour le telechargement.
#pragma once

#include <gtkmm.h>

#include <memory>
#include <string>

#include "DownloadSources.h"

class SourcesDialog : public Gtk::Dialog {
public:
    explicit SourcesDialog(Gtk::Window& parent);
    ~SourcesDialog() override;

    // Le compte Bootcade a change : la fenetre principale le montre aussi.
    sigc::signal<void>& signal_account_changed() { return m_sig_account; }

private:
    void rebuild();
    Gtk::Widget* source_row(const DownloadSources::Source& s);
    void edit_source(const std::string& id);   // "" : nouvelle
    void retroroms_sign_in();
    void refresh_retroroms_status();

    Gtk::Window& m_parent;
    Gtk::Box     m_list{Gtk::ORIENTATION_VERTICAL, 0};
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    std::string  m_retro_text;      // ce que dit le site du compte RetroRoms
    bool         m_retro_busy = false;
    sigc::signal<void> m_sig_account;
};
