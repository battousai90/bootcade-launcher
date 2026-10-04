// src/SettingsUi.h
//
// Les briques communes des ecrans de Bootcade : reglages, manette, gestion
// des ROMs.
//
// Ces ecrans montrent les memes objets : des cartes titrees, des lignes
// « pictogramme / intitule / explication / controle », des tuiles portant une
// icone, des boutons a pictogramme et des pastilles d'etat ; les ecrans de
// donnees y ajoutent des pastilles-compteurs, une barre de filtres, une table,
// un journal et un panneau de detail. Les fabriquer ici une fois est la seule
// facon d'obtenir des ecrans qui appartiennent au meme systeme : une hauteur
// de ligne corrigee a un endroit se corrige partout, et un ecran ajoute plus
// tard nait deja conforme.
//
// La matiere vient du CSS (.set-* dans style-common.css et style-dark.css) ;
// ce fichier ne pose que la geometrie et l'assemblage. Aucune couleur n'est
// ecrite en dur ici.
#pragma once

#include <gtkmm.h>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace SettingsUi {

// Les mesures partagees. Un chiffre recopie a la main dans une page est un
// alignement qui finira par diverger des trois autres.
constexpr int kIconSection = 18;  // pictogramme d'un titre de carte
constexpr int kIconRow     = 17;  // pictogramme d'une ligne de reglage
constexpr int kIconButton  = 17;  // pictogramme dans un bouton
constexpr int kTileSection = 32;  // tuile carree portant le pictogramme de section
constexpr int kTileRow     = 28;
constexpr int kFieldWidth  = 300; // largeur d'une liste deroulante / d'un champ
constexpr int kCardSpacing = 11;  // entre deux cartes d'une page

/* Un pictogramme qui suit l'encre de son contexte.
 *
 * Les traces bc-*.svg sont blancs : rendus tels quels ils disparaissent sur
 * une surface claire. Ce widget relit la couleur CSS `color` de son propre
 * contexte (heritee du bouton, de la tuile, de l'etiquette qui le porte)
 * a chaque changement de style ou d'etat, et re-teinte le trace : encre
 * normale dans un bouton, attenuee dans une tuile (.set-tile), accent dans
 * une tuile accent, blanche sur un bouton principal. Un SVG qui porte ses
 * propres couleurs (manette, pastille) n'est pas teinte. */
class Icon : public Gtk::Image {
public:
    Icon(const std::string& icon_file, int size);
    // Change de trace (icone d'etat) : la teinte suit toujours le contexte.
    void set_file(const std::string& icon_file);
protected:
    void on_style_updated() override;
    void on_map() override;
    // The ink can change without any signal reaching this widget (a
    // notebook tab becoming :checked changes the colour of its label's
    // children through CSS only) : the colour is checked at each draw, and
    // the bitmap re-rendered only when it differs.
    bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr) override;
private:
    void retint();
    std::string m_subpath;
    int         m_size;
    bool        m_mono;
    Gdk::RGBA   m_colour;
    bool        m_painted = false;
};

// Une image chargee depuis assets/icons, deja centree, teintee par le
// contexte (voir Icon).
Gtk::Image* image(const std::string& icon_file, int size);
// Pose un pictogramme dans une Gtk::Image existante (icone d'etat qui
// change a l'execution), teinte avec l'encre courante de cette image.
void set_icon(Gtk::Image& target, const std::string& icon_file, int size);

// Le pictogramme dans son carre arrondi. `accent` peint la tuile en violet :
// reserve a l'en-tete de la fenetre, pour ne pas banaliser l'accent.
Gtk::Widget* tile(const std::string& icon_file, int icon_size, int box_size,
                  bool accent = false);

// Une carte : cadre arrondi, en-tete (tuile, titre, sous-titre, et de la place
// a droite pour des actions), puis un corps a remplir.
struct Card {
    Gtk::Box*   frame    = nullptr;  // ce qu'on empaquete dans la page
    Gtk::Box*   body     = nullptr;  // ou poser le contenu
    Gtk::Box*   head     = nullptr;  // pack_end pour une action alignee sur le titre
    Gtk::Label* title    = nullptr;  // pour retitrer la carte apres coup
    Gtk::Label* subtitle = nullptr;  // nul quand la carte n'en a pas
};
Card card(const std::string& icon_file, const std::string& title,
          const std::string& subtitle);

// Le cadre interieur qui regroupe des lignes, avec un filet entre chacune.
Gtk::Box* rows();
void      add_row(Gtk::Box* container, Gtk::Widget& row);

// Une ligne de reglage. `trailing` est le controle pose a droite ; il peut
// etre nul quand la ligne ne fait qu'informer.
Gtk::Widget* row(const std::string& icon_file, const std::string& title,
                 const std::string& subtitle, Gtk::Widget* trailing);

// Un bouton a pictogramme. `accent` donne le traitement du bouton principal
// (celui de Play et de Save), `danger` celui d'une action destructrice.
enum class Tone { Normal, Accent, Danger };
Gtk::Button* button(const std::string& label, const std::string& icon_file = "",
                    Tone tone = Tone::Normal);

// Un texte d'etat, precede ou non d'une pastille ronde.
enum class State { Ok, Warn, Error, Muted };
Gtk::Label*  status_label(const std::string& text, State state);
Gtk::Widget* status_dot(const std::string& text, State state);

// Les etiquettes courantes, pour ne pas repeter xalign + classe partout.
Gtk::Label* title_label(const std::string& text);
Gtk::Label* sub_label(const std::string& text);
Gtk::Label* card_title_label(const std::string& text);

// Un filet horizontal de 1 px, a la couleur des separateurs de l'ecran.
Gtk::Widget* hairline();

/* Retire un enfant gere (make_managed / manage) et le DETRUIT.
 *
 * Gtk::Container::remove() ne detruit pas un enfant gere : gtkmm le
 * re-reference pour qu'on puisse le reposer ailleurs, et si personne ne le
 * reprend, il vit pour toujours avec tout son sous-arbre. Chaque liste
 * reconstruite a chaque filtre, chaque grille de details refaite a chaque
 * selection, fuyait ainsi ses lignes precedentes : plusieurs Mo par
 * rafraichissement, sans jamais redescendre. On passe par ici quand l'enfant
 * ne servira plus. */
void destroy_child(Gtk::Container& parent, Gtk::Widget* child);
void destroy_children(Gtk::Container& parent);

/* Une pile filtre + tri au-dessus d'un ListStore, RECONSTRUITE plutot que
 * mise a jour.
 *
 * GtkTreeModelSort garde l'offset de chaque ligne et les decale un par un a
 * chaque row-inserted / row-deleted : remplir ou refiltrer un magasin sous
 * lui est quadratique. Mesure sur la bibliotheque (29 436 sets) : 110 s de
 * fenetre figee, que le bureau annonce comme « ne repond pas ». On remplit
 * donc le magasin sans rien de branche dessus, puis on rebatit la pile une
 * fois : la vue s'en trouve rechargee en une fraction de seconde. La colonne
 * de tri choisie par le joueur est memorisee entre deux reconstructions. */
struct ModelStack {
    Glib::RefPtr<Gtk::TreeModelFilter> filter;
    Glib::RefPtr<Gtk::TreeModelSort>   sort;
    int          sort_column = Gtk::TreeSortable::DEFAULT_SORT_COLUMN_ID;
    Gtk::SortType sort_order = Gtk::SORT_ASCENDING;

    // Retient la colonne de tri, detache la vue, laisse tomber les deux
    // modeles. A appeler AVANT de toucher au magasin.
    void detach(Gtk::TreeView& view);
    // Rebatit filtre (avec sa fonction de visibilite) et tri (avec la
    // colonne retenue) au-dessus de `store`, et rattache la vue.
    void attach(Gtk::TreeView& view, const Glib::RefPtr<Gtk::TreeModel>& store,
                const Gtk::TreeModelFilter::SlotVisible& visible);
    // Nombre de lignes visibles.
    int visible_count() const;
};

// La couleur qu'une classe CSS donne au texte, lue dans la feuille de style
// depuis l'interieur de `host` (les regles sont « .set-window .set-ok » :
// une etiquette detachee ne les atteint pas). Pour ce qui ne se peint pas en
// CSS : un TextTag, une cellule de TreeView. Fiable une fois la fenetre
// realisee ; avant, renvoie la couleur du texte courant.
Gdk::RGBA probe_color(Gtk::Container& host, const std::string& css_class);

/* La couleur d'un TON de la charte (« success », « warning », « error »,
 * « info », « muted », « accent »), lue dans la feuille de style, en
 * hexadecimal pour un markup Pango. C'est ainsi qu'une pastille d'etat ou
 * une legende suit la palette sans porter de couleur en dur. */
std::string tone_hex(Gtk::Container& host, const std::string& tone);

/* L'en-tete maison d'une fenetre : la barre de titre de ROM Management,
 * posee sur n'importe quelle fenetre ou boite de dialogue. Une tuile a
 * pictogramme, un titre, un sous-titre facultatif, et la croix de la charte
 * quand `on_close` existe. Elle remplace la barre du bureau (set_titlebar),
 * donc la fenetre reste deplacable, contrairement a set_decorated(false).
 *
 * C'est elle qui pose aussi les classes .cc-window / .set-window : sans
 * elles, les regles de la charte (journal, pastilles, pied) ne portent pas. */
struct Header {
    Gtk::HeaderBar* bar      = nullptr;
    Gtk::Label*     title    = nullptr;
    Gtk::Label*     subtitle = nullptr;   // a tenir a jour pendant un traitement
};
Header window_header(Gtk::Window& win, const std::string& icon_file,
                     const std::string& title, const std::string& subtitle = "",
                     const std::function<void()>& on_close = {});

/* Le pied d'une boite : un filet, puis les actions alignees a DROITE, la
 * principale en dernier. Le contenant est renvoye, on y pack_end ses
 * boutons dans l'ordre inverse de lecture. */
Gtk::Box* footer();

/* L'avertissement de la fiche de jeu, seul dessin que l'application donne a
 * une mise en garde : filet ambre a gauche, pictogramme au trait, intitule
 * en petites capitales. Remplace le « ⚠️ » que portaient les vieilles
 * boites. `action` reste nul quand l'avertissement ne demande aucun geste. */
Gtk::Widget* warning_card(const std::string& title, const std::string& message,
                          Gtk::Button** action = nullptr,
                          const std::string& action_label = "");

/* Une notification, dans le langage visuel de Bootcade.
 *
 * Gtk::MessageDialog donne la boite du bureau : fond gris, gros pictogramme
 * generique, bouton plat. Posee a cote d'un ecran de reglages peint en bleu
 * nuit, elle a l'air de venir d'une autre application. Celle-ci reprend la
 * tuile a pictogramme, les titres et le bouton principal des cartes.
 *
 * Bloquante, comme le MessageDialog qu'elle remplace : elle rend la main
 * quand le joueur a lu.
 */
void notice(Gtk::Window& parent, const std::string& title,
            const std::string& message,
            const std::string& icon_file = "bc-info.svg");

/* La meme boite, qui propose UN geste a cote de « OK » : « definir comme
 * chemin FBNeo », « ouvrir le dossier »... Renvoie true si le joueur a
 * choisi ce geste. Le bouton principal, a droite, est ce geste-la : « OK »
 * n'est que la sortie. */
bool offer(Gtk::Window& parent, const std::string& title, const std::string& message,
           const std::string& action_label,
           const std::string& icon_file = "bc-info.svg",
           const std::string& action_icon = "");

// ═══ Les briques des ecrans de donnees ═════════════════════════════════════
//
// Elles ne portent que les cinq teintes que l'application connait deja : les
// trois couleurs d'etat des pastilles, l'accent, et le neutre translucide.
// Un ecran qui voudrait une sixieme couleur n'a pas un besoin de couleur, il
// a un besoin de classement.

enum class PillTone { Neutral, Accent, Ok, Warn, Error, Info };

/* Une pastille « intitule + compteur ».
 *
 * Deux usages, un seul dessin : informative (« Total 29 519 »), ou filtre :
 * cliquable, avec un etat actif / inactif, pour que le resume d'un ecran
 * soit aussi son filtre. Fond a peine teinte, bordure et texte a la couleur
 * du ton : jamais d'aplat.
 */
class Pill : public Gtk::Box {
public:
    Pill(const std::string& label, PillTone tone, bool filter = false);

    void set_label(const std::string& text);
    // Sans compteur affiche tant que set_count n'a pas ete appele.
    void set_count(long count);
    void clear_count();
    void set_tone(PillTone tone);

    // Filtre seulement : sans effet sur une pastille informative.
    bool active() const;
    void set_active(bool on);
    sigc::signal<void, bool>& signal_toggled() { return m_toggled; }

private:
    void apply_tone();
    PillTone           m_tone;
    bool               m_filter;
    Gtk::ToggleButton* m_button = nullptr;   // filtre
    Gtk::Widget*       m_face   = nullptr;   // ce qui porte les classes
    Gtk::Label         m_label;
    Gtk::Label         m_count;
    sigc::signal<void, bool> m_toggled;
};

/* La barre au-dessus d'une table : une recherche locale, des listes
 * deroulantes que l'ecran ajoute a sa guise, et a droite le decompte de ce
 * que la table montre. Un seul signal quand quelque chose change ; la
 * saisie est temporisee, pour ne pas refiltrer 29 000 lignes a chaque
 * touche.
 */
class FilterBar : public Gtk::Box {
public:
    explicit FilterBar(const std::string& search_placeholder);

    Gtk::ComboBoxText* add_combo(const std::string& label);
    /* Regarnit un combo pose par add_combo : `first` (« All ») puis `items`,
     * en gardant `keep` choisi s'il est encore la. Sans signal_changed
     * pendant l'operation : remove_all() puis set_active() l'emettaient a
     * chaque entree retiree ou posee, et chaque emission refiltrait toute la
     * table. Mesure sur la bibliotheque : 17 systemes, 17 refiltrages de
     * 29 000 lignes, 25 s de fenetre figee pour un simple audit. */
    void set_combo_items(Gtk::ComboBoxText* combo, const std::string& first,
                         const std::set<std::string>& items, const Glib::ustring& keep);
    std::string search_text() const;
    void        set_summary(const std::string& text);
    Gtk::Entry& entry() { return m_entry; }

    sigc::signal<void>& signal_changed() { return m_changed; }

private:
    void schedule();
    Gtk::Entry        m_entry;
    Gtk::Label        m_summary;
    sigc::connection  m_pending;
    sigc::signal<void> m_changed;
    bool              m_quiet = false;   // combos being refilled : no emission
};

/* Une table de donnees : la TreeView de GTK, dans le cadre des cartes, avec
 * ce qu'un ecran de bureau attend d'elle : selection simple ou multiple,
 * defilement, tri par colonne, menu contextuel. Les colonnes et le modele
 * restent a l'appelant : la table ne sait rien de ce qu'elle montre.
 */
struct ColumnOptions {
    bool expand    = false;
    bool mono      = false;   // CRC, noms de fichiers
    int  min_width = -1;
    bool sortable  = true;
    float xalign   = 0.0f;
};

class Table : public Gtk::Box {
public:
    explicit Table(Gtk::SelectionMode mode = Gtk::SELECTION_SINGLE);

    Gtk::TreeView&       view()     { return m_view; }
    Gtk::ScrolledWindow& scrolled() { return m_scroll; }

    // Une colonne de texte ; renvoie la colonne pour la retoucher.
    Gtk::TreeViewColumn* add_text_column(const std::string& title,
                                         const Gtk::TreeModelColumn<Glib::ustring>& column,
                                         const ColumnOptions& options = ColumnOptions());
    // Une colonne de cases a cocher ; l'appelant recoit le chemin de la
    // ligne cliquee et bascule la valeur dans son modele.
    Gtk::TreeViewColumn* add_check_column(const Gtk::TreeModelColumn<bool>& column,
                                         const sigc::slot<void, const Glib::ustring&>& on_toggled);

    // Clic droit : la ligne visee est selectionnee (sans defaire une
    // selection multiple qui la contient deja), puis l'ecran est appele avec
    // son chemin et la colonne, pour construire le menu qu'il veut.
    sigc::signal<void, const Gtk::TreeModel::Path&, Gtk::TreeViewColumn*, GdkEventButton*>&
    signal_context_menu() { return m_context_menu; }

private:
    bool on_button_press(GdkEventButton* event);
    Gtk::ScrolledWindow m_scroll;
    Gtk::TreeView       m_view;
    sigc::signal<void, const Gtk::TreeModel::Path&, Gtk::TreeViewColumn*, GdkEventButton*> m_context_menu;
};

/* Le journal d'un traitement : un texte monospace qui defile, avec, si
 * l'ecran les demande, ses trois commandes : exporter, effacer, suivre.
 * Chaque ligne porte un niveau, peint avec les couleurs d'etat de
 * l'application : lues dans la feuille de style, jamais ecrites ici.
 */
class LogPanel : public Gtk::Box {
public:
    enum Actions { None = 0, Export = 1, Clear = 2, AutoScroll = 4, All = 7 };
    enum class Level { Info, Ok, Warn, Error, Muted };

    LogPanel(const std::string& title, const std::string& subtitle, int actions = All);

    void append(const std::string& line, Level level = Level::Info);
    void clear();
    std::string text() const;
    void set_auto_scroll(bool on);
    bool auto_scroll() const;

private:
    void ensure_tags();
    void on_export();
    Gtk::ScrolledWindow  m_scroll;
    Gtk::TextView        m_view;
    Glib::RefPtr<Gtk::TextBuffer> m_buffer;
    Gtk::CheckButton*    m_follow = nullptr;
    bool                 m_follow_default = true;
    bool                 m_tags_ready = false;
};

/* Deux zones l'une au-dessus de l'autre, separees d'une poignee a trois
 * points, au milieu : la table et le panneau de detail d'un onglet, par
 * exemple. Tirer la poignee donne de la hauteur a l'une aux depens de
 * l'autre ; la fenetre ne bouge pas. `bottom_height` est la hauteur du bas
 * au depart ; le haut prend le reste, et garde ce que la fenetre gagne.
 * Un Gtk::Paned dont le separateur est peint en poignee (.set-splitter).
 */
Gtk::Paned* splitter(Gtk::Widget& top, Gtk::Widget& bottom, int bottom_height);

/* Le panneau de detail au bas d'un ecran : une carte dont le corps est
 * rempli par l'ecran a chaque selection : une liste de ROMs, une grille
 * d'informations, ce qu'il veut. Vide, il le dit. `height` est sa hauteur
 * minimale ; sous un splitter c'est la poignee qui decide du reste.
 */
class DetailPanel : public Gtk::Box {
public:
    DetailPanel(const std::string& icon_file, const std::string& title,
                const std::string& subtitle, int height);

    void set_title(const std::string& text);
    void set_subtitle(const std::string& text);
    // Remplace le contenu ; le panneau prend possession du widget.
    void set_content(Gtk::Widget* content);
    // Retire le contenu et affiche le message d'attente.
    void show_placeholder(const std::string& message);
    Gtk::Box& head() { return *m_card.head; }

private:
    Card                m_card;
    Gtk::Label*         m_title = nullptr;
    Gtk::Label*         m_subtitle = nullptr;
    Gtk::ScrolledWindow m_scroll;
    Gtk::Box            m_body{Gtk::ORIENTATION_VERTICAL, 0};
    Gtk::Widget*        m_content = nullptr;
};

/* Une liste de choix qui s'ouvre SOUS son bouton.
 *
 * Gtk::ComboBoxText pose l'element choisi pile sur le bouton : sur une
 * longue liste (35 filtres d'image) dont on a choisi le dernier, le menu
 * remonte au-dessus de l'ecran, coupe, avec du vide en haut et en bas. Ici,
 * la liste tombe sous le bouton dans une bulle de hauteur bornee qui defile,
 * et revient d'elle-meme sur l'element choisi. Memes appels que ComboBoxText
 * pour ce que les reglages en utilisent. */
class Choice : public Gtk::MenuButton {
public:
    explicit Choice(int max_height = 360);

    void        append(const std::string& id, const std::string& label);
    void        remove_all();
    // Rend false, sans rien changer, si l'identifiant n'est pas dans la liste.
    bool        set_active_id(const std::string& id);
    void        set_active(int index);
    std::string get_active_id() const;
    size_t      size() const { return m_items.size(); }
    sigc::signal<void>& signal_changed() { return m_changed; }

private:
    void choose(int index, bool emit);
    std::vector<std::pair<std::string, std::string>> m_items;
    int                 m_active = -1;
    Gtk::Box            m_face{Gtk::ORIENTATION_HORIZONTAL, 8};
    Gtk::Label          m_label;
    Gtk::Popover        m_popover;
    Gtk::ScrolledWindow m_scroll;
    Gtk::ListBox        m_list;
    sigc::signal<void>  m_changed;
};

}  // namespace SettingsUi
