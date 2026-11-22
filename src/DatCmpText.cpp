// src/DatCmpText.cpp
#include "DatCmpText.h"
#include <pugixml.hpp>
#include <fstream>
#include <filesystem>
#include <cstring>
#include <initializer_list>

namespace {

bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

// Tokens : "(", ")", a quoted string (\" inside it is a quote) or a bare
// word, which ends at a blank or a parenthesis.
struct Lexer {
    const std::string& s;
    size_t p = 0;
    enum Kind { End, Open, Close, Word };
    Kind kind = End;
    std::string word;

    explicit Lexer(const std::string& text) : s(text) {
        if (s.compare(0, 3, "\xEF\xBB\xBF") == 0) p = 3;
    }
    Kind next() {
        while (p < s.size() && is_blank(s[p])) ++p;
        word.clear();
        if (p >= s.size()) return kind = End;
        const char c = s[p];
        if (c == '(') { ++p; return kind = Open; }
        if (c == ')') { ++p; return kind = Close; }
        if (c == '"') {
            ++p;
            while (p < s.size() && s[p] != '"') {
                if (s[p] == '\\' && p + 1 < s.size() && s[p + 1] == '"') ++p;
                word += s[p++];
            }
            if (p < s.size()) ++p;   // closing quote
            return kind = Word;
        }
        while (p < s.size() && !is_blank(s[p]) && s[p] != '(' && s[p] != ')') word += s[p++];
        return kind = Word;
    }
};

bool one_of(const std::string& k, std::initializer_list<const char*> list) {
    for (const char* x : list) if (k == x) return true;
    return false;
}

void set_attr(pugi::xml_node el, const std::string& key, const std::string& value) {
    if (!el.attribute(key.c_str())) el.append_attribute(key.c_str()).set_value(value.c_str());
}

void add_text_child(pugi::xml_node el, const std::string& key, const std::string& value) {
    el.append_child(key.c_str()).text().set(value.c_str());
}

enum class Ctx { Header, Set, Item };

// The format's own words for what a Logiqx datafile names differently.
void finish_item(pugi::xml_node el) {
    const std::string tag = el.name();
    if (tag == "rom" || tag == "disk") {
        // flags nodump / baddump : the datafile's status=.
        if (auto f = el.attribute("flags"); f && !el.attribute("status")) set_attr(el, "status", f.value());
    } else if (tag == "video") {
        if (auto a = el.attribute("x")) set_attr(el, "width", a.value());
        if (auto a = el.attribute("y")) set_attr(el, "height", a.value());
        if (auto a = el.attribute("freq")) set_attr(el, "refresh", a.value());
    }
}

// The inside of one `key ( ... )` block, up to its closing parenthesis.
bool read_block(Lexer& lx, pugi::xml_node el, Ctx ctx, std::string* error) {
    pugi::xml_node cmp;   // the header's <clrmamepro .../>
    for (;;) {
        Lexer::Kind k = lx.next();
        if (k == Lexer::Close) break;
        if (k == Lexer::End) { if (error) *error = "unexpected end of file inside a block"; return false; }
        if (k == Lexer::Open) {   // a block with no key : read and dropped
            pugi::xml_document scrap;
            if (!read_block(lx, scrap.append_child("x"), Ctx::Item, error)) return false;
            continue;
        }
        const std::string key = lx.word;
        k = lx.next();
        if (k == Lexer::End) { if (error) *error = "unexpected end of file inside a block"; return false; }
        if (k == Lexer::Close) break;   // a key with no value closes the block
        if (k == Lexer::Open) {
            pugi::xml_node child = el.append_child(key.c_str());
            if (!read_block(lx, child, Ctx::Item, error)) return false;
            finish_item(child);
            continue;
        }
        const std::string& value = lx.word;
        switch (ctx) {
        case Ctx::Header:
            if (one_of(key, {"forcemerging", "forcenodump", "forcepacking", "forcezipping", "header"})) {
                if (!cmp) cmp = el.append_child("clrmamepro");
                set_attr(cmp, key, value);
            } else {
                add_text_child(el, key, value);
            }
            break;
        case Ctx::Set:
            if (one_of(key, {"description", "year", "manufacturer", "comment", "category",
                             "genre", "family", "players", "hiscore"}))
                add_text_child(el, key, value);
            else if (one_of(key, {"sample", "device_ref", "archive"}))
                el.append_child(key.c_str()).append_attribute("name").set_value(value.c_str());
            else
                set_attr(el, key, value);
            break;
        case Ctx::Item:
            set_attr(el, key, value);
            break;
        }
    }
    return true;
}

bool load(const std::string& text, pugi::xml_document& doc, std::string* error) {
    doc.reset();
    if (!DatCmpText::looks_like(text)) {
        if (error) *error = "not a ClrMamePro text DAT";
        return false;
    }
    pugi::xml_node root = doc.append_child("datafile");
    pugi::xml_node header;
    Lexer lx(text);
    for (;;) {
        Lexer::Kind k = lx.next();
        if (k == Lexer::End) break;
        if (k != Lexer::Word) continue;   // a stray parenthesis
        const std::string key = lx.word;
        if (lx.next() != Lexer::Open) {   // a top-level `key value` : nothing to keep
            if (lx.kind == Lexer::End) break;
            continue;
        }
        if (key == "clrmamepro") {
            if (!header) header = root.prepend_child("header");
            if (!read_block(lx, header, Ctx::Header, error)) return false;
        } else if (key == "game" || key == "machine" || key == "resource") {
            // A resource is a BIOS set.
            pugi::xml_node set = root.append_child(key == "machine" ? "machine" : "game");
            if (key == "resource") set.append_attribute("isbios").set_value("yes");
            if (!read_block(lx, set, Ctx::Set, error)) return false;
        } else {
            pugi::xml_node other = root.append_child(key.c_str());
            if (!read_block(lx, other, Ctx::Item, error)) return false;
        }
    }
    return true;
}

} // namespace

namespace DatCmpText {

bool looks_like(const std::string& text) {
    Lexer lx(text);
    if (lx.next() != Lexer::Word) return false;
    const std::string first = lx.word;
    if (!(first == "clrmamepro" || first == "game" || first == "machine" || first == "resource")) return false;
    return lx.next() == Lexer::Open;
}

bool is_file(const std::string& path) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) return false;
    std::string head(4096, '\0');
    in.read(&head[0], (std::streamsize)head.size());
    head.resize((size_t)in.gcount());
    return looks_like(head);
}

bool load_file(const std::string& path, pugi::xml_document& doc, std::string* error) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) { if (error) *error = "cannot open the file"; return false; }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return load(text, doc, error);
}

bool load_string(const std::string& text, pugi::xml_document& doc, std::string* error) {
    return load(text, doc, error);
}

} // namespace DatCmpText
