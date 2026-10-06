// src/SettingsUi.cpp
#include "SettingsUi.h"

#include <algorithm>
#include <iostream>
#include <cstdlib>

#include "IconManager.h"
#include "i18n.h"

#include <cmath>
#include <map>
#include <memory>
#include <cstdio>
#include <fstream>

namespace SettingsUi {

Icon::Icon(const std::string& icon_file, int size)
    : m_subpath("icons/" + icon_file), m_size(size), m_mono(IconManager::is_monochrome(m_subpath)) {
    if (!m_mono) set(IconManager::load(m_subpath, size, size));
    else retint();
    // Hover, pressed, insensitive : the ink changes with the state.
    signal_state_flags_changed().connect([this](Gtk::StateFlags) { retint(); });
}

void Icon::set_file(const std::string& icon_file) {
    m_subpath = "icons/" + icon_file;
    m_mono = IconManager::is_monochrome(m_subpath);
    m_painted = false;
    if (!m_mono) set(IconManager::load(m_subpath, m_size, m_size));
    else retint();
}

void Icon::retint() {
    if (!m_mono) return;
    const Gdk::RGBA colour = get_style_context()->get_color(get_state_flags());
    if (m_painted && colour == m_colour) return;
    m_colour = colour;
    m_painted = true;
    set(IconManager::load_tinted(m_subpath, m_size, m_size, colour));
}

void Icon::on_style_updated() {
    Gtk::Image::on_style_updated();
    retint();
}

void Icon::on_map() {
    Gtk::Image::on_map();
    retint();
}

bool Icon::on_draw(const Cairo::RefPtr<Cairo::Context>& cr) {
    retint();
    return Gtk::Image::on_draw(cr);
}

void set_icon(Gtk::Image& target, const std::string& icon_file, int size) {
    const std::string subpath = "icons/" + icon_file;
    if (IconManager::is_monochrome(subpath))
        target.set(IconManager::load_tinted(subpath, size, size, target.get_style_context()->get_color(target.get_state_flags())));
    else
        target.set(IconManager::load(subpath, size, size));
}

Gtk::Image* image(const std::string& icon_file, int size) {
    auto* img = Gtk::make_managed<Icon>(icon_file, size);
    img->set_halign(Gtk::ALIGN_CENTER);
    img->set_valign(Gtk::ALIGN_CENTER);
    return img;
}

/* Le pictogramme est centre par la GEOMETRIE de sa tuile, jamais par des
 * marges : c'est ce qui garantit que douze lignes d'une page ont leurs icones
 * exactement sur la meme verticale, quelle que soit la taille du trace. */
Gtk::Widget* tile(const std::string& icon_file, int icon_size, int box_size,
                  bool accent) {
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 0);
    box->set_size_request(box_size, box_size);
    box->pack_start(*image(icon_file, icon_size), Gtk::PACK_EXPAND_WIDGET);
    box->get_style_context()->add_class(accent ? "set-brand" : "set-tile");
    box->set_halign(Gtk::ALIGN_CENTER);
    box->set_valign(Gtk::ALIGN_CENTER);
    return box;
}

Gtk::Label* title_label(const std::string& text) {
    auto* l = Gtk::make_managed<Gtk::Label>(text);
    l->set_xalign(0.0f);
    l->get_style_context()->add_class("set-row-title");
    return l;
}

Gtk::Label* sub_label(const std::string& text) {
    auto* l = Gtk::make_managed<Gtk::Label>(text);
    l->set_xalign(0.0f);
    l->set_line_wrap(true);
    l->set_line_wrap_mode(Pango::WRAP_WORD_CHAR);
    l->get_style_context()->add_class("set-sub");
    return l;
}

Gtk::Label* card_title_label(const std::string& text) {
    auto* l = Gtk::make_managed<Gtk::Label>(text);
    l->set_xalign(0.0f);
    l->get_style_context()->add_class("set-card-title");
    return l;
}

Gdk::RGBA probe_color(Gtk::Container& host, const std::string& css_class) {
    // The probe needs a parent for the sheet's ".set-window .x" rules to
    // reach it. A Bin (a Window, a Frame) holds one child only : go down to
    // the first container that takes another one.
    Gtk::Container* parent = &host;
    while (auto* bin = dynamic_cast<Gtk::Bin*>(parent)) {
        auto* inner = dynamic_cast<Gtk::Container*>(bin->get_child());
        if (!inner) break;
        parent = inner;
    }
    if (auto* bin = dynamic_cast<Gtk::Bin*>(parent); bin && bin->get_child())
        return host.get_style_context()->get_color(Gtk::STATE_FLAG_NORMAL);
    auto* probe = Gtk::make_managed<Gtk::Label>();
    probe->get_style_context()->add_class(css_class);
    probe->set_no_show_all(true);
    parent->add(*probe);
    Gdk::RGBA colour = probe->get_style_context()->get_color(Gtk::STATE_FLAG_NORMAL);
    destroy_child(*parent, probe);
    return colour;
}

std::string tone_hex(Gtk::Container& host, const std::string& tone) {
    const Gdk::RGBA c = probe_color(host, "tone-" + tone);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x",
                  (int)std::lround(c.get_red() * 255), (int)std::lround(c.get_green() * 255), (int)std::lround(c.get_blue() * 255));
    return buf;
}

void destroy_child(Gtk::Container& parent, Gtk::Widget* child) {
    if (!child) return;
    parent.remove(*child);
    delete child;   // a managed widget's destructor destroys its C object
}

void destroy_children(Gtk::Container& parent) {
    for (auto* child : parent.get_children()) destroy_child(parent, child);
}

void ModelStack::detach(Gtk::TreeView& view) {
    if (sort) {
        int col; Gtk::SortType order;
        if (sort->get_sort_column_id(col, order)) { sort_column = col; sort_order = order; }
    }
    view.unset_model();
    sort.reset();
    filter.reset();
}

void ModelStack::attach(Gtk::TreeView& view, const Glib::RefPtr<Gtk::TreeModel>& store,
                        const Gtk::TreeModelFilter::SlotVisible& visible) {
    filter = Gtk::TreeModelFilter::create(store);
    filter->set_visible_func(visible);
    sort = Gtk::TreeModelSort::create(filter);
    // GTK sorts string columns with g_utf8_collate(), which builds a collation
    // key per comparison : ~1.3 s to sort 29 000 rows. Rows here are ROM
    // names, file names and systems, ASCII in practice : a byte-wise,
    // case-folded comparison gives the same order a hundred times faster.
    const int n_columns = store->get_n_columns();
    for (int c = 0; c < n_columns; ++c) {
        if (store->get_column_type(c) != G_TYPE_STRING) continue;
        sort->set_sort_func(c, [c](const Gtk::TreeModel::iterator& a, const Gtk::TreeModel::iterator& b) -> int {
            Glib::ustring sa, sb;
            a->get_value(c, sa);
            b->get_value(c, sb);
            return g_ascii_strcasecmp(sa.c_str(), sb.c_str());
        });
    }
    if (sort_column != Gtk::TreeSortable::DEFAULT_SORT_COLUMN_ID &&
        sort_column != Gtk::TreeSortable::DEFAULT_UNSORTED_COLUMN_ID)
        sort->set_sort_column(sort_column, sort_order);
    view.set_model(sort);
}

int ModelStack::visible_count() const {
    return filter ? (int)filter->children().size() : 0;
}

Gtk::Widget* hairline() {
    auto* sep = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 0);
    sep->set_size_request(-1, 1);
    sep->get_style_context()->add_class("set-rowsep");
    return sep;
}

Card card(const std::string& icon_file, const std::string& title,
          const std::string& subtitle) {
    Card c;
    c.frame = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 14);
    c.frame->get_style_context()->add_class("cc-card");

    c.head = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 13);
    c.head->pack_start(*tile(icon_file, kIconSection, kTileSection),
                       Gtk::PACK_SHRINK);

    auto* txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 2);
    txt->set_valign(Gtk::ALIGN_CENTER);
    c.title = card_title_label(title);
    txt->pack_start(*c.title, Gtk::PACK_SHRINK);
    if (!subtitle.empty()) {
        c.subtitle = sub_label(subtitle);
        txt->pack_start(*c.subtitle, Gtk::PACK_SHRINK);
    }
    c.head->pack_start(*txt, Gtk::PACK_EXPAND_WIDGET);
    c.frame->pack_start(*c.head, Gtk::PACK_SHRINK);

    c.body = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
    c.frame->pack_start(*c.body, Gtk::PACK_EXPAND_WIDGET);
    /* La carte s'etire en hauteur, elle ne se retracte pas.
     *
     * Sur une page a deux colonnes, la colonne la plus courte doit descendre
     * jusqu'au bas de l'autre : « Your Profile » s'aligne sur « Network
     * Status », et le cadre des emulateurs sur celui des options. Deux
     * colonnes qui s'arretent a des hauteurs differentes se lisent comme un
     * defaut d'alignement, pas comme une intention.
     *
     * Le contenu, lui, reste en haut de la carte : c'est le CADRE qui
     * descend, pas les lignes qui s'espacent.
     */
    return c;
}

Gtk::Box* rows() {
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
    box->get_style_context()->add_class("set-rows");
    return box;
}

/* Le filet n'apparait qu'a partir de la deuxieme ligne : un trait au-dessus de
 * la premiere doublerait la bordure du cadre. */
void add_row(Gtk::Box* container, Gtk::Widget& row) {
    if (!container->get_children().empty())
        container->pack_start(*hairline(), Gtk::PACK_SHRINK);
    container->pack_start(row, Gtk::PACK_SHRINK);
}

Gtk::Widget* row(const std::string& icon_file, const std::string& title,
                 const std::string& subtitle, Gtk::Widget* trailing) {
    auto* line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 13);
    line->get_style_context()->add_class("set-row");

    if (!icon_file.empty())
        line->pack_start(*tile(icon_file, kIconRow, kTileRow), Gtk::PACK_SHRINK);

    auto* txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 1);
    txt->set_valign(Gtk::ALIGN_CENTER);
    txt->pack_start(*title_label(title), Gtk::PACK_SHRINK);
    if (!subtitle.empty()) txt->pack_start(*sub_label(subtitle), Gtk::PACK_SHRINK);
    line->pack_start(*txt, Gtk::PACK_EXPAND_WIDGET);

    if (trailing) {
        trailing->set_valign(Gtk::ALIGN_CENTER);
        line->pack_start(*trailing, Gtk::PACK_SHRINK);
    }
    return line;
}

Gtk::Button* button(const std::string& label, const std::string& icon_file,
                    Tone tone) {
    auto* b = Gtk::make_managed<Gtk::Button>(label);
    if (!icon_file.empty()) {
        b->set_image(*image(icon_file, kIconButton));
        b->set_always_show_image(true);
    }
    if (tone == Tone::Accent) b->get_style_context()->add_class("accent-button");
    if (tone == Tone::Danger) b->get_style_context()->add_class("set-danger");
    b->set_valign(Gtk::ALIGN_CENTER);
    return b;
}

namespace {
const char* state_class(State s) {
    switch (s) {
        case State::Ok:    return "set-ok";
        case State::Warn:  return "set-warn";
        case State::Error: return "set-err";
        default:           return "set-sub";
    }
}
}  // namespace

Gtk::Label* status_label(const std::string& text, State state) {
    auto* l = Gtk::make_managed<Gtk::Label>(text);
    l->set_xalign(0.0f);
    l->get_style_context()->add_class(state_class(state));
    return l;
}

Header window_header(Gtk::Window& win, const std::string& icon_file,
                     const std::string& title, const std::string& subtitle,
                     const std::function<void()>& on_close) {
    Header h;
    win.set_title(title);
    win.get_style_context()->add_class("cc-window");
    win.get_style_context()->add_class("set-window");

    auto* head = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 12);
    head->pack_start(*tile(icon_file, 21, 36, /*accent=*/true), Gtk::PACK_SHRINK);

    auto* txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
    txt->set_valign(Gtk::ALIGN_CENTER);
    h.title = Gtk::make_managed<Gtk::Label>(title);
    h.title->set_xalign(0.0f);
    h.title->get_style_context()->add_class("set-head-title");
    txt->pack_start(*h.title, Gtk::PACK_SHRINK);
    h.subtitle = Gtk::make_managed<Gtk::Label>(subtitle);
    h.subtitle->set_xalign(0.0f);
    h.subtitle->set_ellipsize(Pango::ELLIPSIZE_MIDDLE);
    h.subtitle->get_style_context()->add_class("set-head-sub");
    // Toujours pose, meme vide : le sous-titre d'un traitement n'arrive
    // qu'a la premiere etape, et la barre ne doit pas sauter a ce moment-la.
    txt->pack_start(*h.subtitle, Gtk::PACK_SHRINK);
    head->pack_start(*txt, Gtk::PACK_SHRINK);

    h.bar = Gtk::make_managed<Gtk::HeaderBar>();
    h.bar->set_show_close_button(false);
    h.bar->pack_start(*head);
    if (on_close) {
        auto* close = Gtk::make_managed<Gtk::Button>();
        close->set_image(*image("bc-close.svg", 18));
        close->set_tooltip_text(_("Close"));
        close->get_style_context()->add_class("set-close");
        close->set_valign(Gtk::ALIGN_CENTER);
        close->signal_clicked().connect(on_close);
        h.bar->pack_end(*close);
    }
    h.bar->set_custom_title(*Gtk::make_managed<Gtk::Box>());
    h.bar->show_all();
    win.set_titlebar(*h.bar);
    return h;
}

Gtk::Box* footer() {
    auto* foot = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    foot->get_style_context()->add_class("cc-footer");
    return foot;
}

Gtk::Widget* warning_card(const std::string& title, const std::string& message,
                          Gtk::Button** action, const std::string& action_label) {
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 12);
    box->get_style_context()->add_class("warn-card");

    auto* icon = Gtk::make_managed<Icon>("bc-warning.svg", 26);
    icon->set_valign(Gtk::ALIGN_START);
    box->pack_start(*icon, Gtk::PACK_SHRINK);

    auto* txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 4);
    auto* head = Gtk::make_managed<Gtk::Label>(title);
    head->set_xalign(0.0f);
    head->get_style_context()->add_class("warn-title");
    txt->pack_start(*head, Gtk::PACK_SHRINK);

    auto* body = Gtk::make_managed<Gtk::Label>(message);
    body->set_xalign(0.0f);
    body->set_line_wrap(true);
    body->set_line_wrap_mode(Pango::WRAP_WORD);
    body->set_max_width_chars(52);
    txt->pack_start(*body, Gtk::PACK_SHRINK);

    if (action) {
        auto* b = Gtk::make_managed<Gtk::Button>(action_label);
        b->set_halign(Gtk::ALIGN_START);
        b->get_style_context()->add_class("warn-btn");
        txt->pack_start(*b, Gtk::PACK_SHRINK);
        *action = b;
    }
    box->pack_start(*txt, Gtk::PACK_EXPAND_WIDGET);
    return box;
}

namespace {
/* Le corps commun de notice() et offer() : la boite de la charte, un
 * pictogramme en tuile, un titre, un texte, et un pied dont l'appelant
 * garnit la droite. Renvoie la reponse rendue par run(). */
int run_message_box(Gtk::Window& parent, const std::string& title,
                    const std::string& message, const std::string& icon_file,
                    const std::string& action_label, const std::string& action_icon) {
    Gtk::Dialog dlg;
    // Un titre meme sans barre de titre : c'est ce que lisent le gestionnaire
    // de fenetres, la barre des taches et les outils d'accessibilite.
    dlg.set_title(title);
    dlg.set_transient_for(parent);
    dlg.set_modal(true);
    dlg.set_resizable(false);
    // Pas de barre de titre : la boite porte son titre dans son contenu, comme
    // les cartes. Une barre du bureau au-dessus ferait exactement le melange
    // qu'on cherche a eviter.
    dlg.set_decorated(false);
    dlg.get_style_context()->add_class("cc-window");
    dlg.get_style_context()->add_class("set-window");
    dlg.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);

    auto* content = dlg.get_content_area();
    content->set_spacing(0);

    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 15);
    body->get_style_context()->add_class("set-notice");
    body->pack_start(*tile(icon_file, kIconSection, kTileSection), Gtk::PACK_SHRINK);

    auto* txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 4);
    txt->set_valign(Gtk::ALIGN_CENTER);
    txt->pack_start(*card_title_label(title), Gtk::PACK_SHRINK);
    if (!message.empty()) {
        auto* sub = sub_label(message);
        sub->set_max_width_chars(46);
        txt->pack_start(*sub, Gtk::PACK_SHRINK);
    }
    body->pack_start(*txt, Gtk::PACK_EXPAND_WIDGET);
    content->pack_start(*body, Gtk::PACK_EXPAND_WIDGET);

    auto* foot = footer();
    // Sans geste propose, « OK » EST l'action principale ; avec, il n'est que
    // la sortie et c'est le geste qui porte l'accent.
    const bool has_action = !action_label.empty();
    auto* ok = button(_("OK"), "", has_action ? Tone::Normal : Tone::Accent);
    ok->set_size_request(96, -1);
    ok->signal_clicked().connect([&dlg] { dlg.response(Gtk::RESPONSE_CANCEL); });
    Gtk::Button* act = nullptr;
    if (has_action) {
        act = button(action_label, action_icon, Tone::Accent);
        act->signal_clicked().connect([&dlg] { dlg.response(Gtk::RESPONSE_OK); });
        foot->pack_end(*act, Gtk::PACK_SHRINK);
    }
    foot->pack_end(*ok, Gtk::PACK_SHRINK);
    content->pack_start(*foot, Gtk::PACK_SHRINK);

    dlg.show_all_children();
    (act ? act : ok)->grab_focus();
    return dlg.run();
}
}  // namespace

void notice(Gtk::Window& parent, const std::string& title,
            const std::string& message, const std::string& icon_file) {
    run_message_box(parent, title, message, icon_file, "", "");
}

bool offer(Gtk::Window& parent, const std::string& title, const std::string& message,
           const std::string& action_label, const std::string& icon_file,
           const std::string& action_icon) {
    return run_message_box(parent, title, message, icon_file, action_label, action_icon)
           == Gtk::RESPONSE_OK;
}

Gtk::Widget* status_dot(const std::string& text, State state) {
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    auto* dot = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 0);
    dot->get_style_context()->add_class("set-dot");
    dot->get_style_context()->add_class(state_class(state));
    dot->set_valign(Gtk::ALIGN_CENTER);
    box->pack_start(*dot, Gtk::PACK_SHRINK);
    box->pack_start(*status_label(text, state), Gtk::PACK_SHRINK);
    box->set_valign(Gtk::ALIGN_CENTER);
    return box;
}

// ═══ Les briques des ecrans de donnees ═════════════════════════════════════

namespace {
const char* pill_tone_class(PillTone t) {
    switch (t) {
        case PillTone::Accent: return "set-accent";
        case PillTone::Ok:     return "set-ok";
        case PillTone::Warn:   return "set-warn";
        case PillTone::Error:  return "set-err";
        case PillTone::Info:   return "set-info";
        default:               return "set-off";
    }
}

// « 29 519 » : l'espace fine entre les milliers, comme partout dans
// l'application.
std::string thousands(long n) {
    std::string digits = std::to_string(n < 0 ? -n : n);
    std::string out;
    int k = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it, ++k) {
        if (k && k % 3 == 0) out.insert(0, "\u202f");
        out.insert(0, 1, *it);
    }
    return (n < 0 ? "-" : "") + out;
}
}  // namespace

// ── Pill ───────────────────────────────────────────────────────────────────

Pill::Pill(const std::string& label, PillTone tone, bool filter)
    : Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0), m_tone(tone), m_filter(filter) {
    auto* inner = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    m_label.set_text(label);
    m_label.set_xalign(0.0f);
    m_count.get_style_context()->add_class("set-pill-count");
    m_count.set_no_show_all(true);
    inner->pack_start(m_label, Gtk::PACK_SHRINK);
    inner->pack_start(m_count, Gtk::PACK_SHRINK);

    if (m_filter) {
        // Le bouton EST la pastille : c'est lui qui porte les classes, pour
        // que l'etat coche / decoche pilote la teinte.
        m_button = Gtk::make_managed<Gtk::ToggleButton>();
        m_button->add(*inner);
        m_button->set_can_focus(false);
        m_button->signal_toggled().connect([this] { m_toggled.emit(m_button->get_active()); });
        m_face = m_button;
        pack_start(*m_button, Gtk::PACK_SHRINK);
    } else {
        m_face = inner;
        pack_start(*inner, Gtk::PACK_SHRINK);
    }
    m_face->get_style_context()->add_class("set-pill");
    if (m_filter) m_face->get_style_context()->add_class("set-filter");
    set_valign(Gtk::ALIGN_CENTER);
    apply_tone();
    show_all_children();
}

void Pill::apply_tone() {
    auto ctx = m_face->get_style_context();
    for (const char* c : {"set-off", "set-accent", "set-ok", "set-warn", "set-err"})
        ctx->remove_class(c);
    ctx->add_class(pill_tone_class(m_tone));
}

void Pill::set_label(const std::string& text) { m_label.set_text(text); }
void Pill::set_count(long count) { m_count.set_text(thousands(count)); m_count.show(); }
void Pill::clear_count()         { m_count.hide(); }
void Pill::set_tone(PillTone tone) { m_tone = tone; apply_tone(); }

bool Pill::active() const  { return m_button && m_button->get_active(); }
void Pill::set_active(bool on) { if (m_button) m_button->set_active(on); }

// ── FilterBar ──────────────────────────────────────────────────────────────

FilterBar::FilterBar(const std::string& search_placeholder)
    : Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 12) {
    m_entry.set_placeholder_text(search_placeholder);
    m_entry.set_icon_from_pixbuf(IconManager::load("icons/bc-search.svg", 16, 16),
                                 Gtk::ENTRY_ICON_PRIMARY);
    m_entry.set_size_request(kFieldWidth, -1);
    m_entry.signal_changed().connect(sigc::mem_fun(*this, &FilterBar::schedule));
    pack_start(m_entry, Gtk::PACK_SHRINK);

    m_summary.set_xalign(1.0f);
    m_summary.get_style_context()->add_class("set-sub");
    pack_end(m_summary, Gtk::PACK_SHRINK);
    set_valign(Gtk::ALIGN_CENTER);
}

// La saisie attend que les doigts s'arretent : refiltrer une table de
// 29 000 lignes a chaque touche la rendrait poisseuse.
void FilterBar::schedule() {
    m_pending.disconnect();
    m_pending = Glib::signal_timeout().connect([this] { m_changed.emit(); return false; }, 220);
}

Gtk::ComboBoxText* FilterBar::add_combo(const std::string& label) {
    auto* group = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    auto* l = Gtk::make_managed<Gtk::Label>(label);
    l->get_style_context()->add_class("set-sub");
    group->pack_start(*l, Gtk::PACK_SHRINK);
    auto* combo = Gtk::make_managed<Gtk::ComboBoxText>();
    combo->signal_changed().connect([this] { if (!m_quiet) m_changed.emit(); });
    group->pack_start(*combo, Gtk::PACK_SHRINK);
    group->set_valign(Gtk::ALIGN_CENTER);
    pack_start(*group, Gtk::PACK_SHRINK);
    group->show_all();
    return combo;
}

void FilterBar::set_combo_items(Gtk::ComboBoxText* combo, const std::string& first,
                                const std::set<std::string>& items, const Glib::ustring& keep) {
    m_quiet = true;
    combo->remove_all();
    combo->append(first);
    int active = 0, idx = 1;
    for (const auto& s : items) {
        combo->append(s);
        if (!keep.empty() && keep.raw() == s) active = idx;
        ++idx;
    }
    combo->set_active(active);
    m_quiet = false;
}

std::string FilterBar::search_text() const { return m_entry.get_text().raw(); }
void FilterBar::set_summary(const std::string& text) { m_summary.set_text(text); }

// ── Table ──────────────────────────────────────────────────────────────────

Table::Table(Gtk::SelectionMode mode) : Gtk::Box(Gtk::ORIENTATION_VERTICAL, 0) {
    get_style_context()->add_class("set-table");
    m_view.get_selection()->set_mode(mode);
    m_view.set_headers_visible(true);
    m_view.set_enable_search(false);   // la recherche est celle de la barre de filtres
    m_view.signal_button_press_event().connect(sigc::mem_fun(*this, &Table::on_button_press), false);
    m_scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    m_scroll.add(m_view);
    pack_start(m_scroll, Gtk::PACK_EXPAND_WIDGET);
}

bool Table::on_button_press(GdkEventButton* event) {
    if (event->type != GDK_BUTTON_PRESS || event->button != 3) return false;
    Gtk::TreeModel::Path path;
    Gtk::TreeViewColumn* column = nullptr;
    int cx = 0, cy = 0;
    if (!m_view.get_path_at_pos((int)event->x, (int)event->y, path, column, cx, cy)) return false;
    // Un clic droit sur une ligne deja dans la selection ne la defait pas :
    // c'est ainsi qu'on demande une action sur plusieurs lignes a la fois.
    auto sel = m_view.get_selection();
    if (!sel->is_selected(path)) {
        sel->unselect_all();
        sel->select(path);
    }
    m_context_menu.emit(path, column, event);
    return true;
}

Gtk::TreeViewColumn* Table::add_text_column(const std::string& title,
                                            const Gtk::TreeModelColumn<Glib::ustring>& column,
                                            const ColumnOptions& options) {
    auto* renderer = Gtk::make_managed<Gtk::CellRendererText>();
    renderer->property_xalign() = options.xalign;
    if (options.mono) renderer->property_family() = "monospace";
    if (options.expand) renderer->property_ellipsize() = Pango::ELLIPSIZE_END;

    auto* col = Gtk::make_managed<Gtk::TreeViewColumn>(title, *renderer);
    col->add_attribute(renderer->property_text(), column);
    col->set_resizable(true);
    col->set_expand(options.expand);
    if (options.min_width > 0) col->set_min_width(options.min_width);
    if (options.sortable) col->set_sort_column(column);
    m_view.append_column(*col);
    return col;
}

Gtk::TreeViewColumn* Table::add_check_column(const Gtk::TreeModelColumn<bool>& column,
                                             const sigc::slot<void, const Glib::ustring&>& on_toggled) {
    auto* renderer = Gtk::make_managed<Gtk::CellRendererToggle>();
    renderer->set_activatable(true);
    renderer->signal_toggled().connect(on_toggled);
    auto* col = Gtk::make_managed<Gtk::TreeViewColumn>("", *renderer);
    col->add_attribute(renderer->property_active(), column);
    col->set_resizable(false);
    col->set_expand(false);
    m_view.append_column(*col);
    return col;
}

// ── LogPanel ───────────────────────────────────────────────────────────────

LogPanel::LogPanel(const std::string& title, const std::string& subtitle, int actions)
    : Gtk::Box(Gtk::ORIENTATION_VERTICAL, 10) {
    Card c = card("bc-file.svg", title, subtitle);

    auto* tools = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    tools->set_valign(Gtk::ALIGN_CENTER);
    if (actions & AutoScroll) {
        m_follow = Gtk::make_managed<Gtk::CheckButton>(_("Auto-scroll"));
        m_follow->set_active(true);
        tools->pack_end(*m_follow, Gtk::PACK_SHRINK);
    }
    if (actions & Clear) {
        auto* b = button(_("Clear log"), "bc-clear.svg");
        b->signal_clicked().connect(sigc::mem_fun(*this, &LogPanel::clear));
        tools->pack_end(*b, Gtk::PACK_SHRINK);
    }
    if (actions & Export) {
        auto* b = button(_("Export log"), "bc-save.svg");
        b->signal_clicked().connect(sigc::mem_fun(*this, &LogPanel::on_export));
        tools->pack_end(*b, Gtk::PACK_SHRINK);
    }
    if (actions) c.head->pack_end(*tools, Gtk::PACK_SHRINK);

    m_buffer = Gtk::TextBuffer::create();
    m_view.set_buffer(m_buffer);
    m_view.set_editable(false);
    m_view.set_cursor_visible(false);
    m_view.set_wrap_mode(Gtk::WRAP_NONE);
    m_view.get_style_context()->add_class("set-mono");
    m_view.get_style_context()->add_class("set-log");
    m_scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    m_scroll.get_style_context()->add_class("set-log-frame");
    m_scroll.add(m_view);
    c.body->pack_start(m_scroll, Gtk::PACK_EXPAND_WIDGET);
    c.body->set_margin_top(10);
    pack_start(*c.frame, Gtk::PACK_EXPAND_WIDGET);
}

/* Les couleurs des niveaux sont celles des pastilles d'etat, lues dans la
 * feuille de style au moment ou le panneau est dans une fenetre (la regle
 * est « .set-window .set-ok » : detachee, une etiquette ne l'atteint pas).
 * Un TextTag ne se peint pas en CSS, c'est le seul endroit ou une couleur
 * transite par le code : elle n'y est pas ecrite pour autant. */
void LogPanel::ensure_tags() {
    if (m_tags_ready || !get_toplevel() || !get_toplevel()->get_realized()) return;
    auto table = m_buffer->get_tag_table();
    struct Probe { const char* tag; const char* css; };
    for (const Probe& p : {Probe{"ok", "set-ok"}, Probe{"warn", "set-warn"},
                           Probe{"error", "set-err"}, Probe{"muted", "set-sub"}}) {
        auto tag = Gtk::TextTag::create(p.tag);
        tag->property_foreground_rgba() = probe_color(*this, p.css);
        if (p.tag == std::string("muted")) tag->property_style() = Pango::STYLE_ITALIC;
        table->add(tag);
    }
    m_tags_ready = true;
}

void LogPanel::append(const std::string& line, Level level) {
    ensure_tags();
    Gtk::TextBuffer::iterator end = m_buffer->end();
    const char* tag = nullptr;
    switch (level) {
        case Level::Ok:    tag = "ok";    break;
        case Level::Warn:  tag = "warn";  break;
        case Level::Error: tag = "error"; break;
        case Level::Muted: tag = "muted"; break;
        default: break;
    }
    if (tag && m_tags_ready) m_buffer->insert_with_tag(end, line + "\n", tag);
    else                     m_buffer->insert(end, line + "\n");
    if (auto_scroll()) {
        auto mark = m_buffer->create_mark(m_buffer->end());
        m_view.scroll_to(mark);
        m_buffer->delete_mark(mark);
    }
}

void LogPanel::clear() { m_buffer->set_text(""); }
std::string LogPanel::text() const { return m_buffer->get_text().raw(); }
void LogPanel::set_auto_scroll(bool on) { if (m_follow) m_follow->set_active(on); else m_follow_default = on; }
bool LogPanel::auto_scroll() const { return m_follow ? m_follow->get_active() : m_follow_default; }

void LogPanel::on_export() {
    auto* top = dynamic_cast<Gtk::Window*>(get_toplevel());
    Gtk::FileChooserDialog dlg(_("Export log"), Gtk::FILE_CHOOSER_ACTION_SAVE);
    if (top) dlg.set_transient_for(*top);
    dlg.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dlg.add_button(_("Save"),   Gtk::RESPONSE_OK);
    dlg.set_current_name("bootcade-log.txt");
    dlg.set_do_overwrite_confirmation(true);
    if (dlg.run() != Gtk::RESPONSE_OK) return;
    std::ofstream out(dlg.get_filename());
    if (!out) {
        if (top) notice(*top, _("Could not write the file."), dlg.get_filename());
        return;
    }
    out << text();
}

// ── DetailPanel ────────────────────────────────────────────────────────────

Gtk::Paned* splitter(Gtk::Widget& top, Gtk::Widget& bottom, int bottom_height) {
    auto* paned = Gtk::make_managed<Gtk::Paned>(Gtk::ORIENTATION_VERTICAL);
    paned->get_style_context()->add_class("set-splitter");
    // Un separateur large : c'est lui qui porte les trois points, et c'est
    // toute sa hauteur qui se saisit, pas une ligne d'un pixel.
    paned->set_wide_handle(true);
    // Le haut absorbe les changements de taille de la fenetre ; le bas garde
    // la hauteur que la poignee lui a donnee. Ni l'un ni l'autre ne se
    // reduit sous son minimum.
    paned->pack1(top, true, false);
    paned->pack2(bottom, false, false);
    // La position de depart se pose a la premiere allocation credible : sans
    // cela le bas ne recevrait que son minimum. Une fois posee, GTK la tient.
    auto placed = std::make_shared<bool>(false);
    paned->signal_size_allocate().connect([paned, placed, bottom_height](Gtk::Allocation& a) {
        if (*placed || a.get_height() < bottom_height + 160) return;
        *placed = true;
        const int pos = a.get_height() - bottom_height;
        // Pas de set_position pendant l'allocation elle-meme.
        Glib::signal_idle().connect_once([paned, pos] { paned->set_position(pos); });
    });
    return paned;
}

DetailPanel::DetailPanel(const std::string& icon_file, const std::string& title,
                         const std::string& subtitle, int height)
    : Gtk::Box(Gtk::ORIENTATION_VERTICAL, 0) {
    m_card = card(icon_file, title, subtitle);
    m_title = m_card.title;
    m_subtitle = m_card.subtitle;
    m_scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    m_scroll.add(m_body);
    m_card.body->pack_start(m_scroll, Gtk::PACK_EXPAND_WIDGET);
    m_card.body->set_margin_top(8);
    m_card.frame->set_size_request(-1, height);
    pack_start(*m_card.frame, Gtk::PACK_EXPAND_WIDGET);
    show_placeholder("");
}

void DetailPanel::set_title(const std::string& text) { if (m_title) m_title->set_text(text); }
void DetailPanel::set_subtitle(const std::string& text) { if (m_subtitle) m_subtitle->set_text(text); }

void DetailPanel::set_content(Gtk::Widget* content) {
    // The panel owns its content : the previous one goes away with it.
    if (m_content) { destroy_child(m_body, m_content); m_content = nullptr; }
    if (!content) return;
    m_content = content;
    m_body.pack_start(*content, Gtk::PACK_EXPAND_WIDGET);
    content->show_all();
}

void DetailPanel::show_placeholder(const std::string& message) {
    auto* l = sub_label(message.empty() ? _("Nothing selected.") : message);
    l->set_xalign(0.5f);
    l->set_valign(Gtk::ALIGN_CENTER);
    l->set_margin_top(18);
    set_content(l);
}

Choice::Choice(int max_height) {
    get_style_context()->add_class("set-choice");
    m_label.set_xalign(0.0f);
    m_label.set_ellipsize(Pango::ELLIPSIZE_END);
    m_face.pack_start(m_label, Gtk::PACK_EXPAND_WIDGET);
    m_face.pack_end(*image("bc-chevron-down.svg", 16), Gtk::PACK_SHRINK);
    add(m_face);
    m_face.show_all();

    m_list.set_selection_mode(Gtk::SELECTION_SINGLE);
    m_list.set_activate_on_single_click(true);
    m_list.signal_row_activated().connect([this](Gtk::ListBoxRow* row) {
        if (!row) return;
        m_popover.popdown();
        choose(row->get_index(), true);
    });
    m_scroll.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    m_scroll.set_propagate_natural_height(true);
    m_scroll.set_max_content_height(max_height);
    m_scroll.add(m_list);
    m_scroll.show_all();
    m_popover.get_style_context()->add_class("set-choice-pop");
    m_popover.set_position(Gtk::POS_BOTTOM);
    m_popover.add(m_scroll);
    set_popover(m_popover);
    // La bulle se rouvre sur l'element choisi, pas en haut de la liste.
    m_popover.signal_show().connect([this] {
        if (m_active < 0) return;
        if (auto* row = m_list.get_row_at_index(m_active)) {
            m_list.select_row(*row);
            row->grab_focus();
        }
        // Les rangees n'ont leur place qu'une fois la bulle dessinee : le
        // defilement jusqu'a l'element choisi attend donc ce premier passage.
        Glib::signal_idle().connect_once([this] {
            auto* row = m_active >= 0 ? m_list.get_row_at_index(m_active) : nullptr;
            if (!row) return;
            auto adj = m_scroll.get_vadjustment();
            const auto a = row->get_allocation();
            const double target = a.get_y() - (adj->get_page_size() - a.get_height()) / 2.0;
            adj->set_value(std::clamp(target, adj->get_lower(),
                                      adj->get_upper() - adj->get_page_size()));
        });
    });
}

void Choice::append(const std::string& id, const std::string& label) {
    m_items.emplace_back(id, label);
    auto* l = Gtk::make_managed<Gtk::Label>(label);
    l->set_xalign(0.0f);
    auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
    row->add(*l);
    row->show_all();
    m_list.append(*row);
}

void Choice::remove_all() {
    destroy_children(m_list);
    m_items.clear();
    m_active = -1;
    m_label.set_text("");
}

bool Choice::set_active_id(const std::string& id) {
    for (size_t i = 0; i < m_items.size(); ++i)
        if (m_items[i].first == id) { choose(static_cast<int>(i), true); return true; }
    return false;
}

void Choice::set_active(int index) {
    if (index >= 0 && index < static_cast<int>(m_items.size())) choose(index, true);
}

std::string Choice::get_active_id() const {
    return m_active >= 0 ? m_items[static_cast<size_t>(m_active)].first : std::string();
}

void Choice::choose(int index, bool emit) {
    if (index == m_active) return;
    m_active = index;
    m_label.set_text(m_items[static_cast<size_t>(index)].second);
    if (emit) m_changed.emit();
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
Fold fold(const Card& card, bool open_by_default,
          std::function<std::string()> describe,
          std::function<void()> toggled,
          std::function<void()> revealed) {
    Fold section;
    section.describe = std::move(describe);

    auto* chevron = Gtk::make_managed<Icon>("bc-chevron-down.svg", 15);
    chevron->set_valign(Gtk::ALIGN_CENTER);
    section.chevron = chevron;

    auto* summary = sub_label("");
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
     * presque rien quand on deplie, tout quand on replie. Le recalage de la
     * fenetre (fit_to_page des Settings), meme differe en idle, s'executait dans la premiere milliseconde de ces
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
     * qu'on verra : c'est la que `revealed` est appele.
     */
    if (revealed)
        reveal->property_child_revealed().signal_changed().connect(std::move(revealed));

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
    grip->signal_button_press_event().connect([reveal, toggled = std::move(toggled)](GdkEventButton* ev) {
        if (ev->type != GDK_BUTTON_PRESS || ev->button != 1) return false;
        reveal->set_reveal_child(!reveal->get_reveal_child());
        // Pas de recalage de la fenetre ICI : voir juste au-dessus, la
        // hauteur de la page ne sera connue qu'une fois l'animation finie.
        if (toggled) toggled();
        return true;
    });

    return section;
}

// Chevron dans le bon sens, resume visible seulement quand il remplace
// quelque chose.
void refresh_fold(const Fold& f) {
    if (!f.body) return;
    const bool open = f.body->get_reveal_child();
    if (f.chevron) f.chevron->set_file(open ? "bc-chevron-down.svg" : "bc-chevron-right.svg");
    if (!f.summary) return;
    const std::string text = (!open && f.describe) ? f.describe() : std::string();
    f.summary->set_text(text);
    f.summary->set_visible(!text.empty());
}

/* Une seule taille pour les fenetres de travail.
 *
 * Chacune avait la sienne (1180 de large, 1400 x 900, ou la hauteur de son
 * contenu) : passer de l'une a l'autre faisait sauter le cadre. La reference
 * est un ecran 1920 x 1080, celui d'une Steam Machine branchee sur une TV
 * (1080p, ou 4K affichee a 200 %) : 1400 x 900 y tient avec la barre des
 * taches et la barre de titre. La borne a 90 % de la zone de travail ne sert
 * qu'aux ecrans plus petits. */
void size_work_window(Gtk::Window& win, Gtk::Window* near) {
    int w = kWorkWindowWidth, h = kWorkWindowHeight;
    auto display = Gdk::Display::get_default();
    Glib::RefPtr<Gdk::Monitor> monitor;
    if (display) {
        if (near && near->get_window())
            monitor = display->get_monitor_at_window(near->get_window());
        if (!monitor) monitor = display->get_primary_monitor();
        if (!monitor && display->get_n_monitors() > 0) monitor = display->get_monitor(0);
    }
    if (monitor) {
        Gdk::Rectangle work;
        monitor->get_workarea(work);
        if (work.get_width() > 0)  w = std::min(w, work.get_width()  * 9 / 10);
        if (work.get_height() > 0) h = std::min(h, work.get_height() * 9 / 10);
    }
    win.set_default_size(w, h);
    win.get_style_context()->add_class("work-window");
}

/* Distinguer la fenetre de travail de la fenetre principale.
 *
 * Les deux ont le meme fond sombre : posee par-dessus, une fenetre de
 * reglages se confondait avec ce qu'il y avait derriere. La fenetre
 * principale s'efface donc (voir « behind-work » dans la feuille de style),
 * mais seulement quand elle n'est pas active : ces fenetres ne sont pas
 * modales, et un clic sur la bibliotheque doit la rendre lisible aussitot.
 * Un compteur, parce que Settings, ROM Management et les manettes peuvent
 * etre ouvertes ensemble. map / unmap plutot que show / hide : une fenetre
 * detruite sans avoir ete cachee passe tout de meme par unmap. */
void dim_behind(Gtk::Window& win, Gtk::Window& main) {
    static std::map<Gtk::Window*, int> open_count;
    Gtk::Window* m = &main;
    win.signal_map().connect([m] {
        if (open_count[m]++ == 0) m->get_style_context()->add_class("behind-work");
    });
    win.signal_unmap().connect([m] {
        if (open_count[m] > 0 && --open_count[m] == 0)
            m->get_style_context()->remove_class("behind-work");
    });
}

}  // namespace SettingsUi
