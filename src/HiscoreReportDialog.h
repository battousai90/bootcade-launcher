// src/HiscoreReportDialog.h
//
// Les deux fenetres des signalements de scores.
//
// ReportDialog : le joueur vient de finir une partie qui n'a pas compte comme
// il l'attendait. Il dit le score vu a l'ecran, un mot, et joint une capture :
// c'est ce qui permet a l'administrateur de trancher sans deviner.
//
// MyReportsDialog : ses signalements et nos reponses. C'est ce qui lui montre
// qu'on l'a lu ; sans retour, signaler ressemble a parler dans le vide.
#pragma once

#include <gtkmm.h>
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "HiscoreClient.h"

class ReportDialog : public Gtk::Dialog {
public:
    // `title` : le nom du jeu tel que le joueur le connait. `screenshots` :
    // les captures de la partie, la plus recente en dernier ; proposee jointe.
    ReportDialog(Gtk::Window& parent, const HiscoreClient::ReportForm& target,
                 const std::string& title, const std::vector<std::string>& screenshots);
    ~ReportDialog() override;

private:
    void update_send_state();
    void on_send();
    void on_sent();
    void show_status(const Glib::ustring& text, const char* tone);

    HiscoreClient::ReportForm m_form;
    Gtk::Box         m_box{Gtk::ORIENTATION_VERTICAL, 12};
    Gtk::Label       m_intro;
    Gtk::Entry       m_score;
    Gtk::TextView    m_message;
    Gtk::Label       m_count;
    Gtk::CheckButton m_attach;
    Gtk::Image       m_thumb;
    Gtk::FileChooserButton m_pick{Gtk::FILE_CHOOSER_ACTION_OPEN};
    Gtk::Label       m_status;
    Gtk::Spinner     m_spinner;
    Gtk::Button*     m_send = nullptr;
    std::string      m_screenshot;          // la capture proposee, s'il y en a une

    std::thread               m_worker;
    Glib::Dispatcher          m_done;
    HiscoreClient::ReportReply m_reply;
    sigc::connection          m_close_timer;
};

class MyReportsDialog : public Gtk::Dialog {
public:
    // `title_of` : le nom affiche d'un jeu ; `format` : un score mis en forme
    // selon ce que mesure le jeu (un chrono n'est pas un nombre).
    using TitleOf = std::function<std::string(const std::string& system, const std::string& game)>;
    using Format  = std::function<std::string(const std::string& system, const std::string& game, long long)>;
    MyReportsDialog(Gtk::Window& parent, TitleOf title_of, Format format);
    ~MyReportsDialog() override;

private:
    void on_loaded();
    void fill(const std::vector<HiscoreClient::PlayerReport>& reports);

    TitleOf     m_title_of;
    Format      m_format;
    Gtk::Box    m_box{Gtk::ORIENTATION_VERTICAL, 12};
    Gtk::ScrolledWindow m_scroll;
    Gtk::Box    m_list{Gtk::ORIENTATION_VERTICAL, 10};
    Gtk::Label  m_status;
    Gtk::Spinner m_spinner;

    std::thread       m_worker;
    Glib::Dispatcher  m_done;
    HiscoreClient::Fetched<std::vector<HiscoreClient::PlayerReport>> m_result;
};
