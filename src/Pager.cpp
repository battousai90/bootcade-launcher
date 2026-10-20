// src/Pager.cpp
#include "Pager.h"

#include "AppContext.h"
#include "SettingsUi.h"
#include "i18n.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace SettingsUi {
namespace {

constexpr int kSizes[] = {10, 20, 50, 100};
constexpr int kDefaultSize = 50;

int load_size() {
    try {
        std::ifstream in(AppContext::get_config_path());
        nlohmann::json j; in >> j;
        const int v = j.value("list_page_size", kDefaultSize);
        for (int s : kSizes) if (s == v) return v;
    } catch (...) {}
    return kDefaultSize;
}

void save_size(int size) {
    nlohmann::json j;
    const std::string path = AppContext::get_config_path();
    { std::ifstream in(path); if (in) { try { in >> j; } catch (...) { j = nlohmann::json::object(); } } }
    if (!j.is_object()) j = nlohmann::json::object();
    j["list_page_size"] = size;
    std::ofstream out(path, std::ios::trunc);
    if (out) out << j.dump(4) << std::endl;
}

}  // namespace

Pager::Pager() : Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 10) {
    set_margin_top(8);
    auto* rows_label = Gtk::make_managed<Gtk::Label>(_("Rows per page"));
    rows_label->get_style_context()->add_class("set-sub");
    pack_start(*rows_label, Gtk::PACK_SHRINK);
    for (int s : kSizes) m_combo.append(std::to_string(s), std::to_string(s));
    m_size = load_size();
    m_combo.set_active_id(std::to_string(m_size));
    m_combo.signal_changed().connect([this] {
        if (m_updating) return;
        const int v = std::atoi(m_combo.get_active_id().c_str());
        if (v <= 0 || v == m_size) return;
        m_size = v;
        save_size(v);
        m_index = 0;
        update();
        m_changed.emit();
    });
    pack_start(m_combo, Gtk::PACK_SHRINK);

    // Les commandes a droite : premiere, precedente, « Page 3 / 120 », suivante, derniere.
    auto mk = [](Gtk::Button& b, const char* text, const Glib::ustring& tip) {
        b.set_label(text);
        b.set_tooltip_text(tip);
        b.get_style_context()->add_class("set-btn");
    };
    mk(m_first, "«", _("First page"));
    mk(m_prev,  "‹", _("Previous page"));
    mk(m_next,  "›", _("Next page"));
    mk(m_last,  "»", _("Last page"));
    m_first.signal_clicked().connect([this] { go(0); });
    m_prev.signal_clicked().connect([this] { go(m_index - 1); });
    m_next.signal_clicked().connect([this] { go(m_index + 1); });
    m_last.signal_clicked().connect([this] { go(1 << 30); });
    m_label.get_style_context()->add_class("set-sub");
    pack_end(m_last, Gtk::PACK_SHRINK);
    pack_end(m_next, Gtk::PACK_SHRINK);
    pack_end(m_label, Gtk::PACK_SHRINK);
    pack_end(m_prev, Gtk::PACK_SHRINK);
    pack_end(m_first, Gtk::PACK_SHRINK);
    update();
}

void Pager::set_total(int rows) {
    m_total = std::max(0, rows);
    const int pages = std::max(1, (m_total + m_size - 1) / m_size);
    m_index = std::min(m_index, pages - 1);
    update();
}

void Pager::go(int index) {
    const int pages = std::max(1, (m_total + m_size - 1) / m_size);
    index = std::max(0, std::min(index, pages - 1));
    if (index == m_index) return;
    m_index = index;
    update();
    m_changed.emit();
}

void Pager::update() {
    const int pages = std::max(1, (m_total + m_size - 1) / m_size);
    m_label.set_text(Glib::ustring::compose(_("Page %1 / %2"), m_index + 1, pages));
    m_first.set_sensitive(m_index > 0);
    m_prev.set_sensitive(m_index > 0);
    m_next.set_sensitive(m_index < pages - 1);
    m_last.set_sensitive(m_index < pages - 1);
    m_updating = true;
    m_combo.set_active_id(std::to_string(m_size));
    m_updating = false;
}

}  // namespace SettingsUi
