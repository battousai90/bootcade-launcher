// src/DownloadDialog.cpp
#include "DownloadDialog.h"
#include "i18n.h"
#include "SettingsUi.h"
#include "IconManager.h"
#include "AppContext.h"
#include "HostLibraries.h"
#include <curl/curl.h>
#include <zip.h>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

// Callback function for curl to write data
size_t write_data(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return fwrite(ptr, size, nmemb, stream);
}

// Callback function for curl progress
int progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    DownloadDialog* dialog = static_cast<DownloadDialog*>(clientp);
    if (dltotal > 0) {
        double progress = static_cast<double>(dlnow) / static_cast<double>(dltotal);
        dialog->update_progress(progress, "Downloading FBNeo...");
    }
    // Non-zero aborts the transfer : Cancel (and the dialog's destructor,
    // which joins this thread) used to wait for the whole download.
    return dialog->cancel_requested() ? 1 : 0;
}

std::string DownloadDialog::choose_fbneo_folder(Gtk::Window& parent, const std::string& current_exe,
                                                bool ask) {
    const char* home_env = std::getenv("HOME");
    const fs::path home = home_env ? fs::path(home_env) : fs::current_path();

    std::error_code ec;
    fs::path folder;
    if (!current_exe.empty() && fs::exists(current_exe, ec))
        folder = fs::path(current_exe).parent_path();
    // Les versions precedentes l'installaient a la racine du dossier
    // personnel : on n'y reinstalle pas, on propose un vrai dossier.
    if (folder.empty() || fs::equivalent(folder, home, ec))
        folder = home / "Bootcade" / "FBNeo";
    else if (!ask)
        return folder.string();

    // Le selecteur ne s'ouvre que sur un dossier qui existe. Cree pour
    // l'occasion, il disparait si le joueur annule.
    fs::path created;
    if (!fs::exists(folder, ec)) {
        for (fs::path p = folder; !p.empty() && !fs::exists(p, ec); p = p.parent_path()) created = p;
        fs::create_directories(folder, ec);
    }

    Gtk::FileChooserDialog dialog(parent, _("Install FinalBurn Neo in"),
                                  Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
    dialog.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dialog.add_button(_("Install here"), Gtk::RESPONSE_OK);
    dialog.set_current_folder(folder.string());
    const bool ok = dialog.run() == Gtk::RESPONSE_OK;
    std::string chosen = ok ? dialog.get_filename() : std::string();
    if (ok && chosen.empty()) chosen = dialog.get_current_folder();

    if (!created.empty() && (chosen.empty() || fs::path(chosen) != folder)) {
        // Seulement ce qui est reste vide : remove, pas remove_all.
        for (fs::path p = folder; ; p = p.parent_path()) {
            fs::remove(p, ec);
            if (p == created || p.empty()) break;
        }
    }
    return chosen;
}

DownloadDialog::DownloadDialog(Gtk::Window& parent, const std::string& url, const std::string& destination)
    : Gtk::Dialog()
    , m_url(url)
    , m_destination(destination)
    , m_settings_entry(nullptr) {
    namespace ui = SettingsUi;

    set_transient_for(parent);
    set_modal(true);
    set_resizable(false);
    set_default_size(520, -1);
    set_position(Gtk::WIN_POS_CENTER_ON_PARENT);

    // L'en-tete de la charte ; l'etat du telechargement tient dans son
    // sous-titre, la ou les autres fenetres mettent le leur.
    ui::Header head = ui::window_header(*this, "bc-download.svg", _("Download FBNeo"),
                                        _("Preparing download..."),
                                        [this] { on_cancel_clicked(); });
    m_step_label = head.subtitle;

    m_content_box.set_spacing(14);
    m_content_box.set_margin_start(22);
    m_content_box.set_margin_end(22);
    m_content_box.set_margin_top(20);
    m_content_box.set_margin_bottom(20);

    m_progress_bar.set_show_text(false);
    m_content_box.pack_start(m_progress_bar, Gtk::PACK_SHRINK);

    m_progress_label.set_text("0%");
    m_progress_label.set_halign(Gtk::ALIGN_END);
    m_progress_label.get_style_context()->add_class("set-sub");
    m_content_box.pack_start(m_progress_label, Gtk::PACK_SHRINK);

    auto* root = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
    root->pack_start(m_content_box, Gtk::PACK_EXPAND_WIDGET);

    auto* foot = ui::footer();
    m_cancel_button = ui::button(_("Cancel"));
    m_cancel_button->set_size_request(96, -1);
    foot->pack_end(*m_cancel_button, Gtk::PACK_SHRINK);
    root->pack_start(*foot, Gtk::PACK_SHRINK);

    get_content_area()->set_spacing(0);
    get_content_area()->pack_start(*root, Gtk::PACK_EXPAND_WIDGET);
    m_cancel_button->signal_clicked().connect(sigc::mem_fun(*this, &DownloadDialog::on_cancel_clicked));

    // Setup dispatchers for thread communication
    m_progress_dispatcher.connect([this]() {
        m_progress_bar.set_fraction(m_shared_data.progress.load());
        int percentage = static_cast<int>(m_shared_data.progress.load() * 100);
        m_progress_label.set_text(std::to_string(percentage) + "%");
        std::string status;
        { std::lock_guard<std::mutex> lk(m_shared_data.text_mutex); status = m_shared_data.status_text; }
        m_step_label->set_text(status);
    });
    
    m_complete_dispatcher.connect([this]() {
        namespace ui = SettingsUi;
        if (m_shared_data.success.load()) {
            // Built from the actual extraction destination, not a hardcoded
            // "./fbneo" : a relative literal would land in config.json and
            // break as soon as Bootcade starts from another directory.
            std::string fbneo_path = (std::filesystem::path(m_destination) / "fbneo").string();

            // On vient de le telecharger POUR s'en servir : il devient
            // l'emulateur, sans question. La demander ne servait qu'a
            // laisser un premier lancement sans emulateur sur un « OK ».
            if (m_settings_entry) m_settings_entry->set_text(fbneo_path);
            // Read-modify-write on the real config path : the other keys stay.
            const std::string config_path = AppContext::get_config_path();
            nlohmann::json j;
            {
                std::ifstream in(config_path);
                if (in) { try { in >> j; } catch (...) { j = nlohmann::json{}; } }
            }
            j["fbneo_executable"] = fbneo_path;
            std::ofstream config(config_path);
            if (config.is_open()) config << j.dump(4);

            // Le dire tout de suite : sinon le joueur ne l'apprend qu'au
            // premier jeu, par une fenetre qui ne s'ouvre pas.
            const auto libs = HostLibraries::missing(fbneo_path);
            if (!libs.empty())
                ui::notice(*this, _("FinalBurn Neo cannot start yet"),
                           HostLibraries::explain(libs), "bc-error.svg");

            response(Gtk::RESPONSE_OK);
        } else {
            std::string final_message;
            { std::lock_guard<std::mutex> lk(m_shared_data.text_mutex); final_message = m_shared_data.final_message; }
            ui::notice(*this, _("Download failed"), final_message, "bc-error.svg");
            response(Gtk::RESPONSE_CANCEL);
        }
    });
    
    show_all();
}

DownloadDialog::~DownloadDialog() {
    m_cancel_requested = true;
    if (m_download_thread.joinable()) {
        m_download_thread.join();
    }
}

void DownloadDialog::set_settings_entry(Gtk::Entry* entry) {
    m_settings_entry = entry;
}

void DownloadDialog::start_download() {
    m_download_thread = std::thread(&DownloadDialog::download_worker, this);
}

void DownloadDialog::on_cancel_clicked() {
    m_cancel_requested = true;
    response(Gtk::RESPONSE_CANCEL);
}

void DownloadDialog::update_progress(double progress, const std::string& status) {
    m_shared_data.progress.store(progress);
    { std::lock_guard<std::mutex> lk(m_shared_data.text_mutex); m_shared_data.status_text = status; }
    m_progress_dispatcher.emit();
}

void DownloadDialog::download_worker() {
    CURL *curl;
    FILE *fp;
    CURLcode res;
    
    // Create temp file path
    m_temp_file = m_destination + "/fbneo_download.zip";
    
    // Ensure destination directory exists
    try {
        fs::create_directories(m_destination);
    } catch (const std::exception& e) {
        m_shared_data.success.store(false);
        { std::lock_guard<std::mutex> lk(m_shared_data.text_mutex); m_shared_data.final_message = _("Failed to create destination directory: ") + std::string(e.what()); }
        m_complete_dispatcher.emit();
        return;
    }
    
    curl = curl_easy_init();
    if (!curl) {
        m_shared_data.success.store(false);
        { std::lock_guard<std::mutex> lk(m_shared_data.text_mutex); m_shared_data.final_message = _("Failed to initialize curl"); }
        m_complete_dispatcher.emit();
        return;
    }
    
    fp = fopen(m_temp_file.c_str(), "wb");
    if (!fp) {
        curl_easy_cleanup(curl);
        m_shared_data.success.store(false);
        { std::lock_guard<std::mutex> lk(m_shared_data.text_mutex); m_shared_data.final_message = _("Failed to create temporary file"); }
        m_complete_dispatcher.emit();
        return;
    }
    
    // Configure curl
    curl_easy_setopt(curl, CURLOPT_URL, m_url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "bootcade/1.0");
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L); // 5 minute timeout
    
    // Perform download
    res = curl_easy_perform(curl);
    fclose(fp);
    
    if (m_cancel_requested) {
        curl_easy_cleanup(curl);
        fs::remove(m_temp_file);
        return;
    }
    
    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        fs::remove(m_temp_file);
        m_shared_data.success.store(false);
        { std::lock_guard<std::mutex> lk(m_shared_data.text_mutex); m_shared_data.final_message = _("Download failed: ") + std::string(curl_easy_strerror(res)); }
        m_complete_dispatcher.emit();
        return;
    }
    
    curl_easy_cleanup(curl);
    
    // Update status for extraction
    update_progress(1.0, "Extracting archive...");
    
    // Extract ZIP file
    if (!extract_zip(m_temp_file, m_destination)) {
        fs::remove(m_temp_file);
        m_shared_data.success.store(false);
        { std::lock_guard<std::mutex> lk(m_shared_data.text_mutex); m_shared_data.final_message = _("Failed to extract ZIP archive"); }
        m_complete_dispatcher.emit();
        return;
    }
    
    // Clean up temp file
    fs::remove(m_temp_file);
    
    // Success
    m_shared_data.success.store(true);
    { std::lock_guard<std::mutex> lk(m_shared_data.text_mutex);
      m_shared_data.extracted_path = m_destination;
      m_shared_data.final_message = _("FBNeo downloaded and extracted successfully to: ") + m_shared_data.extracted_path; }
    m_complete_dispatcher.emit();
}

bool DownloadDialog::extract_zip(const std::string& zip_path, const std::string& extract_path) {
    int err = 0;
    zip_t *z = zip_open(zip_path.c_str(), 0, &err);
    
    if (z == nullptr) {
        return false;
    }
    
    zip_int64_t num_entries = zip_get_num_entries(z, 0);
    if (num_entries < 0) {
        zip_close(z);
        return false;
    }
    
    for (zip_int64_t i = 0; i < num_entries; i++) {
        if (m_cancel_requested) {
            zip_close(z);
            return false;
        }
        
        const char* name = zip_get_name(z, i, 0);
        if (name == nullptr) {
            continue;
        }
        
        // Create full path
        std::string full_path = extract_path + "/" + name;
        
        // Create directories if needed
        if (name[strlen(name) - 1] == '/') {
            try {
                fs::create_directories(full_path);
            } catch (...) {
                // Ignore directory creation errors
            }
            continue;
        }
        
        // Create parent directories
        try {
            fs::create_directories(fs::path(full_path).parent_path());
        } catch (...) {
            // Ignore directory creation errors
        }
        
        // Extract file
        zip_file_t *f = zip_fopen_index(z, i, 0);
        if (f == nullptr) {
            continue;
        }
        
        std::ofstream outfile(full_path, std::ios::binary);
        if (!outfile) {
            zip_fclose(f);
            continue;
        }
        
        char buffer[8192];
        zip_int64_t bytes_read;
        while ((bytes_read = zip_fread(f, buffer, sizeof(buffer))) > 0) {
            outfile.write(buffer, bytes_read);
        }
        
        outfile.close();
        zip_fclose(f);
        
        // Make executable if it's the fbneo binary
        if (name == std::string("fbneo") || 
            (full_path.find("fbneo") != std::string::npos && 
             full_path.find(".exe") == std::string::npos)) {
            try {
                fs::permissions(full_path, fs::perms::owner_all | fs::perms::group_read | fs::perms::others_read);
            } catch (...) {
                // Ignore permission errors
            }
        }
    }
    
    zip_close(z);
    return true;
}