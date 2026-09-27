// src/ModelColumns.h
#pragma once
#include <gtkmm.h>

struct ModelColumns : public Gtk::TreeModel::ColumnRecord {
    Gtk::TreeModelColumn<Glib::RefPtr<Gdk::Pixbuf>> m_col_icon;
    Gtk::TreeModelColumn<bool>          m_col_favorite;  // ★ toggle
    // "●" when the score service can rank this game, empty otherwise. Text
    // rather than bool so the column shows nothing at all on the vast
    // majority of rows : a column of empty checkboxes across 29 000 games
    // would read as "unchecked", i.e. as a promise the game broke.
    Gtk::TreeModelColumn<Glib::ustring> m_col_hiscore;
    // ISO-8601 or empty. Carried in the model so "recently played" can sort
    // without a database round-trip per row.
    Gtk::TreeModelColumn<Glib::ustring> m_col_last_played;
    // De quel catalogue vient la ligne : « fbneo » ou « mame ». Sans elle,
    // deux machines de meme nom dans les deux catalogues seraient
    // indiscernables au moment de lancer.
    Gtk::TreeModelColumn<Glib::ustring> m_col_emulator;
    Gtk::TreeModelColumn<Glib::ustring> m_col_name;
    Gtk::TreeModelColumn<Glib::ustring> m_col_title;
    Gtk::TreeModelColumn<Glib::ustring> m_col_year;
    Gtk::TreeModelColumn<Glib::ustring> m_col_manufacturer;
    Gtk::TreeModelColumn<Glib::ustring> m_col_system;
    Gtk::TreeModelColumn<Glib::ustring> m_col_status;
    
    // Video columns
    Gtk::TreeModelColumn<Glib::ustring> m_col_video_type;
    Gtk::TreeModelColumn<Glib::ustring> m_col_orientation;
    Gtk::TreeModelColumn<Glib::ustring> m_col_width;
    Gtk::TreeModelColumn<Glib::ustring> m_col_height;
    Gtk::TreeModelColumn<Glib::ustring> m_col_aspect;
    
    // Driver column
    Gtk::TreeModelColumn<Glib::ustring> m_col_driver_status;
    
    // Additional columns
    Gtk::TreeModelColumn<Glib::ustring> m_col_comment;
    Gtk::TreeModelColumn<Glib::ustring> m_col_cloneof;
    Gtk::TreeModelColumn<Glib::ustring> m_col_sourcefile;

    ModelColumns() {
        add(m_col_icon);
        add(m_col_favorite);
        add(m_col_hiscore);
        add(m_col_last_played);
        add(m_col_emulator);
        add(m_col_name);
        add(m_col_title);
        add(m_col_year);
        add(m_col_manufacturer);
        add(m_col_system);
        add(m_col_status);
        add(m_col_video_type);
        add(m_col_orientation);
        add(m_col_width);
        add(m_col_height);
        add(m_col_aspect);
        add(m_col_driver_status);
        add(m_col_comment);
        add(m_col_cloneof);
        add(m_col_sourcefile);
    }
};