// src/SettingsPanel.cpp
//
// L'ecran des reglages : une coquille (barre de titre, quatre onglets, pied
// d'actions) et quatre pages baties avec les memes briques (SettingsUi).
//
// La MISE EN PAGE a ete entierement refaite d'apres les maquettes ; les
// COMPORTEMENTS, eux, sont ceux d'avant, widget pour widget et signal pour
// signal. C'est volontaire : une refonte visuelle qui reecrit au passage la
// lecture de la configuration, le balayage des ROMs ou la connexion au compte
// ne se debogue plus, parce qu'on ne sait plus ce qui a change.
#include "SettingsPanel.h"
#include "BootcadeAuth.h"
#include "IconManager.h"
#include "LoginDialog.h"
#include "SettingsUi.h"
#include "Countries.h"
#include <cctype>
#include "AppContext.h"
#include "ConfirmationDialog.h"
#include "DatabaseManager.h"
#include "DownloadDialog.h"
#include "GenerateDAT.h"
#include "FbneoUpdateCheck.h"
#include "HiscoreClient.h"
#include "MameCatalog.h"
#include "ThumbnailDownloader.h"
#include "i18n.h"
#include <map>
#include <gtkmm/filechooserdialog.h>
#include <gtkmm/messagedialog.h>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <random>
#include <thread>
#include <unistd.h>

namespace {

namespace ui = SettingsUi;

// "player" followed by ten digits. Two things are wanted of it at once: that
// two fresh installs practically never collide, and that it reads as an
// obvious placeholder, so nobody mistakes it on the leaderboard for a name its
// owner actually chose.
std::string generated_player_name() {
    static std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<int> digit(0, 9);
    std::string name = "player";
    for (int i = 0; i < 10; ++i) name += char('0' + digit(rng));
    return name;
}

// The country the machine is already set to. Deliberately read from the
// locale and nowhere else: a geolocation lookup would hand someone's IP
// address to a third party purely to draw a flag next to their score, which is
// a far bigger imposition than the flag is worth. An unrecognised or absent
// locale simply yields no country, which the rest of the panel already treats
// as a valid state.
std::string locale_country_code() {
    for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* value = std::getenv(var);
        if (!value) continue;
        std::string s = value;                       // e.g. "fr_FR.UTF-8"
        auto underscore = s.find('_');
        if (underscore == std::string::npos) continue;
        std::string code = s.substr(underscore + 1, 2);
        if (code.size() != 2) continue;
        for (auto& c : code) c = std::toupper((unsigned char)c);
        for (const auto& entry : kCountries)
            if (code == entry.code) return code;
    }
    return {};
}

/* ── Le registre des emulateurs ────────────────────────────────────────
 *
 * Bootcade lance FinalBurn Neo, et lui seul. La page n'est pourtant pas batie
 * AUTOUR de FBNeo : la liste de gauche et le panneau de droite se construisent
 * a partir de cette description, si bien qu'ajouter un emulateur plus tard
 * revient a ajouter une entree ici plus la poignee de gestes qui lui sont
 * propres, et non a redessiner l'ecran.
 *
 * Il n'y a donc qu'une entree. La maquette en montre six ; les cinq autres ne
 * sont pas supportees, et les afficher grisees ferait passer une absence de
 * fonctionnalite pour une panne.
 */
struct EmulatorEntry {
    const char* id;
    const char* logo;          // assets/icons/…
    const char* name;
    const char* kind;          // « Arcade emulator »
    const char* description;
};

// Une marque d'emulateur, chargee telle quelle. ui::tile() pose le
// pictogramme dans un carre arrondi et le repeint avec l'encre du contexte :
// parfait pour une icone monochrome, desastreux pour un logo, qui y perd
// justement ce qui le rend reconnaissable.
Gtk::Widget* brand_logo(const std::string& rel, int w, int h) {
    auto* img = Gtk::make_managed<Gtk::Image>();
    try {
        img->set(Gdk::Pixbuf::create_from_file(
            AppContext::get_asset_path("icons/" + rel), w, h, true));
    } catch (const Glib::Error&) {
        // Fichier absent : on laisse la place vide plutot qu'un pictogramme
        // d'erreur, le nom de l'emulateur est juste a cote.
    }
    img->set_size_request(w, h);
    img->set_valign(Gtk::ALIGN_CENTER);
    return img;
}

const std::vector<EmulatorEntry>& emulator_registry() {
    static const std::vector<EmulatorEntry> kEntries = {
        {"fbneo", "emulators/fbneo.svg", "FinalBurn Neo", N_("Arcade emulator"),
         N_("Play arcade games from multiple systems with FinalBurn Neo.")},
        {"mame", "emulators/mame.svg", "MAME", N_("Arcade emulator"),
         N_("Read the catalog straight from the MAME installed on this system.")},
    };
    return kEntries;
}

/* L'emulateur dont les reglages sont ceux que le reste de l'application lit
 * quand elle ne precise rien. FinalBurn Neo, parce que c'est le seul que
 * Bootcade ait jamais eu : tout ce qui existait avant les entrees par
 * emulateur decrivait le sien. */
constexpr const char* kDefaultEmulator = "fbneo";

std::string emulator_name(const std::string& id) {
    for (const auto& e : emulator_registry())
        if (id == e.id) return e.name;
    return id;
}

// La liste de dossiers telle que MAME l'attend en ligne de commande.
std::string join_paths(const std::vector<std::string>& paths) {
    std::string out;
    for (const auto& p : paths) {
        if (p.empty()) continue;
        if (!out.empty()) out += ';';
        out += p;
    }
    return out;
}

std::vector<std::string> split_paths(const std::string& joined) {
    std::vector<std::string> out;
    std::string one;
    for (char c : joined) {
        if (c == ';') { if (!one.empty()) out.push_back(one); one.clear(); }
        else          { one += c; }
    }
    if (!one.empty()) out.push_back(one);
    return out;
}

// La date d'un fichier, en AAAA-MM-JJ, ou vide s'il n'existe pas.
std::string file_date(const std::string& path) {
    struct stat st;
    if (path.empty() || ::stat(path.c_str(), &st) != 0) return {};
    char buf[16];
    std::tm tm{};
    localtime_r(&st.st_mtime, &tm);
    if (!std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm)) return {};
    return buf;
}

std::string today_iso() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char buf[16];
    if (!std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm)) return {};
    return buf;
}

// Une tuile de statistique du panneau emulateur : pictogramme, intitule,
// valeur. Trois cotes a cote forment la bande de la maquette.
Gtk::Widget* stat_tile(const std::string& icon_file, const std::string& label,
                       Gtk::Label& value) {
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 11);
    box->get_style_context()->add_class("cc-subcard");
    box->pack_start(*ui::tile(icon_file, ui::kIconRow, ui::kTileRow),
                    Gtk::PACK_SHRINK);
    auto* txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 1);
    txt->set_valign(Gtk::ALIGN_CENTER);
    txt->pack_start(*ui::sub_label(label), Gtk::PACK_SHRINK);
    value.set_xalign(0.0f);
    value.get_style_context()->add_class("set-row-title");
    txt->pack_start(value, Gtk::PACK_SHRINK);
    box->pack_start(*txt, Gtk::PACK_EXPAND_WIDGET);
    return box;
}

// Un champ de chemin suivi de ses boutons : la ligne que Library repete trois
// fois (previsualisations, titres, DAT).
Gtk::Widget* path_row(const std::string& title, const std::string& subtitle,
                      Gtk::Entry& entry, Gtk::Button& browse,
                      Gtk::Button& action) {
    auto* line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 12);
    line->get_style_context()->add_class("set-row");

    auto* txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 1);
    txt->set_valign(Gtk::ALIGN_CENTER);
    txt->set_size_request(240, -1);
    txt->pack_start(*ui::title_label(title), Gtk::PACK_SHRINK);
    txt->pack_start(*ui::sub_label(subtitle), Gtk::PACK_SHRINK);
    line->pack_start(*txt, Gtk::PACK_SHRINK);

    entry.set_hexpand(true);
    entry.set_valign(Gtk::ALIGN_CENTER);
    line->pack_start(entry, Gtk::PACK_EXPAND_WIDGET);
    browse.set_valign(Gtk::ALIGN_CENTER);
    action.set_valign(Gtk::ALIGN_CENTER);
    line->pack_start(browse, Gtk::PACK_SHRINK);
    line->pack_start(action, Gtk::PACK_SHRINK);
    return line;
}

/* ── Une ligne d'option : interrupteur seul, ou interrupteur ET valeur ──
 *
 * MAME expose des centaines d'options, et plusieurs d'entre elles ne sont pas
 * des oui/non : le pilote video, le pilote son, le volume, le nommage des
 * captures ont une valeur a choisir. Une option de ce genre demande donc deux
 * gestes — l'allumer, puis dire laquelle — et c'est l'alignement qui rend les
 * deux lisibles cote a cote.
 *
 * L'emplacement du selecteur est TOUJOURS reserve, meme quand la ligne n'en a
 * pas : sans cela, l'interrupteur d'une option simple remonterait la ou la
 * ligne voisine met sa liste deroulante, et la colonne d'interrupteurs
 * partirait en dents de scie. Reserver la place coute quelques pixels de vide
 * et rend la carte lisible d'un coup d'oeil.
 */
constexpr int kValueSlot = 220;

Gtk::Widget* option_row(const std::string& icon_file, const std::string& title,
                        const std::string& subtitle, Gtk::Switch& sw,
                        Gtk::Widget* value = nullptr) {
    auto* trailing = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 12);
    auto* slot = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 6);
    slot->set_size_request(kValueSlot, -1);
    slot->set_valign(Gtk::ALIGN_CENTER);
    if (value) {
        value->set_valign(Gtk::ALIGN_CENTER);
        slot->pack_start(*value, Gtk::PACK_EXPAND_WIDGET);
    }
    trailing->pack_start(*slot, Gtk::PACK_SHRINK);
    sw.set_valign(Gtk::ALIGN_CENTER);
    trailing->pack_start(sw, Gtk::PACK_SHRINK);
    return ui::row(icon_file, title, subtitle, trailing);
}

// Les valeurs multiples d'une option se lisent dans une liste deroulante, et
// nulle part ailleurs : un champ libre laisserait ecrire « openg1 », que MAME
// refuse a la seconde ou le jeu devrait demarrer.
void fill_combo(Gtk::ComboBoxText& combo,
                const std::vector<std::pair<const char*, std::string>>& items) {
    for (const auto& item : items) combo.append(item.first, item.second);
    combo.set_size_request(kValueSlot, -1);
    combo.set_active(0);
}

// « -plugin hiscore,autofire » : MAME attend UNE valeur, pas un drapeau par
// extension.
std::string join_commas(const std::vector<std::string>& parts) {
    std::string out;
    for (const auto& part : parts) {
        if (!out.empty()) out += ',';
        out += part;
    }
    return out;
}

}  // namespace

SettingsPanel::~SettingsPanel() {
    // Avant tout demontage : un fil de sonde reseau qui testerait le drapeau
    // puis emettrait sur un objet detruit attend ici, voit le drapeau tombe,
    // et renonce.
    std::lock_guard<std::mutex> lock(m_alive->mutex);
    m_alive->alive = false;
}

SettingsPanel::SettingsPanel() : Box(Gtk::ORIENTATION_VERTICAL, 0) {
    build_shell();
}

// ─────────────────────────────────────────────────────────────────────────
//  Coquille : barre de titre, onglets, pages, pied
// ─────────────────────────────────────────────────────────────────────────

void SettingsPanel::build_shell() {
    get_style_context()->add_class("cc-window");
    get_style_context()->add_class("set-window");

    // ── Barre de titre ───────────────────────────────────────────────────
    // Bootcade decore ses fenetres lui-meme, comme Controller Configuration :
    // une HeaderBar posee par la fenetre, la marque a gauche, et NOTRE croix a
    // droite plutot que la pastille du bureau, qui ne ressemble a rien d'autre
    // dans l'application.
    m_header.pack_start(*ui::tile("gear.svg", 21, 36, /*accent=*/true),
                        Gtk::PACK_SHRINK);
    m_header_title.set_text(_("Settings"));
    m_header_title.set_xalign(0.0f);
    m_header_title.get_style_context()->add_class("set-head-title");
    m_header_sub.set_text(_("Customize Bootcade to match your setup and preferences"));
    m_header_sub.set_xalign(0.0f);
    m_header_sub.get_style_context()->add_class("set-head-sub");
    auto* head_txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
    head_txt->set_valign(Gtk::ALIGN_CENTER);
    head_txt->pack_start(m_header_title, Gtk::PACK_SHRINK);
    head_txt->pack_start(m_header_sub,   Gtk::PACK_SHRINK);
    m_header.pack_start(*head_txt, Gtk::PACK_SHRINK);

    m_btn_close.set_image(*ui::image("bc-close.svg", 18));
    m_btn_close.set_tooltip_text(_("Close"));
    m_btn_close.get_style_context()->add_class("set-close");
    m_btn_close.set_valign(Gtk::ALIGN_CENTER);
    m_btn_close.signal_clicked().connect([this] { m_sig_close.emit(); });

    /* La barre de titre ne porte AUCUNE classe de fenetre.
     *
     * Elle en portait, pour que « .set-window .set-brand » atteigne la tuile
     * violette. Mais « .set-window » peint aussi un fond : le bandeau prenait
     * la couleur des pages et disparaissait, alors que la fenetre principale
     * et Controller Configuration laissent tous deux la barre au gris du
     * theme. Les elements de l'entete se stylent donc par leur propre classe,
     * sans exiger un ancetre, et le bandeau redevient gris comme partout
     * ailleurs. */
    m_headerbar.set_show_close_button(false);
    m_headerbar.pack_start(m_header);
    m_headerbar.pack_end(m_btn_close);
    // Titre personnalise vide : sans lui GTK dessine SON titre au centre en
    // plus de la marque, et l'ecran porte alors deux titres.
    m_headerbar.set_custom_title(*Gtk::make_managed<Gtk::Box>());
    m_headerbar.show_all();

    // ── Onglets ──────────────────────────────────────────────────────────
    m_tabbar.get_style_context()->add_class("set-tabbar");
    /* Aucune transition.
     *
     * Un fondu interpole la hauteur du Stack pendant toute sa duree : la
     * fenetre etait donc recalee sur une page a moitie affichee, et gardait
     * cette hauteur-la. Sur un ecran de reglages, l'effet ne valait pas ce
     * qu'il coutait.
     */
    m_pages.set_transition_type(Gtk::STACK_TRANSITION_TYPE_NONE);
    m_pages.set_transition_duration(0);
    /* Chaque page a SA hauteur.
     *
     * Un Gtk::Stack est homogene par defaut : les quatre pages prenaient donc
     * la hauteur de la plus haute, et les trois autres se terminaient par une
     * bande vide qui ne disait rien. La fenetre suit maintenant la page
     * affichee (voir fit_to_page).
     */
    m_pages.set_vhomogeneous(false);

    /* Les pages sont posees telles quelles, sans zone defilante.
     *
     * Un ecran de reglages qui defile cache la moitie de ses options derriere
     * un geste : on ne voit plus ce qui existe, et le pied d'actions flotte
     * au-dessus d'un contenu tronque. La fenetre prend donc la hauteur de sa
     * page la plus haute : Gtk::Stack est homogene par defaut, donc les
     * quatre pages partagent la meme taille et la fenetre ne saute plus d'un
     * onglet a l'autre. Seule la LISTE des dossiers de ROMs defile, parce
     * qu'elle est une liste : son contenu n'a pas de hauteur previsible. */
    m_pages.add(*build_page_general(),  "general");
    m_pages.add(*build_page_library(),  "library");
    m_pages.add(*build_page_emulator(), "emulator");
    m_pages.add(*build_page_online(),   "online");
    m_pages.add(*build_page_random(),   "random");

    add_tab("general",  "gear.svg",          _("General"));
    add_tab("library",  "bc-folder.svg",     _("Library"));
    add_tab("emulator", "bc-controller.svg", _("Emulator"));
    add_tab("online",   "bc-globe.svg",      _("Online"));
    add_tab("random",   "bc-dice.svg",       _("Random play"));

    pack_start(m_tabbar, Gtk::PACK_SHRINK);
    pack_start(m_pages,  Gtk::PACK_EXPAND_WIDGET);

    // ── Pied ─────────────────────────────────────────────────────────────
    // Ce qui defait a gauche, ce qui valide a droite : c'est la disposition de
    // la maquette, et c'est aussi ce qui evite de cliquer « tout remettre a
    // zero » en visant « enregistrer ».
    m_btn_restore_defaults.set_label(_("Restore Default Settings"));
    m_btn_restore_defaults.set_image(*ui::image("bc-restore.svg", ui::kIconButton));
    m_btn_restore_defaults.set_always_show_image(true);
    m_btn_restore_defaults.signal_clicked().connect(
        sigc::mem_fun(*this, &SettingsPanel::on_restore_defaults_clicked));

    m_btn_cancel.set_label(_("Cancel"));
    m_btn_cancel.signal_clicked().connect([this] { m_sig_close.emit(); });

    m_btn_save.set_label(_("Save"));
    m_btn_save.set_image(*ui::image("bc-save.svg", ui::kIconButton));
    m_btn_save.set_always_show_image(true);
    m_btn_save.get_style_context()->add_class("accent-button");
    m_btn_save.signal_clicked().connect([this] { m_sig_save.emit(); });

    m_footer.get_style_context()->add_class("cc-footer");
    m_footer.pack_start(m_btn_restore_defaults, Gtk::PACK_SHRINK);
    m_footer.pack_end(m_btn_save,   Gtk::PACK_SHRINK);
    m_footer.pack_end(m_btn_cancel, Gtk::PACK_SHRINK);
    pack_start(m_footer, Gtk::PACK_SHRINK);

    m_net_done.connect([this] {
        set_network_state(m_net_state.load());
        // Reactive le bouton : sans cela il restait grise apres le premier
        // appui, et « Test Connection » n'etait cliquable qu'une seule fois
        // dans la vie de la fenetre : de l'exterieur, un bouton mort.
        m_btn_test_net.set_sensitive(true);
    });
    m_update_done.connect([this] {
        std::string tag;
        bool failed;
        {
            std::lock_guard<std::mutex> lock(m_update_mutex);
            tag = m_update_tag;
            failed = m_update_failed;
        }
        m_btn_check_updates.set_sensitive(true);
        if (failed)
            set_update_state(_("Could not check for updates."), "warn");
        else if (tag.empty())
            set_update_state(_("You are up to date"), "ok");
        else
            set_update_state(Glib::ustring::compose(_("Bootcade %1 is available"), tag),
                             "warn");
    });
    m_emu_update_done.connect([this] {
        std::string msg, tone;
        {
            std::lock_guard<std::mutex> lock(m_emu_mutex);
            msg = m_emu_update_msg;
            tone = m_emu_update_tone;
        }
        m_btn_emu_updates.set_sensitive(true);
        m_lbl_emu_note.set_text(msg);
        auto ctx = m_lbl_emu_note.get_style_context();
        for (const char* c : {"set-ok", "set-warn", "set-err", "set-sub"})
            ctx->remove_class(c);
        ctx->add_class(tone == "ok" ? "set-ok" : tone == "warn" ? "set-warn" : "set-sub");
        m_lbl_emu_note.show();
        // La verification vient d'ecrire sa date : la tuile la relit, sinon
        // elle continue d'afficher « Never » juste apres une verification.
        if (m_emu_shown_mame) refresh_mame_state();
    });

    // Rien ne doit paraitre mis en avant par hasard : sans defaut declare, le
    // premier bouton de la page portait l'anneau de focus et se lisait comme
    // l'action recommandee.
    m_btn_save.set_can_default(true);
    /* Ce qui se montre selon l'etat ne doit PAS obeir a show_all().
     *
     * La fenetre qui accueille le panneau appelle show_all() apres coup, ce
     * qui remontrait d'un bloc le profil connecte et l'invitation a se
     * connecter en meme temps. Ces widgets-la decident eux-memes, et
     * refresh_account_row est seul juge. */
    /* L'ordre compte : on montre TOUT d'abord, ce qui donne a chaque enfant
     * son etat « visible », puis on pose le drapeau. Les blocs peuvent des
     * lors s'allumer et s'eteindre d'un seul show()/hide() sans que le
     * show_all() de la fenetre vienne tout rallumer par-dessus. Poser le
     * drapeau AVANT laissait leurs enfants jamais montres, et le bloc
     * s'affichait vide. */
    show_all_children();
    for (Gtk::Widget* w : {static_cast<Gtk::Widget*>(&m_profile_signed),
                           static_cast<Gtk::Widget*>(&m_profile_empty),
                           static_cast<Gtk::Widget*>(&m_account_avatar),
                           static_cast<Gtk::Widget*>(&m_hiscore_hint),
                           static_cast<Gtk::Widget*>(&m_lbl_emu_note)})
        w->set_no_show_all(true);
    m_lbl_emu_note.hide();   // rien a dire tant qu'on n'a pas verifie
    refresh_account_row();
    refresh_emulator_state();
}

/* Quatre pastilles a pictogramme, et une seule enfoncee.
 *
 * Un Gtk::Notebook aurait suffi a empiler les pages, mais ses onglets
 * n'acceptent pas proprement la geometrie de la maquette (48 px, pictogramme,
 * coins hauts arrondis). Des ToggleButton pilotant un Gtk::Stack donnent
 * exactement le dessin voulu, au prix d'une exclusion mutuelle a tenir : d'ou
 * le drapeau, sans lequel decocher l'onglet courant depuis le gestionnaire
 * rappellerait le gestionnaire.
 */
void SettingsPanel::add_tab(const std::string& id, const std::string& icon_file,
                            const std::string& label) {
    auto* btn = Gtk::make_managed<Gtk::ToggleButton>();
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 9);
    box->set_halign(Gtk::ALIGN_CENTER);
    box->pack_start(*ui::image(icon_file, 17), Gtk::PACK_SHRINK);
    box->pack_start(*Gtk::make_managed<Gtk::Label>(label), Gtk::PACK_SHRINK);
    btn->add(*box);
    btn->get_style_context()->add_class("set-tab");
    btn->set_active(m_tabs_buttons.empty());
    /* On ecoute « clicked », pas « toggled ».
     *
     * Avec « toggled » il fallait rattraper deux cas a la main : l'onglet
     * courant qu'un clic decochait, et les trois autres qu'il fallait
     * eteindre : chaque extinction rappelant le gestionnaire. Un clic dit
     * simplement « c'est celui-la », et set_active n'emet que « toggled »,
     * jamais « clicked » : il n'y a donc plus de reentrance a garder.
     */
    btn->signal_clicked().connect([this, btn, id] {
        if (m_tab_switching) return;
        m_tab_switching = true;
        for (auto* other : m_tabs_buttons) other->set_active(other == btn);
        m_pages.set_visible_child(id);
        m_tab_switching = false;
        fit_to_page();
    });
    m_tabs_buttons.push_back(btn);
    m_tabbar.pack_start(*btn, Gtk::PACK_SHRINK);
}

// ─────────────────────────────────────────────────────────────────────────
//  General
// ─────────────────────────────────────────────────────────────────────────

Gtk::Widget* SettingsPanel::build_page_general() {
    auto* page = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL,
                                             ui::kCardSpacing);
    page->get_style_context()->add_class("set-page");

    // ── Appearance & Startup ─────────────────────────────────────────────
    auto appearance = ui::card("bc-brush.svg", _("Appearance & Startup"),
                               _("Customize the look and behavior of Bootcade"));
    auto* app_rows = ui::rows();

    // Friendly names for known language codes; unknown codes show the raw code.
    // Each language is named in itself : someone looking for their own language
    // recognises "ไทย", not "th". Add an entry here whenever a locale/<code>.json
    // is added, otherwise the picker falls back to showing the bare code.
    static const std::map<std::string, std::string> lang_names = {
        {"en","English"}, {"fr","Français"}, {"es","Español"}, {"de","Deutsch"},
        {"pt","Português"}, {"zh","中文"}, {"ja","日本語"}, {"th","ไทย"}};
    m_combo_language.append("", _("System"));
    for (const auto& code : i18n::available_languages()) {
        auto it = lang_names.find(code);
        m_combo_language.append(code, it != lang_names.end() ? it->second : code);
    }
    m_combo_language.set_active_id("");
    m_combo_language.set_size_request(ui::kFieldWidth, -1);
    ui::add_row(app_rows, *ui::row("bc-globe.svg", _("Language"),
                                   _("Select the application language."),
                                   &m_combo_language));

    m_combo_theme.append("system", _("System"));
    m_combo_theme.append("dark",   _("Dark"));
    m_combo_theme.append("light",  _("Light"));
    m_combo_theme.set_active_id("dark");
    m_combo_theme.set_size_request(ui::kFieldWidth, -1);
    ui::add_row(app_rows, *ui::row("bc-palette.svg", _("Theme"),
                                   _("Choose the application theme."),
                                   &m_combo_theme));

    m_combo_startup.append("last_played",   _("Last played game"));
    m_combo_startup.append("most_played",   _("Most played game"));
    m_combo_startup.append("best_score",    _("Best personal highscore"));
    m_combo_startup.append("last_selected", _("Last selected game"));
    m_combo_startup.append("first",         _("First available game"));
    m_combo_startup.set_active_id("last_played");
    m_combo_startup.set_size_request(ui::kFieldWidth, -1);
    m_combo_startup.set_tooltip_text(
        _("If the chosen game cannot be found, the first available one is shown."));
    ui::add_row(app_rows, *ui::row("bc-power.svg", _("Game selected at startup"),
                                   _("Choose which game Bootcade selects when it starts."),
                                   &m_combo_startup));
    appearance.body->pack_start(*app_rows, Gtk::PACK_SHRINK);
    page->pack_start(*appearance.frame, Gtk::PACK_SHRINK);

    m_combo_theme.signal_changed().connect([this] {
        if (!m_suppress_appearance_signals) m_sig_theme_changed.emit(m_combo_theme.get_active_id());
    });
    m_combo_language.signal_changed().connect([this] {
        if (!m_suppress_appearance_signals) m_sig_language_changed.emit(m_combo_language.get_active_id());
    });

    // ── Behavior ─────────────────────────────────────────────────────────
    // Deux interrupteurs, et deux seulement : ce sont les deux seuls gestes de
    // cette section qui commandent quelque chose que le lanceur fait deja.
    auto behavior = ui::card("gear.svg", _("Behavior"),
                             _("Configure optional application behavior."));
    auto* beh_rows = ui::rows();
    m_switch_window_state.set_active(true);
    m_switch_window_state.set_valign(Gtk::ALIGN_CENTER);
    ui::add_row(beh_rows, *ui::row("bc-window.svg", _("Restore last window state"),
                                   _("Remember window size and position."),
                                   &m_switch_window_state));
    m_switch_play_history.set_active(true);
    m_switch_play_history.set_valign(Gtk::ALIGN_CENTER);
    m_switch_play_history.set_tooltip_text(
        _("Off, Bootcade stops recording how long you play. Already recorded history is kept."));
    ui::add_row(beh_rows, *ui::row("bc-clock.svg", _("Keep play history"),
                                   _("Track recently played games."),
                                   &m_switch_play_history));
    behavior.body->pack_start(*beh_rows, Gtk::PACK_SHRINK);
    page->pack_start(*behavior.frame, Gtk::PACK_SHRINK);

    // ── Updates et Data & Storage, cote a cote ───────────────────────────
    auto* pair = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL,
                                             ui::kCardSpacing);
    pair->set_homogeneous(true);

    auto updates = ui::card("bc-sync.svg", _("Updates"),
                            _("Manage application updates."));
    auto* upd_rows = ui::rows();
    m_switch_auto_update.set_active(true);
    m_switch_auto_update.set_valign(Gtk::ALIGN_CENTER);
    ui::add_row(upd_rows, *ui::row("bc-download.svg",
                                   _("Check for updates automatically"),
                                   _("Notify when a new version is available."),
                                   &m_switch_auto_update));

    auto* upd_bottom = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 14);
    upd_bottom->get_style_context()->add_class("set-row");
    m_btn_check_updates.set_label(_("Check for updates now"));
    m_btn_check_updates.set_image(*ui::image("bc-external.svg", ui::kIconButton));
    m_btn_check_updates.set_always_show_image(true);
    m_btn_check_updates.signal_clicked().connect([this] {
        m_btn_check_updates.set_sensitive(false);
        set_update_state(_("Checking…"), "muted");
        check_launcher_update_async();
    });
    upd_bottom->pack_start(m_btn_check_updates, Gtk::PACK_EXPAND_WIDGET);

    auto* ver_box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 3);
    ver_box->set_valign(Gtk::ALIGN_CENTER);
    ver_box->pack_start(*ui::sub_label(_("Current version")), Gtk::PACK_SHRINK);
#ifdef BOOTCADE_VERSION
    m_lbl_version.set_text(std::string("v") + BOOTCADE_VERSION);
#else
    m_lbl_version.set_text("—");
#endif
    m_lbl_version.set_xalign(0.0f);
    m_lbl_version.get_style_context()->add_class("set-row-title");
    ver_box->pack_start(m_lbl_version, Gtk::PACK_SHRINK);
    m_update_state_text.set_xalign(0.0f);
    m_update_state.pack_start(m_update_state_icon, Gtk::PACK_SHRINK);
    m_update_state.pack_start(m_update_state_text, Gtk::PACK_SHRINK);
    ver_box->pack_start(m_update_state, Gtk::PACK_SHRINK);
    upd_bottom->pack_start(*ver_box, Gtk::PACK_SHRINK);

    updates.body->pack_start(*upd_rows, Gtk::PACK_SHRINK);
    updates.body->pack_start(*upd_bottom, Gtk::PACK_SHRINK);
    pair->pack_start(*updates.frame, Gtk::PACK_EXPAND_WIDGET);

    auto storage = ui::card("database.svg", _("Data & Storage"),
                            _("Manage local application data."));
    auto* sto_rows = ui::rows();
    m_btn_clear_cache.set_label(_("Clear…"));
    m_btn_clear_cache.signal_clicked().connect(
        sigc::mem_fun(*this, &SettingsPanel::on_clear_cache_clicked));
    ui::add_row(sto_rows, *ui::row("bc-trash.svg", _("Clear local cache"),
                                   _("Remove cached images and temporary files."),
                                   &m_btn_clear_cache));
    m_btn_reset_settings.set_label(_("Reset…"));
    m_btn_reset_settings.get_style_context()->add_class("set-danger");
    m_btn_reset_settings.signal_clicked().connect(
        sigc::mem_fun(*this, &SettingsPanel::on_reset_settings_clicked));
    ui::add_row(sto_rows, *ui::row("bc-sync.svg", _("Reset all settings"),
                                   _("Restore all settings to their default values."),
                                   &m_btn_reset_settings));
    storage.body->pack_start(*sto_rows, Gtk::PACK_SHRINK);
    pair->pack_start(*storage.frame, Gtk::PACK_EXPAND_WIDGET);

    page->pack_start(*pair, Gtk::PACK_SHRINK);

    set_update_state(_("Not checked yet"), "muted");
    return page;
}

void SettingsPanel::set_update_state(const std::string& text, const std::string& tone) {
    m_update_state_text.set_text(text);
    auto ctx = m_update_state_text.get_style_context();
    for (const char* c : {"set-ok", "set-warn", "set-err", "set-sub"})
        ctx->remove_class(c);
    ctx->add_class(tone == "ok" ? "set-ok" : tone == "warn" ? "set-warn" : "set-sub");
    if (tone == "ok")
        m_update_state_icon.set_file("bc-detected.svg");
    else
        m_update_state_icon.set_file("bc-info.svg");
    m_update_state_icon.show();
}

/* La verification tourne HORS du fil graphique.
 *
 * fetch_launcher_latest interroge GitHub : sur un reseau lent elle bloque
 * plusieurs secondes, et la faire sur le fil principal figerait la fenetre
 * entiere le temps de l'appel.
 */
void SettingsPanel::check_launcher_update_async() {
    std::thread([this, alive = m_alive] {
        auto r = FbneoUpdateCheck::fetch_launcher_latest();
        std::string tag = r.tag;
        if (!tag.empty() && tag[0] == 'v') tag.erase(0, 1);
#ifdef BOOTCADE_VERSION
        const std::string me = BOOTCADE_VERSION;
#else
        const std::string me;
#endif
        {
            std::lock_guard<std::mutex> lock(m_update_mutex);
            m_update_failed = !r.ok;
            // Egale ou anterieure : rien a annoncer. La comparaison se fait sur
            // les seuls nombres, comme dans la fenetre principale : un build de
            // developpement porte « 1.0.20+7.g1a2b3c4 », dont std::stoi ne lit
            // que la partie publiee.
            m_update_tag.clear();
            if (r.ok && !tag.empty() && !me.empty()) {
                auto parts = [](const std::string& v) {
                    std::vector<int> out;
                    size_t i = 0;
                    while (i < v.size() && out.size() < 3) {
                        try { out.push_back(std::stoi(v.substr(i))); } catch (...) { break; }
                        size_t dot = v.find('.', i);
                        if (dot == std::string::npos) break;
                        i = dot + 1;
                    }
                    out.resize(3, 0);
                    return out;
                };
                auto a = parts(tag), b = parts(me);
                if (a > b) m_update_tag = tag;
            }
        }
        std::lock_guard<std::mutex> live(alive->mutex);
        if (alive->alive) m_update_done.emit();
    }).detach();
}

// ─────────────────────────────────────────────────────────────────────────
//  Library
// ─────────────────────────────────────────────────────────────────────────


// ── Random play ───────────────────────────────────────────────────────────
Gtk::Widget* SettingsPanel::build_page_random() {
    auto* page = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL,
                                             ui::kCardSpacing);
    page->get_style_context()->add_class("set-page");

    // ── Random game ───────────────────────────────────────────────────────
    // Le bouton « de » de la barre du haut tire un jeu quand on ne sait pas
    // a quoi jouer. Par defaut il pioche dans ce qui est affiche : la colonne
    // de gauche et la recherche sont deja tous les filtres qu'on peut vouloir,
    // les redoubler ici donnerait deux endroits pour la meme question.
    auto rnd = ui::card("bc-dice.svg", _("Random game"),
                        _("What the dice button in the top bar may pick."));
    auto* rnd_rows = ui::rows();
    m_combo_random_from.append("shown", _("The games currently shown"));
    m_combo_random_from.append("own",   _("The systems ticked below"));
    m_combo_random_from.set_active_id("shown");
    m_combo_random_from.set_valign(Gtk::ALIGN_CENTER);
    ui::add_row(rnd_rows, *ui::row("bc-sliders.svg", _("Draw from"),
                                   _("The current filters and search, or your own choice of systems."),
                                   &m_combo_random_from));
    for (auto* sw : {&m_switch_random_hiscore, &m_switch_random_originals,
                     &m_switch_random_unplayed, &m_switch_random_launch}) {
        sw->set_active(false);
        sw->set_valign(Gtk::ALIGN_CENTER);
    }
    ui::add_row(rnd_rows, *ui::row("bc-trophy.svg", _("Only games with a leaderboard"),
                                   _("Games that carry the Highscore badge."),
                                   &m_switch_random_hiscore));
    ui::add_row(rnd_rows, *ui::row("bc-package.svg", _("Only originals"),
                                   _("Leave clones and alternate versions out."),
                                   &m_switch_random_originals));
    ui::add_row(rnd_rows, *ui::row("bc-clock.svg", _("Only games never played"),
                                   _("Discover something new every time."),
                                   &m_switch_random_unplayed));
    ui::add_row(rnd_rows, *ui::row("play.svg", _("Launch immediately"),
                                   _("Start the game as soon as it is picked, without pressing Play."),
                                   &m_switch_random_launch));
    rnd.body->pack_start(*rnd_rows, Gtk::PACK_SHRINK);
    m_random_systems_box.set_selection_mode(Gtk::SELECTION_NONE);
    m_random_systems_box.set_max_children_per_line(4);
    m_random_systems_box.set_column_spacing(12);
    m_random_systems_box.set_row_spacing(4);
    m_random_systems_box.set_margin_top(8);
    rnd.body->pack_start(m_random_systems_box, Gtk::PACK_SHRINK);
    m_combo_random_from.signal_changed().connect([this] {
        m_random_systems_box.set_visible(m_combo_random_from.get_active_id() == "own");
    });
    m_random_systems_box.set_no_show_all(true);
    page->pack_start(*rnd.frame, Gtk::PACK_SHRINK);

    return page;
}

Gtk::Widget* SettingsPanel::build_page_library() {
    auto* page = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL,
                                             ui::kCardSpacing);
    page->get_style_context()->add_class("set-page");

    /* ── A quel emulateur tout cela s'applique ────────────────────────────
     *
     * En TETE de page, avant les trois cartes, parce que c'est la premiere
     * chose a savoir en les lisant : « ROM Directories » ne veut rien dire
     * tant qu'on ignore de quelle collection il parle.
     *
     * Des onglets, et non une liste deroulante : ce que la page regle est
     * alors visible en permanence au lieu de dormir dans un menu ferme, et
     * passer d'un emulateur a l'autre coute un clic au lieu de deux. Meme
     * dessin que la barre de la fenetre (add_tab) : une pastille par entree,
     * une seule enfoncee. Elle est posee DANS la page, donc deja en retrait
     * des 18 px de marge : imbriquee, elle se lit comme une precision de la
     * barre du dessus et non comme une seconde navigation.
     */
    auto* emu_bar = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    emu_bar->get_style_context()->add_class("set-tabbar");
    for (const auto& entry : emulator_registry()) {
        const std::string id = entry.id;
        auto* btn = Gtk::make_managed<Gtk::ToggleButton>();
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 9);
        box->set_halign(Gtk::ALIGN_CENTER);
        /* brand_logo, surtout pas ui::image : les pictogrammes de l'interface
         * se repeignent avec l'encre du contexte, et un onglet enfonce les
         * passe en blanc. Un logo qui change de couleur n'est plus le logo. */
        box->pack_start(*brand_logo(entry.logo, 44, 24), Gtk::PACK_SHRINK);
        // Le nom d'une marque ne se traduit pas : pas de _() ici.
        box->pack_start(*Gtk::make_managed<Gtk::Label>(entry.name), Gtk::PACK_SHRINK);
        btn->add(*box);
        btn->get_style_context()->add_class("set-tab");
        /* « clicked », comme la barre principale : « toggled » obligerait a
         * rattraper a la main l'onglet courant qu'un clic decoche et les
         * autres qu'il faut eteindre. C'est library_show qui remet les
         * bascules d'aplomb, dans les deux cas. */
        btn->signal_clicked().connect([this, id] {
            if (m_library_switching) return;
            library_show(id);
        });
        m_library_tabs.emplace_back(id, btn);
        emu_bar->pack_start(*btn, Gtk::PACK_SHRINK);
    }
    page->pack_start(*emu_bar, Gtk::PACK_SHRINK);

    // Sous la barre, la ou elle repond a la question que les onglets posent :
    // ce qui suit ne regle QUE l'emulateur dont l'onglet est enfonce.
    auto* emu_note = ui::sub_label(
        _("These library settings apply to the emulator selected here."));
    emu_note->set_margin_top(3);
    // Alignee sous le PREMIER onglet, pas sous le bord de la barre : c'est
    // aux onglets que « selected here » renvoie, et un texte decale de leur
    // colonne se lirait comme le titre de ce qui suit.
    emu_note->set_margin_start(18);
    emu_note->set_ellipsize(Pango::ELLIPSIZE_END);
    page->pack_start(*emu_note, Gtk::PACK_SHRINK);

    // ── ROM Directories ──────────────────────────────────────────────────
    auto roms = ui::card("bc-folder-plus.svg", _("ROM Directories"),
                         _("Add the folders that contain your ROMs. Bootcade will "
                           "scan these directories for supported games."));

    m_button_add_roms.set_label(_("Add Folder"));
    m_button_add_roms.set_image(*ui::image("bc-folder-plus.svg", ui::kIconButton));
    m_button_add_roms.set_always_show_image(true);
    m_button_add_roms.get_style_context()->add_class("accent-button");
    m_button_add_roms.signal_clicked().connect(
        sigc::mem_fun(*this, &SettingsPanel::on_add_roms_path_clicked));
    m_button_remove_roms.set_label(_("Remove"));
    m_button_remove_roms.set_image(*ui::image("bc-trash-red.svg", ui::kIconButton));
    m_button_remove_roms.set_always_show_image(true);
    m_button_remove_roms.get_style_context()->add_class("set-danger");
    m_button_remove_roms.signal_clicked().connect(
        sigc::mem_fun(*this, &SettingsPanel::on_remove_roms_path_clicked));
    auto* roms_actions = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    roms_actions->set_valign(Gtk::ALIGN_CENTER);
    roms_actions->pack_start(m_button_add_roms,    Gtk::PACK_SHRINK);
    roms_actions->pack_start(m_button_remove_roms, Gtk::PACK_SHRINK);
    roms.head->pack_end(*roms_actions, Gtk::PACK_SHRINK);

    /* La liste garde sa TreeView.
     *
     * Elle porte la selection dont depend « Remove », et l'index de la ligne
     * choisie est ce que remove_roms_path attend. La remplacer par une ListBox
     * aurait reecrit ce chemin-la pour un gain purement visuel : ce sont ses
     * COLONNES qui changent, pas elle.
     */
    m_model_roms = Gtk::ListStore::create(m_cols_roms);
    m_treeview_roms.set_model(m_model_roms);
    m_treeview_roms.set_headers_visible(false);
    m_treeview_roms.get_style_context()->add_class("set-romlist");
    m_treeview_roms.append_column("", m_cols_roms.icon);
    m_treeview_roms.append_column("", m_cols_roms.path);
    if (auto* col = m_treeview_roms.get_column(1)) col->set_expand(true);
    {
        // Pastille et mot dans UNE cellule : une colonne de plus les aurait
        // separes, et la pastille se serait retrouvee a distance de son texte.
        auto* cell = Gtk::manage(new Gtk::CellRendererText());
        cell->property_xalign() = 1.0f;
        auto* col = Gtk::manage(new Gtk::TreeViewColumn("", *cell));
        col->add_attribute(cell->property_markup(), m_cols_roms.status);
        m_treeview_roms.append_column(*col);
    }
    m_scrolled_roms.add(m_treeview_roms);
    m_scrolled_roms.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    /* Hauteur EXACTE, pas un minimum.
     *
     * Avec un simple set_size_request la zone prenait sa hauteur naturelle,
     * c'est-a-dire celle de son contenu : dix dossiers donnaient dix lignes,
     * et la poignee ne commandait plus rien. Bornee des deux cotes, la liste
     * fait la hauteur demandee quel que soit le nombre de dossiers, et c'est
     * elle qui defile.
     */
    m_scrolled_roms.set_propagate_natural_height(false);
    apply_roms_list_height();
    m_scrolled_roms.get_style_context()->add_class("set-rows");
    roms.body->pack_start(m_scrolled_roms, Gtk::PACK_SHRINK);

    /* ── La poignee ──────────────────────────────────────────────────────
     *
     * Trois points sous la liste, centres : c'est la ou l'oeil cherche de
     * quoi tirer, et c'est la convention. Elle n'a l'air active qu'au survol,
     * pour ne pas se lire comme un separateur decoratif de plus.
     */
    auto* grip_icon = ui::image("bc-grip.svg", 20);
    m_roms_grip.add(*grip_icon);
    m_roms_grip.get_style_context()->add_class("set-grip");
    m_roms_grip.set_halign(Gtk::ALIGN_CENTER);
    m_roms_grip.set_tooltip_text(_("Drag to resize the list"));
    /* Le masque d'evenements, a poser NOUS-MEMES.
     *
     * Un Gtk::EventBox possede sa propre fenetre GDK, et GTK3 ne complete pas
     * son masque pour les gestes qu'on lui attache : sans ces trois bits, la
     * poignee recevait bien le survol (c'est pourquoi elle s'allumait) mais
     * jamais l'appui, et le geste ne demarrait pas. A poser avant que le
     * widget soit realise.
     */
    m_roms_grip.add_events(Gdk::BUTTON_PRESS_MASK | Gdk::BUTTON_RELEASE_MASK
                           | Gdk::BUTTON1_MOTION_MASK);
    // Le curseur dit ce que fait la poignee avant qu'on l'essaie.
    m_roms_grip.signal_realize().connect([this] {
        if (auto win = m_roms_grip.get_window())
            win->set_cursor(Gdk::Cursor::create(m_roms_grip.get_display(), "ns-resize"));
    });

    /* Un geste, pas des evenements bruts.
     *
     * Suivre soi-meme press / motion / release oblige a poser les bons masques
     * ET a tenir le grab du pointeur : sans grab, les deplacements partent au
     * widget survole des que le curseur quitte la poignee, c'est-a-dire des le
     * premier pixel, puisqu'elle bouge avec ce qu'elle redimensionne.
     * Gtk::GestureDrag s'en charge et rend directement le deplacement cumule
     * depuis le debut du geste : une valeur ABSOLUE, donc rien a accumuler.
     */
    m_grip_drag = Gtk::GestureDrag::create(m_roms_grip);
    m_grip_drag->signal_drag_begin().connect([this](double, double) {
        m_grip_start_height = m_roms_list_height;
        /* La borne haute se fige ICI, pas a chaque pixel.
         *
         * Elle depend de la place restante entre la fenetre et le bord de
         * l'ecran ; la recalculer pendant le glisser reviendrait a lire une
         * geometrie qui bouge. Une fenetre plus haute que la zone de travail
         * mettrait son pied d'actions hors de portee.
         */
        m_grip_max_height = 900;
        if (auto* win = dynamic_cast<Gtk::Window*>(get_toplevel())) {
            if (auto gdkwin = win->get_window()) {
                int win_w = 0, win_h = 0;
                win->get_size(win_w, win_h);
                Gdk::Rectangle work;
                auto monitor = win->get_display()->get_monitor_at_window(gdkwin);
                if (monitor) {
                    monitor->get_workarea(work);
                    m_grip_max_height =
                        m_roms_list_height + (work.get_height() - win_h);
                }
            }
        }
        if (m_grip_max_height < 120) m_grip_max_height = 120;
    });
    m_grip_drag->signal_drag_update().connect([this](double, double offset_y) {
        set_roms_list_height(m_grip_start_height + static_cast<int>(offset_y));
    });

    roms.body->pack_start(m_roms_grip, Gtk::PACK_SHRINK);
    m_lbl_lib_roms_sub = roms.subtitle;
    page->pack_start(*roms.frame, Gtk::PACK_SHRINK);

    // ── Artwork & Media ──────────────────────────────────────────────────
    auto art = ui::card("bc-image.svg", _("Artwork & Media"),
                        _("Configure where Bootcade stores and downloads game "
                          "artwork, previews and titles."));
    auto* art_rows = ui::rows();

    m_button_browse_previews.set_label(_("Browse..."));
    m_button_browse_previews.set_image(*ui::image("bc-folder.svg", ui::kIconButton));
    m_button_browse_previews.set_always_show_image(true);
    m_button_browse_previews.signal_clicked().connect([this] {
        on_folder_clicked(&m_entry_previews);
    });
    m_button_download_previews.set_label(_("Download All"));
    m_button_download_previews.set_image(*ui::image("bc-download.svg", ui::kIconButton));
    m_button_download_previews.set_always_show_image(true);
    m_button_download_previews.signal_clicked().connect(
        sigc::mem_fun(*this, &SettingsPanel::on_download_previews_clicked));
    ui::add_row(art_rows, *path_row(_("Previews"),
                                    _("Path for game preview images (screenshots)."),
                                    m_entry_previews, m_button_browse_previews,
                                    m_button_download_previews));

    m_button_browse_titles.set_label(_("Browse..."));
    m_button_browse_titles.set_image(*ui::image("bc-folder.svg", ui::kIconButton));
    m_button_browse_titles.set_always_show_image(true);
    m_button_browse_titles.signal_clicked().connect([this] {
        on_folder_clicked(&m_entry_titles);
    });
    m_button_download_titles.set_label(_("Download All"));
    m_button_download_titles.set_image(*ui::image("bc-download.svg", ui::kIconButton));
    m_button_download_titles.set_always_show_image(true);
    m_button_download_titles.signal_clicked().connect(
        sigc::mem_fun(*this, &SettingsPanel::on_download_titles_clicked));
    ui::add_row(art_rows, *path_row(_("Titles"),
                                    _("Path for game title images (logos, marquees, etc)."),
                                    m_entry_titles, m_button_browse_titles,
                                    m_button_download_titles));
    art.body->pack_start(*art_rows, Gtk::PACK_SHRINK);
    m_lbl_lib_art_sub = art.subtitle;

    /* ── Ou les images se telechargent ───────────────────────────────────
     *
     * Une adresse unique et en dur ne servait qu'un emulateur : celle de
     * FBNeo-extras ignore le catalogue MAME, et le joueur qui heberge sa
     * propre collection n'avait aucun moyen de la designer. La liste est
     * donc editable, propre a l'onglet affiche, et essayee DANS L'ORDRE :
     * la premiere qui rend l'image gagne, les suivantes comblent ses trous.
     */
    auto* src_head = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    src_head->set_margin_top(14);
    auto* src_txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 1);
    src_txt->set_valign(Gtk::ALIGN_CENTER);
    src_txt->pack_start(*ui::title_label(_("Download sources")), Gtk::PACK_SHRINK);
    {
        auto* sub = ui::sub_label(
            _("Tried in order until an image is found. Each address is a base "
              "folder holding previews/ and titles/."));
        sub->set_line_wrap(true);
        src_txt->pack_start(*sub, Gtk::PACK_SHRINK);
    }
    src_head->pack_start(*src_txt, Gtk::PACK_EXPAND_WIDGET);

    m_btn_add_source.set_label(_("Add Source"));
    m_btn_add_source.set_image(*ui::image("bc-plus.svg", ui::kIconButton));
    m_btn_add_source.set_always_show_image(true);
    m_btn_add_source.set_valign(Gtk::ALIGN_CENTER);
    m_btn_add_source.signal_clicked().connect([this] {
        m_art_sources.push_back(std::string());
        art_sources_rebuild();
        // Le champ neuf prend le curseur : un rang vide ajoute sans rien a
        // remplir se lirait comme une panne.
        if (auto* row = m_art_sources_list.get_row_at_index(
                static_cast<int>(m_art_sources.size()) - 1))
            row->grab_focus();
    });
    src_head->pack_end(m_btn_add_source, Gtk::PACK_SHRINK);
    art.body->pack_start(*src_head, Gtk::PACK_SHRINK);

    m_art_sources_list.set_selection_mode(Gtk::SELECTION_NONE);
    {
        // Une liste vide doit dire ce qu'elle entraine : sans source, aucun
        // bouton « Download All » de cette carte ne peut rien ramener.
        auto* none = ui::sub_label(_("No source: artwork cannot be downloaded for "
                                     "these games. Add one above."));
        none->set_line_wrap(true);
        none->set_margin_top(10);
        none->set_margin_bottom(10);
        none->set_margin_start(12);
        none->set_margin_end(12);
        none->show();
        m_art_sources_list.set_placeholder(*none);
    }
    m_art_sources_list.get_style_context()->add_class("set-rows");
    m_art_sources_list.set_margin_top(8);
    art.body->pack_start(m_art_sources_list, Gtk::PACK_SHRINK);

    // Les DAT ne se reglent plus ici : leur dossier, leur source et leur
    // generation vivent dans ROM Management, onglet DAT. m_entry_dat reste le
    // porteur de la cle dat_path pour le reste de l'application, sans etre
    // affiche.
    page->pack_start(*art.frame, Gtk::PACK_SHRINK);

    /* ── Le genre des machines MAME ──────────────────────────────────────
     *
     * MAME n'expose aucun genre : ni `-listxml`, ni aucune de ses options.
     * Ces champs sont une extension de notre fork FinalBurn Neo. Filtrer la
     * bibliotheque sur « Racing » ne rendait donc que du FinalBurn Neo, et
     * cela se lisait comme un filtre casse.
     *
     * La carte vit ici et non dans la fiche de l'emulateur : elle remplit
     * une colonne de la BIBLIOTHEQUE, comme les images juste au-dessus. Elle
     * ne parait que sur l'onglet MAME, puisqu'elle ne dit rien des autres.
     */
    auto catver = ui::card("filter-item.svg", _("Game Genres (catver.ini)"),
                           _("MAME does not tell what genre a machine is. "
                             "catver.ini fills the Genre filter for MAME games."));
    auto* catver_rows = ui::rows();

    /* Le champ SOUS son intitule, et non a sa droite.
     *
     * Pose en controle de fin de ligne, un champ cede toute la place au
     * texte explicatif : a la largeur minimale de la fenetre il ne montrait
     * plus que « https://ww », et une adresse qu'on ne peut pas lire ne se
     * corrige pas. En dessous, il prend toute la largeur de la carte. */
    auto field_row = [](const std::string& icon, const std::string& title,
                        const std::string& subtitle, Gtk::Widget& slot) {
        auto* line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 8);
        line->get_style_context()->add_class("set-row");
        auto* top = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 13);
        top->pack_start(*ui::tile(icon, ui::kIconRow, ui::kTileRow), Gtk::PACK_SHRINK);
        auto* txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 1);
        txt->set_valign(Gtk::ALIGN_CENTER);
        txt->pack_start(*ui::title_label(title), Gtk::PACK_SHRINK);
        auto* sub = ui::sub_label(subtitle);
        sub->set_line_wrap(true);
        txt->pack_start(*sub, Gtk::PACK_SHRINK);
        top->pack_start(*txt, Gtk::PACK_EXPAND_WIDGET);
        line->pack_start(*top, Gtk::PACK_SHRINK);
        // Aligne sous le texte, pas sous la tuile : 28 px de tuile + 13 d'ecart.
        slot.set_margin_start(ui::kTileRow + 13);
        line->pack_start(slot, Gtk::PACK_SHRINK);
        return line;
    };

    auto* catver_slot = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 7);
    m_entry_catver.set_width_chars(8);
    m_entry_catver.set_placeholder_text(_("Path to catver.ini"));
    m_entry_catver.signal_activate().connect([this] { apply_catver_now(true); });
    m_entry_catver.signal_focus_out_event().connect([this](GdkEventFocus*) {
        refresh_catver_state();
        return false;
    });
    catver_slot->pack_start(m_entry_catver, Gtk::PACK_EXPAND_WIDGET);

    m_btn_browse_catver.set_image(*ui::image("bc-folder.svg", ui::kIconButton));
    m_btn_browse_catver.set_always_show_image(true);
    m_btn_browse_catver.set_tooltip_text(_("Browse..."));
    m_btn_browse_catver.signal_clicked().connect([this] {
        auto chooser = Gtk::FileChooserDialog(_("Select catver.ini"),
                                              Gtk::FILE_CHOOSER_ACTION_OPEN);
        if (auto* win = dynamic_cast<Gtk::Window*>(get_toplevel()))
            chooser.set_transient_for(*win);
        chooser.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
        chooser.add_button(_("Select"), Gtk::RESPONSE_OK);
        auto filter = Gtk::FileFilter::create();
        filter->set_name(_("Category files"));
        filter->add_pattern("*.ini");
        chooser.add_filter(filter);
        const std::string current = m_entry_catver.get_text();
        if (!current.empty()) chooser.set_filename(current);
        if (chooser.run() == Gtk::RESPONSE_OK) {
            m_entry_catver.set_text(chooser.get_filename());
            // Le designer suffit : personne ne doit deviner qu'il faut encore
            // appuyer ailleurs pour que les genres apparaissent.
            apply_catver_now(true);
        }
    });
    catver_slot->pack_start(m_btn_browse_catver, Gtk::PACK_SHRINK);
    ui::add_row(catver_rows, *field_row("bc-folder.svg", _("Category file"),
                                        _("The catver.ini Bootcade reads. Leave empty to "
                                          "use the copy Bootcade downloaded."),
                                        *catver_slot));

    /* L'adresse, modifiable, et un bouton qui DIT plutot que de telecharger.
     *
     * Sur le modele des sources DAT du gestionnaire de ROMs : on compare ce
     * qu'on a a ce que le serveur publie, et on l'annonce. progetto-SNAPS ne
     * publie aucun manifeste, donc la comparaison se fait sur le numero de
     * version que le fichier porte dans son en-tete. */
    auto* catver_url_slot = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 7);
    m_entry_catver_url.set_width_chars(8);
    m_entry_catver_url.set_placeholder_text(_("Download address"));
    catver_url_slot->pack_start(m_entry_catver_url, Gtk::PACK_EXPAND_WIDGET);

    m_btn_check_catver.set_image(*ui::image("bc-sync.svg", ui::kIconButton));
    m_btn_check_catver.set_always_show_image(true);
    m_btn_check_catver.set_tooltip_text(_("Check for a newer catver.ini"));
    m_btn_check_catver.signal_clicked().connect([this] { check_catver_update_async(); });
    catver_url_slot->pack_start(m_btn_check_catver, Gtk::PACK_SHRINK);

    m_btn_download_catver.set_image(*ui::image("bc-download.svg", ui::kIconButton));
    m_btn_download_catver.set_always_show_image(true);
    m_btn_download_catver.set_tooltip_text(_("Download catver.ini from this address"));
    m_btn_download_catver.signal_clicked().connect([this] { download_catver_clicked(); });
    catver_url_slot->pack_start(m_btn_download_catver, Gtk::PACK_SHRINK);

    ui::add_row(catver_rows, *field_row("bc-globe.svg", _("Download address"),
                                        _("Where the category archive is fetched from. "
                                          "Leave empty to follow the installed MAME version."),
                                        *catver_url_slot));
    catver.body->pack_start(*catver_rows, Gtk::PACK_SHRINK);

    m_lbl_catver_state.set_xalign(0.0f);
    m_lbl_catver_state.set_line_wrap(true);
    m_lbl_catver_state.get_style_context()->add_class("set-sub");
    m_lbl_catver_state.set_margin_top(10);
    catver.body->pack_start(m_lbl_catver_state, Gtk::PACK_SHRINK);

    m_lbl_catver_check.set_xalign(0.0f);
    m_lbl_catver_check.set_line_wrap(true);
    m_lbl_catver_check.get_style_context()->add_class("set-sub");
    m_lbl_catver_check.set_margin_top(4);
    m_lbl_catver_check.set_no_show_all(true);
    catver.body->pack_start(m_lbl_catver_check, Gtk::PACK_SHRINK);

    // Le fil de verification ne touche a rien : il depose son verdict et
    // reveille l'interface, qui seule ecrit dans les widgets.
    m_catver_done.connect([this] {
        std::string msg, found;
        {
            std::lock_guard<std::mutex> lock(m_catver_mutex);
            msg   = m_catver_msg;
            found = m_catver_found_url;
        }
        m_btn_check_catver.set_sensitive(true);
        m_lbl_catver_check.set_text(msg);
        m_lbl_catver_check.show();
        // L'adresse verifiee remplace celle du champ : le bouton de
        // telechargement, juste a cote, ira alors chercher ce qu'on vient de
        // dire qui existe, et non la version precedente.
        if (!found.empty()) m_entry_catver_url.set_text(found);
    });

    m_lib_catver_card = catver.frame;
    m_lib_catver_card->set_no_show_all(true);
    page->pack_start(*catver.frame, Gtk::PACK_SHRINK);

    // ── Scan Options ─────────────────────────────────────────────────────
    auto scan = ui::card("bc-search.svg", _("Scan Options"),
                         _("Configure how Bootcade scans your ROM directories."));
    auto* scan_grid = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 18);
    scan_grid->set_homogeneous(true);

    auto check_block = [](Gtk::CheckButton& check, const std::string& title,
                          const std::string& subtitle) {
        check.set_label(title);
        check.set_active(true);
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 3);
        box->pack_start(check, Gtk::PACK_SHRINK);
        auto* sub = ui::sub_label(subtitle);
        sub->set_margin_start(27);   // aligne sous le libelle, pas sous la case
        box->pack_start(*sub, Gtk::PACK_SHRINK);
        return box;
    };
    scan_grid->pack_start(*check_block(m_check_recursive,
                                       _("Scan directories recursively"),
                                       _("Include subfolders when scanning for games.")),
                          Gtk::PACK_EXPAND_WIDGET);
    scan_grid->pack_start(*check_block(m_check_loose_files,
                                       _("Include loose ROM files (non-zip)"),
                                       _("Also include individual ROM files, not only archives (zip, 7z, etc).")),
                          Gtk::PACK_EXPAND_WIDGET);
    scan.body->pack_start(*scan_grid, Gtk::PACK_SHRINK);
    m_lbl_lib_scan_sub = scan.subtitle;
    page->pack_start(*scan.frame, Gtk::PACK_SHRINK);

    /* La page nait sur le premier emulateur du registre.
     *
     * Elle se remplit avant que config.json soit lu : library_show pose donc
     * des valeurs vides, que load_from_file remplacera. L'important est que
     * m_library_emu soit designe des maintenant, sinon le premier
     * library_store_current rangerait le contenu des widgets sous une cle
     * vide, et les 21 dossiers du joueur y disparaitraient. */
    if (!emulator_registry().empty()) library_show(emulator_registry()[0].id);


    return page;
}

// ─────────────────────────────────────────────────────────────────────────
//  Emulator
// ─────────────────────────────────────────────────────────────────────────

Gtk::Widget* SettingsPanel::build_page_emulator() {
    auto* page = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL,
                                             ui::kCardSpacing);
    page->get_style_context()->add_class("set-page");

    // ── Colonne de gauche : le registre ──────────────────────────────────
    auto list_card = ui::card("bc-controller.svg", _("Emulators"),
                              _("Manage emulators available in Bootcade."));
    list_card.frame->set_size_request(300, -1);
    m_emu_list.set_selection_mode(Gtk::SELECTION_SINGLE);
    m_emu_list.get_style_context()->add_class("set-rows");

    for (const auto& entry : emulator_registry()) {
        auto* line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 12);
        line->get_style_context()->add_class("set-listrow");
        line->pack_start(*brand_logo(entry.logo, 54, 32), Gtk::PACK_SHRINK);
        auto* txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 2);
        txt->set_valign(Gtk::ALIGN_CENTER);
        txt->pack_start(*ui::title_label(entry.name), Gtk::PACK_SHRINK);
        // La pastille d'etat de l'entree choisie est celle du panneau de
        // droite : un seul calcul, donc jamais deux verdicts contradictoires
        // sur le meme binaire.
        // Un libelle par ligne, et non le widget partage du panneau de droite :
        // celui-ci ne peut avoir qu'un seul parent, si bien qu'avec deux
        // emulateurs il migrait vers la derniere ligne construite et la
        // premiere restait sans etat.
        auto* sub = Gtk::make_managed<Gtk::Label>();
        sub->set_markup("<small>" + Glib::Markup::escape_text(_(entry.kind)) + "</small>");
        sub->set_xalign(0.0f);
        sub->get_style_context()->add_class("dim-label");
        txt->pack_start(*sub, Gtk::PACK_SHRINK);
        line->pack_start(*txt, Gtk::PACK_EXPAND_WIDGET);
        line->pack_start(*ui::image("bc-chevron-right.svg", 16), Gtk::PACK_SHRINK);
        auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
        row->add(*line);
        m_emu_list.append(*row);
    }
    m_emu_list.signal_row_selected().connect([this](Gtk::ListBoxRow* row) {
        if (row) show_emulator_page(static_cast<size_t>(row->get_index()));
    });
    m_emu_list.select_row(*m_emu_list.get_row_at_index(0));
    list_card.body->pack_start(m_emu_list, Gtk::PACK_SHRINK);

    // Ce qui reste a venir se dit en clair plutot que sous forme d'entrees
    // grisees : une liste d'emulateurs non supportes ressemble a une panne.
    auto* soon = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 3);
    soon->get_style_context()->add_class("cc-subcard");
    soon->set_margin_top(12);
    auto* soon_head = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 9);
    soon_head->set_halign(Gtk::ALIGN_CENTER);
    soon_head->pack_start(*ui::image("bc-plus.svg", 17), Gtk::PACK_SHRINK);
    soon_head->pack_start(*ui::title_label(_("Add Emulator")), Gtk::PACK_SHRINK);
    soon->pack_start(*soon_head, Gtk::PACK_SHRINK);
    auto* soon_sub = ui::sub_label(_("Support for more emulators is coming."));
    soon_sub->set_justify(Gtk::JUSTIFY_CENTER);
    soon_sub->set_xalign(0.5f);
    soon->pack_start(*soon_sub, Gtk::PACK_SHRINK);
    list_card.body->pack_start(*soon, Gtk::PACK_SHRINK);
    /* La SEULE carte de la page qui ne se replie pas.
     *
     * Elle n'est pas de la lecture, elle est la commande : repliee, on ne
     * peut plus changer d'emulateur sans la rouvrir, et sa colonne de 300 px
     * reste la, vide, sur toute la hauteur. Essaye et regarde : on gagne une
     * ligne et on perd le seul bouton de la page.
     */
    page->pack_start(*list_card.frame, Gtk::PACK_SHRINK);

    // ── Colonne de droite : la configuration de l'emulateur choisi ───────
    auto* right = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL,
                                              ui::kCardSpacing);

    auto* head_card = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 16);
    head_card->get_style_context()->add_class("cc-card");
    auto* head = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 16);
    m_emu_head_logo = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 0);
    head->pack_start(*m_emu_head_logo, Gtk::PACK_SHRINK);
    auto* head_txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 3);
    head_txt->set_valign(Gtk::ALIGN_CENTER);
    m_emu_head_txt = head_txt;
    head->pack_start(*head_txt, Gtk::PACK_EXPAND_WIDGET);
    m_emu_head_pill.get_style_context()->add_class("set-pill");
    m_emu_head_pill.set_valign(Gtk::ALIGN_CENTER);
    m_emu_head_dot.get_style_context()->add_class("set-dot");
    m_emu_head_dot.set_valign(Gtk::ALIGN_CENTER);
    m_emu_head_pill.pack_start(m_emu_head_dot,  Gtk::PACK_SHRINK);
    m_emu_head_pill.pack_start(m_emu_head_text, Gtk::PACK_SHRINK);
    head->pack_start(m_emu_head_pill, Gtk::PACK_SHRINK);
    head_card->pack_start(*head, Gtk::PACK_SHRINK);

    // Bande de trois tuiles : ce que le lanceur SAIT du binaire installe.
    auto* stats = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 12);
    stats->set_homogeneous(true);
    stats->pack_start(*stat_tile("bc-package.svg", _("Build"), m_lbl_emu_version),
                      Gtk::PACK_EXPAND_WIDGET);
    stats->pack_start(*stat_tile("database.svg", _("Installed"), m_lbl_emu_systems),
                      Gtk::PACK_EXPAND_WIDGET);
    stats->pack_start(*stat_tile("bc-clock.svg", _("Last checked"), m_lbl_emu_checked),
                      Gtk::PACK_EXPAND_WIDGET);
    m_emu_stats_row = stats;
    head_card->pack_start(*stats, Gtk::PACK_SHRINK);

    /* La meme bande pour MAME, et au meme endroit.
     *
     * Elle vivait dans une carte a part, plus bas, qui reposait autrement les
     * memes questions : on lisait donc la meme chose a deux endroits, dans
     * deux dessins differents. Seuls les intitules changent, parce que MAME
     * n'a pas de numero de revision et qu'on peut lui designer n'importe
     * quel binaire — d'ou la tuile « Location ».
     */
    auto* mstats = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 12);
    mstats->set_homogeneous(true);
    mstats->pack_start(*stat_tile("bc-package.svg", _("Build"), m_lbl_mame_build),
                       Gtk::PACK_EXPAND_WIDGET);
    mstats->pack_start(*stat_tile("bc-folder.svg", _("Location"), m_lbl_mame_path),
                       Gtk::PACK_EXPAND_WIDGET);
    mstats->pack_start(*stat_tile("database.svg", _("Installed"), m_lbl_mame_date),
                       Gtk::PACK_EXPAND_WIDGET);
    mstats->pack_start(*stat_tile("bc-clock.svg", _("Last checked"), m_lbl_mame_checked),
                       Gtk::PACK_EXPAND_WIDGET);
    // Un chemin est long : sans ellipse il elargirait sa tuile jusqu'a
    // deformer toute la bande.
    m_lbl_mame_path.set_ellipsize(Pango::ELLIPSIZE_MIDDLE);
    m_lbl_mame_path.set_max_width_chars(14);
    m_emu_mame_stats_row = mstats;
    head_card->pack_start(*mstats, Gtk::PACK_SHRINK);

    auto* upd_line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 12);
    m_btn_emu_updates.set_label(_("Check for updates"));
    m_btn_emu_updates.set_image(*ui::image("bc-restore.svg", ui::kIconButton));
    m_btn_emu_updates.set_always_show_image(true);
    m_btn_emu_updates.signal_clicked().connect([this] {
        m_btn_emu_updates.set_sensitive(false);
        m_lbl_emu_note.set_text(_("Checking…"));
        m_lbl_emu_note.show();
        // Un seul bouton parce que le bandeau ne montre qu'un emulateur a la
        // fois : c'est celui-la qu'il verifie.
        if (m_emu_shown_mame) check_mame_update_async();
        else                  check_emulator_update_async();
    });
    upd_line->pack_start(m_btn_emu_updates, Gtk::PACK_SHRINK);
    m_lbl_emu_note.set_xalign(0.0f);
    m_lbl_emu_note.set_valign(Gtk::ALIGN_CENTER);
    upd_line->pack_start(m_lbl_emu_note, Gtk::PACK_SHRINK);
    m_emu_upd_row = upd_line;
    head_card->pack_start(*upd_line, Gtk::PACK_SHRINK);
    right->pack_start(*head_card, Gtk::PACK_SHRINK);

    // ── L'executable ─────────────────────────────────────────────────────
    auto exe = ui::card("bc-folder.svg", _("Executable"),
                        _("Select the FinalBurn Neo executable used to launch games."));
    auto* exe_line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    m_entry_fbneo.set_hexpand(true);
    exe_line->pack_start(m_entry_fbneo, Gtk::PACK_EXPAND_WIDGET);

    m_button_browse_fbneo.set_label(_("Browse..."));
    m_button_browse_fbneo.set_image(*ui::image("bc-folder.svg", ui::kIconButton));
    m_button_browse_fbneo.set_always_show_image(true);
    m_button_browse_fbneo.signal_clicked().connect([this] {
        auto dialog = Gtk::FileChooserDialog(_("Select FBNeo Executable"), Gtk::FILE_CHOOSER_ACTION_OPEN);
        dialog.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
        dialog.add_button(_("Select"), Gtk::RESPONSE_OK);

        auto filter = Gtk::FileFilter::create();
        filter->set_name("Executable");
        filter->add_pattern("*fbneo*");
        dialog.add_filter(filter);

        if (!m_entry_fbneo.get_text().empty()) {
            dialog.set_filename(m_entry_fbneo.get_text());
        }

        if (dialog.run() == Gtk::RESPONSE_OK) {
            m_entry_fbneo.set_text(dialog.get_filename());
        }
    });
    exe_line->pack_start(m_button_browse_fbneo, Gtk::PACK_SHRINK);

    m_button_download_fbneo.set_label(_("Download"));
    m_button_download_fbneo.set_image(*ui::image("bc-download.svg", ui::kIconButton));
    m_button_download_fbneo.set_always_show_image(true);
    m_button_download_fbneo.signal_clicked().connect(
        sigc::mem_fun(*this, &SettingsPanel::on_download_fbneo_clicked));
    exe_line->pack_start(m_button_download_fbneo, Gtk::PACK_SHRINK);
    exe.body->pack_start(*exe_line, Gtk::PACK_SHRINK);

    auto* exe_foot = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 12);
    exe_foot->set_margin_top(12);
    m_exe_state_text.set_xalign(0.0f);
    m_exe_state.set_valign(Gtk::ALIGN_CENTER);
    m_exe_state.pack_start(m_exe_state_icon, Gtk::PACK_SHRINK);
    m_exe_state.pack_start(m_exe_state_text, Gtk::PACK_SHRINK);
    exe_foot->pack_start(m_exe_state, Gtk::PACK_EXPAND_WIDGET);

    m_btn_test_emu.set_label(_("Test Emulator"));
    m_btn_test_emu.set_image(*ui::image("play.svg", ui::kIconButton));
    m_btn_test_emu.set_always_show_image(true);
    m_btn_test_emu.signal_clicked().connect([this] {
        const std::string path = m_entry_fbneo.get_text();
        auto* win = dynamic_cast<Gtk::Window*>(get_toplevel());
        if (path.empty() || ::access(path.c_str(), X_OK) != 0) {
            refresh_emulator_state();
            if (win)
                ui::notice(*win, _("The emulator cannot be run."),
                           _("Set a valid FinalBurn Neo executable, or download one."),
                           "bc-info.svg");
            return;
        }
        // Le vrai test, c'est de le LANCER : un fichier executable qui refuse
        // de demarrer (bibliotheque manquante, architecture) passe tous les
        // controles de permissions et echoue quand meme au premier jeu.
        try {
            Glib::spawn_async("", AppContext::host_command({path}),
                              Glib::SPAWN_SEARCH_PATH | Glib::SPAWN_DO_NOT_REAP_CHILD);
            m_exe_state_text.set_text(_("Emulator started. Close its window to come back."));
        } catch (const Glib::Error& e) {
            m_exe_state_text.set_text(
                Glib::ustring::compose(_("Could not start the emulator: %1"), e.what()));
        }
    });
    exe_foot->pack_start(m_btn_test_emu, Gtk::PACK_SHRINK);
    exe.body->pack_start(*exe_foot, Gtk::PACK_SHRINK);
    // Repliee, elle rappelle QUEL binaire est retenu : c'est la seule chose
    // qu'on vient verifier une fois qu'il est choisi.
    m_emu_exe_frame = collapsible(exe, "emulator.fbneo_executable", true, [this] {
        const std::string path = m_entry_fbneo.get_text();
        return path.empty() ? std::string(_("No executable set")) : path;
    });
    right->pack_start(*m_emu_exe_frame, Gtk::PACK_SHRINK);

    /* ── MAME : son executable, exactement comme FinalBurn Neo ──────────
     *
     * La page tenait pour acquis que MAME venait toujours de la distribution.
     * C'est faux : un binaire depose dans ~/Apps, une compilation locale, un
     * AppImage sont des installations ordinaires, et le joueur qui en a une
     * n'avait aucun moyen de le dire a Bootcade. Le champ prime donc sur la
     * detection, qui n'est plus qu'un repli — affiche en clair, pour que
     * « lequel ? » ait toujours une reponse.
     */
    auto* mame_col = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL,
                                                 ui::kCardSpacing);

    auto mame_exe = ui::card("bc-folder.svg", _("Executable"),
                             _("Select the MAME executable used to read the catalog and launch games."));
    auto* mame_exe_line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    m_entry_mame_exe.set_hexpand(true);
    m_entry_mame_exe.set_placeholder_text(_("Leave empty to let Bootcade find MAME"));
    // Pas de sonde a chaque touche : interroger le binaire coute un processus,
    // et « /usr/ga » n'est l'executable de personne. Le verdict se refait
    // quand la saisie est finie.
    // Par la pastille, pas par la carte seule : le bandeau annonce « Active »
    // ou « Not installed », et un chemin qui vient de changer le decide.
    m_entry_mame_exe.signal_activate().connect([this] { refresh_emulator_pill(); });
    m_entry_mame_exe.signal_focus_out_event().connect([this](GdkEventFocus*) {
        refresh_emulator_pill();
        return false;
    });
    mame_exe_line->pack_start(m_entry_mame_exe, Gtk::PACK_EXPAND_WIDGET);

    m_button_browse_mame.set_label(_("Browse..."));
    m_button_browse_mame.set_image(*ui::image("bc-folder.svg", ui::kIconButton));
    m_button_browse_mame.set_always_show_image(true);
    m_button_browse_mame.signal_clicked().connect([this] {
        auto dialog = Gtk::FileChooserDialog(_("Select MAME Executable"),
                                             Gtk::FILE_CHOOSER_ACTION_OPEN);
        dialog.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
        dialog.add_button(_("Select"), Gtk::RESPONSE_OK);
        // Le binaire peut s'appeler mame, mame0289, mame64... : filtrer sur
        // « *mame* » cacherait des installations parfaitement valides.
        auto filter = Gtk::FileFilter::create();
        filter->set_name(_("Executable"));
        filter->add_custom(Gtk::FILE_FILTER_FILENAME,
                           [](const Gtk::FileFilter::Info& info) {
                               return ::access(info.filename.c_str(), X_OK) == 0;
                           });
        dialog.add_filter(filter);
        const std::string current = mame_executable();
        if (!current.empty()) dialog.set_filename(current);
        if (dialog.run() == Gtk::RESPONSE_OK) {
            m_entry_mame_exe.set_text(dialog.get_filename());
            refresh_emulator_pill();
        }
    });
    mame_exe_line->pack_start(m_button_browse_mame, Gtk::PACK_SHRINK);
    mame_exe.body->pack_start(*mame_exe_line, Gtk::PACK_SHRINK);

    // Ce que la detection a trouve : la reponse a « et si je laisse vide ? ».
    m_lbl_mame_exe.set_xalign(0.0f);
    m_lbl_mame_exe.set_line_wrap(true);
    m_lbl_mame_exe.get_style_context()->add_class("set-sub");
    m_lbl_mame_exe.set_margin_top(8);
    mame_exe.body->pack_start(m_lbl_mame_exe, Gtk::PACK_SHRINK);

    auto* mame_exe_foot = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 12);
    mame_exe_foot->set_margin_top(12);
    m_mame_exe_state_text.set_xalign(0.0f);
    m_mame_exe_state.set_valign(Gtk::ALIGN_CENTER);
    m_mame_exe_state.pack_start(m_mame_exe_state_icon, Gtk::PACK_SHRINK);
    m_mame_exe_state.pack_start(m_mame_exe_state_text, Gtk::PACK_SHRINK);
    mame_exe_foot->pack_start(m_mame_exe_state, Gtk::PACK_EXPAND_WIDGET);

    m_btn_test_mame.set_label(_("Test Emulator"));
    m_btn_test_mame.set_image(*ui::image("play.svg", ui::kIconButton));
    m_btn_test_mame.set_always_show_image(true);
    m_btn_test_mame.signal_clicked().connect([this] {
        const std::string path = mame_executable();
        auto* win = dynamic_cast<Gtk::Window*>(get_toplevel());
        if (path.empty() || ::access(path.c_str(), X_OK) != 0) {
            refresh_emulator_pill();
            if (win)
                ui::notice(*win, _("The emulator cannot be run."),
                           _("Set a valid MAME executable, or install MAME with your "
                             "package manager."),
                           "bc-info.svg");
            return;
        }
        // Meme raison que pour FinalBurn Neo : un fichier executable qui
        // refuse de demarrer passe tous les controles de permissions et
        // echoue quand meme au premier jeu. Seul le lancer le prouve.
        try {
            Glib::spawn_async("", AppContext::host_command({path}),
                              Glib::SPAWN_SEARCH_PATH | Glib::SPAWN_DO_NOT_REAP_CHILD);
            m_mame_exe_state_text.set_text(_("Emulator started. Close its window to come back."));
        } catch (const Glib::Error& e) {
            m_mame_exe_state_text.set_text(
                Glib::ustring::compose(_("Could not start the emulator: %1"), e.what()));
        }
    });
    mame_exe_foot->pack_start(m_btn_test_mame, Gtk::PACK_SHRINK);
    mame_exe.body->pack_start(*mame_exe_foot, Gtk::PACK_SHRINK);
    mame_col->pack_start(*collapsible(mame_exe, "emulator.mame_executable", true, [this] {
        const std::string path = mame_executable();
        return path.empty() ? std::string(_("MAME not found")) : path;
    }), Gtk::PACK_SHRINK);

    /* ── Ce que Bootcade sait du MAME retenu ─────────────────────────────
     *
     * Version, emplacement et date sont montes dans le bandeau d'identite,
     * aupres de celles de FinalBurn Neo. Ne restent ici que les deux
     * particularites qui n'ont pas d'equivalent chez l'autre emulateur, et le
     * reglage des flippers.
     */
    auto mame = ui::card("bc-info.svg", _("MAME on this system"),
                         _("What works differently with MAME."));

    /* Cette phrase n'est vraie que d'un MAME installe en paquet. Devant le
     * binaire que le joueur a lui-meme designe, elle mentirait : ce n'est
     * pas son gestionnaire de paquets qui le met a jour. Elle parait donc
     * sous condition, et refresh_mame_state est seul juge.
     *
     * La ligne « aucun DAT a gerer » qui l'accompagnait a disparu : elle
     * etait vraie quand MAME ne savait que se lister lui-meme, elle ne l'est
     * plus depuis que le gestionnaire de ROMs genere des DAT depuis MAME.
     * C'est la-bas que les DAT se gerent, pour les deux emulateurs. */
    auto* mame_rows = ui::rows();
    mame_rows->set_margin_top(12);
    ui::add_row(mame_rows, *ui::row("bc-package.svg",
                                    _("Updates come from your distribution"),
                                    _("MAME is installed and updated by your package manager, "
                                      "not by Bootcade."),
                                    nullptr));
    m_mame_distro_rows = mame_rows;
    mame.body->pack_start(*mame_rows, Gtk::PACK_SHRINK);

    /* Le champ « MAME ROM folders » a disparu d'ici.
     *
     * Il faisait doublon avec « ROM Directories » vu depuis MAME : deux
     * endroits pour une seule liste, donc tot ou tard deux listes qui ne
     * disent pas la meme chose. Les dossiers de MAME se declarent desormais
     * la ou se declarent ceux de tous les emulateurs, page Library, et
     * mame_rompaths() les y lit. */
    auto* mame_rows2 = ui::rows();
    mame_rows2->set_margin_top(12);
    m_switch_mechanical.set_valign(Gtk::ALIGN_CENTER);
    // Le resume de la section repliee parle de cet interrupteur : sans cela il
    // resterait faux jusqu'au prochain pliage.
    m_switch_mechanical.property_active().signal_changed().connect(
        [this] { refresh_sections(); });
    m_switch_mechanical.set_tooltip_text(
        _("Pinball and slot machines are emulated but barely playable with a "
          "keyboard or a pad."));
    ui::add_row(mame_rows2, *ui::row("bc-controller.svg", _("Show MAME pinball machines"),
                                     _("Adds 15,000 mechanical machines to the library. "
                                       "Takes effect on the next start."),
                                     &m_switch_mechanical));

    /* Le genre des machines MAME a demenage page Library.
     *
     * catver.ini ne regle pas l'emulateur : il remplit la colonne « Genre »
     * de la bibliotheque, au meme titre que les images de previsualisation.
     * Il se regle donc avec elles, sur l'onglet MAME de cette page. */
    mame.body->pack_start(*mame_rows2, Gtk::PACK_SHRINK);
    /* Repliee, la carte rappelle le seul reglage qu'elle contient encore.
     * Elle annoncait la version de MAME, qui se lit desormais dans le
     * bandeau : la repeter ne disait plus rien de ce qui etait cache. La cle
     * « emulator.mame_system » ne bouge pas, pour que les sections deja
     * refermees par le joueur le restent. */
    mame_col->pack_start(*collapsible(mame, "emulator.mame_system", true, [this] {
        return std::string(m_switch_mechanical.get_active()
                               ? _("Pinball machines shown")
                               : _("Pinball machines hidden"));
    }), Gtk::PACK_SHRINK);
    m_emu_mame_frame = mame_col;
    right->pack_start(*mame_col, Gtk::PACK_SHRINK);

    // ── Options propres a l'emulateur ────────────────────────────────────
    /* Elles vivent ICI et non dans General : ce sont des options de
     * l'emulateur, pas de Bootcade. Le plein ecran et la mise a l'echelle
     * entiere de FBNeo sont exactement les reglages qu'offrait deja le menu
     * « Launch » ; ils ne sont pas dupliques, ils ont demenage, et le menu
     * reste en phase parce que les deux ecrivent la meme cle et s'ecoutent
     * l'un l'autre.
     *
     * Une carte, mais DEUX groupes de lignes : « Start FinalBurn Neo in
     * fullscreen » s'affichait jusqu'ici sous MAME, ou elle ne voulait rien
     * dire et ne commandait rien.
     */
    auto options = ui::card("gear.svg", _("Options"),
                            _("Configure emulator specific options."));
    auto* opt_rows = ui::rows();

    m_switch_fullscreen.set_valign(Gtk::ALIGN_CENTER);
    m_switch_fullscreen.property_active().signal_changed().connect([this] {
        if (!m_suppress_appearance_signals) m_sig_launch_options.emit();
    });
    ui::add_row(opt_rows, *ui::row("bc-window.svg", _("Launch games fullscreen"),
                                   _("Start FinalBurn Neo in fullscreen instead of a window."),
                                   &m_switch_fullscreen));

    m_switch_integerscale.set_valign(Gtk::ALIGN_CENTER);
    m_switch_integerscale.set_tooltip_text(
        _("Scales the picture by whole pixels only: sharper, with black borders."));
    m_switch_integerscale.property_active().signal_changed().connect([this] {
        if (!m_suppress_appearance_signals) m_sig_launch_options.emit();
    });
    ui::add_row(opt_rows, *ui::row("bc-image.svg", _("Integer scaling"),
                                   _("Avoid blurry scaling by using whole pixel multiples."),
                                   &m_switch_integerscale));

    m_entry_emu_args.set_size_request(ui::kFieldWidth, -1);
    m_entry_emu_args.set_placeholder_text(_("e.g. -nohiscores"));
    m_entry_emu_args.set_tooltip_text(
        _("Passed to FinalBurn Neo before the game name, separated by spaces."));
    ui::add_row(opt_rows, *ui::row("bc-sliders.svg",
                                   _("Additional command line arguments"),
                                   _("Extra arguments added to every game launch."),
                                   &m_entry_emu_args));
    m_emu_opt_fbneo = opt_rows;
    options.body->pack_start(*opt_rows, Gtk::PACK_SHRINK);

    m_emu_opt_mame = build_mame_options();
    options.body->pack_start(*m_emu_opt_mame, Gtk::PACK_SHRINK);
    // Meme regle qu'a l'onglet Online : la derniere carte de la colonne
    // descend jusqu'en bas pour s'aligner sur le cadre d'en face.
    /* La carte la plus longue de la page : quinze lignes pour MAME. C'est
     * elle qui rend le repli necessaire, et son resume compte ce qui est
     * allume pour qu'on sache s'il vaut la peine de rouvrir. */
    right->pack_start(*collapsible(options, "emulator.options", true, [this] {
        int on = 0;
        if (m_emu_shown_mame) {
            for (const Gtk::Switch* sw : {&m_sw_mame_fullscreen, &m_sw_mame_keepaspect,
                                          &m_sw_mame_intscale, &m_sw_mame_video,
                                          &m_sw_mame_vsync, &m_sw_mame_sound,
                                          &m_sw_mame_volume, &m_sw_mame_skipinfo,
                                          &m_sw_mame_hiscore, &m_sw_mame_autofire,
                                          &m_sw_mame_autosave, &m_sw_mame_snapdir,
                                          &m_sw_mame_snapname})
                if (sw->get_active()) ++on;
        } else {
            if (m_switch_fullscreen.get_active())   ++on;
            if (m_switch_integerscale.get_active()) ++on;
        }
        // Le catalogue de Bootcade est une table de chaines, sans regle de
        // pluriel : les deux formes s'ecrivent donc a la main.
        return Glib::ustring::compose(on == 1 ? _("%1 option on")
                                              : _("%1 options on"), on).raw();
    }), Gtk::PACK_SHRINK);
    /* Un vide qui pousse, plutot qu'une carte qui s'etire.
     *
     * La derniere carte descendait jusqu'en bas pour s'aligner sur le cadre
     * d'en face. Repliee, elle devenait un grand rectangle vide portant deux
     * lignes de titre. Ce sont les cartes qui font leur hauteur, et c'est ce
     * vide qui tient la colonne jusqu'en bas. */
    right->pack_start(*Gtk::make_managed<Gtk::Box>(), Gtk::PACK_EXPAND_WIDGET);

    /* La colonne de droite defile, et elle seule.
     *
     * La fiche de MAME porte une quinzaine d'options : empilees, elles
     * demandent une fenetre plus haute que beaucoup d'ecrans, et le pied
     * « Cancel / Save » finirait sous le bord. Le defilement ne change rien
     * aux fiches qui tiennent deja — set_propagate_natural_height laisse la
     * fenetre se regler au pixel sur leur hauteur — il ne sert que la ou il
     * n'y a plus le choix.
     */
    auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroller->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    scroller->set_propagate_natural_height(true);
    /* Sans hauteur minimale declaree, un volet defilant transmet celle de son
     * contenu : la fenetre ne pouvait alors plus redescendre sous la taille
     * de la fiche MAME, et le pied d'actions restait hors de l'ecran sur une
     * dalle 1080p. Ce plancher la laisse retrecir jusqu'a ce que l'ecran
     * permet ; c'est le defilement qui rend le reste atteignable. */
    scroller->set_min_content_height(360);
    /* Et un plafond, sans quoi la fiche MAME reclamerait a elle seule une
     * fenetre de 1900 px de haut : plus que la dalle de beaucoup de monde.
     * La valeur est reglee sur la fiche la plus longue qui tienne sans
     * defiler, celle de FinalBurn Neo, pour que passer d'un emulateur a
     * l'autre ne fasse pas sauter la fenetre. */
    scroller->set_max_content_height(820);
    scroller->set_shadow_type(Gtk::SHADOW_NONE);
    scroller->add(*right);
    page->pack_start(*scroller, Gtk::PACK_EXPAND_WIDGET);
    // La selection posee plus haut est arrivee avant que le panneau de
    // droite n'existe : c'est ici, et seulement ici, qu'il peut se remplir.
    show_emulator_page(0);
    return page;
}

/* ── Une carte que l'on peut replier ────────────────────────────────────
 *
 * Meme langage visuel que le volet de details de la fenetre principale : le
 * titre reste, et une fois la section fermee un RESUME prend la place de ce
 * qui disparait. Replier sans resume effacerait l'information au lieu de la
 * ranger, et il faudrait rouvrir chaque section rien que pour savoir
 * laquelle rouvrir.
 *
 * L'en-tete de la carte EST la poignee : la tuile, le titre et le sous-titre
 * sont deja la ou l'oeil vise, et un bouton de repli pose a cote aurait
 * donne deux commandes pour un seul geste.
 *
 * Pas de Gtk::Expander ici, malgre le volet de details : sa fleche est celle
 * du theme du bureau, que la feuille de style de cette fenetre efface deja
 * (« .cc-window expander > title > arrow »), et son etiquette garde sa
 * largeur NATURELLE — le resume et le chevron se collaient au sous-titre au
 * lieu de tenir le bord droit, et deux cartes voisines ne les alignaient
 * plus. L'en-tete reste donc un enfant ordinaire de la carte, qui prend
 * toute sa largeur comme les boutons d'action des autres cartes, et c'est le
 * CORPS qui se montre ou se cache.
 */
Gtk::Widget* SettingsPanel::collapsible(const ui::Card& card, const std::string& key,
                                        bool open_by_default,
                                        std::function<std::string()> describe) {
    Section section;
    section.describe = std::move(describe);

    auto* chevron = Gtk::make_managed<ui::Icon>("bc-chevron-down.svg", 15);
    chevron->set_valign(Gtk::ALIGN_CENTER);
    section.chevron = chevron;

    auto* summary = ui::sub_label("");
    summary->get_style_context()->add_class("dock-summary");
    summary->set_ellipsize(Pango::ELLIPSIZE_MIDDLE);
    summary->set_max_width_chars(44);
    summary->set_xalign(1.0f);
    summary->set_valign(Gtk::ALIGN_CENTER);
    // Le show_all() de la fenetre rallume tout ce qui ne porte pas ce
    // drapeau : sans lui, le resume reapparaitrait section ouverte.
    summary->set_no_show_all(true);
    section.summary = summary;

    card.head->pack_end(*chevron, Gtk::PACK_SHRINK);
    card.head->pack_end(*summary, Gtk::PACK_SHRINK);

    /* Le corps passe dans un Gtk::Revealer : replie, il ne prend plus aucune
     * hauteur, alors qu'un simple hide() laisse le show_all() de la fenetre
     * le rallumer a la premiere occasion. */
    auto* reveal = Gtk::make_managed<Gtk::Revealer>();
    reveal->set_transition_type(Gtk::REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    reveal->set_transition_duration(140);
    card.body->set_margin_top(14);
    /* remove() ne detruit pas un enfant gere : gtkmm le re-reference pour
     * qu'on puisse le reposer ailleurs, ce qui est exactement ce qu'on fait. */
    card.frame->remove(*card.body);
    reveal->add(*card.body);
    reveal->set_reveal_child(open_by_default);
    card.frame->pack_start(*reveal, Gtk::PACK_EXPAND_WIDGET);
    section.body = reveal;

    /* ── Quand recaler la fenetre : a la FIN de l'animation ──────────────
     *
     * Un Gtk::Revealer interpole sa hauteur pendant toute la transition. Au
     * moment du clic il annonce donc encore la hauteur d'AVANT le geste :
     * presque rien quand on deplie, tout quand on replie. fit_to_page, meme
     * differe en idle, s'executait dans la premiere milliseconde de ces
     * 140 ms et mesurait une page qui n'avait pas encore bouge : deplier
     * recalait la fenetre sur la hauteur repliee — elle retombait au
     * minimum, pied d'actions compris, et le contenu ouvert se retrouvait
     * derriere le defilement de la colonne — et replier ne la faisait pas
     * redescendre. Une temporisation arbitraire n'aurait fait que parier sur
     * la duree de l'animation.
     *
     * « child-revealed » est notifie quand la position courante atteint sa
     * cible, c'est-a-dire a la fin de la transition, dans les deux sens (et
     * immediatement quand il n'y a pas de transition, revealer non affiche).
     * C'est le seul instant ou la hauteur naturelle de la page est celle
     * qu'on verra.
     */
    reveal->property_child_revealed().signal_changed().connect(
        [this] { fit_to_page(); });

    /* L'en-tete devient cliquable par une EventBox : elle a sa propre fenetre
     * GDK, donc il faut lui poser nous-memes le masque des clics, comme la
     * poignee de la liste des dossiers. */
    auto* grip = Gtk::make_managed<Gtk::EventBox>();
    grip->set_above_child(false);
    grip->set_visible_window(false);
    grip->add_events(Gdk::BUTTON_PRESS_MASK);
    card.frame->remove(*card.head);
    grip->add(*card.head);
    card.frame->pack_start(*grip, Gtk::PACK_SHRINK);
    card.frame->reorder_child(*grip, 0);
    // Le curseur dit que l'en-tete se clique avant qu'on l'essaie.
    grip->signal_realize().connect([grip] {
        if (auto win = grip->get_window())
            win->set_cursor(Gdk::Cursor::create(grip->get_display(), "pointer"));
    });
    grip->signal_button_press_event().connect([this, key](GdkEventButton* ev) {
        if (ev->type != GDK_BUTTON_PRESS || ev->button != 1) return false;
        auto it = m_sections.find(key);
        if (it == m_sections.end() || !it->second.body) return false;
        it->second.body->set_reveal_child(!it->second.body->get_reveal_child());
        refresh_sections();
        // Pas de fit_to_page ICI : voir juste au-dessus, la hauteur de la
        // page ne sera connue qu'une fois l'animation finie.
        return true;
    });

    m_sections[key] = section;
    return card.frame;
}

// Chaque section dit ou elle en est : chevron dans le bon sens, resume
// visible seulement quand il remplace quelque chose.
void SettingsPanel::refresh_sections() {
    for (auto& [key, section] : m_sections) {
        (void)key;
        if (!section.body) continue;
        const bool open = section.body->get_reveal_child();
        if (section.chevron)
            section.chevron->set_file(open ? "bc-chevron-down.svg"
                                           : "bc-chevron-right.svg");
        if (!section.summary) continue;
        const std::string text = (!open && section.describe) ? section.describe() : std::string();
        section.summary->set_text(text);
        section.summary->set_visible(!text.empty());
    }
}

/* ── Les options de MAME ────────────────────────────────────────────────
 *
 * MAME expose des centaines d'options en ligne de commande. On n'en montre
 * que celles qu'un joueur change vraiment, et chacune est verifiee contre
 * `mame -showusage` : une option inventee ne se voit qu'au moment ou le jeu
 * refuse de demarrer.
 *
 * Les oui/non sont ecrits DANS LES DEUX SENS (-keepaspect / -nokeepaspect).
 * MAME lit d'abord mame.ini, que Bootcade n'ecrit pas et ne controle pas :
 * un interrupteur eteint qui n'ajouterait aucun argument laisserait donc
 * gagner le fichier, et l'ecran afficherait un reglage que le jeu ne
 * respecte pas. Les options a valeur, elles, ne s'ajoutent que si on les a
 * allumees : ne rien dire, c'est laisser le choix par defaut de MAME, qui
 * est le bon reglage pour la plupart des machines.
 */
Gtk::Widget* SettingsPanel::build_mame_options() {
    auto* rows = ui::rows();

    // ── Image ────────────────────────────────────────────────────────────
    ui::add_row(rows, *option_row("bc-window.svg", _("Launch games fullscreen"),
                                  _("Start MAME in fullscreen instead of a window."),
                                  m_sw_mame_fullscreen));

    ui::add_row(rows, *option_row("filter-aspect.svg", _("Keep aspect ratio"),
                                  _("Never stretch the picture away from the shape the "
                                    "game was drawn in."),
                                  m_sw_mame_keepaspect));

    ui::add_row(rows, *option_row("bc-image.svg", _("Integer scaling"),
                                  _("Scale by whole pixels only: sharper, with black borders."),
                                  m_sw_mame_intscale));

    fill_combo(m_cb_mame_video, {{"opengl", _("OpenGL")},
                                 {"bgfx",   _("BGFX (shaders)")},
                                 {"accel",  _("Accelerated (SDL)")},
                                 {"soft",   _("Software")}});
    ui::add_row(rows, *option_row("bc-palette.svg", _("Video driver"),
                                  _("How MAME draws the picture. Left off, it picks one itself."),
                                  m_sw_mame_video, &m_cb_mame_video));

    ui::add_row(rows, *option_row("bc-sync.svg", _("Wait for vertical sync"),
                                  _("Flips the picture at the start of a screen refresh: "
                                    "no tearing, slightly more input lag."),
                                  m_sw_mame_vsync));

    // ── Son ──────────────────────────────────────────────────────────────
    fill_combo(m_cb_mame_sound, {{"sdl",       _("SDL")},
                                 {"pulse",     _("PulseAudio")},
                                 {"portaudio", _("PortAudio")},
                                 {"none",      _("No sound")}});
    ui::add_row(rows, *option_row("bc-system.svg", _("Sound driver"),
                                  _("Which audio backend MAME plays through."),
                                  m_sw_mame_sound, &m_cb_mame_sound));

    // En decibels, comme MAME les compte : 0 est le maximum, jamais le silence.
    fill_combo(m_cb_mame_volume, {{"0",   _("0 dB (full)")},
                                  {"-3",  _("-3 dB")},
                                  {"-6",  _("-6 dB")},
                                  {"-9",  _("-9 dB")},
                                  {"-12", _("-12 dB")},
                                  {"-20", _("-20 dB")},
                                  {"-30", _("-30 dB (quiet)")}});
    ui::add_row(rows, *option_row("bc-chart.svg", _("Volume"),
                                  _("Attenuates MAME's own output, in decibels."),
                                  m_sw_mame_volume, &m_cb_mame_volume));

    // ── Confort de jeu ───────────────────────────────────────────────────
    ui::add_row(rows, *option_row("bc-info.svg", _("Skip the information screen"),
                                  _("Go straight into the game instead of the warning "
                                    "and system information screen."),
                                  m_sw_mame_skipinfo));

    ui::add_row(rows, *option_row("bc-trophy.svg", _("High score plugin"),
                                  _("Lets MAME save and restore the arcade high score tables."),
                                  m_sw_mame_hiscore));

    ui::add_row(rows, *option_row("bc-buttons.svg", _("Autofire plugin"),
                                  _("Adds MAME's autofire menu, configured per game in its "
                                    "own menu."),
                                  m_sw_mame_autofire));

    ui::add_row(rows, *option_row("bc-save.svg", _("Automatic save state"),
                                  _("Restores the machine where you left it, on the drivers "
                                    "that support it."),
                                  m_sw_mame_autosave));

    // ── Captures d'ecran ─────────────────────────────────────────────────
    auto* snap_slot = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 6);
    /* Ni hexpand, ni largeur naturelle par defaut.
     *
     * hexpand se propage aux parents : le champ rendait extensible la boite
     * d'accroche de toute la ligne, qui cessait alors d'etre poussee a
     * droite, et l'interrupteur de cette ligne-la se retrouvait cinquante
     * pixels a gauche de tous les autres. Le champ occupe l'emplacement
     * reserve par PACK_EXPAND_WIDGET, qui ne demande rien aux parents. */
    m_entry_mame_snapdir.set_width_chars(8);
    m_entry_mame_snapdir.set_placeholder_text(_("/path/to/snaps"));
    snap_slot->pack_start(m_entry_mame_snapdir, Gtk::PACK_EXPAND_WIDGET);
    m_button_browse_snapdir.set_image(*ui::image("bc-folder.svg", ui::kIconButton));
    m_button_browse_snapdir.set_always_show_image(true);
    m_button_browse_snapdir.set_tooltip_text(_("Browse..."));
    m_button_browse_snapdir.signal_clicked().connect(
        [this] { on_folder_clicked(&m_entry_mame_snapdir); });
    snap_slot->pack_start(m_button_browse_snapdir, Gtk::PACK_SHRINK);
    ui::add_row(rows, *option_row("bc-folder.svg", _("Screenshot folder"),
                                  _("Where MAME writes the pictures it takes."),
                                  m_sw_mame_snapdir, snap_slot));

    // %g = le nom du jeu, %i = un numero : les deux seuls jetons que MAME
    // remplace. Une liste plutot qu'un champ libre, sans quoi un modele mal
    // ecrit produit des fichiers qu'on ne retrouve plus.
    fill_combo(m_cb_mame_snapname, {{"%g/%i",    _("One folder per game")},
                                    {"%g_%i",    _("Game name and number")},
                                    {"%g/%g_%i", _("Folder, then full name")}});
    ui::add_row(rows, *option_row("bc-file.svg", _("Screenshot naming"),
                                  _("How each picture file is named."),
                                  m_sw_mame_snapname, &m_cb_mame_snapname));

    // ── Le reste, a la main ──────────────────────────────────────────────
    // Meme presentation et meme place que pour FinalBurn Neo : les options
    // au-dessus couvrent l'usage courant, pas les centaines d'autres.
    m_entry_mame_args.set_size_request(ui::kFieldWidth, -1);
    m_entry_mame_args.set_placeholder_text(_("e.g. -bgfx_screen_chains crt-geom"));
    m_entry_mame_args.set_tooltip_text(
        _("Passed to MAME before the machine name, separated by spaces."));
    ui::add_row(rows, *ui::row("bc-sliders.svg",
                               _("Additional command line arguments"),
                               _("Extra arguments added to every game launch."),
                               &m_entry_mame_args));

    // Un selecteur n'a de sens qu'une fois son option allumee : il suit donc
    // son interrupteur, maintenant et a chaque bascule.
    for (auto* sw : {&m_sw_mame_video, &m_sw_mame_sound, &m_sw_mame_volume,
                     &m_sw_mame_snapdir, &m_sw_mame_snapname})
        sw->property_active().signal_changed().connect(
            sigc::mem_fun(*this, &SettingsPanel::sync_mame_option_sensitivity));
    sync_mame_option_sensitivity();
    return rows;
}

void SettingsPanel::sync_mame_option_sensitivity() {
    m_cb_mame_video.set_sensitive(m_sw_mame_video.get_active());
    m_cb_mame_sound.set_sensitive(m_sw_mame_sound.get_active());
    m_cb_mame_volume.set_sensitive(m_sw_mame_volume.get_active());
    const bool snapdir = m_sw_mame_snapdir.get_active();
    m_entry_mame_snapdir.set_sensitive(snapdir);
    m_button_browse_snapdir.set_sensitive(snapdir);
    m_cb_mame_snapname.set_sensitive(m_sw_mame_snapname.get_active());
}

/* La ligne de commande que ces interrupteurs decrivent.
 *
 * Chaque argument est un element a part : « -volume » et « -6 » ne forment
 * une valeur que pour un shell, et il n'y en a pas ici.
 */
std::vector<std::string> SettingsPanel::mame_launch_args() const {
    std::vector<std::string> args;
    auto flag = [&args](bool on, const char* yes, const char* no) {
        args.emplace_back(on ? yes : no);
    };
    flag(m_sw_mame_fullscreen.get_active(), "-nowindow", "-window");
    flag(m_sw_mame_keepaspect.get_active(), "-keepaspect", "-nokeepaspect");
    // MAME ne connait pas d'option « integer scale » : il connait le droit
    // d'etirer autrement qu'en entier, et on le lui retire.
    flag(m_sw_mame_intscale.get_active(), "-nounevenstretch", "-unevenstretch");
    flag(m_sw_mame_vsync.get_active(), "-waitvsync", "-nowaitvsync");
    flag(m_sw_mame_skipinfo.get_active(), "-skip_gameinfo", "-noskip_gameinfo");
    flag(m_sw_mame_autosave.get_active(), "-autosave", "-noautosave");

    auto valued = [&args](bool on, const char* option, const std::string& value) {
        if (!on || value.empty()) return;
        args.emplace_back(option);
        args.push_back(value);
    };
    valued(m_sw_mame_video.get_active(),  "-video",  m_cb_mame_video.get_active_id());
    valued(m_sw_mame_sound.get_active(),  "-sound",  m_cb_mame_sound.get_active_id());
    valued(m_sw_mame_volume.get_active(), "-volume", m_cb_mame_volume.get_active_id());
    valued(m_sw_mame_snapdir.get_active(), "-snapshot_directory",
           m_entry_mame_snapdir.get_text().raw());
    valued(m_sw_mame_snapname.get_active(), "-snapname",
           m_cb_mame_snapname.get_active_id());

    // Les extensions ne sont pas des drapeaux : MAME attend une liste.
    std::vector<std::string> on_plugins, off_plugins;
    auto plugin = [&](bool active, const char* name) {
        (active ? on_plugins : off_plugins).emplace_back(name);
    };
    plugin(m_sw_mame_hiscore.get_active(),  "hiscore");
    plugin(m_sw_mame_autofire.get_active(), "autofire");
    if (!on_plugins.empty()) {
        args.emplace_back("-plugin");
        args.push_back(join_commas(on_plugins));
    }
    if (!off_plugins.empty()) {
        args.emplace_back("-noplugin");
        args.push_back(join_commas(off_plugins));
    }
    return args;
}

/* Les valeurs par defaut sont celles de MAME lui-meme.
 *
 * Une premiere ouverture de l'ecran ne doit rien changer a la facon dont les
 * jeux se lancaient la veille : la carte decrit d'abord l'existant, et le
 * joueur decide ensuite de s'en ecarter.
 */
void SettingsPanel::load_mame_options(const nlohmann::json& j) {
    nlohmann::json o = nlohmann::json::object();
    if (j.contains("mame_options") && j["mame_options"].is_object())
        o = j["mame_options"];

    m_sw_mame_fullscreen.set_active(o.value("fullscreen",    true));
    m_sw_mame_keepaspect.set_active(o.value("keep_aspect",   true));
    m_sw_mame_intscale.set_active(  o.value("integer_scale", false));
    m_sw_mame_vsync.set_active(     o.value("vsync",         false));
    m_sw_mame_skipinfo.set_active(  o.value("skip_gameinfo", false));
    m_sw_mame_autosave.set_active(  o.value("autosave",      false));
    m_sw_mame_hiscore.set_active(   o.value("plugin_hiscore",  false));
    m_sw_mame_autofire.set_active(  o.value("plugin_autofire", false));

    auto pick = [](Gtk::ComboBoxText& combo, const std::string& id) {
        // Un identifiant qu'une version future de MAME aurait retire laisse
        // la liste sur son premier element plutot que sur du vide.
        if (id.empty() || !combo.set_active_id(id)) combo.set_active(0);
    };
    m_sw_mame_video.set_active(o.value("video", false));
    pick(m_cb_mame_video, o.value("video_driver", std::string()));
    m_sw_mame_sound.set_active(o.value("sound", false));
    pick(m_cb_mame_sound, o.value("sound_driver", std::string()));
    m_sw_mame_volume.set_active(o.value("volume", false));
    pick(m_cb_mame_volume, o.value("volume_db", std::string()));
    m_sw_mame_snapdir.set_active(o.value("snapshot_directory", false));
    m_entry_mame_snapdir.set_text(o.value("snapshot_path", std::string()));
    m_sw_mame_snapname.set_active(o.value("snapname", false));
    pick(m_cb_mame_snapname, o.value("snapname_pattern", std::string()));

    m_entry_mame_args.set_text(j.value("mame_extra_args", std::string()));
    sync_mame_option_sensitivity();
}

void SettingsPanel::save_mame_options(nlohmann::json& j) const {
    nlohmann::json o;
    o["fullscreen"]      = m_sw_mame_fullscreen.get_active();
    o["keep_aspect"]     = m_sw_mame_keepaspect.get_active();
    o["integer_scale"]   = m_sw_mame_intscale.get_active();
    o["vsync"]           = m_sw_mame_vsync.get_active();
    o["skip_gameinfo"]   = m_sw_mame_skipinfo.get_active();
    o["autosave"]        = m_sw_mame_autosave.get_active();
    o["plugin_hiscore"]  = m_sw_mame_hiscore.get_active();
    o["plugin_autofire"] = m_sw_mame_autofire.get_active();
    o["video"]           = m_sw_mame_video.get_active();
    o["video_driver"]    = m_cb_mame_video.get_active_id();
    o["sound"]           = m_sw_mame_sound.get_active();
    o["sound_driver"]    = m_cb_mame_sound.get_active_id();
    o["volume"]          = m_sw_mame_volume.get_active();
    o["volume_db"]       = m_cb_mame_volume.get_active_id();
    o["snapshot_directory"] = m_sw_mame_snapdir.get_active();
    o["snapshot_path"]      = m_entry_mame_snapdir.get_text();
    o["snapname"]           = m_sw_mame_snapname.get_active();
    o["snapname_pattern"]   = m_cb_mame_snapname.get_active_id();
    j["mame_options"] = o;
}

/* Le panneau de droite suit la ligne choisie a gauche.
 *
 * Les deux emulateurs ne se configurent pas de la meme facon : FBNeo est un
 * binaire que Bootcade choisit, telecharge et tient a jour, MAME appartient
 * a la distribution et n'est qu'interroge. Montrer les memes cartes aux deux
 * promettrait des gestes qui n'existent pas pour l'un d'eux.
 */
void SettingsPanel::show_emulator_page(size_t index) {
    const auto& registry = emulator_registry();
    if (registry.empty()) return;
    if (index >= registry.size()) index = 0;
    const EmulatorEntry& entry = registry[index];

    // La liste se remplit avant le panneau, et choisir sa premiere ligne
    // appelle deja ici : il n'y a alors rien a remplir.
    if (!m_emu_head_logo || !m_emu_head_txt) return;

    ui::destroy_children(*m_emu_head_logo);
    ui::destroy_children(*m_emu_head_txt);
    m_emu_head_logo->pack_start(*brand_logo(entry.logo, 86, 50), Gtk::PACK_SHRINK);
    m_emu_head_txt->pack_start(*ui::card_title_label(entry.name), Gtk::PACK_SHRINK);
    m_emu_head_txt->pack_start(*ui::sub_label(_(entry.kind)), Gtk::PACK_SHRINK);
    m_emu_head_txt->pack_start(*ui::sub_label(_(entry.description)), Gtk::PACK_SHRINK);
    m_emu_head_logo->show_all();
    m_emu_head_txt->show_all();

    const bool fbneo = std::string(entry.id) == "fbneo";
    /* Cacher ne suffit pas : le show_all() de la fenetre rallume tout ce qui
     * ne porte pas le drapeau, et montrer a nouveau demande un show_all()
     * puisque les enfants d'un bloc saute n'ont jamais recu leur etat. */
    auto reveal = [](Gtk::Widget* w, bool on) {
        if (!w) return;
        w->set_no_show_all(!on);
        if (on) w->show_all();
        else    w->hide();
    };
    reveal(m_emu_exe_frame,  fbneo);
    reveal(m_emu_stats_row,  fbneo);
    reveal(m_emu_mame_stats_row, !fbneo);
    // Les deux emulateurs se verifient : le bouton ne depend plus de celui
    // qui est affiche, seul son verdict en depend.
    reveal(m_emu_upd_row,    true);
    reveal(m_emu_mame_frame, !fbneo);
    // La carte « Options » reste, son contenu change : les reglages de FBNeo
    // s'affichaient jusqu'ici sous MAME, ou ils ne commandaient rien.
    reveal(m_emu_opt_fbneo,  fbneo);
    reveal(m_emu_opt_mame,  !fbneo);
    // La pastille du bandeau est unique : elle ne peut dire l'etat du bon
    // emulateur que si on lui dit lequel est a l'ecran. Pour MAME, c'est
    // elle qui declenche la sonde, et la carte y lit sa reponse.
    m_emu_shown_mame = !fbneo;
    // Le verdict affiche parle de l'emulateur qu'on vient de quitter : le
    // laisser le ferait lire comme celui du nouveau.
    m_lbl_emu_note.hide();
    refresh_emulator_pill();
    // Les resumes des sections repliees parlent de l'emulateur affiche :
    // « 3 options on » n'est pas le meme chiffre d'un emulateur a l'autre.
    refresh_sections();
    // Les deux fiches n'ont pas la meme hauteur : sans cela, la fenetre garde
    // celle de la precedente, trop grande ou trop petite.
    fit_to_page();
}

/* Quel MAME, et dans quel etat.
 *
 * La carte annoncait « Bootcade utilise le MAME de votre distribution » sans
 * jamais nommer le binaire ni dire s'il repondait : devant un jeu qui ne se
 * lance pas, c'etait la seule question qui comptait, et elle restait sans
 * reponse. Le champ prime toujours sur la detection, et l'ecran le dit.
 */
std::string SettingsPanel::mame_executable() const {
    const std::string typed = m_entry_mame_exe.get_text();
    if (!typed.empty()) return typed;
    // La detection ne coute qu'un access() dans les cas courants : la garder
    // evite quand meme de la refaire a chaque rafraichissement de la carte.
    if (!m_mame_probed) {
        m_mame_probed = true;
        m_mame_exe = MameCatalog::find_executable();
    }
    return m_mame_exe;
}

/* ── catver.ini ───────────────────────────────────────────────────────────
 *
 * Trois etats, et trois phrases qui ne disent que ce qui est vrai :
 * aucun fichier (les machines MAME n'ont pas de genre, et c'est normal),
 * un fichier lu (combien de machines il a classees), un fichier illisible.
 */
void SettingsPanel::refresh_catver_state() {
    const std::string chosen = m_entry_catver.get_text();
    const std::string used   = MameCatalog::catver_path(chosen);
    const int         loaded = m_database ? m_database->countMameGenres() : 0;

    if (used.empty()) {
        m_lbl_catver_state.set_text(
            _("No catver.ini yet: MAME machines have no genre, so they do not "
              "show up under the Genre filter. Download it, or point Bootcade "
              "at a copy you already have."));
    } else if (loaded > 0) {
        m_lbl_catver_state.set_text(Glib::ustring::compose(
            _("%1 machines classified from %2"), loaded, used));
    } else {
        m_lbl_catver_state.set_text(Glib::ustring::compose(
            _("%1 was read but classified no machine."), used));
    }
    m_lbl_catver_state.set_tooltip_text(used);

    /* Le champ d'adresse montre la valeur par defaut plutot qu'un vide :
     * le joueur voit d'ou vient le fichier et n'a qu'a corriger ce qui ne
     * lui convient pas. save_to_file la reconnait et ne la fige pas. */
    if (m_entry_catver_url.get_text().empty())
        m_entry_catver_url.set_text(catver_url_default());

    // Sans adresse (ni saisie, ni constructible faute de version de MAME),
    // les deux boutons s'eteignent plutot que de partir chercher un numero
    // invente.
    const bool have_url = !catver_url_in_use().empty();
    m_btn_download_catver.set_sensitive(have_url);
    m_btn_check_catver.set_sensitive(have_url);
}

std::string SettingsPanel::catver_url_default() const {
    const std::string exe = mame_executable();
    const bool ready = !exe.empty() && ::access(exe.c_str(), X_OK) == 0;
    const std::string version =
        ready ? MameCatalog::version_number(MameCatalog::installed_build(exe))
              : std::string();
    return MameCatalog::catver_url(version);
}

std::string SettingsPanel::catver_url_in_use() const {
    const std::string typed = m_entry_catver_url.get_text();
    return typed.empty() ? catver_url_default() : typed;
}

void SettingsPanel::check_catver_update_async() {
    const std::string url   = catver_url_in_use();
    const std::string local = MameCatalog::catver_path(m_entry_catver.get_text());
    if (url.empty()) return;

    m_btn_check_catver.set_sensitive(false);
    m_lbl_catver_check.set_text(_("Checking for a newer catver.ini..."));
    m_lbl_catver_check.show();

    std::thread([this, alive = m_alive, url, local] {
        const auto c = MameCatalog::check_catver(url, local);
        const bool has_file = !local.empty();
        auto ver = [](int v) { return "0." + std::to_string(v); };

        std::string msg, found;
        if (c.error == "no version number in the address") {
            msg = _("This address carries no version number, so Bootcade cannot "
                    "tell whether a newer catver.ini exists.");
        } else if (!c.asked) {
            msg = _("Could not reach the server: Bootcade cannot tell whether a "
                    "newer catver.ini exists.");
        } else if (c.local >= 0 && c.newest > c.local) {
            msg = Glib::ustring::compose(
                _("catver.ini %1 is available, you have %2. Press Download to get it."),
                ver(c.newest), ver(c.local));
            found = c.url;
        } else if (c.local >= 0) {
            // On dit jusqu'ou l'on a regarde : « a jour » ne vaut que pour
            // ce que le serveur a bien voulu dire.
            msg = Glib::ustring::compose(
                _("catver.ini %1 is the newest published: %2 is not out yet."),
                ver(c.local), ver(c.local + 1));
        } else if (c.newest >= 0) {
            msg = Glib::ustring::compose(
                has_file ? _("The catver.ini in use does not state its version. "
                             "The newest published is %1.")
                         : _("No catver.ini here yet. The newest published is %1."),
                ver(c.newest));
            found = c.url;
        } else {
            msg = has_file
                ? std::string(_("The catver.ini in use does not state its version, "
                                "so Bootcade cannot compare it. Nothing newer than "
                                "this address is published."))
                : std::string(_("No catver.ini here yet. Nothing newer than this "
                                "address is published: Download gets it."));
        }
        {
            std::lock_guard<std::mutex> lock(m_catver_mutex);
            m_catver_msg       = msg;
            m_catver_found_url = found;
        }
        std::lock_guard<std::mutex> live(alive->mutex);
        if (alive->alive) m_catver_done.emit();
    }).detach();
}

/* ── Les sources d'images ─────────────────────────────────────────────────
 *
 * Une ligne par source : le champ d'adresse, puis monter, descendre,
 * retirer. Des boutons a pictogramme seuls, avec infobulle : trois libelles
 * en toutes lettres mangeraient la place du champ a la largeur minimale de
 * la fenetre, et c'est l'adresse qu'on doit pouvoir lire.
 */
void SettingsPanel::art_sources_rebuild() {
    ui::destroy_children(m_art_sources_list);

    const int count = static_cast<int>(m_art_sources.size());
    for (int i = 0; i < count; ++i) {
        auto* line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 6);
        line->get_style_context()->add_class("set-row");

        // Le rang dit l'ordre d'essai sans qu'il faille l'expliquer.
        auto* rank = ui::sub_label(std::to_string(i + 1) + ".");
        rank->set_width_chars(3);
        rank->set_valign(Gtk::ALIGN_CENTER);
        line->pack_start(*rank, Gtk::PACK_SHRINK);

        auto* entry = Gtk::make_managed<Gtk::Entry>();
        entry->set_width_chars(8);
        entry->set_hexpand(true);
        entry->set_valign(Gtk::ALIGN_CENTER);
        entry->set_placeholder_text("https://…/");
        entry->set_text(m_art_sources[i]);
        entry->set_tooltip_text(m_art_sources[i]);
        entry->signal_changed().connect([this, i, entry] {
            if (i < static_cast<int>(m_art_sources.size())) {
                m_art_sources[i] = entry->get_text();
                entry->set_tooltip_text(entry->get_text());
            }
        });
        line->pack_start(*entry, Gtk::PACK_EXPAND_WIDGET);

        auto icon_button = [](const std::string& icon, const std::string& tip) {
            auto* b = Gtk::make_managed<Gtk::Button>();
            b->set_image(*ui::image(icon, ui::kIconButton));
            b->set_always_show_image(true);
            b->set_tooltip_text(tip);
            b->set_valign(Gtk::ALIGN_CENTER);
            return b;
        };
        auto* up = icon_button("bc-up.svg", _("Try this source earlier"));
        up->set_sensitive(i > 0);
        up->signal_clicked().connect([this, i] {
            if (i <= 0 || i >= static_cast<int>(m_art_sources.size())) return;
            std::swap(m_art_sources[i], m_art_sources[i - 1]);
            // Differe : on detruirait le bouton pendant qu'il emet.
            Glib::signal_idle().connect_once([this] { art_sources_rebuild(); });
        });
        auto* down = icon_button("bc-down.svg", _("Try this source later"));
        down->set_sensitive(i + 1 < count);
        down->signal_clicked().connect([this, i] {
            if (i + 1 >= static_cast<int>(m_art_sources.size())) return;
            std::swap(m_art_sources[i], m_art_sources[i + 1]);
            Glib::signal_idle().connect_once([this] { art_sources_rebuild(); });
        });
        auto* del = icon_button("bc-trash.svg", _("Remove this source"));
        del->signal_clicked().connect([this, i] {
            if (i >= static_cast<int>(m_art_sources.size())) return;
            m_art_sources.erase(m_art_sources.begin() + i);
            Glib::signal_idle().connect_once([this] { art_sources_rebuild(); });
        });
        line->pack_start(*up,   Gtk::PACK_SHRINK);
        line->pack_start(*down, Gtk::PACK_SHRINK);
        line->pack_start(*del,  Gtk::PACK_SHRINK);

        m_art_sources_list.append(*line);
    }
    m_art_sources_list.show_all();
}

void SettingsPanel::apply_catver_now(bool announce) {
    if (!m_database) return;
    const std::string used = MameCatalog::catver_path(m_entry_catver.get_text());
    if (used.empty()) { refresh_catver_state(); return; }

    const auto r = MameCatalog::apply_catver(m_database, used);
    refresh_catver_state();
    if (!announce) return;

    auto* win = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (!win) return;
    if (r.ok)
        // Le compte des machines JOUABLES, pas celui des lignes ecrites :
        // catver.ini classe aussi les pieces internes, que personne ne voit
        // dans la bibliotheque. Annoncer 50 368 sous une phrase d'etat qui en
        // dit 43 050 ferait douter des deux.
        ui::notice(*win, _("Genres loaded"),
                   Glib::ustring::compose(
                       _("%1 MAME machines now have a genre. Restart Bootcade to see "
                         "them under the Genre filter."),
                       m_database->countMameGenres()),
                   "bc-check.svg");
    else
        ui::notice(*win, _("Could not read catver.ini"), r.error, "bc-error.svg");
}

void SettingsPanel::download_catver_clicked() {
    auto* win = dynamic_cast<Gtk::Window*>(get_toplevel());

    const std::string url = catver_url_in_use();
    if (url.empty()) {
        // L'archive porte le numero de version dans son nom. Sans version on
        // ne peut que le dire : en deviner un ramenerait le classement d'une
        // autre version de MAME.
        if (win) ui::notice(*win, _("Which version of MAME?"),
                            _("Bootcade could not read the version of MAME installed "
                              "here, and the file to download is named after it. Set a "
                              "working MAME executable first."),
                            "bc-info.svg");
        return;
    }

    /* Le telechargement bloque la fenetre le temps de 900 Ko.
     *
     * Un fil et une barre de progression coutent ici plus qu'ils ne
     * rapportent : le fichier tient en une seconde sur une liaison ordinaire,
     * et l'ecran des reglages n'a rien d'autre a faire pendant ce temps. Le
     * bouton s'eteint, le curseur change, et on rend la main.
     */
    m_btn_download_catver.set_sensitive(false);
    m_lbl_catver_state.set_text(_("Downloading catver.ini..."));
    while (Gtk::Main::events_pending()) Gtk::Main::iteration(false);

    const auto dl = MameCatalog::download_catver(
        url, AppContext::get_user_config_dir());

    if (!dl.ok) {
        refresh_catver_state();
        if (win) ui::notice(*win, _("Download failed"), dl.error, "bc-error.svg");
        return;
    }

    // Le champ reste le reglage de l'utilisateur : on n'y ecrit le chemin de
    // notre copie que s'il n'a rien designe lui-meme.
    if (m_entry_catver.get_text().empty()) m_entry_catver.set_text(dl.path);
    apply_catver_now(true);
}

void SettingsPanel::refresh_mame_state() {
    const bool        chosen = !m_entry_mame_exe.get_text().empty();
    const std::string exe    = mame_executable();
    const bool        ready  = !exe.empty() && ::access(exe.c_str(), X_OK) == 0;

    // L'indication sous le champ repond a « et si je laisse vide ? ».
    if (chosen)
        m_lbl_mame_exe.set_text(
            _("Clear this field to let Bootcade detect MAME again."));
    else if (exe.empty())
        m_lbl_mame_exe.set_text(
            _("No MAME was detected. Install it with your package manager, or "
              "point Bootcade at a MAME binary above."));
    else
        m_lbl_mame_exe.set_text(Glib::ustring::compose(
            _("Left empty, Bootcade uses the MAME it found: %1"), exe));

    // Le verdict, dans les mots exacts de la carte de FinalBurn Neo : deux
    // emulateurs dans le meme ecran ne peuvent pas dire la meme chose de deux
    // facons differentes.
    auto state = m_mame_exe_state_text.get_style_context();
    if (ready) {
        m_mame_exe_state_icon.set_file("bc-detected.svg");
        m_mame_exe_state_text.set_text(_("Executable found and working."));
        state->remove_class("set-err");
        state->add_class("set-ok");
    } else {
        m_mame_exe_state_icon.set_file("bc-info.svg");
        m_mame_exe_state_text.set_text(exe.empty()
            ? std::string(_("No executable selected yet."))
            : std::string(_("This path is not an executable Bootcade can run.")));
        state->remove_class("set-ok");
        state->add_class("set-err");
    }

    /* Les tuiles du bandeau decrivent LE binaire retenu, quel qu'il soit :
     * celui que le joueur a designe, sinon celui qu'on a trouve. On
     * n'interroge le binaire que s'il repond.
     *
     * Le numero seul, pas la sortie brute : `mame -version` rend
     * « 0.289 (unknown) », ou le mot entre parentheses est l'identifiant de
     * revision que les paquets ne renseignent pas. Il se lit comme une panne
     * alors que tout va bien, d'ou l'infobulle pour qui veut la sortie
     * exacte. */
    const std::string build = ready ? MameCatalog::installed_build(exe) : std::string();
    m_lbl_mame_build.set_text(build.empty() ? std::string(_("Unknown"))
                                            : MameCatalog::version_number(build));
    m_lbl_mame_build.set_tooltip_text(build);
    m_lbl_mame_path.set_text(exe.empty() ? std::string("—") : exe);
    if (!exe.empty()) m_lbl_mame_path.set_tooltip_text(exe);
    const std::string date = file_date(exe);
    m_lbl_mame_date.set_text(date.empty() ? "—" : date);

    // La date de la derniere verification est un fait enregistre, pas un
    // affichage : elle survit a la fermeture de la fenetre.
    nlohmann::json j;
    { std::ifstream fi(AppContext::get_config_path()); if (fi) { try { fi >> j; } catch (...) {} } }
    const std::string checked = j.value("mame_checked_at", std::string());
    m_lbl_mame_checked.set_text(checked.empty() ? std::string(_("Never")) : checked);

    /* « Les mises a jour viennent de votre distribution » n'est vraie que
     * d'un MAME installe en paquet. Devant un binaire que le joueur a
     * compile ou depose lui-meme, c'est LUI qui le met a jour : la ligne est
     * cachee plutot que de laisser l'ecran affirmer quelque chose de faux. */
    refresh_catver_state();

    if (m_mame_distro_rows) {
        const bool packaged = exe.rfind("/usr/", 0) == 0;
        m_mame_distro_rows->set_no_show_all(!packaged);
        if (packaged) m_mame_distro_rows->show_all();
        else          m_mame_distro_rows->hide();
    }
}

/* La pastille du bandeau parle de l'emulateur AFFICHE.
 *
 * Elle lisait le seul chemin de FinalBurn Neo : la page MAME annoncait donc
 * « Not configured » devant un MAME parfaitement installe, et « Active »
 * devant un MAME absent des que FBNeo etait la. Un verdict faux sur un etat
 * verifiable est pire que pas de verdict du tout.
 */
void SettingsPanel::refresh_emulator_pill() {
    bool ready = false;
    if (m_emu_shown_mame) {
        refresh_mame_state();
        const std::string exe = mame_executable();
        ready = !exe.empty() && ::access(exe.c_str(), X_OK) == 0;
    } else {
        const std::string exe = get_fbneo_executable();
        ready = !exe.empty() && ::access(exe.c_str(), X_OK) == 0;
    }
    // « Not installed » plutot que « Not configured » : MAME ne se regle pas
    // dans Bootcade, et envoyer chercher un reglage qui n'existe pas ferait
    // perdre plus de temps que de ne rien dire.
    m_emu_head_text.set_text(ready ? _("Active")
                                   : (m_emu_shown_mame ? _("Not installed")
                                                       : _("Not configured")));
    for (auto* w : {static_cast<Gtk::Widget*>(&m_emu_head_dot),
                    static_cast<Gtk::Widget*>(&m_emu_head_text),
                    static_cast<Gtk::Widget*>(&m_emu_head_pill)}) {
        auto c = w->get_style_context();
        c->remove_class("set-ok");
        c->remove_class("set-off");
        c->add_class(ready ? "set-ok" : "set-off");
    }
}

void SettingsPanel::set_launch_flags(bool fullscreen, bool integerscale) {
    // Pose sans emettre : appele par la fenetre principale quand SON menu a
    // bouge. Reemettre ferait revenir le signal a son emetteur.
    m_suppress_appearance_signals = true;
    m_switch_fullscreen.set_active(fullscreen);
    m_switch_integerscale.set_active(integerscale);
    m_suppress_appearance_signals = false;
}

void SettingsPanel::check_emulator_update_async() {
    std::thread([this, alive = m_alive] {
        auto r = FbneoUpdateCheck::fetch_latest();
        std::string known;
        {
            nlohmann::json j;
            std::ifstream fi(AppContext::get_config_path());
            if (fi) { try { fi >> j; } catch (...) {} }
            known = j.value("fbneo_release_sha", std::string());
        }
        {
            std::lock_guard<std::mutex> lock(m_emu_mutex);
            if (!r.ok) {
                m_emu_update_msg  = _("Could not reach the release server.");
                m_emu_update_tone = "warn";
            } else if (known.empty()) {
                // Rien a comparer : le binaire configure n'a pas ete telecharge
                // par le lanceur. Le dire est plus honnete que d'annoncer une
                // mise a jour dont on ne sait rien.
                m_emu_update_msg  = _("No baseline recorded: download through Bootcade to track updates.");
                m_emu_update_tone = "muted";
            } else if (known != r.sha) {
                m_emu_update_msg  = _("A new FinalBurn Neo build is available.");
                m_emu_update_tone = "warn";
            } else {
                m_emu_update_msg  = _("FinalBurn Neo is up to date.");
                m_emu_update_tone = "ok";
            }
        }
        // La date de verification est un fait, pas un affichage : elle est
        // ecrite pour que la tuile la retrouve au prochain lancement.
        if (r.ok) {
            nlohmann::json j;
            const std::string path = AppContext::get_config_path();
            { std::ifstream fi(path); if (fi) { try { fi >> j; } catch (...) {} } }
            j["fbneo_checked_at"] = today_iso();
            std::ofstream fo(path);
            if (fo) fo << j.dump(4);
        }
        std::lock_guard<std::mutex> live(alive->mutex);
        if (alive->alive) m_emu_update_done.emit();
    }).detach();
}

/* La meme verification pour MAME, avec ce qu'elle ne peut PAS promettre.
 *
 * MAMEdev ne publie de binaires que pour Windows : sous Linux, il n'y a que
 * les sources et ce que la distribution en fait. Bootcade peut donc dire
 * qu'une version plus recente existe et ou la chercher, jamais l'installer :
 * proposer un telechargement comme pour FinalBurn Neo serait une promesse
 * qu'aucun bouton ne pourrait tenir.
 */
void SettingsPanel::check_mame_update_async() {
    // La version installee se lit AVANT de partir : elle demande le binaire,
    // donc un widget, et un fil detache n'a rien a faire dans les widgets.
    const std::string exe = mame_executable();
    const bool ready = !exe.empty() && ::access(exe.c_str(), X_OK) == 0;
    const std::string installed =
        ready ? MameCatalog::version_number(MameCatalog::installed_build(exe))
              : std::string();

    std::thread([this, alive = m_alive, installed] {
        const auto r = MameCatalog::fetch_latest_release();
        {
            std::lock_guard<std::mutex> lock(m_emu_mutex);
            if (!r.ok) {
                // Une page qui a change de forme ne doit surtout pas se
                // traduire par « vous etes a jour » : on n'en sait rien.
                m_emu_update_msg  = _("Could not read the latest version from mamedev.org.");
                m_emu_update_tone = "warn";
            } else if (installed.empty()) {
                m_emu_update_msg  = Glib::ustring::compose(
                    _("MAME %1 is the latest release. Bootcade could not read the version installed here."),
                    r.version);
                m_emu_update_tone = "muted";
            } else if (MameCatalog::compare_versions(installed, r.version) < 0) {
                m_emu_update_msg  = Glib::ustring::compose(
                    _("MAME %1 is out. Update it with your package manager, or build it from mamedev.org: there is no official Linux download."),
                    r.version);
                m_emu_update_tone = "warn";
            } else {
                m_emu_update_msg  = Glib::ustring::compose(_("MAME %1 is up to date."),
                                                           installed);
                m_emu_update_tone = "ok";
            }
        }
        if (r.ok) {
            nlohmann::json j;
            const std::string path = AppContext::get_config_path();
            { std::ifstream fi(path); if (fi) { try { fi >> j; } catch (...) {} } }
            j["mame_checked_at"] = today_iso();
            std::ofstream fo(path);
            if (fo) fo << j.dump(4);
        }
        std::lock_guard<std::mutex> live(alive->mutex);
        if (alive->alive) m_emu_update_done.emit();
    }).detach();
}

/* Pose la hauteur exacte de la liste, dans le BON ORDRE.
 *
 * GTK refuse un minimum superieur au maximum en vigueur, et inversement :
 * poser les deux dans un ordre fixe echouait donc une fois sur deux, en
 * agrandissant ou en retrecissant selon l'ordre choisi. L'assertion sautait,
 * le minimum n'etait pas applique, et la fenetre ne suivait pas la poignee.
 * Relacher la contrainte d'abord rend les deux affectations toujours valides.
 */
void SettingsPanel::apply_roms_list_height() {
    m_scrolled_roms.set_min_content_height(-1);
    m_scrolled_roms.set_max_content_height(m_roms_list_height);
    m_scrolled_roms.set_min_content_height(m_roms_list_height);
}

/* Regle la hauteur de la liste. La fenetre se recale ensuite, en ABSOLU.
 *
 * Elle grandissait auparavant de la difference a chaque evenement de souris,
 * a partir d'une taille relue chez GTK : or un redimensionnement est
 * asynchrone, la taille relue etait donc celle d'avant, et l'erreur
 * s'additionnait a chaque pixel de deplacement. Au bout d'un glisser la
 * fenetre depassait de plusieurs centaines de pixels la hauteur de son
 * contenu : l'enorme bande vide.
 *
 * fit_to_page ne calcule pas de difference : il redemande la hauteur
 * MINIMALE de la page. Rien ne s'accumule, et tirer vers le haut retrecit la
 * fenetre aussi bien que tirer vers le bas l'agrandit.
 */
void SettingsPanel::set_roms_list_height(int height) {
    static constexpr int kMinHeight = 120;   // trois lignes : en dessous ce
                                             // n'est plus une liste
    if (height < kMinHeight)        height = kMinHeight;
    if (height > m_grip_max_height) height = m_grip_max_height;
    if (height == m_roms_list_height) return;

    m_roms_list_height = height;
    apply_roms_list_height();
    fit_to_page();
}

/* Recale la fenetre sur la hauteur de la page affichee.
 *
 * La hauteur est CALCULEE, pas devinee. On a d'abord essaye de redemander
 * 1 px en comptant sur GTK pour remonter au minimum : il ne le fait pas. Une
 * fenetre GTK3 redimensionnable accepte d'etre plus petite que le minimum de
 * son contenu, qu'elle se contente alors de rogner. La fenetre restait donc
 * a la hauteur de la premiere page ouverte, quelle que soit la page affichee
 * ensuite, et la liste agrandie a la poignee etait comprimee sans que rien ne
 * bouge : exactement le symptome constate.
 *
 * On demande donc la hauteur NATURELLE du panneau, pas la minimale : c'est
 * celle a laquelle rien n'est comprime, et c'est tout l'objet d'un ecran qui
 * ne defile pas.
 *
 * SANS la barre de titre : gtk_window_resize compte deja la barre posee par
 * set_titlebar. L'ajouter faisait une fenetre trop haute d'exactement une
 * barre, d'ou la bande vide au-dessus du pied, sur les quatre pages.
 *
 * Differe en BASSE priorite : au moment du clic la nouvelle page n'a pas
 * encore negocie sa taille, et a l'ouverture la fenetre n'est meme pas encore
 * affichee.
 */
void SettingsPanel::fit_to_page() {
    // Une seule en attente : un glisser emet des dizaines d'evenements, et
    // autant de redimensionnements empiles se marcheraient dessus.
    m_fit_conn.disconnect();
    m_fit_conn = Glib::signal_idle().connect([this] {
        auto* win = dynamic_cast<Gtk::Window*>(get_toplevel());
        if (win) {
            int panel_min = 0, panel_nat = 0;
            get_preferred_height(panel_min, panel_nat);
            int w = 0, h = 0;
            win->get_size(w, h);
            /* Jamais plus haut que l'ecran.
             *
             * La hauteur naturelle d'une page peut depasser la zone de
             * travail (la fiche MAME et ses options), et une fenetre plus
             * haute que l'ecran met son pied d'actions hors de portee : on
             * ne peut alors plus ni annuler ni enregistrer. Ce qui deborde
             * defile, c'est le role du volet de la page. */
            int limit = 0;
            if (auto gdkwin = win->get_window()) {
                if (auto monitor = win->get_display()->get_monitor_at_window(gdkwin)) {
                    Gdk::Rectangle work;
                    monitor->get_workarea(work);
                    limit = work.get_height();
                }
            }
            // Repli : sans zone de travail connue (fenetre pas encore
            // realisee, serveur sans gestionnaire), la hauteur de l'ecran
            // reste une borne bien meilleure que pas de borne du tout.
            if (limit <= 0 && win->get_screen()) limit = win->get_screen()->get_height();
            int wanted = panel_nat;
            if (limit > 0 && wanted > limit) wanted = limit;
            if (wanted > 0 && wanted != h) win->resize(w, wanted);
        }
        return false;           // une seule fois
    }, Glib::PRIORITY_LOW);
}

void SettingsPanel::refresh_emulator_state() {
    const std::string exe = get_fbneo_executable();
    const bool ready = !exe.empty() && ::access(exe.c_str(), X_OK) == 0;

    m_emu_status_text.set_text(ready ? _("Active") : _("Not configured"));
    refresh_emulator_pill();
    auto pill = m_emu_status_pill.get_style_context();
    pill->remove_class("set-ok");
    pill->remove_class("set-err");
    pill->add_class(ready ? "set-ok" : "set-err");
    auto txt = m_emu_status_text.get_style_context();
    txt->remove_class("set-ok");
    txt->remove_class("set-sub");
    txt->add_class(ready ? "set-ok" : "set-sub");

    if (ready) {
        m_exe_state_icon.set_file("bc-detected.svg");
        m_exe_state_text.set_text(_("Executable found and working."));
        m_exe_state_text.get_style_context()->remove_class("set-err");
        m_exe_state_text.get_style_context()->add_class("set-ok");
    } else {
        m_exe_state_icon.set_file("bc-info.svg");
        m_exe_state_text.set_text(exe.empty()
            ? std::string(_("No executable selected yet."))
            : std::string(_("This path is not an executable Bootcade can run.")));
        m_exe_state_text.get_style_context()->remove_class("set-ok");
        m_exe_state_text.get_style_context()->add_class("set-err");
    }

    nlohmann::json j;
    { std::ifstream fi(AppContext::get_config_path()); if (fi) { try { fi >> j; } catch (...) {} } }
    // « Build » : l'empreinte du commit que le lanceur a telecharge. C'est la
    // seule identite de version que cette distribution de FBNeo expose ; en
    // inventer un numero serait plus lisible et faux.
    std::string sha = j.value("fbneo_release_sha", std::string());
    m_lbl_emu_version.set_text(sha.empty() ? std::string(_("Unknown"))
                                           : sha.substr(0, 7));
    const std::string installed = file_date(exe);
    m_lbl_emu_systems.set_text(installed.empty() ? "—" : installed);
    const std::string checked = j.value("fbneo_checked_at", std::string());
    m_lbl_emu_checked.set_text(checked.empty() ? std::string(_("Never")) : checked);
}

// ─────────────────────────────────────────────────────────────────────────
//  Online
// ─────────────────────────────────────────────────────────────────────────

Gtk::Widget* SettingsPanel::build_page_online() {
    auto* page = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL,
                                             ui::kCardSpacing);
    page->get_style_context()->add_class("set-page");

    auto* left = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL,
                                             ui::kCardSpacing);

    // ── Compte ───────────────────────────────────────────────────────────
    // Ne pas avoir de compte est un ETAT NORMAL, pas une erreur : Bootcade se
    // joue entierement hors ligne. La carte ne porte donc ni avertissement ni
    // couleur d'alerte quand personne n'est connecte.
    auto account = ui::card("bc-account.svg", _("Account"),
                            _("Sign in to access Bootcade online features and "
                              "manage your profile."));
    m_account_row.get_style_context()->add_class("cc-subcard");
    m_account_name.set_xalign(0.0f);
    m_account_sub.set_xalign(0.0f);
    m_account_text.set_valign(Gtk::ALIGN_CENTER);
    m_account_text.pack_start(m_account_name, Gtk::PACK_SHRINK);
    m_account_text.pack_start(m_account_sub,  Gtk::PACK_SHRINK);
    m_account_avatar.set_valign(Gtk::ALIGN_CENTER);
    m_account_row.pack_start(m_account_avatar, Gtk::PACK_SHRINK);
    m_account_row.pack_start(m_account_text,   Gtk::PACK_EXPAND_WIDGET);

    auto* acc_buttons = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 9);
    acc_buttons->set_valign(Gtk::ALIGN_CENTER);
    m_button_manage_account.set_label(_("Manage Account"));
    m_button_manage_account.set_image(*ui::image("bc-account-manage.svg", ui::kIconButton));
    m_button_manage_account.set_always_show_image(true);
    m_button_manage_account.signal_clicked().connect([this] {
        /* Le COMPTE, pas le profil : ce sont deux choses differentes.
         *
         * Le profil est la vitrine publique sur le site : pseudo, avatar,
         * scores. Le compte est ce que gere Keycloak : mot de passe, adresse
         * de courriel, double authentification, sessions ouvertes, suppression
         * du compte. « Manage Account » doit mener la, et nulle part ailleurs.
         */
        auto* win = dynamic_cast<Gtk::Window*>(get_toplevel());
        const std::string url = BootcadeAuth::account_console_url();
        gtk_show_uri_on_window(win ? GTK_WINDOW(win->gobj()) : nullptr,
                               url.c_str(), GDK_CURRENT_TIME, nullptr);
    });
    acc_buttons->pack_start(m_button_manage_account, Gtk::PACK_SHRINK);

    m_button_account.signal_clicked().connect([this] {
        if (BootcadeAuth::signed_in()) {
            BootcadeAuth::sign_out();
        } else {
            auto* win = dynamic_cast<Gtk::Window*>(get_toplevel());
            if (!win) return;
            LoginDialog dlg(*win);
            dlg.run();
        }
        refresh_account_row();
        m_sig_account_changed.emit();
    });
    acc_buttons->pack_start(m_button_account, Gtk::PACK_SHRINK);
    m_account_row.pack_start(*acc_buttons, Gtk::PACK_SHRINK);
    account.body->pack_start(m_account_row, Gtk::PACK_SHRINK);
    left->pack_start(*account.frame, Gtk::PACK_SHRINK);

    // ── Fonctions en ligne ───────────────────────────────────────────────
    auto features = ui::card("bc-globe.svg", _("Online Features"),
                             _("Configure Bootcade's online services."));
    auto* feat_rows = ui::rows();
    m_switch_hiscore.set_active(false);
    m_switch_hiscore.set_valign(Gtk::ALIGN_CENTER);
    m_switch_hiscore.set_tooltip_text(
        _("On, your scores and play time are published to the leaderboard. Off, no badge is shown and the launcher never contacts the service."));
    m_switch_hiscore.property_active().signal_changed().connect([this] {
        if (!m_suppress_appearance_signals)
            m_sig_hiscore_toggled.emit(m_switch_hiscore.get_active());
    });
    ui::add_row(feat_rows, *ui::row("bc-trophy.svg", _("Online highscores"),
                                    _("Publish your scores and use Bootcade leaderboards."),
                                    &m_switch_hiscore));

    /* Les trois reglages suivants commandent chacun quelque chose de precis.
     *
     * Ils ne sont pas trois nuances du meme interrupteur : le premier decide
     * si l'on TELECHARGE les classements des autres, le deuxieme QUAND on
     * synchronise, le troisieme ce qu'on JOINT a un score envoye. On peut
     * vouloir publier ses scores sans rapatrier ceux du monde entier, ou
     * publier un score sans annoncer combien d'heures on y a passe.
     */
    m_switch_community.set_valign(Gtk::ALIGN_CENTER);
    m_switch_community.set_active(true);
    m_switch_community.set_tooltip_text(
        _("Off, Bootcade never downloads global leaderboards; your own scores are still published."));
    ui::add_row(feat_rows, *ui::row("bc-users.svg", _("Community features"),
                                    _("Access global leaderboards and player rankings."),
                                    &m_switch_community));

    m_switch_autosync.set_valign(Gtk::ALIGN_CENTER);
    m_switch_autosync.set_active(true);
    m_switch_autosync.set_tooltip_text(
        _("Off, scores stay queued until you sync from the account menu. Nothing is ever lost."));
    ui::add_row(feat_rows, *ui::row("bc-cloud.svg", _("Automatic sync"),
                                    _("Send queued scores and refresh leaderboards at startup."),
                                    &m_switch_autosync));

    m_switch_playstats.set_valign(Gtk::ALIGN_CENTER);
    m_switch_playstats.set_active(true);
    m_switch_playstats.set_tooltip_text(
        _("Off, your scores are still published, but without how long you played."));
    ui::add_row(feat_rows, *ui::row("bc-chart.svg", _("Share play statistics"),
                                    _("Include your play time alongside a published score."),
                                    &m_switch_playstats));

    m_hiscore_hint.set_xalign(0.0f);
    m_hiscore_hint.get_style_context()->add_class("set-sub");
    m_hiscore_hint.set_margin_start(14);
    m_hiscore_hint.set_margin_top(8);

    // Le pays reste ici : il accompagne le score publie, et c'est le seul
    // endroit ou un joueur sans compte peut le choisir.
    m_entry_hiscore_country.set_placeholder_text(_("start typing, e.g. France"));
    m_entry_hiscore_country.set_size_request(ui::kFieldWidth, -1);
    build_country_completion();
    ui::add_row(feat_rows, *ui::row("bc-globe.svg", _("Country"),
                                    _("Shown next to your score on the leaderboard."),
                                    &m_entry_hiscore_country));
    features.body->pack_start(*feat_rows, Gtk::PACK_SHRINK);
    features.body->pack_start(m_hiscore_hint, Gtk::PACK_SHRINK);
    left->pack_start(*features.frame, Gtk::PACK_SHRINK);

    // ── Etat du reseau ───────────────────────────────────────────────────
    /* NET est INDEPENDANT du compte et des classements.
     *
     * Il vient de la sonde de joignabilite et d'elle seule : une reponse HTTP,
     * quel qu'en soit le code, prouve que le service repond. Le deduire de
     * l'authentification afficherait « hors ligne » a tout joueur sans compte,
     * sur une machine parfaitement connectee.
     */
    auto network = ui::card("bc-wifi.svg", _("Network Status"),
                            _("Current connection status to Bootcade services."));
    m_net_row.get_style_context()->add_class("cc-subcard");
    m_net_dot.get_style_context()->add_class("set-dot");
    m_net_dot.set_valign(Gtk::ALIGN_CENTER);
    m_net_row.pack_start(m_net_dot, Gtk::PACK_SHRINK);
    auto* net_txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 2);
    net_txt->set_valign(Gtk::ALIGN_CENTER);
    m_net_title.set_xalign(0.0f);
    m_net_title.get_style_context()->add_class("set-row-title");
    m_net_sub.set_xalign(0.0f);
    m_net_sub.get_style_context()->add_class("set-sub");
    net_txt->pack_start(m_net_title, Gtk::PACK_SHRINK);
    net_txt->pack_start(m_net_sub,   Gtk::PACK_SHRINK);
    m_net_row.pack_start(*net_txt, Gtk::PACK_EXPAND_WIDGET);
    m_btn_test_net.set_label(_("Test Connection"));
    m_btn_test_net.set_image(*ui::image("bc-wifi.svg", ui::kIconButton));
    m_btn_test_net.set_always_show_image(true);
    m_btn_test_net.signal_clicked().connect([this] { probe_network_async(); });
    m_net_row.pack_start(m_btn_test_net, Gtk::PACK_SHRINK);
    network.body->pack_start(m_net_row, Gtk::PACK_SHRINK);
    /* La DERNIERE carte de la colonne absorbe la place restante.
     *
     * Sans cela la colonne s'arrete a la hauteur de son contenu tandis que la
     * carte d'en face, seule dans sa colonne, descend jusqu'en bas : les deux
     * colonnes se terminaient a des hauteurs differentes. C'est la derniere
     * carte qui s'etire, pas les intervalles entre les cartes, sinon la page
     * se disloquerait au lieu de s'aligner.
     */
    left->pack_start(*network.frame, Gtk::PACK_EXPAND_WIDGET);
    page->pack_start(*left, Gtk::PACK_EXPAND_WIDGET);
    set_network_state(0);

    // ── Colonne de droite : le profil ────────────────────────────────────
    auto profile = ui::card("bc-trophy.svg", _("Your Profile"),
                            _("Your Bootcade identity, as the launcher knows it."));
    profile.frame->set_size_request(360, -1);

    m_profile_avatar.set_halign(Gtk::ALIGN_CENTER);
    m_profile_signed.pack_start(m_profile_avatar, Gtk::PACK_SHRINK);
    m_profile_name.get_style_context()->add_class("set-card-title");
    m_profile_name.set_halign(Gtk::ALIGN_CENTER);
    m_profile_signed.pack_start(m_profile_name, Gtk::PACK_SHRINK);
    m_profile_country.get_style_context()->add_class("set-sub");
    m_profile_country.set_halign(Gtk::ALIGN_CENTER);
    m_profile_signed.pack_start(m_profile_country, Gtk::PACK_SHRINK);
    m_profile_dot.get_style_context()->add_class("set-dot");
    m_profile_dot.set_valign(Gtk::ALIGN_CENTER);
    m_profile_state.set_halign(Gtk::ALIGN_CENTER);
    m_profile_state.pack_start(m_profile_dot, Gtk::PACK_SHRINK);
    m_profile_state.pack_start(m_profile_state_text, Gtk::PACK_SHRINK);
    m_profile_signed.pack_start(m_profile_state, Gtk::PACK_SHRINK);
    m_profile_signed.pack_start(*ui::hairline(), Gtk::PACK_SHRINK);

    /* Trois compteurs, tous LOCAUX.
     *
     * Les classements personnels viennent du cache que la couche de scores
     * tient deja ; les jeux joues et les favoris, de la base. Aucun n'est
     * demande au serveur : la carte doit s'afficher hors ligne, et un
     * compteur qui disparait quand le reseau tombe ressemble a une perte de
     * donnees.
     */
    auto* stats_row = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    stats_row->set_homogeneous(true);
    auto stat = [](const std::string& icon, Gtk::Label& value,
                   const std::string& label) {
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 4);
        box->get_style_context()->add_class("cc-subcard");
        box->pack_start(*ui::image(icon, 20), Gtk::PACK_SHRINK);
        value.set_halign(Gtk::ALIGN_CENTER);
        value.get_style_context()->add_class("set-stat-value");
        box->pack_start(value, Gtk::PACK_SHRINK);
        auto* l = ui::sub_label(label);
        l->set_halign(Gtk::ALIGN_CENTER);
        l->set_xalign(0.5f);
        box->pack_start(*l, Gtk::PACK_SHRINK);
        return box;
    };
    stats_row->pack_start(*stat("bc-trophy.svg", m_stat_hiscores, _("Highscores")),
                          Gtk::PACK_EXPAND_WIDGET);
    stats_row->pack_start(*stat("bc-controller.svg", m_stat_played, _("Games played")),
                          Gtk::PACK_EXPAND_WIDGET);
    stats_row->pack_start(*stat("star-gold.svg", m_stat_favorites, _("Favorites")),
                          Gtk::PACK_EXPAND_WIDGET);
    m_profile_signed.pack_start(*stats_row, Gtk::PACK_SHRINK);
    // Le seul compteur que le lanceur detient VRAIMENT : ce qui attend d'etre
    // envoye. Le service n'expose ni succes ni nombre de parties, et les
    // dessiner quand meme ferait de cette carte une vitrine.
    m_profile_queued.set_halign(Gtk::ALIGN_CENTER);
    m_profile_queued.get_style_context()->add_class("set-sub");
    m_profile_signed.pack_start(m_profile_queued, Gtk::PACK_SHRINK);
    m_btn_view_profile.set_label(_("View full profile"));
    m_btn_view_profile.set_image(*ui::image("bc-external.svg", ui::kIconButton));
    m_btn_view_profile.set_always_show_image(true);
    m_btn_view_profile.signal_clicked().connect([this] {
        auto* win = dynamic_cast<Gtk::Window*>(get_toplevel());
        std::string url = "https://bootcade.netlify.app/profile/";
        if (BootcadeAuth::signed_in()) url += "?sso=1";
        gtk_show_uri_on_window(win ? GTK_WINDOW(win->gobj()) : nullptr,
                               url.c_str(), GDK_CURRENT_TIME, nullptr);
    });
    m_profile_signed.pack_start(m_btn_view_profile, Gtk::PACK_SHRINK);
    profile.body->pack_start(m_profile_signed, Gtk::PACK_SHRINK);

    // Sans compte, la carte explique ce qu'un compte apporte : elle n'annonce
    // pas un probleme, parce qu'il n'y en a pas.
    m_profile_empty.pack_start(*ui::tile("bc-account.svg", 26, 54), Gtk::PACK_SHRINK);
    auto* empty_title = ui::title_label(_("No account"));
    empty_title->set_halign(Gtk::ALIGN_CENTER);
    m_profile_empty.pack_start(*empty_title, Gtk::PACK_SHRINK);
    auto* empty_sub = ui::sub_label(
        _("Bootcade works fully offline. Sign in only if you want your scores "
          "to appear on the online leaderboards."));
    empty_sub->set_justify(Gtk::JUSTIFY_CENTER);
    empty_sub->set_xalign(0.5f);
    empty_sub->set_max_width_chars(34);
    m_profile_empty.pack_start(*empty_sub, Gtk::PACK_SHRINK);
    profile.body->pack_start(m_profile_empty, Gtk::PACK_SHRINK);

    page->pack_start(*profile.frame, Gtk::PACK_SHRINK);
    return page;
}

void SettingsPanel::set_network_state(int state) {
    static const char* kTones[] = {"set-sub", "set-ok", "set-err"};
    m_net_title.set_text(state == 1 ? _("Connected")
                       : state == 2 ? _("Offline")
                                    : _("Checking…"));
    m_net_sub.set_text(state == 1 ? _("Bootcade services are reachable.")
                     : state == 2 ? _("Bootcade works normally; online features wait.")
                                  : _("Testing the connection to Bootcade services."));
    for (auto* w : {static_cast<Gtk::Widget*>(&m_net_dot),
                    static_cast<Gtk::Widget*>(&m_net_title)}) {
        auto ctx = w->get_style_context();
        for (const char* c : kTones) ctx->remove_class(c);
        ctx->add_class(kTones[state < 0 || state > 2 ? 0 : state]);
    }
    // Le profil affiche le MEME etat reseau : deux pastilles contradictoires
    // dans la meme page ont deja ete un bug, on ne le refait pas.
    auto pctx = m_profile_dot.get_style_context();
    for (const char* c : kTones) pctx->remove_class(c);
    pctx->add_class(kTones[state < 0 || state > 2 ? 0 : state]);
    m_profile_state_text.set_text(state == 1 ? _("Online") : _("Offline"));
}

void SettingsPanel::probe_network_async() {
    m_btn_test_net.set_sensitive(false);
    set_network_state(0);
    std::thread([this, alive = m_alive] {
        m_net_state.store(HiscoreClient::probe_reachable() ? 1 : 2);
        std::lock_guard<std::mutex> live(alive->mutex);
        if (alive->alive) m_net_done.emit();
    }).detach();
}

void SettingsPanel::refresh_profile_stats() {
    // Les classements personnels sont un cache local : ils restent lisibles
    // hors ligne, ce qui est precisement le cas ou l'on veut encore savoir ou
    // l'on en est.
    m_stat_hiscores.set_text(
        std::to_string(HiscoreClient::cached_personal_ranks().size()));
    if (m_database) {
        m_stat_played.set_text(std::to_string(m_database->countPlayedGames()));
        m_stat_favorites.set_text(std::to_string(m_database->countFavorites()));
    } else {
        // Sans base, on n'affiche pas zero : zero est une affirmation, et
        // celle-ci serait fausse.
        m_stat_played.set_text("—");
        m_stat_favorites.set_text("—");
    }
}

void SettingsPanel::on_window_shown() {
    // Ce que la fenetre principale a pu changer pendant que l'ecran etait
    // ferme : la session, l'emulateur telecharge depuis un menu.
    // MAME a pu etre installe entre deux ouvertures : la reponse gardee vaut
    // le temps d'une visite, pas celui de la session. Oubliee AVANT le reste,
    // pour que la pastille et la carte relisent un verdict frais.
    m_mame_probed = false;
    refresh_account_row();
    refresh_emulator_state();
    refresh_roms_list();
    refresh_profile_stats();
    // A l'ouverture aussi : la fenetre est creee avant que les pages aient
    // negocie leur taille, et resterait sur une hauteur d'avance.
    fit_to_page();
    m_btn_test_net.set_sensitive(true);
    probe_network_async();
}

// ─────────────────────────────────────────────────────────────────────────
//  Pied : valeurs par defaut, cache, remise a zero
// ─────────────────────────────────────────────────────────────────────────

/* Remet les REGLAGES a leur valeur d'usine, sans toucher aux donnees.
 *
 * Les chemins, la bibliotheque et le compte restent : ce bouton repond a
 * « j'ai bricole trois options et je ne sais plus lesquelles », pas a
 * « efface tout ». La remise a zero complete, elle, a son propre bouton et sa
 * propre confirmation dans Data & Storage.
 */
void SettingsPanel::apply_defaults() {
    m_suppress_appearance_signals = true;
    m_combo_theme.set_active_id("dark");
    m_combo_language.set_active_id("");
    m_suppress_appearance_signals = false;
    // Emis explicitement : le theme et la langue doivent suivre tout de suite,
    // sinon l'ecran annonce des valeurs que l'application n'applique pas.
    m_sig_theme_changed.emit(m_combo_theme.get_active_id());
    m_sig_language_changed.emit(m_combo_language.get_active_id());

    m_combo_startup.set_active_id("last_played");
    m_switch_window_state.set_active(true);
    m_switch_play_history.set_active(true);
    m_switch_auto_update.set_active(true);
    m_check_recursive.set_active(true);
    m_check_loose_files.set_active(true);
    // Les options de balayage sont propres a chaque emulateur : ne remettre
    // que celles de l'entree affichee laisserait les autres sur un reglage
    // que l'ecran annonce pourtant comme revenu a son defaut.
    for (auto& [id, lib] : m_library) {
        (void)id;
        lib.scan_recursive   = true;
        lib.scan_loose_files = true;
    }
    m_switch_community.set_active(true);
    m_switch_autosync.set_active(true);
    m_switch_playstats.set_active(true);
    m_switch_fullscreen.set_active(false);
    m_switch_integerscale.set_active(false);
    m_entry_emu_args.set_text("");
    // Les options MAME repartent sur le comportement par defaut de MAME
    // lui-meme : l'objet vide suffit a le dire, load_mame_options connait
    // deja ces valeurs. Le chemin de l'executable n'en fait pas partie : ce
    // bouton remet des OPTIONS, il ne debranche pas un emulateur.
    load_mame_options(nlohmann::json::object());
    m_entry_mame_args.set_text("");
    // Le menu « Launch » de la fenetre principale doit suivre : deux endroits
    // qui affichent le meme reglage ne doivent jamais diverger.
    m_sig_launch_options.emit();
}

void SettingsPanel::on_restore_defaults_clicked() {
    auto* win = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (!win) return;
    ConfirmationDialog dlg(
        *win, _("Restore default settings?"),
        _("Appearance, startup, behavior, update and scan options go back to "
          "their default values.\n\nYour ROM folders, artwork paths, emulator "
          "and account are left untouched."),
        "↺", false);
    if (!dlg.show_and_confirm()) return;
    apply_defaults();
}

void SettingsPanel::on_clear_cache_clicked() {
    auto* win = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (!win) return;

    /* Ce qui est efface est EXACTEMENT ce qui se reconstruit tout seul.
     *
     * Ni games.db (elle porte les favoris et l'historique de jeu, que rien ne
     * regenere), ni la file d'attente des scores (un score gare est du travail
     * du joueur, pas un fichier temporaire).
     */
    const std::string dir = AppContext::get_user_config_dir();
    const std::vector<std::string> files = {
        dir + "/filter_cache.json",
        dir + "/hiscore-cache.json",
        dir + "/debug.log",
    };
    uintmax_t total = 0;
    for (const auto& f : files) {
        std::error_code ec;
        auto size = std::filesystem::file_size(f, ec);
        if (!ec) total += size;
    }
    if (total == 0) {
        ui::notice(*win, _("Nothing to clear."),
                   _("The local cache is already empty."));
        return;
    }

    /* En kilo-octets sous le mega-octet.
     *
     * Un cache de 15 Ko s'annoncait « 0.0 MB », ce qui dit exactement le
     * contraire de ce que fait le bouton : rien a effacer. Un ordre de
     * grandeur faux est pire qu'une unite inhabituelle.
     */
    const Glib::ustring size_text =
        total >= 1024 * 1024
            ? Glib::ustring::format(std::fixed, std::setprecision(1),
                                    double(total) / (1024.0 * 1024.0)) + " MB"
            : Glib::ustring::format(std::max<uintmax_t>(1, total / 1024)) + " kB";
    ConfirmationDialog dlg(
        *win, _("Clear local cache?"),
        Glib::ustring::compose(
            _("Bootcade will delete %1 of cached filters, cached leaderboards "
              "and the debug log.\n\nYour games, favourites, play history and "
              "pending scores are not touched. Everything deleted here is "
              "rebuilt automatically."),
            size_text),
        "bc-clear.svg", true);
    if (!dlg.show_and_confirm()) return;

    for (const auto& f : files) {
        std::error_code ec;
        std::filesystem::remove(f, ec);
    }
    ui::notice(*win, _("Local cache cleared."),
               _("The deleted files are rebuilt automatically as you use Bootcade."),
               "bc-detected.svg");
}

void SettingsPanel::on_reset_settings_clicked() {
    auto* win = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (!win) return;
    ConfirmationDialog dlg(
        *win, _("Reset all settings?"),
        _("Every setting goes back to its default value, including your ROM "
          "folders, artwork paths and emulator path. Bootcade will need to be "
          "configured again.\n\nYour games, favourites and play history stay "
          "where they are, and you stay signed in."),
        "bc-warning.svg", true);
    if (!dlg.show_and_confirm()) return;

    apply_defaults();
    // Toutes les bibliotheques, pas seulement celle qui est a l'ecran : la
    // confirmation annonce « vos dossiers de ROMs », sans distinguer.
    for (auto& [id, lib] : m_library) { (void)id; lib = LibrarySettings{}; }
    set_roms_paths({});
    set_dat_path("");
    set_previews_path("");
    set_titles_path("");
    set_fbneo_executable("");
    refresh_emulator_state();
    // Ecrit tout de suite : une remise a zero confirmee qu'un Cancel annulerait
    // laisserait le joueur croire qu'elle a eu lieu.
    save_to_file(AppContext::get_config_path());
}

// ─────────────────────────────────────────────────────────────────────────
//  Scores en ligne : identite et pays (comportement inchange)
// ─────────────────────────────────────────────────────────────────────────

namespace {
/* La version publiee, sans les metadonnees de construction.
 *
 * BOOTCADE_VERSION vaut « 1.3.2 » pour une release mais « 1.3.2+4.gabc123.dirty »
 * dans un arbre de travail : comparer la chaine entiere reposerait la question
 * a chaque recompilation pendant le developpement, et jamais deux fois de
 * suite la meme. Seule la partie SemVer compte ici.
 */
std::string released_version() {
#ifdef BOOTCADE_VERSION
    /* Ne garder que « majeur.mineur.correctif ».
     *
     * Un arbre de travail produit « 1.3.2+4.gabc123.dirty », et un arbre pose
     * sur le tag mais modifie produit « 1.3.2.dirty » : couper au '+' laissait
     * passer le second. La reponse d'un joueur aurait alors ete rattachee a
     * une version qui n'existe pour personne d'autre, et la question reposee
     * a chaque recompilation. On lit donc les trois nombres, et on s'arrete.
     */
    const std::string v = BOOTCADE_VERSION;
    std::string out;
    int parts = 1;
    for (char c : v) {
        if (c >= '0' && c <= '9') { out += c; continue; }
        if (c == '.' && parts < 3 && !out.empty() && out.back() != '.') {
            out += c;
            ++parts;
            continue;
        }
        break;
    }
    return out.empty() ? std::string("0") : out;
#else
    return "0";
#endif
}
}  // namespace


bool SettingsPanel::was_account_asked_this_version() const {
    return m_account_asked_version == released_version();
}


void SettingsPanel::record_account_answer() {
    m_account_asked_version = released_version();
}


std::string SettingsPanel::get_hiscore_player() const {
    return std::string(m_entry_hiscore_player.get_text());
}

void SettingsPanel::ensure_hiscore_identity() {
    // Both are filled only when missing: a name or a country the player has
    // set, in any past version, is never overwritten.
    if (get_hiscore_player().empty())
        m_entry_hiscore_player.set_text(generated_player_name());
    if (get_hiscore_country().empty())
        set_hiscore_country(locale_country_code());
}

void SettingsPanel::record_hiscore_answer(bool publish) {
    m_hiscore_asked = true;
    // Assigned through the switch rather than to a separate flag, so the
    // answer travels the same signal as a later change made in Settings: the
    // badges appear or vanish immediately, with no second code path to keep
    // in step.
    m_switch_hiscore.set_active(publish);
}

bool SettingsPanel::is_hiscore_enabled() const {
    return m_switch_hiscore.get_active();
}

// -> the ISO code, or "" when the field is empty or holds something that is
// not a country. Resolved on read rather than pinned when a completion is
// accepted, so a name typed in full without touching the popup still counts.
std::string SettingsPanel::get_hiscore_country() const {
    std::string text = m_entry_hiscore_country.get_text();
    while (!text.empty() && std::isspace((unsigned char)text.front())) text.erase(text.begin());
    while (!text.empty() && std::isspace((unsigned char)text.back()))  text.pop_back();
    if (text.empty()) return {};

    std::string upper = text;
    for (auto& c : upper) c = std::toupper((unsigned char)c);
    if (upper.size() == 2)
        for (const auto& c : kCountries)
            if (upper == c.code) return c.code;

    // Case-insensitive, and against BOTH the translated name and the English
    // one. The translation is what the field shows, but a config written under
    // another language : or a name typed in English : must still resolve.
    Glib::ustring folded = Glib::ustring(text).lowercase();
    for (const auto& c : kCountries)
        if (folded == Glib::ustring(_(c.name)).lowercase()
         || folded == Glib::ustring(c.name).lowercase())
            return c.code;

    // Typed but unrecognised: no country rather than a guess. A wrong flag on
    // a leaderboard is worse than none, and the field is optional anyway.
    return {};
}

void SettingsPanel::set_hiscore_country(const std::string& code) {
    for (const auto& c : kCountries)
        if (code == c.code) { m_entry_hiscore_country.set_text(_(c.name)); return; }
    m_entry_hiscore_country.set_text("");
}

void SettingsPanel::build_country_completion() {
    m_country_model = Gtk::ListStore::create(m_country_cols);
    // Sorted on the TRANSLATED name: kCountries is alphabetical in English, so
    // a French list would run Afghanistan, Albanie, Algérie… then jump about.
    std::vector<std::pair<std::string, std::string>> sorted;
    sorted.reserve(sizeof(kCountries) / sizeof(kCountries[0]));
    for (const auto& c : kCountries) sorted.emplace_back(_(c.name), c.code);
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) {
                  return g_utf8_collate(a.first.c_str(), b.first.c_str()) < 0;
              });
    for (const auto& [name, code] : sorted) {
        auto row = *m_country_model->append();
        row[m_country_cols.name] = name;
        row[m_country_cols.code] = code;
    }
    m_country_completion = Gtk::EntryCompletion::create();
    m_country_completion->set_model(m_country_model);
    m_country_completion->set_text_column(m_country_cols.name);
    m_country_completion->set_minimum_key_length(1);
    m_country_completion->set_popup_completion(true);
    // No inline completion: it would type ahead of the user inside the entry,
    // and "FR" would become "France" mid-keystroke while they meant "FRO".
    m_country_completion->set_inline_completion(false);

    // Matches a prefix of the name OR the two-letter code, so both "fra" and
    // "fr" find France. GTK hands `key` already folded to lower case.
    m_country_completion->set_match_func(
        [this](const Glib::ustring& key, const Gtk::TreeModel::const_iterator& iter) {
            if (!iter) return false;
            Glib::ustring name = (*iter)[m_country_cols.name];
            Glib::ustring code = (*iter)[m_country_cols.code];
            return name.lowercase().find(key) == 0 || code.lowercase().find(key) == 0;
        });
    m_entry_hiscore_country.set_completion(m_country_completion);

    // Unrecognised text is marked as you type. Without this the field fails
    // silently : a typo simply means no flag, discovered days later on the
    // leaderboard, with nothing on screen to explain it.
    m_entry_hiscore_country.signal_changed().connect([this] {
        bool typed = !m_entry_hiscore_country.get_text().empty();
        bool known = !get_hiscore_country().empty();
        auto ctx = m_entry_hiscore_country.get_style_context();
        if (typed && !known) ctx->add_class("error");
        else                 ctx->remove_class("error");
    });
}

std::string SettingsPanel::get_theme() const {
    auto id = m_combo_theme.get_active_id();
    return id.empty() ? "system" : std::string(id);
}
void SettingsPanel::set_theme(const std::string& mode) {
    m_suppress_appearance_signals = true;
    m_combo_theme.set_active_id(mode.empty() ? "system" : mode);
    m_suppress_appearance_signals = false;
}
std::string SettingsPanel::get_language() const {
    return std::string(m_combo_language.get_active_id());
}
void SettingsPanel::set_language(const std::string& code) {
    m_suppress_appearance_signals = true;
    m_combo_language.set_active_id(code);
    m_suppress_appearance_signals = false;
}

void SettingsPanel::on_folder_clicked(Gtk::Entry* entry) {
    auto dialog = Gtk::FileChooserDialog(_("Select Folder"), Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
    dialog.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dialog.add_button(_("Select"), Gtk::RESPONSE_OK);

    auto current = entry->get_text();
    if (!current.empty()) {
        dialog.set_current_folder(Glib::path_get_dirname(current));
    }

    if (dialog.run() == Gtk::RESPONSE_OK) {
        entry->set_text(dialog.get_filename());
    }
}

/* ── La bibliotheque d'un emulateur ─────────────────────────────────────
 *
 * Les widgets de la page Library editent UNE entree a la fois. Ranger avant
 * de recharger est tout le mecanisme : sans cela, changer de liste deroulante
 * jetterait ce qui vient d'etre saisi.
 */
void SettingsPanel::library_store_current() {
    if (m_library_emu.empty()) return;
    LibrarySettings& lib = m_library[m_library_emu];
    lib.roms_paths       = m_roms_paths;
    lib.previews_path    = m_entry_previews.get_text();
    lib.titles_path      = m_entry_titles.get_text();
    lib.artwork_sources  = m_art_sources;
    lib.scan_recursive   = m_check_recursive.get_active();
    lib.scan_loose_files = m_check_loose_files.get_active();
}

void SettingsPanel::library_show(const std::string& emulator) {
    std::string id = emulator;
    if (id.empty()) id = kDefaultEmulator;

    /* Les onglets se recalent EN PREMIER, et meme quand l'emulateur ne change
     * pas : le clic qui nous amene ici vient de decocher l'onglet courant, et
     * sans cette passe la page n'en afficherait plus aucun d'enfonce.
     * set_active emet « clicked » en GTK3, d'ou le drapeau : sans lui, chaque
     * extinction rentrerait a nouveau dans le gestionnaire. */
    m_library_switching = true;
    for (auto& tab : m_library_tabs) tab.second->set_active(tab.first == id);
    m_library_switching = false;

    if (id == m_library_emu) return;
    library_store_current();
    m_library_emu = id;

    const LibrarySettings& lib = m_library[id];
    m_roms_paths = lib.roms_paths;
    refresh_roms_list();
    m_entry_previews.set_text(lib.previews_path);
    m_entry_titles.set_text(lib.titles_path);
    m_art_sources = lib.artwork_sources;
    art_sources_rebuild();
    m_check_recursive.set_active(lib.scan_recursive);
    m_check_loose_files.set_active(lib.scan_loose_files);

    /* no_show_all se leve avant show_all : GTK ignore show_all sur un
     * widget qui le porte, et la carte ne paraitrait jamais. Il se repose en
     * la cachant, pour que le show_all de la fenetre ne la ramene pas sur
     * l'onglet d'un autre emulateur. */
    if (m_lib_catver_card) {
        const bool mame = id == "mame";
        m_lib_catver_card->set_no_show_all(!mame);
        if (mame) {
            m_lib_catver_card->show_all();
            // La ligne de verdict ne parait qu'une fois une verification faite.
            if (m_lbl_catver_check.get_text().empty()) m_lbl_catver_check.hide();
            refresh_catver_state();
        } else {
            m_lib_catver_card->hide();
        }
    }

    const std::string name = emulator_name(id);
    if (m_lbl_lib_roms_sub)
        m_lbl_lib_roms_sub->set_text(Glib::ustring::compose(
            _("Add the folders that contain your %1 ROMs. Bootcade will scan "
              "these directories for supported games."), name));
    if (m_lbl_lib_art_sub)
        m_lbl_lib_art_sub->set_text(Glib::ustring::compose(
            _("Where Bootcade stores and downloads the preview and title "
              "images of your %1 games."), name));
    if (m_lbl_lib_scan_sub)
        m_lbl_lib_scan_sub->set_text(Glib::ustring::compose(
            _("How Bootcade scans the %1 ROM directories."), name));
}

SettingsPanel::LibrarySettings
SettingsPanel::library_for(const std::string& emulator) const {
    const std::string id = emulator.empty() ? std::string(kDefaultEmulator) : emulator;
    // L'entree affichee vit dans les widgets, pas dans la carte : la lire
    // ailleurs rendrait ce que le joueur vient de taper invisible tant qu'il
    // n'a pas change de liste deroulante.
    if (id == m_library_emu) {
        LibrarySettings lib;
        lib.roms_paths       = m_roms_paths;
        lib.previews_path    = m_entry_previews.get_text();
        lib.titles_path      = m_entry_titles.get_text();
        lib.artwork_sources  = m_art_sources;
        lib.scan_recursive   = m_check_recursive.get_active();
        lib.scan_loose_files = m_check_loose_files.get_active();
        return lib;
    }
    auto it = m_library.find(id);
    return it == m_library.end() ? LibrarySettings{} : it->second;
}

std::vector<std::string> SettingsPanel::get_roms_paths(const std::string& e) const {
    return library_for(e).roms_paths;
}
std::string SettingsPanel::get_previews_path(const std::string& e) const {
    return library_for(e).previews_path;
}
std::string SettingsPanel::get_titles_path(const std::string& e) const {
    return library_for(e).titles_path;
}
bool SettingsPanel::is_scan_recursive(const std::string& e) const {
    return library_for(e).scan_recursive;
}
bool SettingsPanel::is_scan_loose_files(const std::string& e) const {
    return library_for(e).scan_loose_files;
}

// --- Getters ---
std::string SettingsPanel::get_roms_path() const {
    // Deprecated: return first path for compatibility
    const auto paths = get_roms_paths();
    return paths.empty() ? "" : paths[0];
}

std::vector<std::string> SettingsPanel::get_roms_paths() const {
    return library_for(kDefaultEmulator).roms_paths;
}

std::string SettingsPanel::get_dat_path() const { return m_entry_dat.get_text(); }
std::string SettingsPanel::get_previews_path() const { return library_for(kDefaultEmulator).previews_path; }
std::string SettingsPanel::get_outbox_path() const { return m_outbox_path; }
std::string SettingsPanel::get_quarantine_path() const { return m_quarantine_path; }
void SettingsPanel::set_outbox_path(const std::string& path) { m_outbox_path = path; }
void SettingsPanel::set_quarantine_path(const std::string& path) { m_quarantine_path = path; }
std::string SettingsPanel::get_titles_path() const { return library_for(kDefaultEmulator).titles_path; }
bool SettingsPanel::is_scan_recursive()   const { return library_for(kDefaultEmulator).scan_recursive; }
bool SettingsPanel::is_scan_loose_files() const { return library_for(kDefaultEmulator).scan_loose_files; }

/* MAME lit ses dossiers la ou tous les autres lisent les leurs. La liste
 * separee par des points-virgules reste la forme attendue par le lancement :
 * c'est la SOURCE qui change, pas le format. */
std::string SettingsPanel::mame_rompaths() const {
    return join_paths(library_for("mame").roms_paths);
}
void SettingsPanel::set_mame_rompaths(const std::string& v) {
    auto paths = split_paths(v);
    if (m_library_emu == "mame") {
        set_roms_paths(paths);
    } else {
        m_library["mame"].roms_paths = std::move(paths);
    }
}
std::string SettingsPanel::get_fbneo_executable() const { return m_entry_fbneo.get_text(); }

// --- Setters ---
void SettingsPanel::set_roms_path(const std::string& path) {
    // Deprecated: set as first path for compatibility
    if (m_roms_paths.empty()) {
        m_roms_paths.push_back(path);
    } else {
        m_roms_paths[0] = path;
    }
    refresh_roms_list();
}

void SettingsPanel::set_roms_paths(const std::vector<std::string>& paths) {
    m_roms_paths = paths;
    refresh_roms_list();
}

void SettingsPanel::add_roms_path(const std::string& path) {
    m_roms_paths.push_back(path);
    refresh_roms_list();
}

void SettingsPanel::remove_roms_path(int index) {
    if (index >= 0 && index < static_cast<int>(m_roms_paths.size())) {
        m_roms_paths.erase(m_roms_paths.begin() + index);
        refresh_roms_list();
    }
}

void SettingsPanel::set_dat_path(const std::string& path) { m_entry_dat.set_text(path); }
void SettingsPanel::set_previews_path(const std::string& path) { m_entry_previews.set_text(path); }
void SettingsPanel::set_titles_path(const std::string& path) { m_entry_titles.set_text(path); }
void SettingsPanel::set_fbneo_executable(const std::string& path) {
    m_entry_fbneo.set_text(path);
}

// --- Load / Save ---
bool SettingsPanel::load_from_file(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) return false;

    try {
        nlohmann::json j;
        file >> j;

        if (j.contains("dat_path")) set_dat_path(j["dat_path"]);
        if (j.contains("rom_manager") && j["rom_manager"].is_object()) {
            const auto& rm = j["rom_manager"];
            if (rm.contains("outbox_path") && rm["outbox_path"].is_string())         set_outbox_path(rm["outbox_path"]);
            if (rm.contains("quarantine_path") && rm["quarantine_path"].is_string()) set_quarantine_path(rm["quarantine_path"]);
        }

        /* ── La bibliotheque : les cles a plat deviennent celles de FBNeo ──
         *
         * Tout ce qu'un fichier ecrit avant les entrees par emulateur decrit
         * la seule collection que Bootcade connaissait, celle de FinalBurn
         * Neo. Elle est donc reprise TELLE QUELLE sous "emulators"/"fbneo" :
         * un joueur qui a declare vingt dossiers a la main ne doit pas avoir
         * a les redeclarer parce qu'un second emulateur est apparu.
         *
         * Les cles a plat gagnent : "emulators" n'existe pas encore. Une fois
         * qu'il existe, c'est lui qui fait foi, et les cles a plat ne sont
         * plus qu'une copie pour les binaires plus anciens.
         */
        LibrarySettings legacy;
        if (j.contains("roms_paths") && j["roms_paths"].is_array()) {
            for (const auto& path : j["roms_paths"])
                if (path.is_string()) legacy.roms_paths.push_back(path.get<std::string>());
        } else if (j.contains("roms_path") && j["roms_path"].is_string()) {
            legacy.roms_paths.push_back(j["roms_path"].get<std::string>());
        }
        // thumbnails_path : le nom qu'avaient les previsualisations il y a
        // trois versions, et qui dort encore dans des fichiers en service.
        legacy.previews_path    = j.value("previews_path",
                                          j.value("thumbnails_path", std::string()));
        legacy.titles_path      = j.value("titles_path", std::string());
        legacy.scan_recursive   = j.value("scan_recursive", true);
        legacy.scan_loose_files = j.value("scan_loose_files", true);

        m_library.clear();
        m_library_emu.clear();
        const nlohmann::json emus =
            (j.contains("emulators") && j["emulators"].is_object())
                ? j["emulators"] : nlohmann::json::object();
        for (const auto& entry : emulator_registry()) {
            const std::string id = entry.id;
            LibrarySettings lib;
            if (emus.contains(id) && emus.at(id).is_object()) {
                const auto& e = emus.at(id);
                if (e.contains("roms_paths") && e["roms_paths"].is_array())
                    for (const auto& path : e["roms_paths"])
                        if (path.is_string()) lib.roms_paths.push_back(path.get<std::string>());
                lib.previews_path    = e.value("previews_path", std::string());
                lib.titles_path      = e.value("titles_path", std::string());
                lib.scan_recursive   = e.value("scan_recursive", true);
                lib.scan_loose_files = e.value("scan_loose_files", true);
                lib.artwork_sources  = ArtworkSources::defaults_for(id);
                if (e.contains("artwork_sources") && e["artwork_sources"].is_array()) {
                    lib.artwork_sources.clear();
                    for (const auto& v : e["artwork_sources"]) {
                        if (v.is_string())
                            lib.artwork_sources.push_back(v.get<std::string>());
                        else if (v.is_object() && v.contains("url") && v["url"].is_string())
                            lib.artwork_sources.push_back(v["url"].get<std::string>());
                    }
                }
            } else if (id == kDefaultEmulator) {
                lib = legacy;
            } else if (id == "mame") {
                // La carte MAME avait son propre champ de dossiers : il
                // devient les dossiers de l'entree, sans quoi la suppression
                // du champ effacerait ce que le joueur y avait mis.
                lib.roms_paths       = split_paths(j.value("mame_rompaths", std::string()));
                lib.scan_recursive   = legacy.scan_recursive;
                lib.scan_loose_files = legacy.scan_loose_files;
            }
            /* Une entree ecrite avant les sources n'en porte aucune : elle
             * recoit celles d'usine, soit exactement l'adresse que le
             * telechargement utilisait en dur. Rien ne change pour elle tant
             * que le joueur ne touche pas a la liste. */
            if (!(emus.contains(id) && emus.at(id).is_object()))
                lib.artwork_sources = ArtworkSources::defaults_for(id);
            m_library[id] = std::move(lib);
        }
        library_show(kDefaultEmulator);

        if (j.contains("fbneo_executable")) set_fbneo_executable(j["fbneo_executable"]);
        if (j.contains("theme")) set_theme(j["theme"].get<std::string>());
        if (j.contains("language")) set_language(j["language"].get<std::string>());
        if (j.contains("hiscore_player"))
            m_entry_hiscore_player.set_text(j["hiscore_player"].get<std::string>());
        if (j.contains("startup_selection"))
            m_combo_startup.set_active_id(j["startup_selection"].get<std::string>());
        // Absent means off. Opting in has to be a deliberate act, so a config
        // written before this option existed must not switch it on.
        m_switch_hiscore.set_active(j.value("hiscore_enabled", false));
        // Absent means nobody has ever been asked : except on a config that
        // already carries a choice under the old always-on default, whose
        // owner has been publishing for weeks and must not be interrogated
        // about a decision they already live with.
        m_hiscore_asked = j.value("hiscore_asked", j.contains("hiscore_enabled"));
        // Absent d'une configuration ecrite avant les comptes : c'est
        // exactement le cas qu'il faut rattraper, d'ou le defaut vide.
        m_account_asked_version = j.value("hiscore_account_asked_version", std::string());
        if (j.contains("hiscore_country"))
            set_hiscore_country(j["hiscore_country"].get<std::string>());
        // Les trois reglages de comportement. Absents, ils valent « oui » :
        // c'est ce que le lanceur faisait avant qu'ils soient reglables, et
        // une mise a jour ne doit pas eteindre en silence ce qui marchait.
        // La hauteur choisie a la poignee. Relue avant que la fenetre existe,
        // donc posee directement sur le widget : set_roms_list_height
        // voudrait redimensionner une fenetre qui n'est pas encore la.
        m_roms_list_height = j.value("roms_list_height", 200);
        if (m_roms_list_height < 120)  m_roms_list_height = 120;
        if (m_roms_list_height > 1200) m_roms_list_height = 1200;
        apply_roms_list_height();
        m_switch_window_state.set_active(j.value("restore_window_state", true));
        m_switch_play_history.set_active(j.value("keep_play_history", true));
        m_combo_random_from.set_active_id(j.value("random_from", std::string("shown")) == "own" ? "own" : "shown");
        m_switch_random_hiscore.set_active(j.value("random_hiscore_only", false));
        m_switch_random_originals.set_active(j.value("random_originals_only", false));
        m_switch_random_unplayed.set_active(j.value("random_unplayed_only", false));
        m_switch_random_launch.set_active(j.value("random_launch", false));
        m_random_systems_saved.clear();
        if (j.contains("random_systems") && j["random_systems"].is_array())
            for (const auto& v : j["random_systems"]) if (v.is_string()) m_random_systems_saved.insert(v.get<std::string>());
        for (auto* c : m_random_system_checks)
            c->set_active(m_random_systems_saved.empty() || m_random_systems_saved.count(c->get_label()));
        m_switch_auto_update.set_active(j.value("check_updates_auto", true));
        // Les fonctions en ligne : allumees par defaut, parce que c'est ce que
        // le lanceur faisait deja quand les classements etaient actifs. Les
        // eteindre en silence a la premiere mise a jour serait une regression.
        m_switch_community.set_active(j.value("hiscore_community", true));
        m_switch_autosync.set_active(j.value("hiscore_auto_sync", true));
        m_switch_playstats.set_active(j.value("hiscore_share_playtime", true));
        // Options de l'emulateur. Le plein ecran et la mise a l'echelle
        // entiere partagent leurs cles avec le menu « Launch » : ce sont les
        // memes reglages, pas des copies.
        set_launch_flags(j.value("launch_fullscreen", false),
                         j.value("launch_integerscale", false));
        m_entry_emu_args.set_text(j.value("fbneo_extra_args", std::string()));
        m_switch_mechanical.set_active(j.value("mame_show_mechanical", false));
        // Le binaire choisi a la main : vide veut dire « detecte-le », et non
        // « MAME est absent ». La sonde gardee est donc oubliee, sans quoi un
        // chemin lu du fichier ne se verrait qu'au prochain demarrage.
        m_entry_mame_exe.set_text(j.value("mame_executable", std::string()));
        m_mame_probed = false;
        m_entry_catver.set_text(j.value("mame_catver_path", std::string()));
        m_entry_catver_url.set_text(j.value("mame_catver_url", std::string()));
        load_mame_options(j);

        /* L'etat plie / deplie de chaque section de la page Emulator.
         *
         * Il se relit ICI et pas ailleurs : les sections sont baties par le
         * constructeur, donc elles existent deja, et refaire le meme pliage a
         * chaque ouverture serait un reglage qui ne se souvient de rien. */
        if (j.contains("settings_sections") && j["settings_sections"].is_object()) {
            const auto& secs = j["settings_sections"];
            for (auto& [key, section] : m_sections) {
                if (!section.body) continue;
                if (secs.contains(key) && secs.at(key).is_boolean())
                    section.body->set_reveal_child(secs.at(key).get<bool>());
            }
        }
        refresh_sections();
    } catch (...) {
        return false;
    }

    // A relative dat_path/fbneo_executable resolves against whatever directory
    // the process happened to be launched from, which silently differs between
    // a desktop launcher, a terminal, and a dev checkout : leading to reads and
    // writes quietly landing in two unrelated folders across runs. Pin any
    // legacy relative value to an absolute one and persist it immediately so
    // this migration only ever has to happen once.
    //
    // Anchor at $HOME, not the executable's own directory: the Flatpak build
    // runs from a read-only /app/bin, so get_executable_dir() would point
    // somewhere the app can never write to. $HOME is what a bare "./support/…"
    // has always actually resolved to for a normally-launched (desktop/Flatpak)
    // session in practice, since that is the working directory it starts in.
    bool migrated = false;
    const char* home_env = std::getenv("HOME");
    std::string home = home_env ? home_env : ".";
    auto pin_absolute = [&](std::string current, auto setter) {
        if (current.empty() || std::filesystem::path(current).is_absolute()) return;
        std::string absolute = (std::filesystem::path(home) / current).lexically_normal().string();
        (this->*setter)(absolute);
        migrated = true;
    };
    pin_absolute(get_dat_path(), &SettingsPanel::set_dat_path);
    pin_absolute(get_fbneo_executable(), &SettingsPanel::set_fbneo_executable);
    if (migrated) save_to_file(filename);

    refresh_emulator_state();
    refresh_roms_list();
    return true;
}

bool SettingsPanel::save_to_file(const std::string& filename) {
    // Read existing file first to preserve keys written by other parts of the app
    // (e.g. launch_fullscreen, launch_integerscale, controllers)
    nlohmann::json j;
    {
        std::ifstream fi(filename);
        if (fi) { try { fi >> j; } catch (...) { j = nlohmann::json{}; } }
    }

    /* ── Une entree par emulateur ──────────────────────────────────────
     *
     * Les entrees inconnues du registre sont laissees en place : elles
     * appartiennent a une version qui en sait plus que celle-ci, et les
     * effacer ferait perdre au joueur ce qu'une mise a jour lui rendrait.
     */
    library_store_current();
    if (!j.contains("emulators") || !j["emulators"].is_object())
        j["emulators"] = nlohmann::json::object();
    for (const auto& entry : emulator_registry()) {
        const LibrarySettings lib = library_for(entry.id);
        nlohmann::json e;
        e["roms_paths"] = nlohmann::json::array();
        for (const auto& path : lib.roms_paths) e["roms_paths"].push_back(path);
        e["previews_path"]    = lib.previews_path;
        e["titles_path"]      = lib.titles_path;
        e["artwork_sources"]  = nlohmann::json::array();
        for (const auto& url : lib.artwork_sources)
            if (!url.empty()) e["artwork_sources"].push_back(url);
        e["scan_recursive"]   = lib.scan_recursive;
        e["scan_loose_files"] = lib.scan_loose_files;
        j["emulators"][entry.id] = e;
    }

    /* Les cles a plat, ecrites UNE VERSION DE PLUS, avec les valeurs de
     * FinalBurn Neo. Un Bootcade plus ancien — celui d'un paquet pas encore
     * mis a jour, celui d'une machine qui partage la meme configuration —
     * ne connait qu'elles : les retirer tout de suite lui ferait annoncer
     * une bibliotheque vide. */
    const LibrarySettings fbneo = library_for(kDefaultEmulator);
    j["roms_paths"] = j["emulators"][kDefaultEmulator]["roms_paths"];
    j["roms_path"]  = fbneo.roms_paths.empty() ? std::string() : fbneo.roms_paths.front();
    j["previews_path"]    = fbneo.previews_path;
    j["titles_path"]      = fbneo.titles_path;
    j["scan_recursive"]   = fbneo.scan_recursive;
    j["scan_loose_files"] = fbneo.scan_loose_files;

    j["dat_path"] = get_dat_path();
    j["rom_manager"]["outbox_path"]     = get_outbox_path();
    j["rom_manager"]["quarantine_path"] = get_quarantine_path();
    j["fbneo_executable"] = get_fbneo_executable();
    j["theme"] = get_theme();
    j["language"] = get_language();
    j["hiscore_player"] = get_hiscore_player();
    j["startup_selection"] = get_startup_selection();
    j["hiscore_enabled"] = m_switch_hiscore.get_active();
    j["hiscore_asked"] = m_hiscore_asked;
    j["hiscore_account_asked_version"] = m_account_asked_version;
    j["hiscore_country"] = get_hiscore_country();
    j["roms_list_height"]     = m_roms_list_height;
    j["restore_window_state"] = m_switch_window_state.get_active();
    j["keep_play_history"]    = m_switch_play_history.get_active();
    j["random_from"]           = m_combo_random_from.get_active_id() == "own" ? "own" : "shown";
    j["random_hiscore_only"]   = m_switch_random_hiscore.get_active();
    j["random_originals_only"] = m_switch_random_originals.get_active();
    j["random_unplayed_only"]  = m_switch_random_unplayed.get_active();
    j["random_launch"]         = m_switch_random_launch.get_active();
    {
        // Tous coches = liste vide = « tous », pour qu'un systeme ajoute plus
        // tard soit compris sans que le joueur ait a revenir cocher.
        nlohmann::json arr = nlohmann::json::array();
        bool all = true;
        for (auto* c : m_random_system_checks) if (!c->get_active()) { all = false; break; }
        if (!all) for (auto* c : m_random_system_checks) if (c->get_active()) arr.push_back(c->get_label().raw());
        j["random_systems"] = arr;
    }
    j["check_updates_auto"]   = m_switch_auto_update.get_active();
    j["hiscore_community"]      = m_switch_community.get_active();
    j["hiscore_auto_sync"]      = m_switch_autosync.get_active();
    j["hiscore_share_playtime"] = m_switch_playstats.get_active();
    j["launch_fullscreen"]      = m_switch_fullscreen.get_active();
    j["launch_integerscale"]    = m_switch_integerscale.get_active();
    j["fbneo_extra_args"]       = get_emulator_extra_args();
    j["mame_show_mechanical"]   = m_switch_mechanical.get_active();
    // Meme raison que les cles a plat : un binaire plus ancien lit encore
    // celle-ci pour savoir ou MAME range ses ROMs.
    j["mame_rompaths"]          = mame_rompaths();
    // Le chemin est garde TEL QUE SAISI : y ecrire le resultat de la
    // detection figerait dans le fichier un /usr/games/mame qui n'a aucune
    // raison de survivre a un changement de distribution.
    j["mame_executable"]        = m_entry_mame_exe.get_text();
    // Le catver.ini que le joueur a designe. Vide veut dire « celui que
    // Bootcade a telecharge, s'il existe » : MameCatalog::catver_path tranche.
    j["mame_catver_path"]       = m_entry_catver.get_text();
    /* L'adresse n'est ecrite que si elle differe de celle que Bootcade
     * construit : l'enregistrer telle quelle la figerait sur la version de
     * MAME du jour, et la mise a jour suivante irait chercher l'ancien
     * classement. Vide = « suis la version installee ». */
    {
        const std::string typed = m_entry_catver_url.get_text();
        j["mame_catver_url"] = (typed == catver_url_default()) ? std::string() : typed;
    }
    j["mame_extra_args"]        = mame_extra_args();
    save_mame_options(j);
    for (const auto& [key, section] : m_sections)
        if (section.body) j["settings_sections"][key] = section.body->get_reveal_child();
    j["window_width"] = 1000;
    j["window_height"] = 600;

    std::ofstream file(filename);
    if (!file.is_open()) return false;

    file << j.dump(4); // Pretty print with 4 spaces
    return true;
}

// --- New methods for ROMs path management ---
void SettingsPanel::on_add_roms_path_clicked() {
    auto dialog = Gtk::FileChooserDialog(_("Select ROMs Directory"), Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
    dialog.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dialog.add_button(_("Select"), Gtk::RESPONSE_OK);

    if (dialog.run() == Gtk::RESPONSE_OK) {
        std::string path = dialog.get_filename();
        // Check if path already exists
        auto it = std::find(m_roms_paths.begin(), m_roms_paths.end(), path);
        if (it == m_roms_paths.end()) {
            add_roms_path(path);
        }
    }
}

void SettingsPanel::on_remove_roms_path_clicked() {
    auto selection = m_treeview_roms.get_selection();
    auto iter = selection->get_selected();
    if (iter) {
        // Find the index of the selected item
        auto path = m_model_roms->get_path(iter);
        int index = path[0];  // Get the first (and only) index
        remove_roms_path(index);
    }
}

void SettingsPanel::refresh_roms_list() {
    if (!m_model_roms) return;
    m_model_roms->clear();
    // Cell renderers take a pixbuf, not a widget : tint it once with the
    // list's muted ink (a path is text, its folder glyph reads as a label).
    auto folder = IconManager::load_tinted("icons/bc-folder.svg", 18, 18,
                                           SettingsUi::probe_color(*this, "set-sub"));
    for (const auto& path : m_roms_paths) {
        // Un dossier configure mais introuvable est la premiere cause de
        // « mes jeux ont disparu » : le dire dans la liste evite d'aller le
        // chercher dans un journal.
        std::error_code ec;
        const bool present = std::filesystem::is_directory(path, ec);
        auto row = *m_model_roms->append();
        row[m_cols_roms.icon]   = folder;
        row[m_cols_roms.path]   = path;
        row[m_cols_roms.status] = present
            ? Glib::ustring("<span foreground='" + SettingsUi::tone_hex(*this, "success") + "'>● ") + _("Active") + "</span>"
            : Glib::ustring("<span foreground='" + SettingsUi::tone_hex(*this, "error") + "'>● ") + _("Missing") + "</span>";
    }
}

void SettingsPanel::on_download_fbneo_clicked() {
    auto parent_window = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (!parent_window) return;

    // $HOME, not current_path(): the working directory a launch happens to
    // start in is not stable across a desktop icon vs. a terminal vs. a dev
    // checkout, so this could silently extract into a different folder each time.
    const char* home_env = std::getenv("HOME");
    auto download_dialog = std::make_unique<DownloadDialog>(
        *parent_window,
        // finalburnneo/FBNeo stopped maintaining the SDL/Linux build (missing DAT
        // export calls for several systems, upstream declined the fix) : this fork
        // tracks their master daily and carries just that fix. See website FAQ.
        "https://github.com/battousai90/FBNeo/releases/download/latest/linux-sdl2-x86_64.zip",
        home_env ? std::string(home_env) : std::filesystem::current_path().string()
    );

    download_dialog->set_settings_entry(&m_entry_fbneo);
    download_dialog->start_download();
    download_dialog->run();

    // Record what "latest" pointed at just now, so a future startup check has
    // a baseline to notice the next time our fork moves past this build.
    auto r = FbneoUpdateCheck::fetch_latest();
    if (r.ok) {
        nlohmann::json j;
        const std::string path = AppContext::get_config_path();
        { std::ifstream fi(path); if (fi) { try { fi >> j; } catch (...) { j = nlohmann::json{}; } } }
        j["fbneo_release_sha"] = r.sha;
        std::ofstream fo(path);
        if (fo) fo << j.dump(4);
    }
    refresh_emulator_state();
}


void SettingsPanel::on_download_previews_clicked() {
    // Cette méthode sera connectée depuis MainWindow
    // pour avoir accès aux jeux et aux méthodes de progression
    std::cout << "[INFO] Download previews button clicked" << std::endl;
}

void SettingsPanel::on_download_titles_clicked() {
    // Cette méthode sera connectée depuis MainWindow
    // pour avoir accès aux jeux et aux méthodes de progression
    std::cout << "[INFO] Download titles button clicked" << std::endl;
}

void SettingsPanel::refresh_account_row() {
    const bool in = BootcadeAuth::signed_in();

    // Le drapeau se derive du code ISO en deux points de code Unicode :
    // aucune image a embarquer, aucune liste a tenir a jour.
    auto flag_for = [](const std::string& cc) -> Glib::ustring {
        if (cc.size() != 2) return {};
        gunichar a = 0x1F1E6 + (g_ascii_toupper(cc[0]) - 'A');
        gunichar b = 0x1F1E6 + (g_ascii_toupper(cc[1]) - 'A');
        return Glib::ustring(1, a) + Glib::ustring(1, b);
    };

    if (in) {
        // L'avatar choisi, sinon celui par defaut : un joueur qui n'en a pas
        // pris doit quand meme voir une image, pas un trou.
        std::string id = BootcadeAuth::avatar_id();
        if (id.empty()) id = "joystick";
        m_account_avatar.set(IconManager::load("avatars/" + id + ".svg", 52, 52));
        m_account_avatar.show();
        m_profile_avatar.set(IconManager::load("avatars/" + id + ".svg", 96, 96));

        const std::string cc = BootcadeAuth::country();
        Glib::ustring flag = flag_for(cc);

        m_account_name.set_markup(
            "<b>" + Glib::Markup::escape_text(BootcadeAuth::username()) + "</b>"
            + (flag.empty() ? Glib::ustring() : Glib::ustring(" ") + flag));
        m_account_sub.set_markup("<span size='small' alpha='70%'>"
                                 + Glib::Markup::escape_text(_("Signed in")) + "</span>");
        m_button_account.set_label(_("Sign out"));
        m_button_account.set_image(*SettingsUi::image("bc-signout.svg", SettingsUi::kIconButton));
        m_button_account.set_always_show_image(true);
        m_button_account.get_style_context()->add_class("set-danger");

        m_profile_name.set_text(BootcadeAuth::username());
        Glib::ustring country_line;
        for (const auto& c : kCountries)
            if (cc == c.code) { country_line = _(c.name); break; }
        m_profile_country.set_text(flag.empty() ? country_line
                                                : flag + " " + country_line);
        const int queued = HiscoreClient::outbox_size();
        m_profile_queued.set_text(
            queued == 0 ? Glib::ustring(_("No score waiting to be sent."))
                        : Glib::ustring::compose(
                              _("%1 score(s) waiting to be sent."), queued));
        refresh_profile_stats();
        m_profile_signed.show();
        m_profile_empty.hide();
    } else {
        m_account_avatar.hide();
        m_account_name.set_markup("<span alpha='70%'>"
                                  + Glib::Markup::escape_text(_("Not signed in"))
                                  + "</span>");
        /* « Un compte est facultatif » reste vrai pour jouer, et faux des que
         * le joueur a demande a publier ses scores : le service les refuse
         * sans jeton. Dire les deux au meme endroit brouillait le message ;
         * la carte dit donc celui des deux qui correspond a l'etat reel. */
        const int queued = HiscoreClient::outbox_size();
        Glib::ustring sub =
            !is_hiscore_enabled()
                ? Glib::ustring(_("Bootcade works fully offline. An account is optional."))
            : queued > 0
                ? Glib::ustring::compose(
                      _("%1 score(s) waiting: sign in to publish them."), queued)
                : Glib::ustring(_("Publishing your scores needs an account."));
        m_account_sub.set_markup("<span size='small' alpha='70%'>"
                                 + Glib::Markup::escape_text(sub) + "</span>");
        m_button_account.set_label(_("Sign in"));
        m_button_account.set_image(*SettingsUi::image("bc-account.svg", SettingsUi::kIconButton));
        m_button_account.set_always_show_image(true);
        m_button_account.get_style_context()->remove_class("set-danger");

        m_profile_signed.hide();
        m_profile_empty.show();
    }
    // « Manage Account » n'a de sens qu'avec un compte : la page du site
    // renverrait sinon vers un formulaire de connexion.
    m_button_manage_account.set_sensitive(in);

    // L'interrupteur suit la connexion : publier sans compte est impossible
    // depuis que les scores sont rattaches a un compte, et laisser le reglage
    // actif ferait croire le contraire. Connecte, le joueur reste libre de
    // refuser la publication.
    m_switch_hiscore.set_sensitive(in);
    m_hiscore_hint.set_text(in ? "" : _("Sign in to publish your scores."));
    m_hiscore_hint.set_visible(!in);

    // Le pays vient du compte : le laisser modifiable ici ferait croire au
    // joueur qu'il agit sur quelque chose, alors que le serveur prend celui du
    // jeton. Il reste lisible, et modifiable pour qui n'a pas de compte.
    m_entry_hiscore_country.set_sensitive(!in);
    if (in && !BootcadeAuth::country().empty())
        set_hiscore_country(BootcadeAuth::country());
}


// ── Jeu au hasard ─────────────────────────────────────────────────────────
SettingsPanel::RandomPick SettingsPanel::random_pick() const {
    RandomPick r;
    r.from_shown     = m_combo_random_from.get_active_id() != "own";
    r.hiscore_only   = m_switch_random_hiscore.get_active();
    r.originals_only = m_switch_random_originals.get_active();
    r.unplayed_only  = m_switch_random_unplayed.get_active();
    r.launch         = m_switch_random_launch.get_active();
    bool all = true;
    for (auto* c : m_random_system_checks) if (!c->get_active()) { all = false; break; }
    if (!all) for (auto* c : m_random_system_checks) if (c->get_active()) r.systems.insert(c->get_label().raw());
    if (m_random_system_checks.empty()) r.systems = m_random_systems_saved;
    return r;
}

void SettingsPanel::set_random_systems(const std::vector<std::string>& systems) {
    for (auto* c : m_random_system_checks) m_random_systems_box.remove(*c);
    m_random_system_checks.clear();
    for (const auto& sys : systems) {
        auto* c = Gtk::make_managed<Gtk::CheckButton>(sys);
        c->set_active(m_random_systems_saved.empty() || m_random_systems_saved.count(sys));
        m_random_systems_box.add(*c);
        m_random_system_checks.push_back(c);
    }
    m_random_systems_box.show_all_children();
    m_random_systems_box.set_visible(m_combo_random_from.get_active_id() == "own");
}
