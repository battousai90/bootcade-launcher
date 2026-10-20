// src/Pager.h : « lignes par page » et pages precedente / suivante, au pied d'une liste.
#pragma once

#include <gtkmm.h>

namespace SettingsUi {

class Pager : public Gtk::Box {
public:
    Pager();

    // Le nombre de lignes du resultat (apres filtres) : recalcule le nombre de pages et
    // ramene la page courante dans les bornes.
    void set_total(int rows);
    // Retour a la premiere page (un filtre a change).
    void reset() { m_index = 0; }
    int  page_index() const { return m_index; }
    int  page_size()  const { return m_size; }   // 0 : tout

    // Le joueur a change de page ou de taille de page.
    sigc::signal<void>& signal_changed() { return m_changed; }

private:
    void go(int index);
    void update();

    Gtk::ComboBoxText m_combo;
    Gtk::Button       m_first, m_prev, m_next, m_last;
    Gtk::Label        m_label;
    int               m_total = 0, m_index = 0, m_size = 50;
    bool              m_updating = false;
    sigc::signal<void> m_changed;
};

}  // namespace SettingsUi
