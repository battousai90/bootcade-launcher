// src/FileChooser.h
//
// Le selecteur de fichier ou de dossier. Sous Linux c'est celui de GTK, tel
// quel. Sous Windows c'est celui de Windows : un joueur s'attend a l'Explorateur
// qu'il connait (acces rapide, OneDrive, ses lecteurs, sa mise a l'echelle), pas
// a un selecteur GTK d'une autre epoque dont la taille ignore celle de l'ecran.
//
// Meme interface que Gtk::FileChooserDialog pour les appels qu'utilise le
// programme, pour que les appelants ne changent que le nom du type.
#pragma once
#include <gtkmm.h>

#ifdef _WIN32

#include <string>
#include <vector>

class BcFileChooser {
public:
    BcFileChooser(const Glib::ustring& title, Gtk::FileChooserAction action)
        : m_native(Gtk::FileChooserNative::create(title, action)) {}
    BcFileChooser(Gtk::Window& parent, const Glib::ustring& title, Gtk::FileChooserAction action)
        : m_native(Gtk::FileChooserNative::create(title, parent, action)) {}

    void set_transient_for(Gtk::Window& parent) { m_native->set_transient_for(parent); }

    // Les deux seuls boutons du selecteur Windows : celui qui annule, et celui
    // qui valide. run() rend l'identifiant que l'appelant a donne au second.
    void add_button(const Glib::ustring& label, int response) {
        if (response == Gtk::RESPONSE_CANCEL || response == Gtk::RESPONSE_CLOSE) {
            m_native->set_cancel_label(label);
        } else {
            m_native->set_accept_label(label);
            m_accept = response;
        }
    }

    bool set_current_folder(const std::string& f) { return m_native->set_current_folder(f); }
    std::string get_current_folder() const { return m_native->get_current_folder(); }
    bool set_filename(const std::string& f) { return m_native->set_filename(f); }
    std::string get_filename() const { return m_native->get_filename(); }
    std::vector<std::string> get_filenames() const { return m_native->get_filenames(); }
    void set_current_name(const Glib::ustring& n) { m_native->set_current_name(n); }
    void set_do_overwrite_confirmation(bool b = true) { m_native->set_do_overwrite_confirmation(b); }
    void set_select_multiple(bool b = true) { m_native->set_select_multiple(b); }
    void add_filter(const Glib::RefPtr<Gtk::FileFilter>& f) { m_native->add_filter(f); }
    void hide() { m_native->hide(); }

    int run() {
        const int r = m_native->run();
        return r == Gtk::RESPONSE_ACCEPT || r == Gtk::RESPONSE_OK ? m_accept : Gtk::RESPONSE_CANCEL;
    }

private:
    Glib::RefPtr<Gtk::FileChooserNative> m_native;
    int m_accept = Gtk::RESPONSE_ACCEPT;
};

#else

using BcFileChooser = Gtk::FileChooserDialog;

#endif
