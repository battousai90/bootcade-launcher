// src/ControllerDialog.cpp
#include "ControllerDialog.h"
#include "ConfirmationDialog.h"
#include "IconManager.h"
#include "SettingsUi.h"
#include "i18n.h"
#include <iostream>
#include <cctype>
#include <unistd.h>
#include <cmath>
#include <memory>

namespace {
// Le nom que porte la touche sur le clavier du joueur. gdk_keyval_name rend
// un identifiant technique ("Left", "space", "a") : on le rend presentable
// sans le traduire, un nom de touche n'ayant de sens que tel qu'il est grave.
// Le nom que porte, sur le clavier branche, la touche designee par un code de
// l'emulateur. Sert aux prereglages : le code 0x1E est le A d'un QWERTY et le
// Q d'un AZERTY, et l'afficher au hasard tromperait la moitie des joueurs.
std::string key_name_for_code(int fbneo_key) {
    const unsigned keycode = ControllerManager::gtk_keycode_from_fbneo(fbneo_key);
    if (!keycode) return {};
    auto* keymap = gdk_keymap_get_for_display(gdk_display_get_default());
    if (!keymap) return {};
    guint* keyvals = nullptr;
    GdkKeymapKey* keys = nullptr;
    gint count = 0;
    if (!gdk_keymap_get_entries_for_keycode(keymap, keycode, &keys, &keyvals, &count)
        || count <= 0) {
        g_free(keys); g_free(keyvals);
        return {};
    }
    const char* raw = gdk_keyval_name(gdk_keyval_to_upper(keyvals[0]));
    std::string name = raw ? raw : "";
    g_free(keys); g_free(keyvals);
    if (name == "space") name = "Space";
    for (auto& c : name) if (c == '_') c = ' ';
    return name;
}

std::string key_name_from_event(const GdkEventKey* ev) {
    const char* raw = gdk_keyval_name(gdk_keyval_to_upper(ev->keyval));
    if (!raw) return {};
    std::string name = raw;
    if (name == "space") return "Space";
    for (auto& c : name) if (c == '_') c = ' ';
    return name;
}
/* Quelle manette est branchee, et a quoi elle ressemble.
 *
 * Le panneau de droite montrait un pictogramme unique quel que soit
 * l'appareil : il ne disait donc rien. Le pilote Linux donne un nom, et ce
 * nom suffit a reconnaitre la famille, donc la disposition des boutons et le
 * dessin a afficher. L'ordre des tests compte : << Switch 2 >> contient
 * << Switch >>, et une DualSense contient rarement << PlayStation >>.
 */
/* Metriques communes a tout l'ecran.
 *
 * Chaque composant se cale sur ces valeurs plutot que sur une marge posee au
 * cas par cas : c'est la seule facon d'obtenir des lignes de meme hauteur,
 * des pastilles de meme diametre et des colonnes qui tombent en face les unes
 * des autres. Une correction se fait ici, pas dans vingt endroits.
 */
constexpr int kRowHeight   = 38;   // hauteur d'une ligne de controle
constexpr int kCellSize    = 34;   // conteneur carre d'un pictogramme de ligne
constexpr int kCellIcon    = 18;   // pictogramme a l'interieur de ce conteneur
constexpr int kBadgeSize   = 28;   // pastille numerotee des boutons
constexpr int kFieldWidth  = 136;  // largeur d'un champ de liaison
constexpr int kSectionIcon = 20;   // pictogramme d'un titre de section
constexpr int kArtWidth    = 322;  // aire de presentation de l'illustration
constexpr int kArtHeight   = 226;

struct PadFamily {
    const char* photo;    // l'illustration fournie, dans assets/controllers
    const char* alt;      // repli sur la famille voisine, nullptr sinon
    const char* svg;      // trace vectoriel, dernier recours
    const char* family;   // ce que le lanceur a RECONNU, jamais ce qui est choisi
    const char* preset;   // prereglage conseille, nullptr s'il n'y en a pas
};

/* La famille que designe un prereglage choisi a la main.
 *
 * Le nom rendu par le pilote ne dit pas tout : une copie de DualShock 3 se
 * presente comme << USB Gamepad >> et rien ne permet de la reconnaitre. Quand
 * le joueur choisit un prereglage, il dit lui-meme a quoi ressemble sa
 * manette, et l'illustration doit suivre. Le TEXTE, lui, continue d'annoncer
 * le materiel reellement detecte : c'est le dessin qui suit le prereglage,
 * pas l'identite.
 */
static const PadFamily* family_for_preset(const std::string& preset) {
    static const PadFamily kXbox  = {"xbox360.png", nullptr, "bc-pad-xbox360.svg",
                                     N_("Xbox 360 controller"), "Xbox"};
    static const PadFamily kArc   = {"arcade.png", nullptr, "bc-pad-generic.svg",
                                     N_("Arcade stick"), "Arcade"};
    static const PadFamily kPs3   = {"ps3.png", nullptr, "bc-pad-ps4.svg",
                                     N_("DualShock 3 controller"), "PlayStation 3"};
    static const PadFamily kPs4   = {"ps4.png", nullptr, "bc-pad-ps4.svg",
                                     N_("DualShock 4 controller"), "PlayStation 4"};
    static const PadFamily kPs5   = {"ps5.png", nullptr, "bc-pad-ps5.svg",
                                     N_("DualSense controller"), "PlayStation 5"};
    static const PadFamily kSwtch = {"switch.png", nullptr, "bc-pad-switch.svg",
                                     N_("Switch Pro controller"), "Switch Pro"};
    if (preset == "Xbox")          return &kXbox;
    if (preset == "Arcade")        return &kArc;
    if (preset == "PlayStation 3") return &kPs3;
    if (preset == "PlayStation 4") return &kPs4;
    if (preset == "PlayStation 5") return &kPs5;
    if (preset == "Switch Pro")    return &kSwtch;
    return nullptr;   // Keyboard, ou un prereglage sans manette associee
}

static PadFamily pad_family_for(const std::string& raw) {
    std::string n;
    n.reserve(raw.size());
    for (unsigned char c : raw) n += static_cast<char>(std::tolower(c));
    auto has = [&n](const char* k) { return n.find(k) != std::string::npos; };

    if (has("keyboard"))
        return {"keyboard.png", nullptr, "bc-pad-keyboard.svg",
                N_("Keyboard"), "Keyboard"};
    if (has("arcade") || has("fightstick") || has("fight stick")
        || has("hitbox") || has("qanba") || has("mayflash") || has("sanwa"))
        return {"arcade.png", nullptr, "bc-pad-generic.svg",
                N_("Arcade stick"), "Arcade"};
    if (has("joy-con") || has("joycon"))
        return {"joycon.png", "switch.png", "bc-pad-joycon.svg",
                N_("Joy-Con pair"), "Switch Pro"};
    if ((has("xbox") || has("x-box")) && has("360"))
        return {"xbox360.png", nullptr, "bc-pad-xbox360.svg",
                N_("Xbox 360 controller"), "Xbox"};
    if (has("xbox") || has("x-box") || has("xinput"))
        return {"xbox.png", "xbox360.png", "bc-pad-xbox.svg",
                N_("Xbox controller"), "Xbox"};
    if (has("dualshock 3") || has("sixaxis") || has("playstation(r)3")
        || has("playstation 3") || has("ps3"))
        return {"ps3.png", nullptr, "bc-pad-ps4.svg",
                N_("DualShock 3 controller"), "PlayStation 3"};
    if (has("dualsense") || has("ps5"))
        return {"ps5.png", nullptr, "bc-pad-ps5.svg",
                N_("DualSense controller"), "PlayStation 5"};
    if (has("dualshock") || has("ps4") || has("wireless controller"))
        return {"ps4.png", nullptr, "bc-pad-ps4.svg",
                N_("DualShock 4 controller"), "PlayStation 4"};
    if (has("switch 2") || has("switch2"))
        return {"switch2.png", "switch.png", "bc-pad-switch2.svg",
                N_("Switch 2 Pro controller"), "Switch Pro"};
    if (has("switch") || has("nintendo") || has("pro controller"))
        return {"switch.png", nullptr, "bc-pad-switch.svg",
                N_("Switch Pro controller"), "Switch Pro"};

    // Rien de reconnu : on le dit. Deduire une famille d'un prereglage choisi
    // a la main reviendrait a inventer un materiel que personne n'a branche.
    return {"generic.png", nullptr, "bc-pad-generic.svg",
            N_("Generic controller"), nullptr};
}

// Etiquette d'une liaison analogique, telle que le joueur la lit.
static std::string analog_label(const AnalogBinding& b) {
    if (!b.is_set()) return _("Not used");
    if (b.source == AnalogSource::KEY_PAIR) {
        auto shown = [](int code, const std::string& name) {
            return name.empty() ? key_label(code) : name;
        };
        return shown(b.key_neg, b.key_neg_name) + " / " + shown(b.key_pos, b.key_pos_name);
    }
    return Glib::ustring::compose(_("Axis %1"), b.index).raw();
}

}  // namespace

ControllerDialog::ControllerDialog(const std::map<std::string, ControllerConfig>& profiles,
                                   const std::string& active_profile,
                                   const std::string& config_path)
    : Gtk::Dialog()
    , m_profiles(profiles)
    , m_active_profile_name(active_profile)
    , m_config_path(config_path)
    , m_global_active_profile(active_profile)
{
    // Widgets carry English literals in the header as a fallback; the
    // translated text can only be applied once the catalogue is loaded.
    /* Assez grande d'emblee.
     *
     * La fenetre s'ouvrait a sa taille naturelle, c'est-a-dire au plus
     * serre : douze lignes de liaisons, deux onglets et une section
     * analogique tenaient dans une boite ou tout se touchait. */
    set_title(_("Controller Configuration"));

    m_profile_label.set_text(_("Profile:"));
    m_btn_new.set_label(_("New…"));
    m_btn_rename.set_label(_("Rename…"));
    m_btn_delete.set_label(_("Delete"));

    // Ensure active profile exists
    if (!m_profiles.count(m_active_profile_name)) {
        if (!m_profiles.empty()) m_active_profile_name = m_profiles.begin()->first;
        else { m_profiles["Default"] = ControllerConfig{}; m_active_profile_name = "Default"; }
    }
    m_config = m_profiles[m_active_profile_name];

    /* Largeur de la maquette, hauteur laissee au contenu.
     *
     * Deux appels a set_default_size se contredisaient, et le second imposait
     * 600 px de haut a un ecran qui en demande davantage : d'ou la bande morte
     * sous les controles des qu'on agrandissait. Une hauteur a -1 veut dire
     * << pas de defaut >>, et GTK prend alors la hauteur naturelle. Encore
     * faut-il que la zone defilante la propage, sinon elle annonce son
     * minimum et la fenetre se replie sur rien.
     */
    set_default_size(1180, -1);
    set_position(Gtk::WIN_POS_CENTER_ON_PARENT);

    m_binding_labels.resize(2 * GAME_ACTION_COUNT, nullptr);
    m_bind_buttons.resize(2 * GAME_ACTION_COUNT, nullptr);
    m_devices = ControllerManager::list_devices();

    // Layout: profile bar on top, notebook below
    get_style_context()->add_class("cc-window");

    auto* vbox = get_content_area();
    vbox->set_spacing(0);

    /* ── Entete : pictogramme, titre, sous-titre ────────────────────────
     *
     * La maquette ouvre sur une entete qui dit ce que fait l'ecran. Un
     * dialogue GTK ordinaire n'a que la barre de titre du gestionnaire de
     * fenetres, ce qui ne raconte rien.
     */
    /* ── Barre de titre, a la maniere de la fenetre principale ──────────
     *
     * Bootcade decore ses fenetres lui-meme : une Gtk::HeaderBar posee par
     * set_titlebar, la marque a gauche, et le bouton de fermeture que GTK y
     * place. L'entete dessine dans le contenu venait s'ajouter a celle du
     * gestionnaire de fenetres : deux titres, deux croix, et un ecran qui ne
     * ressemblait plus au reste de l'application.
     */
    m_header_icon.set_valign(Gtk::ALIGN_CENTER);
    m_header_title.set_markup("<b>" +
        Glib::Markup::escape_text(_("Controller Configuration")) + "</b>");
    m_header_title.set_xalign(0.0f);
    m_header_title.get_style_context()->add_class("cc-title");
    m_header_sub.set_text(_("Configure your controllers to play your favorite games"));
    m_header_sub.set_xalign(0.0f);
    m_header_sub.get_style_context()->add_class("cc-sub");

    auto* head_txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
    head_txt->set_valign(Gtk::ALIGN_CENTER);
    head_txt->pack_start(m_header_title, Gtk::PACK_SHRINK);
    head_txt->pack_start(m_header_sub,   Gtk::PACK_SHRINK);
    m_header.pack_start(m_header_icon, Gtk::PACK_SHRINK);
    m_header.pack_start(*head_txt,     Gtk::PACK_SHRINK);

    m_headerbar.set_show_close_button(true);
    m_headerbar.pack_start(m_header);
    /* Titre personnalise vide, comme la fenetre principale : sans lui, GTK
     * dessine SON titre au centre en plus de la marque, et repose le nom de
     * la fenetre sur celui de l'application. */
    m_headerbar.set_custom_title(*Gtk::make_managed<Gtk::Box>());
    set_titlebar(m_headerbar);
    m_headerbar.show_all();
    set_title(_("Controller Configuration"));

    // ── Barre de configuration : profil, puis manette ──────────────────
    build_profile_bar();
    m_topbar.pack_start(m_profile_bar, Gtk::PACK_SHRINK);
    m_device_stack.set_hexpand(true);
    m_topbar.pack_start(m_device_stack, Gtk::PACK_EXPAND_WIDGET);
    m_topbar.get_style_context()->add_class("cc-topbar");
    vbox->pack_start(m_topbar, Gtk::PACK_SHRINK);

    build_player_tab(0);
    build_player_tab(1);
    analog_ui_from_config();   // widgets exist now; fill them from the profile
    m_notebook.get_style_context()->add_class("cc-tabs");

    /* Le prereglage se pose au bout de la ligne des onglets, comme sur la
     * maquette. set_action_widget est la place que GTK prevoit pour cela ;
     * une ligne separee sous les onglets ajoutait une barre de plus. */
    m_preset_stack.set_margin_end(18);
    m_preset_stack.set_margin_bottom(6);
    m_notebook.set_action_widget(&m_preset_stack, Gtk::PACK_END);
    m_preset_stack.show();

    /* hide precede destroy : c'est le dernier moment ou les widgets sont
     * encore valides, donc l'endroit ou lever le drapeau. */
    signal_hide().connect([this] { m_closing = true; });
    signal_delete_event().connect([this](GdkEventAny*) {
        m_closing = true;
        return false;
    }, false);

    m_notebook.signal_switch_page().connect(
        [this](Gtk::Widget*, guint page) {
            if (m_closing) return;
            const std::string name = std::to_string(page);
            m_device_stack.set_visible_child(name);
            m_preset_stack.set_visible_child(name);
            update_device_panel(static_cast<int>(page));
        });
    vbox->pack_start(m_notebook, Gtk::PACK_EXPAND_WIDGET);

    /* ── Pied a deux groupes ────────────────────────────────────────────
     *
     * Actions destructives a gauche, validation a droite : c'est la
     * disposition de la maquette, et c'est aussi la convention qui evite de
     * cliquer « tout effacer » en visant « enregistrer ».
     */
    m_btn_clear_all.set_image(*SettingsUi::image("bc-clear.svg", 18));
    m_btn_clear_all.set_label(_("Clear All Bindings"));
    m_btn_clear_all.set_always_show_image(true);
    m_btn_clear_all.signal_clicked().connect([this] {
        const int p = m_notebook.get_current_page();
        if (p < 0) return;
        m_config.players[p].bindings.clear();
        refresh_bindings(p);
    });

    m_btn_restore.set_image(*SettingsUi::image("bc-restore.svg", 18));
    m_btn_restore.set_label(_("Restore Default"));
    m_btn_restore.set_always_show_image(true);
    m_btn_restore.signal_clicked().connect([this] {
        const int p = m_notebook.get_current_page();
        if (p < 0 || controller_presets().empty()) return;
        apply_preset(m_config.players[p], controller_presets().front());
        refresh_bindings(p);
    });

    auto* cancel = Gtk::make_managed<Gtk::Button>(_("Cancel"));
    cancel->set_image(*SettingsUi::image("bc-close.svg", 18));
    cancel->set_always_show_image(true);
    /* La fenetre est ouverte sans boucle run() : personne ne la referme a
     * notre place. response() seul laissait Cancel et Save sans effet
     * visible, la fenetre restait la. hide() declenche la relecture des
     * profils et la liberation cote fenetre principale. */
    cancel->signal_clicked().connect([this] {
        response(Gtk::RESPONSE_CANCEL);
        hide();
    });

    auto* save = Gtk::make_managed<Gtk::Button>(_("Save"));
    save->set_image(*SettingsUi::image("bc-save.svg", 18));
    save->set_always_show_image(true);
    save->get_style_context()->add_class("accent-button");
    /* Enregistrer ne ferme pas.
     *
     * On regle rarement une manette d'un seul coup : on lie, on essaie, on
     * corrige. Fermer a chaque enregistrement obligeait a rouvrir l'ecran
     * pour la moindre retouche. Le bouton confirme donc a sa place, et c'est
     * Cancel ou la croix qui ferment. */
    save->signal_clicked().connect([this, save] {
        on_save_clicked();
        response(Gtk::RESPONSE_OK);

        save->set_label(_("Saved"));
        save->set_sensitive(false);
        m_saved_timer.disconnect();
        m_saved_timer = Glib::signal_timeout().connect([this, save] {
            if (m_closing) return false;
            save->set_label(_("Save"));
            save->set_sensitive(true);
            return false;
        }, 1400);
    });

    /* Retour au profil par defaut pour ce jeu. Sans lui, une assignation
     * serait definitive : le seul moyen de la defaire serait d'editer
     * config.json a la main. Visible seulement en portee jeu, voir
     * set_game_scope. */
    m_btn_use_default.set_label(_("Use default profile"));
    m_btn_use_default.set_image(*SettingsUi::image("bc-restore.svg", 18));
    m_btn_use_default.set_always_show_image(true);
    m_btn_use_default.set_no_show_all(true);
    m_btn_use_default.signal_clicked().connect([this] {
        ControllerManager::set_game_profile(m_config_path, m_game_rom, "");
        if (!m_game_mame && m_profiles.count(m_global_active_profile)) {
            const auto& cfg = m_profiles.at(m_global_active_profile);
            ControllerManager::write_game_config(cfg, m_game_rom);
            ControllerManager::apply_analog_bindings(m_game_rom, cfg);
        }
        std::cout << "[ControllerDialog] " << m_game_rom << ": back to the default profile\n";
        response(Gtk::RESPONSE_OK);
        hide();
    });

    m_footer.pack_start(m_btn_clear_all, Gtk::PACK_SHRINK);
    m_footer.pack_start(m_btn_restore,   Gtk::PACK_SHRINK);
    m_footer.pack_start(m_btn_use_default, Gtk::PACK_SHRINK);
    m_footer.pack_end(*save,   Gtk::PACK_SHRINK);
    m_footer.pack_end(*cancel, Gtk::PACK_SHRINK);
    m_footer.get_style_context()->add_class("cc-footer");
    vbox->pack_start(m_footer, Gtk::PACK_SHRINK);

    set_default_response(Gtk::RESPONSE_OK);
    show_all_children();
}

ControllerDialog::~ControllerDialog() {
    m_closing = true;
    m_saved_timer.disconnect();
    stop_binding();
}

// ── Profile bar ───────────────────────────────────────────────────────────

void ControllerDialog::build_profile_bar() {
    m_profile_label.set_text(_("Profile"));
    m_profile_label.set_halign(Gtk::ALIGN_START);
    m_profile_bar.pack_start(m_profile_label, Gtk::PACK_SHRINK);

    populate_profile_combo();
    // Largeur fixe : le profil ne doit pas manger la place que la maquette
    // donne au selecteur de manette, sur la meme ligne.
    m_profile_combo.set_size_request(210, -1);
    m_profile_combo.signal_changed().connect(
        sigc::mem_fun(*this, &ControllerDialog::on_profile_changed));
    m_profile_bar.pack_start(m_profile_combo, Gtk::PACK_SHRINK);

    /* Trois boutons deviennent trois entrees d'un menu.
     *
     * Creer, renommer et supprimer un profil sont des gestes RARES : les
     * garder en permanence a cote du selecteur donnait autant de poids a
     * « supprimer » qu'a « choisir », alors que l'un se fait tous les jours
     * et l'autre presque jamais. Les gestionnaires ne changent pas.
     */
    m_mi_new.set_label(_("New profile..."));
    m_mi_rename.set_label(_("Rename..."));
    m_mi_delete.set_label(_("Delete"));
    m_mi_new.signal_activate().connect(sigc::mem_fun(*this, &ControllerDialog::on_new_profile_clicked));
    m_mi_rename.signal_activate().connect(sigc::mem_fun(*this, &ControllerDialog::on_rename_profile_clicked));
    m_mi_delete.signal_activate().connect(sigc::mem_fun(*this, &ControllerDialog::on_delete_profile_clicked));
    m_profile_menu.append(m_mi_new);
    m_profile_menu.append(m_mi_rename);
    m_profile_menu.append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());
    m_profile_menu.append(m_mi_delete);
    m_profile_menu.show_all();
    m_btn_profile_more.set_label("\u22ef");
    m_btn_profile_more.set_tooltip_text(_("Profile actions"));
    m_btn_profile_more.set_popup(m_profile_menu);
    m_profile_bar.pack_start(m_btn_profile_more, Gtk::PACK_SHRINK);
}

void ControllerDialog::populate_profile_combo() {
    m_profile_switching = true;
    m_profile_combo.remove_all();
    for (const auto& [name, _] : m_profiles)
        m_profile_combo.append(name, name);
    m_profile_combo.set_active_id(m_active_profile_name);
    m_profile_switching = false;
}

// ── Save active m_config → m_profiles ────────────────────────────────────

void ControllerDialog::save_active_to_profiles() {
    m_profiles[m_active_profile_name] = m_config;
}

// ── Load a profile into m_config and refresh UI ───────────────────────────

void ControllerDialog::load_profile(const std::string& name) {
    m_active_profile_name = name;
    m_config = m_profiles.count(name) ? m_profiles[name] : ControllerConfig{};
    refresh_from_config();
    analog_ui_from_config();
}

// ── Profile change (combo changed by user) ────────────────────────────────

void ControllerDialog::on_profile_changed() {
    if (m_profile_switching) return;
    save_active_to_profiles();
    load_profile(m_profile_combo.get_active_id());
}

// ── New profile ───────────────────────────────────────────────────────────

void ControllerDialog::on_new_profile_clicked() {
    Gtk::Dialog dlg(_("New Profile"), *this, Gtk::DIALOG_MODAL | Gtk::DIALOG_DESTROY_WITH_PARENT);
    dlg.set_default_size(320, 100);
    dlg.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);

    auto* entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_placeholder_text(_("Profile name…"));
    entry->set_activates_default(true);
    entry->set_margin_start(12); entry->set_margin_end(12);
    entry->set_margin_top(12);
    dlg.get_content_area()->pack_start(*entry, Gtk::PACK_SHRINK);
    dlg.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dlg.add_button(_("Create"),  Gtk::RESPONSE_OK);
    dlg.set_default_response(Gtk::RESPONSE_OK);
    dlg.show_all_children();

    if (dlg.run() != Gtk::RESPONSE_OK) return;
    std::string name = entry->get_text();
    if (name.empty()) return;
    if (m_profiles.count(name)) {
        SettingsUi::notice(*this, _("Profile already exists"),
                           Glib::ustring::compose(_("Profile \"%1\" already exists."), name),
                           "bc-warning.svg");
        return;
    }

    save_active_to_profiles();
    // New profile starts as empty (no bindings, no device)
    m_profiles[name] = ControllerConfig{};

    populate_profile_combo();
    m_profile_switching = true;
    m_profile_combo.set_active_id(name);
    m_profile_switching = false;
    load_profile(name);
}

// ── Rename profile ────────────────────────────────────────────────────────

void ControllerDialog::on_rename_profile_clicked() {
    Gtk::Dialog dlg(_("Rename Profile"), *this, Gtk::DIALOG_MODAL | Gtk::DIALOG_DESTROY_WITH_PARENT);
    dlg.set_default_size(320, 100);
    dlg.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);

    auto* entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_text(m_active_profile_name);
    entry->set_activates_default(true);
    entry->set_margin_start(12); entry->set_margin_end(12);
    entry->set_margin_top(12);
    dlg.get_content_area()->pack_start(*entry, Gtk::PACK_SHRINK);
    dlg.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dlg.add_button(_("Rename"),  Gtk::RESPONSE_OK);
    dlg.set_default_response(Gtk::RESPONSE_OK);
    dlg.show_all_children();

    if (dlg.run() != Gtk::RESPONSE_OK) return;
    std::string new_name = entry->get_text();
    if (new_name.empty() || new_name == m_active_profile_name) return;
    if (m_profiles.count(new_name)) {
        SettingsUi::notice(*this, _("Profile already exists"),
                           Glib::ustring::compose(_("Profile \"%1\" already exists."), new_name),
                           "bc-warning.svg");
        return;
    }

    save_active_to_profiles();
    m_profiles[new_name] = m_profiles[m_active_profile_name];
    m_profiles.erase(m_active_profile_name);
    m_active_profile_name = new_name;

    populate_profile_combo();
    m_profile_switching = true;
    m_profile_combo.set_active_id(new_name);
    m_profile_switching = false;
}

// ── Delete profile ────────────────────────────────────────────────────────

void ControllerDialog::on_delete_profile_clicked() {
    if (m_profiles.size() <= 1) {
        SettingsUi::notice(*this, _("Cannot delete the last profile"),
                           _("At least one controller profile has to remain."),
                           "bc-warning.svg");
        return;
    }

    ConfirmationDialog confirm(*this,
        Glib::ustring::compose(_("Delete profile \"%1\"?"), m_active_profile_name),
        _("Its bindings are lost. The other profiles are untouched."),
        "bc-controller.svg", /*destructive=*/true);
    if (!confirm.show_and_confirm()) return;

    m_profiles.erase(m_active_profile_name);
    // Switch to first available profile
    std::string next = m_profiles.begin()->first;
    m_active_profile_name = next;
    m_config = m_profiles[next];

    populate_profile_combo();
    m_profile_switching = true;
    m_profile_combo.set_active_id(next);
    m_profile_switching = false;
    refresh_from_config();
}

// -- Build one player tab -------------------------------------------------

void ControllerDialog::build_player_tab(int p) {
    /* ---- Ligne manette : elle part dans la barre du haut ---------------
     *
     * La maquette met le choix de la manette sur la MEME ligne que le
     * profil, au-dessus des onglets. La manette reste pourtant reglee par
     * joueur : la ligne va donc dans une pile, une page par joueur, et la
     * page suit l'onglet actif.
     */
    auto* dev_row = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    auto* dev_lbl = Gtk::make_managed<Gtk::Label>(_("Controller"));
    dev_lbl->set_halign(Gtk::ALIGN_START);
    dev_row->pack_start(*dev_lbl, Gtk::PACK_SHRINK);

    auto* combo = Gtk::make_managed<Gtk::ComboBoxText>();
    combo->append("", _("None "));
    for (const auto& d : m_devices)
        combo->append(d.path, d.name);

    const std::string& cur = m_config.players[p].device_path;
    bool present = false;
    for (const auto& d : m_devices) if (d.path == cur) { present = true; break; }
    if (!cur.empty() && !present) {
        const std::string& nm = m_config.players[p].device_name;
        combo->append(cur, (nm.empty() ? cur : nm) +
                           "  " + std::string(_("(not plugged in)")));
    }
    if (!cur.empty()) combo->set_active_id(cur);
    else              combo->set_active(0);

    combo->set_hexpand(true);
    combo->signal_changed().connect(
        sigc::bind(sigc::mem_fun(*this, &ControllerDialog::on_device_changed), p));
    dev_row->pack_start(*combo, Gtk::PACK_EXPAND_WIDGET);
    m_device_combos[p] = combo;

    auto* refresh_btn = Gtk::make_managed<Gtk::Button>();
    refresh_btn->set_image(*SettingsUi::image("bc-restore.svg", 16));
    refresh_btn->set_tooltip_text(_("Refresh controller list"));
    refresh_btn->get_style_context()->add_class("flat");
    refresh_btn->signal_clicked().connect([this, p]() {
        m_devices = ControllerManager::list_devices();
        std::string cur_id = m_device_combos[p]->get_active_id();
        m_device_combos[p]->remove_all();
        m_device_combos[p]->append("", _("None "));
        for (const auto& d : m_devices)
            m_device_combos[p]->append(d.path, d.name);
        if (!cur_id.empty()) m_device_combos[p]->set_active_id(cur_id);
        else                 m_device_combos[p]->set_active(0);
        update_device_panel(p);
    });
    dev_row->pack_start(*refresh_btn, Gtk::PACK_SHRINK);

    /* Pastille d'etat : une manette branchee ou non se voit d'un coup d'oeil,
     * sans avoir a relire le contenu du selecteur. */
    auto* dot = Gtk::make_managed<Gtk::Label>("");
    dot->get_style_context()->add_class("conn-dot");
    dot->set_valign(Gtk::ALIGN_CENTER);
    auto* state = Gtk::make_managed<Gtk::Label>("");
    state->get_style_context()->add_class("cc-conn");
    m_conn_dot[p] = dot;
    m_conn_lbl[p] = state;
    auto* conn = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 7);
    conn->pack_start(*dot,   Gtk::PACK_SHRINK);
    conn->pack_start(*state, Gtk::PACK_SHRINK);
    dev_row->pack_start(*conn, Gtk::PACK_SHRINK);

    auto* identify_btn = Gtk::make_managed<Gtk::Button>(_("Identify"));
    identify_btn->set_image(*SettingsUi::image("bc-identify.svg", 18));
    identify_btn->set_always_show_image(true);
    identify_btn->set_tooltip_text(
        _("Press a button on a controller to assign it to this player."));
    identify_btn->signal_clicked().connect(
        sigc::bind(sigc::mem_fun(*this, &ControllerDialog::identify_device), p));
    dev_row->pack_start(*identify_btn, Gtk::PACK_SHRINK);

    auto* test_btn = Gtk::make_managed<Gtk::Button>(_("Test"));
    test_btn->set_image(*SettingsUi::image("bc-test.svg", 18));
    test_btn->set_always_show_image(true);
    test_btn->set_tooltip_text(
        _("Watch the buttons and axes react, without changing anything."));
    test_btn->signal_clicked().connect(
        sigc::bind(sigc::mem_fun(*this, &ControllerDialog::open_test_dialog), p));
    dev_row->pack_start(*test_btn, Gtk::PACK_SHRINK);

    dev_row->show_all();
    m_device_stack.add(*dev_row, std::to_string(p));

    /* ---- Ligne prereglage : au bout de la ligne des onglets ------------
     *
     * Brancher, designer, puis laisser le lanceur poser les douze liaisons
     * est une seule et meme demarche : la maquette la garde d'un bloc, a
     * droite des selecteurs de joueur.
     */
    auto* preset_row = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    auto* preset_lbl = Gtk::make_managed<Gtk::Label>(_("Preset"));
    preset_row->pack_start(*preset_lbl, Gtk::PACK_SHRINK);

    auto* preset_combo = Gtk::make_managed<Gtk::ComboBoxText>();
    for (const auto& preset : controller_presets())
        preset_combo->append(preset.name, preset.name);
    // Le choix precedent revient : sans cela l'ecran repartait sur Keyboard
    // et l'illustration retombait sur la silhouette generique.
    if (m_config.players[p].preset.empty()
        || !preset_combo->set_active_id(m_config.players[p].preset))
        preset_combo->set_active(0);
    preset_row->pack_start(*preset_combo, Gtk::PACK_SHRINK);
    m_preset_combos[p] = preset_combo;
    // Choisir un prereglage change le DESSIN et se retient, mais ne touche
    // pas aux liaisons : celles-ci n'entrent en jeu qu'avec Apply.
    preset_combo->signal_changed().connect([this, p, preset_combo] {
        m_config.players[p].preset = preset_combo->get_active_id().raw();
        update_device_panel(p);
    });

    auto* apply = Gtk::make_managed<Gtk::Button>(_("Apply"));
    apply->set_tooltip_text(
        _("Fills this profile's buttons and axes. A starting point, not a "
          "guarantee: Linux numbers the buttons in the order its driver "
          "declares them, and that order differs between controller families. "
          "Rebind anything that comes out wrong."));
    auto apply_chosen = [this, p, preset_combo]() {
        const std::string chosen = preset_combo->get_active_id();
        for (const auto& preset : controller_presets()) {
            if (chosen != preset.name) continue;
            apply_preset(m_config.players[p], preset);
            // Les noms se relevent sur le clavier reellement branche : le
            // prereglage ne connait que des positions.
            for (auto& [action, binding] : m_config.players[p].bindings)
                if (binding.source == InputSource::KEY)
                    binding.key_name = key_name_for_code(binding.key);
            refresh_bindings(p);
            break;
        }
    };
    apply->signal_clicked().connect(apply_chosen);

    auto apply_named = [this, p](const std::string& want) {
        for (const auto& preset : controller_presets()) {
            if (want != preset.name) continue;
            apply_preset(m_config.players[p], preset);
            for (auto& [action, binding] : m_config.players[p].bindings)
                if (binding.source == InputSource::KEY)
                    binding.key_name = key_name_for_code(binding.key);
            refresh_bindings(p);
            if (m_preset_combos[p]) m_preset_combos[p]->set_active_id(want);
            return;
        }
    };
    preset_row->pack_start(*apply, Gtk::PACK_SHRINK);

    auto* auto_btn = Gtk::make_managed<Gtk::Button>(_("Configure automatically"));
    auto_btn->set_image(*SettingsUi::image("bc-auto.svg", 18));
    auto_btn->set_always_show_image(true);
    // Accent Bootcade, pas la classe << suggested-action >> du theme : celle-ci
    // prend la couleur du bureau et trahit l'identite de la maquette.
    auto_btn->get_style_context()->add_class("accent-button");
    auto_btn->set_tooltip_text(
        _("Press each control in turn. Close a prompt to skip that control."));
    auto_btn->signal_clicked().connect([this, p]() { run_auto_configure(p); });
    preset_row->pack_start(*auto_btn, Gtk::PACK_SHRINK);

    preset_row->show_all();
    m_preset_stack.add(*preset_row, std::to_string(p));

    /* ---- Contenu de l'onglet ------------------------------------------- */
    auto* tab_box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 18);
    tab_box->set_margin_start(24);
    tab_box->set_margin_end(24);
    tab_box->set_margin_top(20);
    tab_box->set_margin_bottom(22);

    /* Trois cartes cote a cote, comme la maquette.
     *
     * Les douze liaisons se rangent en trois familles qu'un joueur distingue
     * d'instinct sur sa manette. Le regroupement reste PUREMENT visuel :
     * l'ordre de l'enumeration ne change pas, et l'assistant le parcourt
     * toujours de la meme facon.
     */
    struct Group { const char* title; const char* icon; int first, last; };
    const Group groups[] = {
        { N_("Directions"), "bc-directions.svg",
          (int)GameAction::UP,      (int)GameAction::RIGHT   },
        { N_("Buttons"),    "bc-buttons.svg",
          (int)GameAction::BUTTON1, (int)GameAction::BUTTON6 },
        { N_("System"),     "bc-system.svg",
          (int)GameAction::START,   (int)GameAction::COIN    },
    };

    auto* cards = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 16);
    cards->set_homogeneous(true);

    // Une ligne de liaison : pictogramme et nom a gauche, la valeur a droite.
    auto make_row = [this, p](int a) -> std::pair<Gtk::Widget*, Gtk::Widget*> {
        GameAction action = static_cast<GameAction>(a);

        /* Les quatre directions portent leur fleche dans une tuile, les
         * boutons leur numero dans une pastille coloree, Start et
         * Coin leur pictogramme nu. Les trois cas passent par un
         * conteneur de MEME taille : les colonnes tombent donc en face
         * d'une carte a l'autre. */
        const bool is_dir = a >= (int)GameAction::UP
                         && a <= (int)GameAction::RIGHT;

        Gtk::Widget* cell = nullptr;
        if (is_button_action(action)) {
            cell = number_badge(button_number(action));
        } else if (is_dir) {
            static const char* kArrow[] = {"bc-up.svg", "bc-down.svg",
                                           "bc-left.svg", "bc-right.svg"};
            cell = glyph_cell(kArrow[a - (int)GameAction::UP], true);
        } else if (action == GameAction::START) {
            cell = glyph_cell("bc-start.svg", false);
        } else if (action == GameAction::COIN) {
            cell = glyph_cell("bc-coin.svg", false);
        }

        auto* name_row = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 11);
        name_row->set_size_request(-1, kRowHeight);
        if (cell) name_row->pack_start(*cell, Gtk::PACK_SHRINK);
        auto* name_lbl = Gtk::make_managed<Gtk::Label>(_(game_action_name(action)));
        name_lbl->set_halign(Gtk::ALIGN_START);
        name_lbl->set_valign(Gtk::ALIGN_CENTER);
        name_lbl->set_hexpand(true);
        name_row->pack_start(*name_lbl, Gtk::PACK_EXPAND_WIDGET);

        /* La valeur EST le bouton : un clic pour lier, Suppr pour
         * effacer, comme dans toute table de raccourcis. */
        auto* bind_lbl = Gtk::make_managed<Gtk::Label>("");
        bind_lbl->set_halign(Gtk::ALIGN_START);
        bind_lbl->set_ellipsize(Pango::ELLIPSIZE_END);
        m_binding_labels[p * GAME_ACTION_COUNT + a] = bind_lbl;

        auto* btn = Gtk::make_managed<Gtk::Button>();
        btn->add(*bind_lbl);
        btn->set_size_request(kFieldWidth, kRowHeight);
        btn->set_valign(Gtk::ALIGN_CENTER);
        btn->set_tooltip_text(_("Click to bind, Delete to clear"));
        btn->get_style_context()->add_class("bind-cell");
        btn->signal_clicked().connect(
            sigc::bind(sigc::bind(sigc::mem_fun(*this, &ControllerDialog::start_binding), action), p));
        btn->add_events(Gdk::KEY_PRESS_MASK);
        btn->signal_key_press_event().connect([this, p, action, bind_lbl](GdkEventKey* ev) {
            if (ev->keyval != GDK_KEY_Delete && ev->keyval != GDK_KEY_BackSpace)
                return false;
            m_config.players[p].bindings.erase(action);
            bind_lbl->set_text(_("Not set"));
            return true;
        }, false);
        m_bind_buttons[p * GAME_ACTION_COUNT + a] = btn;

        name_row->set_hexpand(true);
        return {name_row, btn};
    };

    for (const auto& g : groups) {
        auto* grid = Gtk::make_managed<Gtk::Grid>();
        grid->set_column_spacing(14);
        grid->set_row_spacing(10);

        int row = 0;
        for (int a = g.first; a <= g.last; ++a, ++row) {
            auto cells = make_row(a);
            grid->attach(*cells.first,  0, row, 1, 1);
            grid->attach(*cells.second, 1, row, 1, 1);
        }
        auto* c = card(_(g.title), g.icon, *grid);
        c->get_style_context()->add_class("cc-subcard");
        c->set_hexpand(true);
        // Quatre, six et deux lignes : sans cela les trois cartes se
        // terminaient a trois hauteurs differentes et la rangee partait de
        // travers. La maquette les aligne sur la plus haute.
        c->set_valign(Gtk::ALIGN_FILL);
        cards->pack_start(*c, Gtk::PACK_EXPAND_WIDGET);
    }

    /* Les boutons au-dela du sixieme, sous les trois cartes.
     *
     * Une manette actuelle en a onze ou douze. Les ajouter a la carte des
     * boutons l'allongeait jusqu'a seize lignes et desalignait la rangee ;
     * ils vont donc dans une carte a part, sur trois colonnes, qui n'apparait
     * que si la manette choisie les possede (voir update_button_rows). La
     * disposition d'une manette a six boutons ne change pas. */
    auto* extra_grid = Gtk::make_managed<Gtk::Grid>();
    extra_grid->set_column_spacing(30);
    extra_grid->set_row_spacing(10);
    extra_grid->set_column_homogeneous(true);
    for (int n = BASE_GAME_BUTTONS + 1; n <= MAX_GAME_BUTTONS; ++n) {
        auto cells = make_row((int)button_action(n));
        auto* pair = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 14);
        pair->pack_start(*cells.first,  Gtk::PACK_EXPAND_WIDGET);
        pair->pack_start(*cells.second, Gtk::PACK_SHRINK);
        pair->set_no_show_all(true);
        const int k = n - BASE_GAME_BUTTONS - 1;
        extra_grid->attach(*pair, k % 3, k / 3, 1, 1);
        m_extra_rows[p][n - 1] = pair;
    }
    auto* extra = card(_("More buttons"), "bc-buttons.svg", *extra_grid);
    extra->get_style_context()->add_class("cc-subcard");
    extra->set_no_show_all(true);
    m_extra_card[p] = extra;

    auto* arcade_body = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 16);
    arcade_body->pack_start(*cards, Gtk::PACK_SHRINK);
    arcade_body->pack_start(*extra, Gtk::PACK_SHRINK);

    /* La carte englobante de la maquette : elle dit en une phrase comment on
     * change une liaison, ce qu'aucune des trois sous-cartes ne peut dire
     * sans se repeter trois fois. */
    auto* arcade = card(_("Arcade controls"), "bc-arcade.svg", *arcade_body,
                        _("Click on a control to change it. Press a button, "
                          "axis or hat on your controller."));
    arcade->set_hexpand(true);

    /* Deux colonnes, comme la maquette : a gauche les liaisons puis les
     * commandes analogiques, a droite la carte manette sur toute la hauteur.
     * Une carte analogique pleine largeur passait SOUS le panneau manette et
     * cassait la colonne de droite en deux. */
    auto* left_col = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 18);
    left_col->pack_start(*arcade, Gtk::PACK_SHRINK);

    auto* main_row = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 18);
    main_row->pack_start(*left_col, Gtk::PACK_EXPAND_WIDGET);

    /* Panneau manette, a droite : le modele reconnu, son dessin, et l'offre
     * d'appliquer d'un clic le prereglage qui lui correspond. */
    {
        auto* title = Gtk::make_managed<Gtk::Label>();
        title->set_xalign(0.0f);
        title->set_line_wrap(true);
        title->set_max_width_chars(26);
        title->get_style_context()->add_class("cc-pad-name");

        auto* sub = Gtk::make_managed<Gtk::Label>();
        sub->set_xalign(0.0f);
        sub->get_style_context()->add_class("cc-sub");

        // Centree entre la disposition annoncee et l'encart vert, comme dans
        // la maquette. Cadrer plus large que la carte la rognait aux poignees.
        /* Aire de presentation fixe : l'image garde ses proportions et se
         * centre dedans, donc changer de manette ne fait plus sauter le
         * dessin d'une famille a l'autre. */
        auto* art = Gtk::make_managed<Gtk::Image>();
        art->set_size_request(kArtWidth, kArtHeight);
        art->set_halign(Gtk::ALIGN_CENTER);
        art->set_valign(Gtk::ALIGN_CENTER);
        art->set_margin_top(18);
        art->set_margin_bottom(18);

        /* Structure explicite, en grille :
         *
         *   [coche]  Controller detected
         *            Recommended mapping: Xbox
         *            [ Apply recommended mapping ]
         *
         * La coche a son propre conteneur carre et se centre sur la LIGNE DU
         * TITRE ; le titre, la description et le bouton partagent la meme
         * colonne, donc le meme bord gauche. Rien n'est place a la marge.
         */
        auto* det_icon = Gtk::make_managed<SettingsUi::Icon>("bc-info.svg", 19);
        det_icon->set_halign(Gtk::ALIGN_CENTER);
        det_icon->set_valign(Gtk::ALIGN_CENTER);

        auto* det_badge = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 0);
        det_badge->set_size_request(kCellSize, kCellSize);
        det_badge->pack_start(*det_icon, Gtk::PACK_EXPAND_WIDGET);
        det_badge->get_style_context()->add_class("cc-badge");
        det_badge->set_valign(Gtk::ALIGN_CENTER);

        auto* det_ttl = Gtk::make_managed<Gtk::Label>();
        det_ttl->set_xalign(0.0f);
        det_ttl->set_valign(Gtk::ALIGN_CENTER);
        det_ttl->set_line_wrap(true);
        det_ttl->get_style_context()->add_class("cc-det-title");

        auto* det_sub = Gtk::make_managed<Gtk::Label>();
        det_sub->set_xalign(0.0f);
        det_sub->set_line_wrap(true);
        det_sub->get_style_context()->add_class("cc-sub");

        auto* det_btn = Gtk::make_managed<Gtk::Button>(_("Apply recommended mapping"));
        det_btn->set_hexpand(true);
        det_btn->set_margin_top(12);

        auto* det = Gtk::make_managed<Gtk::Grid>();
        det->set_column_spacing(12);
        det->set_row_spacing(3);
        det->attach(*det_badge, 0, 0, 1, 1);   // centree sur la ligne du titre
        det->attach(*det_ttl,   1, 0, 1, 1);
        det->attach(*det_sub,   1, 1, 1, 1);
        det->attach(*det_btn,   1, 2, 1, 1);

        det_btn->signal_clicked().connect([this, p, apply_named]() {
            if (!m_reco_preset[p].empty()) apply_named(m_reco_preset[p]);
        });

        m_detected_icon[p]  = det_icon;
        m_detected_title[p] = det_ttl;
        m_detected_sub[p]   = det_sub;
        m_detected_btn[p]   = det_btn;

        auto* panel = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 10);
        panel->pack_start(*title, Gtk::PACK_SHRINK);
        panel->pack_start(*sub,   Gtk::PACK_SHRINK);
        panel->pack_start(*art,   Gtk::PACK_SHRINK);
        panel->pack_end(*det,     Gtk::PACK_SHRINK);
        panel->get_style_context()->add_class("cc-card");
        panel->set_valign(Gtk::ALIGN_START);
        panel->set_size_request(368, -1);

        m_device_title[p] = title;
        m_device_sub[p]   = sub;
        m_device_art[p]   = art;
        m_detected_box[p] = det;
        m_device_panel[p] = panel;
        main_row->pack_start(*panel, Gtk::PACK_SHRINK);
    }

    /* Volant, pedales et visee ne concernent qu'une poignee de jeux : la
     * section garde sa carte et son chevron, comme la maquette, et se replie
     * pour qui n'en a pas besoin. */
    auto* analog_exp = Gtk::make_managed<Gtk::Expander>();
    {
        auto* ehrow = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
        auto* eh = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 2);
        auto* et = Gtk::make_managed<Gtk::Label>();
        et->set_markup("<b>" + Glib::Markup::escape_text(
            _("Advanced / Analog controls")) + "</b>");
        et->set_xalign(0.0f);
        auto* es = Gtk::make_managed<Gtk::Label>(
            _("Steering wheels, light guns, paddles, etc. (not required for "
              "most games)"));
        es->set_xalign(0.0f);
        es->get_style_context()->add_class("cc-sub");
        eh->pack_start(*et, Gtk::PACK_SHRINK);
        eh->pack_start(*es, Gtk::PACK_SHRINK);
        // Le chevron vient de la meme famille que celui d'Arcade controls ;
        // celui que GTK dessine lui-meme appartient au theme du bureau.
        ehrow->pack_start(*icon("bc-arcade.svg", 18), Gtk::PACK_SHRINK);
        ehrow->pack_start(*eh, Gtk::PACK_SHRINK);
        ehrow->show_all();
        analog_exp->set_label_widget(*ehrow);
    }
    analog_exp->set_expanded(true);
    analog_exp->add(*build_analog_section(p));

    auto* analog_card = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
    analog_card->pack_start(*analog_exp, Gtk::PACK_SHRINK);
    analog_card->get_style_context()->add_class("cc-card");
    left_col->pack_start(*analog_card, Gtk::PACK_SHRINK);

    tab_box->pack_start(*main_row, Gtk::PACK_SHRINK);

    // Le contenu peut depasser la fenetre sur un petit ecran, et un bouton
    // Save rogne est inutilisable : le pied reste hors du defilement.
    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    scroll->set_propagate_natural_height(true);
    scroll->set_propagate_natural_width(true);
    scroll->add(*tab_box);

    /* Onglet avec son pictogramme, comme la maquette : deux grands
     * selecteurs de joueur plutot que les petits onglets d'un carnet GTK. */
    {
        auto* tab_head = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
        tab_head->pack_start(*icon(p == 0 ? "bc-player-1.svg" : "bc-player-2.svg", 18),
                             Gtk::PACK_SHRINK);
        auto* tl = Gtk::make_managed<Gtk::Label>(
            Glib::ustring::compose(_("Player %1"), p + 1));
        tab_head->pack_start(*tl, Gtk::PACK_SHRINK);
        tab_head->show_all();
        m_notebook.append_page(*scroll, *tab_head);
    }
    refresh_bindings(p);
    update_device_panel(p);
}

/* Ce que le panneau de droite doit dire : quelle manette est reglee pour ce
 * joueur, et si elle est branchee la maintenant. Sans cette mise a jour, le
 * panneau restait sur un titre generique et un dessin, c'est-a-dire sur rien.
 */
void ControllerDialog::update_device_panel(int p) {
    if (m_closing) return;
    const std::string path = m_config.players[p].device_path;
    const JoystickInfo* found = nullptr;
    for (const auto& d : m_devices)
        if (d.path == path) { found = &d; break; }

    if (m_conn_dot[p] && m_conn_lbl[p]) {
        for (Gtk::Widget* w : {static_cast<Gtk::Widget*>(m_conn_dot[p]),
                               static_cast<Gtk::Widget*>(m_conn_lbl[p])}) {
            auto ctx = w->get_style_context();
            ctx->remove_class(found ? "off" : "on");
            ctx->add_class(found ? "on" : "off");
        }
        m_conn_lbl[p]->set_text(found ? _("Connected") : _("Not connected"));
    }

    if (!m_device_title[p]) return;

    const bool known = found || !m_config.players[p].device_name.empty();
    const std::string name = found ? found->name
                           : (known ? m_config.players[p].device_name
                                    : std::string(_("No controller")));

    m_device_title[p]->set_markup("<b>" + Glib::Markup::escape_text(name) + "</b>");

    /* Sans manette connue on montre un clavier : c'est bien avec lui que se
     * joue le profil par defaut, et une silhouette de manette mentirait. */
    const PadFamily fam = known
        ? pad_family_for(name)
        : PadFamily{"keyboard.png", nullptr, "bc-pad-keyboard.svg",
                    N_("Keyboard"), "Keyboard"};

    /* Le dessin suit le prereglage choisi, quand ce prereglage designe une
     * manette. << Keyboard >> ne remplace jamais une manette detectee : ce
     * serait montrer un clavier alors qu'un pad est branche. */
    const PadFamily* by_preset = m_preset_combos[p]
        ? family_for_preset(m_preset_combos[p]->get_active_id())
        : nullptr;
    const PadFamily& shown = by_preset ? *by_preset : fam;

    /* L'illustration fournie passe avant tout.
     *
     * assets/controllers contient les photos de manettes ; le trace
     * vectoriel de assets/icons ne sert que si le fichier n'est pas la, pour
     * qu'une famille sans photo garde quand meme une image. IconManager rend
     * un pointeur vide quand le fichier manque, il n'y a donc rien a tester
     * sur le disque.
     */
    Glib::RefPtr<Gdk::Pixbuf> art =
        IconManager::load(std::string("controllers/") + shown.photo,
                          kArtWidth, kArtHeight);
    if (!art && shown.alt)
        art = IconManager::load(std::string("controllers/") + shown.alt,
                                kArtWidth, kArtHeight);
    if (!art)
        art = IconManager::load(std::string("icons/") + shown.svg,
                                kArtWidth, kArtHeight);
    m_device_art[p]->set(art);

    /* La ligne sous le nom decrit le MATERIEL reconnu. Elle ne dit
     * << detecte >> que quand la manette est effectivement branchee, et elle
     * ne suit jamais le prereglage choisi a la main. */
    const bool borrowed = by_preset && std::string(shown.family) != fam.family;
    Glib::ustring line;
    if (found)      line = Glib::ustring::compose(_("%1 (detected)"), _(fam.family));
    else if (known) line = Glib::ustring::compose(_("%1, not plugged in"), _(fam.family));
    else            line = _("No controller assigned");
    if (borrowed)
        line += Glib::ustring::compose(_(" · showing %1 layout"), _(shown.family));
    m_device_sub[p]->set_text(line);

    // Le prereglage conseille decoule du materiel, pas de la liste deroulante.
    m_reco_preset[p] = (found && fam.preset) ? fam.preset : "";

    auto ctx = m_detected_box[p]->get_style_context();
    ctx->remove_class("cc-detected");
    ctx->remove_class("cc-neutral");

    if (!m_reco_preset[p].empty()) {
        ctx->add_class("cc-detected");
        m_detected_icon[p]->set_file("bc-detected.svg");
        m_detected_title[p]->set_markup("<b>" + Glib::Markup::escape_text(
            _("Controller detected")) + "</b>");
        m_detected_sub[p]->set_text(Glib::ustring::compose(
            _("Recommended mapping: %1"), m_reco_preset[p]));
        // show_all_children() rappelle tout : sans no_show_all, le bouton
        // reparaissait sous un message qui dit qu'il n'y a rien a appliquer.
        m_detected_btn[p]->set_no_show_all(false);
        m_detected_btn[p]->show();
    } else {
        /* Jamais un conteneur vert vide : quand il n'y a rien a recommander,
         * l'encart le dit en clair et propose la suite. */
        ctx->add_class("cc-neutral");
        m_detected_icon[p]->set_file("bc-info.svg");
        m_detected_btn[p]->set_no_show_all(true);
        m_detected_btn[p]->hide();
        if (found) {
            m_detected_title[p]->set_markup("<b>" + Glib::Markup::escape_text(
                _("No recommended mapping")) + "</b>");
            m_detected_sub[p]->set_text(
                _("This controller is not in the known families. Use Configure "
                  "automatically, or bind each control yourself."));
        } else if (known) {
            m_detected_title[p]->set_markup("<b>" + Glib::Markup::escape_text(
                _("Controller not connected")) + "</b>");
            m_detected_sub[p]->set_text(
                _("Plug it in to detect it and get a recommended mapping."));
        } else {
            m_detected_title[p]->set_markup("<b>" + Glib::Markup::escape_text(
                _("No controller selected")) + "</b>");
            m_detected_sub[p]->set_text(
                _("Pick one above, or play this player on the keyboard."));
        }
    }
}

// ── Refresh all UI from m_config ──────────────────────────────────────────

void ControllerDialog::refresh_from_config() {
    for (int p = 0; p < 2; ++p) {
        if (!m_device_combos[p]) continue;
        const std::string& cur = m_config.players[p].device_path;
        if (!cur.empty()) m_device_combos[p]->set_active_id(cur);
        else              m_device_combos[p]->set_active(0);
        refresh_bindings(p);
    }
}

// ── Refresh binding labels from m_config ─────────────────────────────────

int ControllerDialog::shown_buttons(int p) const {
    int n = BASE_GAME_BUTTONS;
    for (const auto& d : m_devices)
        if (d.path == m_config.players[p].device_path) { n = std::max(n, d.num_buttons); break; }
    // Un bouton deja lie reste visible, meme sur une manette qui en a moins :
    // sinon on ne pourrait plus ni le voir ni l'effacer.
    for (const auto& [action, binding] : m_config.players[p].bindings)
        if (is_button_action(action)) n = std::max(n, button_number(action));
    return std::min(n, MAX_GAME_BUTTONS);
}

bool ControllerDialog::action_shown(int p, GameAction a) const {
    return !is_button_action(a) || button_number(a) <= shown_buttons(p);
}

void ControllerDialog::update_button_rows(int p) {
    if (m_closing || !m_extra_card[p]) return;
    const int shown = shown_buttons(p);
    /* show_all ne fait RIEN sur un widget marque no_show_all : on leve la
     * marque le temps de l'appel. Elle reste posee le reste du temps, pour
     * que le show_all_children de la fenetre ne rappelle pas ce qu'on cache. */
    auto set_shown = [](Gtk::Widget* w, bool on) {
        if (!w) return;
        if (!on) { w->hide(); return; }
        w->set_no_show_all(false);
        w->show_all();
        w->set_no_show_all(true);
    };
    for (int n = BASE_GAME_BUTTONS + 1; n <= MAX_GAME_BUTTONS; ++n)
        set_shown(m_extra_rows[p][n - 1], n <= shown);
    set_shown(m_extra_card[p], shown > BASE_GAME_BUTTONS);
}

void ControllerDialog::refresh_bindings(int p) {
    update_button_rows(p);
    for (int a = 0; a < GAME_ACTION_COUNT; ++a) {
        Gtk::Label* lbl = m_binding_labels[p * GAME_ACTION_COUNT + a];
        if (!lbl) continue;
        GameAction action = static_cast<GameAction>(a);
        auto it = m_config.players[p].bindings.find(action);
        // « Not set » plutot qu'une case vide : maintenant que la valeur est
        // le bouton, une case vide ressemble a un bouton sans libelle et on
        // ne devine pas qu'elle se clique.
        lbl->set_text(it != m_config.players[p].bindings.end()
                      ? it->second.label() : std::string(_("Not set")));
    }
}

// ── Device combo changed ──────────────────────────────────────────────────

void ControllerDialog::on_device_changed(int p) {
    std::string path = m_device_combos[p]->get_active_id();
    m_config.players[p].device_path = path;
    for (const auto& d : m_devices)
        if (d.path == path) { m_config.players[p].device_name = d.name; break; }
    if (path.empty()) m_config.players[p].device_name = "";
    update_device_panel(p);
    update_button_rows(p);
}


// ── Bind: start waiting for input ─────────────────────────────────────────

void ControllerDialog::start_binding(int p, GameAction action) {
    stop_binding();

    // Le clavier suffit : exiger une manette ici empechait de configurer quoi
    // que ce soit sans en avoir une, alors que l'emulateur accepte les deux.
    // Une manette absente ou illisible n'est donc plus une erreur, seulement
    // une source de moins pendant l'attente.
    std::string device_path = m_config.players[p].device_path;
    m_bind_fd = device_path.empty() ? -1 : ControllerManager::open_device(device_path);

    // Drain stale events
    if (m_bind_fd >= 0) {
        InputBinding dummy;
        for (int i = 0; i < 50; ++i) if (!ControllerManager::poll_event(m_bind_fd, dummy)) break;
    }

    Gtk::Dialog wait_dlg(
        Glib::ustring::compose(_("Binding: %1"), _(game_action_name(action))),
        *this, Gtk::DIALOG_MODAL | Gtk::DIALOG_DESTROY_WITH_PARENT);
    wait_dlg.set_default_size(320, 140);
    wait_dlg.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);

    std::string dev_name = m_config.players[p].device_name;
    if (dev_name.empty()) dev_name = device_path;

    auto* info = Gtk::make_managed<Gtk::Label>();
    info->set_markup("<b>" + Glib::Markup::escape_text(
        m_bind_fd >= 0
            ? Glib::ustring::compose(_("Press a key, or a button on %1…"), dev_name)
            : Glib::ustring(_("Press a key…"))) + "</b>");
    info->set_halign(Gtk::ALIGN_CENTER);
    info->set_margin_top(20); info->set_margin_bottom(10);
    wait_dlg.get_content_area()->add(*info);
    wait_dlg.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    wait_dlg.show_all_children();

    Gtk::Label* bind_lbl = m_binding_labels[p * GAME_ACTION_COUNT + static_cast<int>(action)];

    // Une touche pressee dans la fenetre d'attente vaut liaison, au meme titre
    // qu'un bouton de manette. Echap reste la sortie, sans quoi on ne pourrait
    // plus annuler une fois la capture lancee.
    wait_dlg.signal_key_press_event().connect(
        [this, p, action, &wait_dlg, bind_lbl](GdkEventKey* ev) -> bool {
            if (ev->keyval == GDK_KEY_Escape) return false;
            const int code = ControllerManager::fbneo_key_from_gtk(ev->hardware_keycode);
            if (code < 0) return true;                  // touche sans equivalent
            InputBinding result;
            result.valid  = true;
            result.source   = InputSource::KEY;
            result.key      = code;
            result.key_name = key_name_from_event(ev);
            m_config.players[p].bindings[action] = result;
            if (bind_lbl) bind_lbl->set_text(result.label());
            wait_dlg.response(Gtk::RESPONSE_OK);
            return true;
        }, false);

    m_poll_conn = Glib::signal_timeout().connect(
        [this, p, action, &wait_dlg, bind_lbl]() -> bool {
            if (m_bind_fd < 0) return true;             // clavier seul
            InputBinding result;
            if (ControllerManager::poll_event(m_bind_fd, result)) {
                m_config.players[p].bindings[action] = result;
                if (bind_lbl) bind_lbl->set_text(result.label());
                wait_dlg.response(Gtk::RESPONSE_OK);
                return false;
            }
            return true;
        }, 50);

    wait_dlg.run();
    stop_binding();
}

void ControllerDialog::stop_binding() {
    if (m_poll_conn.connected()) m_poll_conn.disconnect();
    ControllerManager::close_device(m_bind_fd);
    m_bind_fd = -1;
}

// ── Save ─────────────────────────────────────────────────────────────────

void ControllerDialog::on_save_clicked() {
    // Sync current working config into profile map
    save_active_to_profiles();

    if (game_scoped()) {
        // Les retouches faites aux profils sont gardees : ce sont les memes
        // profils partout. Mais le profil ACTIF reste celui d'avant, et c'est
        // le .ini du jeu qui recoit les liaisons, pas p1defaults.ini.
        const std::string keep = m_profiles.count(m_global_active_profile)
                               ? m_global_active_profile : m_active_profile_name;
        ControllerManager::save_profiles(m_profiles, keep, m_config_path);
        ControllerManager::set_game_profile(m_config_path, m_game_rom, m_active_profile_name);
        // MAME n'a pas de fichier par jeu a reecrire ici : le profil part au
        // lancement, dans le fichier controleur.
        if (m_game_mame) {
            std::cout << "[ControllerDialog] " << m_game_rom << " uses profile \""
                      << m_active_profile_name << "\" (applied at launch)\n";
            return;
        }
        const int n = ControllerManager::write_game_config(m_config, m_game_rom);
        ControllerManager::apply_analog_bindings(m_game_rom, m_config);
        std::cout << "[ControllerDialog] " << m_game_rom << " uses profile \""
                  << m_active_profile_name << "\""
                  << (n < 0 ? " (applied after the first launch)" : "") << "\n";
        return;
    }

    // Persist all profiles to config.json
    ControllerManager::save_profiles(m_profiles, m_active_profile_name, m_config_path);
    std::cout << "[ControllerDialog] Profiles saved (active: " << m_active_profile_name << ")\n";

    // Write FBNeo input config for the active profile
    std::string fbneo_dir = ControllerManager::get_fbneo_config_dir();
    ControllerManager::write_fbneo_config(m_config, fbneo_dir);
}

void ControllerDialog::set_game_scope(const std::string& fbneo_rom_name,
                                      const std::string& game_title,
                                      const std::string& game_profile,
                                      bool mame) {
    m_game_mame  = mame;
    m_game_rom   = fbneo_rom_name;
    m_game_title = game_title;
    m_header_title.set_markup("<b>" + Glib::Markup::escape_text(game_title) + "</b>");
    m_header_sub.set_text(_("Controls for this game only. Your default profile is not changed."));
    set_title(game_title);
    m_btn_use_default.show();
    // Un profil deja assigne s'ouvre tel quel ; sinon on part du defaut, qui
    // est ce que le jeu utilise reellement aujourd'hui.
    if (!game_profile.empty() && m_profiles.count(game_profile)
        && game_profile != m_active_profile_name) {
        m_profile_switching = true;
        m_profile_combo.set_active_id(game_profile);
        m_profile_switching = false;
        save_active_to_profiles();
        load_profile(game_profile);
    }
}


// ── Analog tab ────────────────────────────────────────────────────────────

Gtk::Widget* ControllerDialog::build_analog_section(int p) {
    /* Une SEULE grille pour les deux colonnes.
     *
     * Deux grilles independantes ne garantissaient rien : chacune calculait
     * ses hauteurs de ligne dans son coin, et les commandes de droite ne
     * tombaient pas en face de celles de gauche. Avec une grille unique, les
     * lignes 0, 1 et 2 sont les memes des deux cotes, et chaque controle
     * partage l'axe vertical de son voisin.
     *
     * L'ordre d'une ligne, identique partout :
     *   pictogramme -> intitule -> champ -> case -> << Invert >>
     */
    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(10);
    grid->set_column_spacing(12);
    grid->set_margin_top(14);
    grid->set_margin_start(4);

    static const char* kIcons[ANALOG_ROLE_COUNT] = {
        "bc-steering.svg", "bc-throttle.svg", "bc-brake.svg",
        "bc-aim-x.svg",    "bc-aim-y.svg",
    };

    // Volant et accelerateur a gauche, frein et visee a droite, comme la
    // maquette. La coupure se fait apres la deuxieme ligne.
    const int split = 2;
    const int kColStride = 5;   // 4 colonnes utiles plus le separateur

    auto* sep = Gtk::make_managed<Gtk::Separator>(Gtk::ORIENTATION_VERTICAL);
    sep->set_margin_start(10);
    sep->set_margin_end(10);
    grid->attach(*sep, 4, 0, 1, 3);

    for (int r = 0; r < ANALOG_ROLE_COUNT; ++r) {
        auto role = static_cast<AnalogRole>(r);
        auto& w   = m_analog_widgets[p][r];

        const int col = (r < split ? 0 : kColStride);
        const int row = (r < split ? r : r - split);

        auto* ico = glyph_cell(kIcons[r], false);
        grid->attach(*ico, col + 0, row, 1, 1);

        auto* name = Gtk::make_managed<Gtk::Label>(_(analog_role_name(role)));
        name->set_xalign(0.0f);
        name->set_valign(Gtk::ALIGN_CENTER);
        grid->attach(*name, col + 1, row, 1, 1);

        // Une seule valeur a remplir. L'ancienne version demandait une
        // source, un numero d'axe, une inversion et un mode : quatre reglages
        // a comprendre pour designer un stick, alors qu'il suffit de le
        // bouger.
        w.bind = Gtk::make_managed<Gtk::Button>(_("Not used"));
        w.bind->set_size_request(kFieldWidth, kRowHeight);
        w.bind->set_valign(Gtk::ALIGN_CENTER);
        w.bind->get_style_context()->add_class("bind-cell");
        w.bind->signal_clicked().connect(
            [this, p, role]() { capture_analog(p, role); });
        grid->attach(*w.bind, col + 2, row, 1, 1);

        w.invert = Gtk::make_managed<Gtk::CheckButton>(_("Invert"));
        w.invert->set_valign(Gtk::ALIGN_CENTER);
        w.invert->set_tooltip_text(_("Tick this if the control goes the wrong way."));
        w.invert->signal_toggled().connect(
            [this] { if (!m_analog_loading) analog_config_from_ui(); });
        grid->attach(*w.invert, col + 3, row, 1, 1);
    }
    return grid;
}

void ControllerDialog::analog_ui_from_config() {
    m_analog_loading = true;
    for (int p = 0; p < 2 && p < (int)m_config.players.size(); ++p) {
        for (int r = 0; r < ANALOG_ROLE_COUNT; ++r) {
            auto role = static_cast<AnalogRole>(r);
            auto& w = m_analog_widgets[p][r];
            if (!w.bind) continue;
            auto it = m_config.players[p].analog.find(role);
            // Le joueur 1 part des valeurs d'origine, le joueur 2 de rien :
            // une deuxieme manette est rarement branchee, et proposer des
            // liaisons pour un appareil absent trompe sur ce qui est actif.
            AnalogBinding b = it != m_config.players[p].analog.end() ? it->second
                            : (p == 0 ? default_analog_binding(role) : AnalogBinding{});
            w.value = b;
            w.bind->set_label(analog_label(b));
            w.invert->set_active(b.invert);
        }
    }
    m_analog_loading = false;
}

// Demande les deux touches d'un axe, l'une apres l'autre. Deux captures
// separees plutot qu'une saisie libre : le joueur appuie sur la touche qu'il
// veut vraiment, et on enregistre le code que l'emulateur attend sans lui
// demander de le connaitre.

// Lie une commande analogique en la faisant bouger, comme on lie un bouton en
// appuyant dessus. Un axe de manette suffit ; au clavier il en faut deux, une
// touche par sens, et l'axe doit revenir au centre quand on relache, ce que le
// code choisit tout seul plutot que de le demander.
void ControllerDialog::capture_analog(int player, AnalogRole role) {
    stop_binding();
    auto& w = m_analog_widgets[player][(int)role];

    const std::string device_path = m_config.players[player].device_path;
    m_bind_fd = device_path.empty() ? -1 : ControllerManager::open_device(device_path);
    if (m_bind_fd >= 0) {
        InputBinding drain;
        for (int i = 0; i < 50; ++i) if (!ControllerManager::poll_event(m_bind_fd, drain)) break;
    }

    Gtk::Dialog dlg(_(analog_role_name(role)), *this,
                    Gtk::DIALOG_MODAL | Gtk::DIALOG_DESTROY_WITH_PARENT);
    dlg.set_default_size(380, 150);
    dlg.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);

    auto* info = Gtk::make_managed<Gtk::Label>();
    info->set_markup("<b>" + Glib::Markup::escape_text(
        _("Move a stick or a trigger, or press a key…")) + "</b>");
    info->set_line_wrap(true);
    info->set_margin_top(20);
    info->set_margin_bottom(10);
    dlg.get_content_area()->add(*info);
    dlg.add_button(_("Not used"), Gtk::RESPONSE_REJECT);
    dlg.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dlg.show_all_children();

    AnalogBinding captured;
    int first_key = -1;
    std::string first_name;

    dlg.signal_key_press_event().connect(
        [&](GdkEventKey* ev) -> bool {
            if (ev->keyval == GDK_KEY_Escape) return false;
            const int code = ControllerManager::fbneo_key_from_gtk(ev->hardware_keycode);
            if (code < 0 || code == first_key) return true;
            if (first_key < 0) {
                first_key  = code;
                first_name = key_name_from_event(ev);
                info->set_markup("<b>" + Glib::Markup::escape_text(
                    _("Now the key for the other direction…")) + "</b>");
                return true;
            }
            captured.source   = AnalogSource::KEY_PAIR;
            captured.relative = true;          // le clavier ne tient pas une position
            captured.key_neg  = first_key;
            captured.key_neg_name = first_name;
            captured.key_pos  = code;
            captured.key_pos_name = key_name_from_event(ev);
            dlg.response(Gtk::RESPONSE_OK);
            return true;
        }, false);

    m_poll_conn = Glib::signal_timeout().connect([&]() -> bool {
        if (m_bind_fd < 0) return true;
        InputBinding event;
        if (ControllerManager::poll_event(m_bind_fd, event) && event.is_axis) {
            captured.source   = AnalogSource::JOY_AXIS;
            captured.index    = event.axis;
            captured.relative = false;         // la position du stick EST celle du volant
            dlg.response(Gtk::RESPONSE_OK);
            return false;
        }
        return true;
    }, 50);

    const int answer = dlg.run();
    stop_binding();

    if (answer == Gtk::RESPONSE_REJECT) captured = AnalogBinding{};
    else if (answer != Gtk::RESPONSE_OK || !captured.is_set()) return;

    captured.invert = w.invert->get_active();
    w.value = captured;
    w.bind->set_label(analog_label(captured));
    analog_config_from_ui();
}

void ControllerDialog::analog_config_from_ui() {
    for (int p = 0; p < 2 && p < (int)m_config.players.size(); ++p) {
        for (int r = 0; r < ANALOG_ROLE_COUNT; ++r) {
            auto& w = m_analog_widgets[p][r];
            if (!w.bind) continue;
            AnalogBinding b = w.value;
            b.invert = w.invert->get_active();
            m_config.players[p].analog[static_cast<AnalogRole>(r)] = b;
        }
    }
}


namespace {

/* Ce que chaque numero designe sur la manette branchee.
 *
 * Le pilote ne rend que des numeros. Sur une manette de la famille Xbox,
 * l'ordre est connu et stable, on peut donc ecrire << A >> ou << LB >> sans
 * mentir. Sur un modele inconnu, on affiche le numero : inventer un nom de
 * bouton serait pire que ne rien dire.
 */
struct PadLayout {
    bool named = false;
    std::vector<std::string> btn;   // libelle par numero de bouton
    int lx = 0, ly = 1, rx = 3, ry = 4;   // sticks
    int lt = 2, rt = 5;                   // gachettes analogiques
    int hx = 6, hy = 7;                   // chapeau directionnel
};

PadLayout layout_for(const std::string& preset, int nbtn, int naxis) {
    PadLayout l;
    if (preset == "Xbox" || preset == "Arcade") {
        // Encodeur XInput : chapeau sur 6 et 7, gachettes analogiques sur
        // 2 et 5, stick droit sur 3 et 4.
        l.named = true;
        l.btn = {"A", "B", "X", "Y", "LB", "RB", "Back", "Start",
                 "Guide", "LS", "RS"};
    } else if (preset == "PlayStation 3") {
        /* Releve sur la manette, pas deduit d'une documentation : le chapeau
         * occupe les axes 4 et 5, le stick droit le 3 en X et le 2 en Y, et
         * L2 et R2 sont des BOUTONS, pas des axes. Sans cela l'ecran montrait
         * le chapeau comme une gachette et renvoyait sa moitie horizontale
         * sur le stick droit. */
        l.named = true;
        l.btn = {"Triangle", "Circle", "Cross", "Square", "L1", "R1",
                 "L2", "R2", "Select", "Start", "L3", "R3"};
        l.rx = 3; l.ry = 2;
        l.hx = 4; l.hy = 5;
        l.lt = -1; l.rt = -1;
    }
    if ((int)l.btn.size() < nbtn)
        for (int i = (int)l.btn.size(); i < nbtn; ++i)
            l.btn.push_back(std::to_string(i));

    /* On ecarte ce que la manette n'a pas, index par index. Le decompte
     * global ne suffit pas : une manette a six axes peut parfaitement porter
     * son chapeau sur le 4 et le 5, et la regle << moins de huit axes, donc
     * pas de chapeau >> le faisait disparaitre. */
    for (int* idx : {&l.lx, &l.ly, &l.rx, &l.ry, &l.lt, &l.rt, &l.hx, &l.hy})
        if (*idx >= naxis) *idx = -1;
    return l;
}

// Couleur d'identite des quatre boutons de facade, quand ils sont nommes.
const char* face_color(const std::string& name) {
    if (name == "A") return "#43a047";
    if (name == "B") return "#d0342c";
    if (name == "X") return "#1f6fd0";
    if (name == "Y") return "#f2b01e";
    return nullptr;
}

struct Vec2 { double x = 0.0, y = 0.0; };

}  // namespace

/* Essayer la manette, sans rien enregistrer.
 *
 * Lier douze commandes a l'aveugle sur un modele inconnu est penible : on ne
 * sait pas quel numero porte quel bouton, ni si un axe repond. Cet ecran
 * montre l'etat brut du peripherique en direct. Il n'ecrit rien : ni profil,
 * ni liaison.
 */
void ControllerDialog::open_test_dialog(int p) {
    // Une capture en cours mangerait les evenements de l'ecran de test.
    stop_binding();

    const std::string path = m_config.players[p].device_path;
    std::string name = m_config.players[p].device_name;
    int nbtn = 0, naxis = 0;
    for (const auto& d : m_devices)
        if (d.path == path) { name = d.name; nbtn = d.num_buttons; naxis = d.num_axes; break; }
    if (nbtn <= 0) nbtn = 12;
    if (naxis <= 0) naxis = 8;
    if (nbtn > 24) nbtn = 24;
    if (naxis > 12) naxis = 12;

    /* Le prereglage choisi commande le dessin ET la lecture des entrees.
     *
     * L'ecran se fiait au seul nom rendu par le pilote : sur une manette qui
     * s'annonce << USB Gamepad >>, il affichait donc la silhouette generique
     * et lisait les axes a la mode Xbox, alors que le joueur avait justement
     * designe PlayStation 3 juste a cote. */
    const PadFamily detected = pad_family_for(name.empty() ? std::string("?") : name);
    std::string chosen_preset = m_config.players[p].preset;
    if (m_preset_combos[p]) {
        const std::string live = m_preset_combos[p]->get_active_id().raw();
        if (!live.empty()) chosen_preset = live;
    }
    const PadFamily* by_preset = family_for_preset(chosen_preset);
    const PadFamily& fam = by_preset ? *by_preset : detected;
    const PadLayout lay = layout_for(fam.preset ? fam.preset : "", nbtn, naxis);
    const int fd = path.empty() ? -1 : ControllerManager::open_device(path);

    Gtk::Dialog dlg(_("Controller Testing"), *this,
                    Gtk::DIALOG_MODAL | Gtk::DIALOG_DESTROY_WITH_PARENT);
    dlg.get_style_context()->add_class("cc-window");
    dlg.set_default_size(1120, -1);
    dlg.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);

    // Meme traitement de fenetre que partout ailleurs : une barre de titre
    // cote client, sa marque a gauche, son unique bouton de fermeture.
    auto* hb = Gtk::make_managed<Gtk::HeaderBar>();
    {
        auto* ico = SettingsUi::image("bc-logo-pad.svg", 26);
        ico->set_valign(Gtk::ALIGN_CENTER);
        auto* t1 = Gtk::make_managed<Gtk::Label>();
        t1->set_markup("<b>" + Glib::Markup::escape_text(_("Controller Testing")) + "</b>");
        t1->set_xalign(0.0f);
        t1->get_style_context()->add_class("cc-title");
        auto* t2 = Gtk::make_managed<Gtk::Label>(_("Test your controller inputs in real time"));
        t2->set_xalign(0.0f);
        t2->get_style_context()->add_class("cc-sub");
        auto* col = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
        col->set_valign(Gtk::ALIGN_CENTER);
        col->pack_start(*t1, Gtk::PACK_SHRINK);
        col->pack_start(*t2, Gtk::PACK_SHRINK);
        auto* brand = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 11);
        brand->pack_start(*ico, Gtk::PACK_SHRINK);
        brand->pack_start(*col, Gtk::PACK_SHRINK);
        hb->set_show_close_button(true);
        hb->pack_start(*brand);
        hb->set_custom_title(*Gtk::make_managed<Gtk::Box>());
        dlg.set_titlebar(*hb);
        hb->show_all();
        dlg.set_title(_("Controller Testing"));
    }

    auto* box = dlg.get_content_area();
    box->set_spacing(0);

    // ── Barre : quelle manette, son etat, quel profil ────────────────────
    {
        auto* bar = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
        bar->get_style_context()->add_class("cc-topbar");
        bar->pack_start(*Gtk::make_managed<Gtk::Label>(_("Controller")), Gtk::PACK_SHRINK);

        auto* combo = Gtk::make_managed<Gtk::ComboBoxText>();
        combo->append("", _("None "));
        for (const auto& d : m_devices) combo->append(d.path, d.name);
        if (!path.empty()) combo->set_active_id(path);
        else               combo->set_active(0);
        combo->set_sensitive(false);   // on essaie la manette du joueur, pas une autre
        combo->set_size_request(300, -1);
        bar->pack_start(*combo, Gtk::PACK_SHRINK);

        auto* dot = Gtk::make_managed<Gtk::Label>("");
        dot->get_style_context()->add_class("conn-dot");
        dot->get_style_context()->add_class(fd >= 0 ? "on" : "off");
        dot->set_valign(Gtk::ALIGN_CENTER);
        auto* st = Gtk::make_managed<Gtk::Label>(fd >= 0 ? _("Connected") : _("Not connected"));
        st->get_style_context()->add_class("cc-conn");
        st->get_style_context()->add_class(fd >= 0 ? "on" : "off");
        auto* cbox = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 7);
        cbox->pack_start(*dot, Gtk::PACK_SHRINK);
        cbox->pack_start(*st, Gtk::PACK_SHRINK);
        bar->pack_start(*cbox, Gtk::PACK_SHRINK);

        auto* pl = Gtk::make_managed<Gtk::Label>(_("Profile"));
        pl->set_margin_start(18);
        bar->pack_end(*Gtk::make_managed<Gtk::Label>(""), Gtk::PACK_EXPAND_WIDGET);
        auto* pc = Gtk::make_managed<Gtk::ComboBoxText>();
        pc->append(m_active_profile_name, m_active_profile_name);
        pc->set_active(0);
        pc->set_sensitive(false);
        pc->set_size_request(220, -1);
        bar->pack_start(*pl, Gtk::PACK_SHRINK);
        bar->pack_start(*pc, Gtk::PACK_SHRINK);
        box->pack_start(*bar, Gtk::PACK_SHRINK);
    }

    auto* main_row = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 16);
    main_row->set_margin_start(20);
    main_row->set_margin_end(20);
    main_row->set_margin_top(18);

    // ── A gauche : la manette reconnue ───────────────────────────────────
    {
        auto* col = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 12);
        auto* ttl = Gtk::make_managed<Gtk::Label>();
        ttl->set_markup("<b>" + Glib::Markup::escape_text(
            name.empty() ? std::string(_("No controller")) : name) + "</b>");
        ttl->set_line_wrap(true);
        ttl->get_style_context()->add_class("cc-pad-name");
        col->pack_start(*ttl, Gtk::PACK_SHRINK);

        auto* art = Gtk::make_managed<Gtk::Image>();
        art->set_size_request(kArtWidth, kArtHeight);
        art->set_halign(Gtk::ALIGN_CENTER);
        art->set_valign(Gtk::ALIGN_CENTER);
        Glib::RefPtr<Gdk::Pixbuf> pb =
            IconManager::load(std::string("controllers/") + fam.photo, kArtWidth, kArtHeight);
        if (!pb && fam.alt)
            pb = IconManager::load(std::string("controllers/") + fam.alt, kArtWidth, kArtHeight);
        if (!pb)
            pb = IconManager::load(std::string("icons/") + fam.svg, kArtWidth, kArtHeight);
        art->set(pb);
        col->pack_start(*art, Gtk::PACK_EXPAND_WIDGET);

        auto* card_l = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
        card_l->pack_start(*col, Gtk::PACK_EXPAND_WIDGET);
        card_l->get_style_context()->add_class("cc-card");
        card_l->set_size_request(400, -1);
        main_row->pack_start(*card_l, Gtk::PACK_SHRINK);
    }

    // ── A droite : l'etat des entrees, en direct ─────────────────────────
    std::vector<Gtk::Label*> lamps(nbtn, nullptr);
    std::vector<Gtk::Label*> dpad(4, nullptr);          // haut bas gauche droite
    auto lstick = std::make_shared<Vec2>();
    auto rstick = std::make_shared<Vec2>();
    Gtk::DrawingArea* lpad = nullptr;
    Gtk::DrawingArea* rpad = nullptr;
    Gtk::Label *lxy = nullptr, *rxy = nullptr;
    Gtk::LevelBar *ltb = nullptr, *rtb = nullptr;
    Gtk::Label *ltv = nullptr, *rtv = nullptr;
    {
        auto* col = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 16);

        auto* head = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
        auto* h = Gtk::make_managed<Gtk::Label>(_("Input status"));
        h->get_style_context()->add_class("cc-card-title");
        head->pack_start(*h, Gtk::PACK_SHRINK);
        auto* live_dot = Gtk::make_managed<Gtk::Label>("");
        live_dot->get_style_context()->add_class("conn-dot");
        live_dot->get_style_context()->add_class(fd >= 0 ? "on" : "off");
        live_dot->set_valign(Gtk::ALIGN_CENTER);
        auto* live = Gtk::make_managed<Gtk::Label>(_("Live"));
        live->get_style_context()->add_class("cc-conn");
        live->get_style_context()->add_class(fd >= 0 ? "on" : "off");
        head->pack_end(*live, Gtk::PACK_SHRINK);
        head->pack_end(*live_dot, Gtk::PACK_SHRINK);
        col->pack_start(*head, Gtk::PACK_SHRINK);

        // Pastilles de boutons : six par rangee.
        auto* grid = Gtk::make_managed<Gtk::Grid>();
        grid->set_row_spacing(10);
        grid->set_column_spacing(10);
        for (int i = 0; i < nbtn; ++i) {
            auto* lbl = Gtk::make_managed<Gtk::Label>(lay.btn[i]);
            lbl->set_halign(Gtk::ALIGN_CENTER);
            lbl->set_valign(Gtk::ALIGN_CENTER);
            auto* cell = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 0);
            cell->set_size_request(46, 46);
            cell->pack_start(*lbl, Gtk::PACK_EXPAND_WIDGET);
            auto ctx = cell->get_style_context();
            ctx->add_class("cc-lamp");
            if (const char* c = lay.named ? face_color(lay.btn[i]) : nullptr) {
                if (std::string(c) == "#43a047") ctx->add_class("face-a");
                if (std::string(c) == "#d0342c") ctx->add_class("face-b");
                if (std::string(c) == "#1f6fd0") ctx->add_class("face-x");
                if (std::string(c) == "#f2b01e") ctx->add_class("face-y");
            }
            grid->attach(*cell, i % 6, i / 6, 1, 1);
            lamps[i] = lbl;
        }
        col->pack_start(*grid, Gtk::PACK_SHRINK);

        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 22);

        // Croix directionnelle, lue sur le chapeau.
        {
            auto* g = Gtk::make_managed<Gtk::Grid>();
            g->set_row_spacing(6);
            g->set_column_spacing(6);
            static const char* kIco[4] = {"bc-up.svg", "bc-down.svg",
                                          "bc-left.svg", "bc-right.svg"};
            static const int kPos[4][2] = {{1, 0}, {1, 2}, {0, 1}, {2, 1}};
            for (int i = 0; i < 4; ++i) {
                auto* im = SettingsUi::image(kIco[i], 18);
                im->set_halign(Gtk::ALIGN_CENTER);
                im->set_valign(Gtk::ALIGN_CENTER);
                auto* cell = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 0);
                cell->set_size_request(44, 44);
                cell->pack_start(*im, Gtk::PACK_EXPAND_WIDGET);
                cell->get_style_context()->add_class("cc-lamp");
                cell->get_style_context()->add_class("round");
                g->attach(*cell, kPos[i][0], kPos[i][1], 1, 1);
                // On retient la boite : c'est elle qui porte la classe allumee.
                dpad[i] = Gtk::make_managed<Gtk::Label>("");
                dpad[i]->set_data("cell", cell);
            }
            auto* wrap = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 6);
            wrap->pack_start(*g, Gtk::PACK_SHRINK);
            wrap->set_valign(Gtk::ALIGN_CENTER);
            if (lay.hx >= 0) row->pack_start(*wrap, Gtk::PACK_SHRINK);
        }

        // Les deux sticks : un carre, une croix, un point.
        auto make_pad = [](const char* title, std::shared_ptr<Vec2> st,
                           Gtk::DrawingArea** out, Gtk::Label** val) {
            auto* col2 = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 7);
            auto* t = Gtk::make_managed<Gtk::Label>(_(title));
            t->get_style_context()->add_class("cc-sub");
            col2->pack_start(*t, Gtk::PACK_SHRINK);

            auto* da = Gtk::make_managed<Gtk::DrawingArea>();
            da->set_size_request(116, 116);
            da->signal_draw().connect([st](const Cairo::RefPtr<Cairo::Context>& cr) {
                const double w = 116, h = 116, r = 10;
                cr->set_line_width(1.0);
                cr->set_source_rgb(0.047, 0.055, 0.082);       // #0c0e15
                cr->begin_new_sub_path();
                cr->arc(w - r - 0.5, r + 0.5, r, -M_PI / 2, 0);
                cr->arc(w - r - 0.5, h - r - 0.5, r, 0, M_PI / 2);
                cr->arc(r + 0.5, h - r - 0.5, r, M_PI / 2, M_PI);
                cr->arc(r + 0.5, r + 0.5, r, M_PI, 3 * M_PI / 2);
                cr->close_path();
                cr->fill_preserve();
                cr->set_source_rgb(0.165, 0.184, 0.239);       // #2a2f3d
                cr->stroke();
                cr->move_to(w / 2, 8); cr->line_to(w / 2, h - 8);
                cr->move_to(8, h / 2); cr->line_to(w - 8, h / 2);
                cr->stroke();
                const double cx = w / 2 + st->x * (w / 2 - 12);
                const double cy = h / 2 + st->y * (h / 2 - 12);
                cr->set_source_rgb(0.486, 0.420, 1.0);         // #7c6bff
                cr->arc(cx, cy, 6.5, 0, 2 * M_PI);
                cr->fill();
                return true;
            });
            col2->pack_start(*da, Gtk::PACK_SHRINK);
            *out = da;

            auto* v = Gtk::make_managed<Gtk::Label>("X: 0   Y: 0");
            v->get_style_context()->add_class("cc-sub");
            col2->pack_start(*v, Gtk::PACK_SHRINK);
            *val = v;
            return col2;
        };
        if (lay.lx >= 0 && lay.ly >= 0)
            row->pack_start(*make_pad(N_("Left stick"), lstick, &lpad, &lxy), Gtk::PACK_SHRINK);
        if (lay.rx >= 0 && lay.ry >= 0)
            row->pack_start(*make_pad(N_("Right stick"), rstick, &rpad, &rxy), Gtk::PACK_SHRINK);
        col->pack_start(*row, Gtk::PACK_SHRINK);

        // Gachettes analogiques.
        auto trig = [&](const char* label, Gtk::LevelBar** bar, Gtk::Label** val) {
            auto* r = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 12);
            auto* l = Gtk::make_managed<Gtk::Label>(_(label));
            l->set_xalign(0.0f);
            l->set_size_request(150, -1);
            r->pack_start(*l, Gtk::PACK_SHRINK);
            auto* b = Gtk::make_managed<Gtk::LevelBar>();
            b->set_min_value(0.0); b->set_max_value(1.0); b->set_value(0.0);
            b->set_hexpand(true);
            b->set_valign(Gtk::ALIGN_CENTER);
            r->pack_start(*b, Gtk::PACK_EXPAND_WIDGET);
            *bar = b;
            auto* v = Gtk::make_managed<Gtk::Label>("0%");
            v->set_xalign(1.0f);
            v->set_size_request(52, -1);
            v->get_style_context()->add_class("cc-sub");
            r->pack_start(*v, Gtk::PACK_SHRINK);
            *val = v;
            return r;
        };
        if (lay.lt >= 0) col->pack_start(*trig(lay.named ? N_("Left trigger (LT)") : N_("Axis 2"),
                                               &ltb, &ltv), Gtk::PACK_SHRINK);
        if (lay.rt >= 0) col->pack_start(*trig(lay.named ? N_("Right trigger (RT)") : N_("Axis 5"),
                                               &rtb, &rtv), Gtk::PACK_SHRINK);

        auto* card_r = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
        card_r->pack_start(*col, Gtk::PACK_EXPAND_WIDGET);
        card_r->get_style_context()->add_class("cc-card");
        card_r->set_hexpand(true);
        main_row->pack_start(*card_r, Gtk::PACK_EXPAND_WIDGET);
    }
    box->pack_start(*main_row, Gtk::PACK_SHRINK);

    // ── Bandeau d'information ────────────────────────────────────────────
    {
        auto* col = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 12);
        auto* head = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 9);
        head->pack_start(*icon("bc-info.svg", kSectionIcon), Gtk::PACK_SHRINK);
        auto* h = Gtk::make_managed<Gtk::Label>(_("Test information"));
        h->get_style_context()->add_class("cc-card-title");
        head->pack_start(*h, Gtk::PACK_SHRINK);
        col->pack_start(*head, Gtk::PACK_SHRINK);

        auto* sub = Gtk::make_managed<Gtk::Label>(
            fd >= 0 ? _("Press any button or move a stick or trigger to see the "
                        "input in real time. Nothing is saved from this screen.")
                    : _("No controller is open, so nothing will move here."));
        sub->set_xalign(0.0f);
        sub->set_line_wrap(true);
        sub->get_style_context()->add_class("cc-sub");
        col->pack_start(*sub, Gtk::PACK_SHRINK);

        int mapped = 0, offered = 0;
        for (int a = 0; a < GAME_ACTION_COUNT; ++a) {
            if (!action_shown(p, static_cast<GameAction>(a))) continue;
            ++offered;
            if (m_config.players[p].bindings.count(static_cast<GameAction>(a))) ++mapped;
        }

        auto tile = [this](const char* ico, const std::string& t, const std::string& v) {
            auto* b = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 11);
            auto* badge = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 0);
            auto* im = icon(ico, 19);
            im->set_halign(Gtk::ALIGN_CENTER);
            im->set_valign(Gtk::ALIGN_CENTER);
            badge->set_size_request(kCellSize, kCellSize);
            badge->pack_start(*im, Gtk::PACK_EXPAND_WIDGET);
            badge->get_style_context()->add_class("cc-badge");
            badge->set_valign(Gtk::ALIGN_CENTER);
            b->pack_start(*badge, Gtk::PACK_SHRINK);
            auto* c = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 2);
            auto* tl = Gtk::make_managed<Gtk::Label>();
            tl->set_markup("<b>" + Glib::Markup::escape_text(t) + "</b>");
            tl->set_xalign(0.0f);
            auto* vl = Gtk::make_managed<Gtk::Label>(v);
            vl->set_xalign(0.0f);
            vl->set_ellipsize(Pango::ELLIPSIZE_END);
            vl->get_style_context()->add_class("cc-sub");
            c->pack_start(*tl, Gtk::PACK_SHRINK);
            c->pack_start(*vl, Gtk::PACK_SHRINK);
            b->pack_start(*c, Gtk::PACK_EXPAND_WIDGET);
            b->get_style_context()->add_class("cc-subcard");
            b->set_hexpand(true);
            return b;
        };
        auto* tiles = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 12);
        tiles->set_homogeneous(true);
        tiles->pack_start(*tile(fd >= 0 ? "bc-detected.svg" : "bc-info.svg",
                                fd >= 0 ? _("Controller detected") : _("No controller"),
                                name.empty() ? std::string("—") : name),
                          Gtk::PACK_EXPAND_WIDGET);
        tiles->pack_start(*tile("bc-system.svg", _("Active profile"), m_active_profile_name),
                          Gtk::PACK_EXPAND_WIDGET);
        tiles->pack_start(*tile("bc-identify.svg", _("Mapped inputs"),
                                std::to_string(mapped) + " / " +
                                std::to_string(offered)),
                          Gtk::PACK_EXPAND_WIDGET);
        col->pack_start(*tiles, Gtk::PACK_SHRINK);

        auto* card_b = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
        card_b->pack_start(*col, Gtk::PACK_SHRINK);
        card_b->get_style_context()->add_class("cc-card");
        card_b->set_margin_start(20);
        card_b->set_margin_end(20);
        card_b->set_margin_top(16);
        card_b->set_margin_bottom(18);
        box->pack_start(*card_b, Gtk::PACK_SHRINK);
    }

    // ── Pied ─────────────────────────────────────────────────────────────
    auto* clear = Gtk::make_managed<Gtk::Button>(_("Clear all inputs"));
    clear->set_image(*SettingsUi::image("bc-restore.svg", 18));
    clear->set_always_show_image(true);

    auto* back = Gtk::make_managed<Gtk::Button>(_("Back to configuration"));
    back->set_image(*SettingsUi::image("bc-system.svg", 18));
    back->set_always_show_image(true);
    back->signal_clicked().connect([&dlg] { dlg.response(Gtk::RESPONSE_CLOSE); });

    auto* done = Gtk::make_managed<Gtk::Button>(_("Done"));
    done->set_image(*SettingsUi::image("bc-detected.svg", 18));
    done->set_always_show_image(true);
    done->get_style_context()->add_class("accent-button");
    done->signal_clicked().connect([&dlg] { dlg.response(Gtk::RESPONSE_OK); });

    auto* foot = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    foot->get_style_context()->add_class("cc-footer");
    foot->pack_start(*clear, Gtk::PACK_SHRINK);
    foot->pack_end(*done, Gtk::PACK_SHRINK);
    foot->pack_end(*back, Gtk::PACK_SHRINK);
    box->pack_start(*foot, Gtk::PACK_SHRINK);

    // ── Remise a zero de l'affichage ─────────────────────────────────────
    auto reset_view = [&]() {
        for (auto* l : lamps)
            if (l && l->get_parent())
                l->get_parent()->get_style_context()->remove_class("on");
        for (auto* d : dpad)
            if (d)
                if (auto* c = static_cast<Gtk::Widget*>(d->get_data("cell")))
                    c->get_style_context()->remove_class("on");
        lstick->x = lstick->y = rstick->x = rstick->y = 0.0;
        if (lpad) lpad->queue_draw();
        if (rpad) rpad->queue_draw();
        if (lxy) lxy->set_text("X: 0   Y: 0");
        if (rxy) rxy->set_text("X: 0   Y: 0");
        if (ltb) { ltb->set_value(0.0); ltv->set_text("0%"); }
        if (rtb) { rtb->set_value(0.0); rtv->set_text("0%"); }
    };
    clear->signal_clicked().connect(reset_view);

    /* Le rafraichissement lit tout ce qui attend, a chaque tour : la file du
     * pilote se remplit vite quand on remue un stick, et n'en prendre qu'un
     * evenement par tour ferait prendre du retard a l'affichage. */
    sigc::connection tick;
    if (fd >= 0) {
        tick = Glib::signal_timeout().connect([&, fd]() {
            ControllerManager::RawInput in;
            int guard = 0;
            bool redraw_l = false, redraw_r = false;
            while (ControllerManager::poll_raw(fd, in) && ++guard < 400) {
                if (!in.is_axis) {
                    if (in.index < 0 || in.index >= (int)lamps.size()) continue;
                    if (auto* par = lamps[in.index]->get_parent()) {
                        auto ctx = par->get_style_context();
                        if (in.value) ctx->add_class("on");
                        else          ctx->remove_class("on");
                    }
                    continue;
                }
                const double f = in.value / 32767.0;
                if (in.index == lay.lx) { lstick->x = f; redraw_l = true; }
                else if (in.index == lay.ly) { lstick->y = f; redraw_l = true; }
                else if (in.index == lay.rx) { rstick->x = f; redraw_r = true; }
                else if (in.index == lay.ry) { rstick->y = f; redraw_r = true; }
                else if (in.index == lay.lt && ltb) {
                    const double v = (f + 1.0) / 2.0;
                    ltb->set_value(v);
                    ltv->set_text(std::to_string((int)(v * 100)) + "%");
                } else if (in.index == lay.rt && rtb) {
                    const double v = (f + 1.0) / 2.0;
                    rtb->set_value(v);
                    rtv->set_text(std::to_string((int)(v * 100)) + "%");
                } else if (in.index == lay.hx || in.index == lay.hy) {
                    const bool horiz = (in.index == lay.hx);
                    const int neg = horiz ? 2 : 0;   // gauche / haut
                    const int pos = horiz ? 3 : 1;   // droite / bas
                    auto set = [&](int i, bool on) {
                        if (!dpad[i]) return;
                        if (auto* c = static_cast<Gtk::Widget*>(dpad[i]->get_data("cell")))
                            on ? c->get_style_context()->add_class("on")
                               : c->get_style_context()->remove_class("on");
                    };
                    set(neg, in.value < -8000);
                    set(pos, in.value >  8000);
                }
            }
            if (redraw_l && lpad) {
                lpad->queue_draw();
                lxy->set_text("X: " + std::to_string((int)(lstick->x * 100)) +
                              "   Y: " + std::to_string((int)(lstick->y * 100)));
            }
            if (redraw_r && rpad) {
                rpad->queue_draw();
                rxy->set_text("X: " + std::to_string((int)(rstick->x * 100)) +
                              "   Y: " + std::to_string((int)(rstick->y * 100)));
            }
            return true;
        }, 16);
    }

    dlg.show_all_children();
    dlg.run();

    // On coupe AVANT de rendre la main : le rafraichissement touche des
    // widgets qui vivent sur la pile de cette fonction.
    if (tick.connected()) tick.disconnect();
    if (fd >= 0) ::close(fd);
}

// ── Identify a controller by pressing a button on it ──────────────────────

void ControllerDialog::identify_device(int p) {
    stop_binding();

    m_devices = ControllerManager::list_devices();
    if (m_devices.empty()) {
        SettingsUi::notice(*this, _("No controller found"),
                           _("Plug a controller in, then press Identify again."),
                           "bc-warning.svg");
        return;
    }

    // Every device is opened at once and watched together: the whole point is
    // that we do not know which one the player is holding, so we cannot ask
    // them to pick it first.
    std::vector<int> fds;
    fds.reserve(m_devices.size());
    for (const auto& d : m_devices) fds.push_back(ControllerManager::open_device(d.path));

    // Drain events queued before the dialog opened, or a stick resting off
    // centre would answer instantly and pick the wrong pad.
    for (int fd : fds) {
        if (fd < 0) continue;
        InputBinding dummy;
        for (int i = 0; i < 50; ++i) if (!ControllerManager::poll_event(fd, dummy)) break;
    }

    Gtk::Dialog wait_dlg(_("Identify controller"), *this,
                         Gtk::DIALOG_MODAL | Gtk::DIALOG_DESTROY_WITH_PARENT);
    wait_dlg.set_default_size(360, 150);
    wait_dlg.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);

    auto* info = Gtk::make_managed<Gtk::Label>();
    info->set_markup("<b>" + Glib::Markup::escape_text(Glib::ustring::compose(
        _("Press a button on the controller you want for player %1…"), p + 1)) + "</b>");
    info->set_line_wrap(true);
    info->set_justify(Gtk::JUSTIFY_CENTER);
    info->set_halign(Gtk::ALIGN_CENTER);
    info->set_margin_top(20); info->set_margin_bottom(10);
    wait_dlg.get_content_area()->add(*info);
    wait_dlg.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    wait_dlg.show_all_children();

    int found = -1;
    auto conn = Glib::signal_timeout().connect(
        [this, &fds, &found, &wait_dlg]() -> bool {
            for (size_t i = 0; i < fds.size(); ++i) {
                if (fds[i] < 0) continue;
                InputBinding result;
                if (ControllerManager::poll_event(fds[i], result)) {
                    found = (int)i;
                    wait_dlg.response(Gtk::RESPONSE_OK);
                    return false;
                }
            }
            return true;
        }, 50);

    wait_dlg.run();
    if (conn.connected()) conn.disconnect();
    for (int fd : fds) if (fd >= 0) ControllerManager::close_device(fd);

    if (found < 0) return;                 // cancelled, or nothing pressed
    // Selecting in the combo fires on_device_changed, which is what actually
    // records the choice : no need to touch m_config here.
    if (m_device_combos[p]) m_device_combos[p]->set_active_id(m_devices[found].path);
}


/* Assistant de configuration.
 *
 * Enchaine les douze commandes dans l'ordre de l'enumeration, qui est aussi
 * l'ordre naturel d'une manette : directions, boutons, Start, Coin. La
 * capture existante se ferme d'elle-meme des qu'une entree est detectee,
 * l'enchainement est donc automatique sans code supplementaire.
 *
 * Trois garanties, apprises en ecrivant le plan :
 *  - on PEUT passer une commande. Tous les pads n'ont pas six boutons, et
 *    bloquer sur « Button 6 » condamnerait l'assistant a moitie parcours ;
 *  - rien n'est ecrit dans le profil avant la fin. On part d'une copie et on
 *    restitue l'ancienne configuration si le joueur renonce ;
 *  - le resume final dit ce qui a ete lie, parce qu'apres douze pressions on
 *    ne se souvient plus de ce qu'on a saute.
 */
void ControllerDialog::run_auto_configure(int p) {
    const PlayerConfig backup = m_config.players[p];

    m_config.players[p].bindings.clear();
    refresh_bindings(p);

    int bound = 0, skipped = 0;
    for (int a = 0; a < GAME_ACTION_COUNT; ++a) {
        const GameAction action = static_cast<GameAction>(a);
        // Pas de bouton 12 a demander sur une manette qui en a huit.
        if (!action_shown(p, action)) continue;
        start_binding(p, action);
        // La capture ne rend rien : on regarde si elle a effectivement pose
        // une liaison. Absente, c'est que le joueur a ferme la fenetre, ce
        // qu'on interprete comme « cette commande, je la saute ».
        if (m_config.players[p].bindings.count(action)) ++bound;
        else                                            ++skipped;
    }

    const bool keep = SettingsUi::offer(*this, _("Keep this configuration?"),
        Glib::ustring::compose(_("%1 controls bound, %2 skipped."), bound, skipped),
        _("Keep"), "bc-controller.svg", "bc-check.svg");
    if (!keep) {
        m_config.players[p] = backup;   // l'ancienne configuration revient intacte
    }
    refresh_bindings(p);
}


/* Une icone Bootcade, jamais une icone du theme.
 *
 * Les pictogrammes du bureau donnent immediatement l'air d'un dialogue
 * GNOME standard, ce qui est exactement ce que la maquette evite. Tous les
 * assets viennent de assets/icons/bc-*.svg et forment une seule famille :
 * meme grille, meme epaisseur de trait, meme blanc.
 */
Gtk::Widget* ControllerDialog::icon(const std::string& file, int px) {
    auto* img = SettingsUi::image(file, px);
    img->set_valign(Gtk::ALIGN_CENTER);
    return img;
}

/* Un pictogramme dans une case carree, centre par la geometrie.
 *
 * L'image est empaquetee en EXPAND_WIDGET avec un alignement centre sur les
 * deux axes : GTK la pose donc au milieu exact de la case, quelle que soit
 * sa taille. Aucune marge n'intervient, et les quatre directions partagent
 * strictement le meme conteneur.
 */
Gtk::Widget* ControllerDialog::glyph_cell(const std::string& file, bool tile) {
    auto* img = SettingsUi::image(file, kCellIcon);
    img->set_halign(Gtk::ALIGN_CENTER);
    img->set_valign(Gtk::ALIGN_CENTER);

    auto* cell = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 0);
    cell->set_size_request(kCellSize, kCellSize);
    cell->pack_start(*img, Gtk::PACK_EXPAND_WIDGET);
    if (tile) cell->get_style_context()->add_class("cc-glyph");
    cell->set_valign(Gtk::ALIGN_CENTER);
    cell->set_halign(Gtk::ALIGN_CENTER);
    return cell;
}

/* La pastille numerotee d'un bouton.
 *
 * Le chiffre etait grave dans le SVG : son centrage dependait alors du trace
 * et de la ligne de base de la police. Il est desormais une etiquette centree
 * dans un disque de taille fixe, ce qui le place au milieu exact quels que
 * soient le chiffre et la police.
 */
Gtk::Widget* ControllerDialog::number_badge(int n) {
    auto* lbl = Gtk::make_managed<Gtk::Label>(std::to_string(n));
    lbl->set_halign(Gtk::ALIGN_CENTER);
    lbl->set_valign(Gtk::ALIGN_CENTER);
    lbl->get_style_context()->add_class("cc-num-label");

    auto* disc = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 0);
    disc->set_size_request(kBadgeSize, kBadgeSize);
    disc->pack_start(*lbl, Gtk::PACK_EXPAND_WIDGET);
    disc->get_style_context()->add_class("cc-num");
    // Six couleurs pour les six boutons de la borne ; au-dela, une pastille
    // neutre plutot que des couleurs qui se repeteraient.
    disc->get_style_context()->add_class(n <= BASE_GAME_BUTTONS
                                         ? "cc-num-" + std::to_string(n)
                                         : std::string("cc-num-extra"));
    disc->set_halign(Gtk::ALIGN_CENTER);
    disc->set_valign(Gtk::ALIGN_CENTER);

    auto* cell = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 0);
    cell->set_size_request(kCellSize, kCellSize);
    cell->pack_start(*disc, Gtk::PACK_EXPAND_WIDGET);
    cell->set_valign(Gtk::ALIGN_CENTER);
    return cell;
}

/* Une carte titree, comme celles de la maquette.
 *
 * Titre avec son pictogramme, puis le contenu, le tout dans un cadre
 * arrondi. C'est ce qui remplace les separateurs horizontaux d'un
 * formulaire empile et donne la hierarchie visuelle du modele.
 */
Gtk::Widget* ControllerDialog::card(const std::string& title,
                                    const std::string& icon_file,
                                    Gtk::Widget& body,
                                    const std::string& subtitle) {
    auto* box  = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 10);
    auto* head = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    auto* lbl  = Gtk::make_managed<Gtk::Label>();
    lbl->set_text(title);
    lbl->set_xalign(0.0f);
    lbl->get_style_context()->add_class("cc-card-title");
    lbl->set_valign(Gtk::ALIGN_CENTER);
    auto* head_ico = icon(icon_file, kSectionIcon);
    head_ico->set_valign(Gtk::ALIGN_CENTER);
    head->pack_start(*head_ico, Gtk::PACK_SHRINK);
    head->pack_start(*lbl, Gtk::PACK_SHRINK);
    box->pack_start(*head, Gtk::PACK_SHRINK);
    if (!subtitle.empty()) {
        auto* sub = Gtk::make_managed<Gtk::Label>(subtitle);
        sub->set_xalign(0.0f);
        sub->set_line_wrap(true);
        sub->get_style_context()->add_class("cc-sub");
        sub->set_margin_start(30);
        box->pack_start(*sub, Gtk::PACK_SHRINK);
    }
    box->pack_start(body, Gtk::PACK_SHRINK);
    box->get_style_context()->add_class("cc-card");
    box->set_valign(Gtk::ALIGN_START);
    return box;
}
