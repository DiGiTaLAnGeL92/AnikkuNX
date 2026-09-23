#include "html/html.hpp"

#include <gumbo.h>

#include <cctype>
#include <cstring>
#include <functional>

#include "net/http.hpp"

namespace html {

// ------------------------------------------------------------------------------------ nodo

static const char* tagName(const GumboNode* n) {
    if (n->type != GUMBO_NODE_ELEMENT) return "";
    const GumboElement& el = n->v.element;
    if (el.tag != GUMBO_TAG_UNKNOWN) return gumbo_normalized_tagname(el.tag);
    return nullptr;  // tag sconosciuto: si ricava dal testo originale
}

std::string Node::tag() const {
    if (!n || n->type != GUMBO_NODE_ELEMENT) return "";
    const char* t = tagName(n);
    if (t) return t;
    GumboStringPiece piece = n->v.element.original_tag;
    gumbo_tag_from_original_text(&piece);
    std::string s(piece.data, piece.length);
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string Node::attr(const std::string& name) const {
    if (!n || n->type != GUMBO_NODE_ELEMENT) return "";
    GumboAttribute* a = gumbo_get_attribute(&n->v.element.attributes, name.c_str());
    return a ? std::string(a->value) : "";
}

bool Node::hasAttr(const std::string& name) const {
    if (!n || n->type != GUMBO_NODE_ELEMENT) return false;
    return gumbo_get_attribute(&n->v.element.attributes, name.c_str()) != nullptr;
}

static void collectText(const GumboNode* n, std::string& out) {
    if (n->type == GUMBO_NODE_TEXT || n->type == GUMBO_NODE_WHITESPACE || n->type == GUMBO_NODE_CDATA) {
        out += n->v.text.text;
        return;
    }
    if (n->type != GUMBO_NODE_ELEMENT && n->type != GUMBO_NODE_DOCUMENT) return;
    if (n->type == GUMBO_NODE_ELEMENT) {
        GumboTag t = n->v.element.tag;
        if (t == GUMBO_TAG_SCRIPT || t == GUMBO_TAG_STYLE) return;
        if (t == GUMBO_TAG_BR) {
            out += ' ';
            return;
        }
    }
    const GumboVector& ch = n->type == GUMBO_NODE_ELEMENT ? n->v.element.children : n->v.document.children;
    for (unsigned i = 0; i < ch.length; i++) collectText((GumboNode*)ch.data[i], out);
    // gli elementi di blocco separano le parole
    if (n->type == GUMBO_NODE_ELEMENT) out += ' ';
}

static std::string normalizeSpaces(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool space = false;
    for (unsigned char c : s) {
        bool isSpace = c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f';
        // nbsp in UTF-8 viene lasciato com'e'
        if (isSpace) {
            space = true;
        } else {
            if (space && !out.empty()) out += ' ';
            space = false;
            out += (char)c;
        }
    }
    return out;
}

std::string Node::text() const {
    if (!n) return "";
    std::string raw;
    collectText(n, raw);
    return normalizeSpaces(raw);
}

std::string Node::data() const {
    if (!n || n->type != GUMBO_NODE_ELEMENT) return "";
    std::string out;
    const GumboVector& ch = n->v.element.children;
    for (unsigned i = 0; i < ch.length; i++) {
        auto* c = (GumboNode*)ch.data[i];
        if (c->type == GUMBO_NODE_TEXT || c->type == GUMBO_NODE_WHITESPACE || c->type == GUMBO_NODE_CDATA)
            out += c->v.text.text;
    }
    return out;
}

std::vector<Node> Node::children() const {
    std::vector<Node> out;
    if (!n || n->type != GUMBO_NODE_ELEMENT) return out;
    const GumboVector& ch = n->v.element.children;
    for (unsigned i = 0; i < ch.length; i++) {
        auto* c = (GumboNode*)ch.data[i];
        if (c->type == GUMBO_NODE_ELEMENT) out.emplace_back(c);
    }
    return out;
}

Node Node::parent() const {
    if (!n || !n->parent || n->parent->type != GUMBO_NODE_ELEMENT) return Node();
    return Node(n->parent);
}

// ------------------------------------------------------------------------------------ selettori

namespace {

struct AttrCond {
    std::string name;
    char op = 0;  // 0 = esiste, '=' '*' '^' '$' '~'
    std::string value;
};

struct Compound {
    std::string tag;  // vuoto o "*" = qualsiasi
    std::vector<std::string> classes;
    std::string id;
    std::vector<AttrCond> attrs;
    std::vector<Compound> nots;
};

struct Step {
    Compound compound;
    char combinator = ' ';  // relazione con lo step precedente: ' ' discendente, '>' figlio
};

using Chain = std::vector<Step>;

class Parser {
  public:
    explicit Parser(const std::string& s) : s(s) {}

    std::vector<Chain> parseList() {
        std::vector<Chain> list;
        while (true) {
            skipWs();
            list.push_back(parseChain());
            skipWs();
            if (pos < s.size() && s[pos] == ',') {
                pos++;
                continue;
            }
            break;
        }
        return list;
    }

  private:
    const std::string& s;
    size_t pos = 0;

    void skipWs() {
        while (pos < s.size() && std::isspace((unsigned char)s[pos])) pos++;
    }

    static bool identChar(char c) {
        return std::isalnum((unsigned char)c) || c == '-' || c == '_' || (unsigned char)c >= 0x80;
    }

    std::string ident() {
        size_t start = pos;
        while (pos < s.size() && identChar(s[pos])) pos++;
        return s.substr(start, pos - start);
    }

    Chain parseChain() {
        Chain chain;
        char comb = ' ';
        while (pos < s.size()) {
            skipWs();
            if (pos >= s.size() || s[pos] == ',' || s[pos] == ')') break;
            if (s[pos] == '>') {
                comb = '>';
                pos++;
                continue;
            }
            Step st;
            st.combinator = chain.empty() ? ' ' : comb;
            st.compound = parseCompound();
            chain.push_back(std::move(st));
            comb = ' ';
        }
        return chain;
    }

    Compound parseCompound() {
        Compound c;
        if (pos < s.size() && s[pos] == '*') {
            pos++;
            c.tag = "*";
        } else if (pos < s.size() && identChar(s[pos])) {
            c.tag = ident();
            for (auto& ch : c.tag) ch = (char)std::tolower((unsigned char)ch);
        }
        while (pos < s.size()) {
            char ch = s[pos];
            if (ch == '.') {
                pos++;
                c.classes.push_back(ident());
            } else if (ch == '#') {
                pos++;
                c.id = ident();
            } else if (ch == '[') {
                pos++;
                c.attrs.push_back(parseAttr());
            } else if (ch == ':') {
                pos++;
                std::string pseudo = ident();
                if (pseudo == "not" && pos < s.size() && s[pos] == '(') {
                    pos++;
                    skipWs();
                    c.nots.push_back(parseCompound());
                    skipWs();
                    if (pos < s.size() && s[pos] == ')') pos++;
                }
            } else {
                break;
            }
        }
        return c;
    }

    AttrCond parseAttr() {
        AttrCond a;
        skipWs();
        a.name = ident();
        skipWs();
        if (pos < s.size() && s[pos] != ']') {
            char op = s[pos];
            if (op == '=') {
                a.op = '=';
                pos++;
            } else if ((op == '*' || op == '^' || op == '$' || op == '~') && pos + 1 < s.size() && s[pos + 1] == '=') {
                a.op = op;
                pos += 2;
            }
            skipWs();
            if (pos < s.size() && (s[pos] == '"' || s[pos] == '\'')) {
                char q = s[pos++];
                size_t start = pos;
                while (pos < s.size() && s[pos] != q) pos++;
                a.value = s.substr(start, pos - start);
                if (pos < s.size()) pos++;
            } else {
                size_t start = pos;
                while (pos < s.size() && s[pos] != ']') pos++;
                a.value = s.substr(start, pos - start);
                while (!a.value.empty() && std::isspace((unsigned char)a.value.back())) a.value.pop_back();
            }
            skipWs();
        }
        if (pos < s.size() && s[pos] == ']') pos++;
        return a;
    }
};

bool hasClass(const Node& n, const std::string& cls) {
    std::string c = n.attr("class");
    size_t pos = 0;
    while (pos < c.size()) {
        while (pos < c.size() && std::isspace((unsigned char)c[pos])) pos++;
        size_t start = pos;
        while (pos < c.size() && !std::isspace((unsigned char)c[pos])) pos++;
        if (c.compare(start, pos - start, cls) == 0 && pos - start == cls.size()) return true;
    }
    return false;
}

bool matchCompound(const Node& n, const Compound& c) {
    if (!n.valid() || n.raw()->type != GUMBO_NODE_ELEMENT) return false;
    if (!c.tag.empty() && c.tag != "*" && n.tag() != c.tag) return false;
    if (!c.id.empty() && n.attr("id") != c.id) return false;
    for (auto& cls : c.classes)
        if (!hasClass(n, cls)) return false;
    for (auto& a : c.attrs) {
        if (!n.hasAttr(a.name)) return false;
        std::string v = n.attr(a.name);
        switch (a.op) {
            case '=':
                if (v != a.value) return false;
                break;
            case '*':
                if (v.find(a.value) == std::string::npos) return false;
                break;
            case '^':
                if (v.rfind(a.value, 0) != 0) return false;
                break;
            case '$':
                if (v.size() < a.value.size() || v.compare(v.size() - a.value.size(), a.value.size(), a.value) != 0)
                    return false;
                break;
            case '~': {
                std::string padded = " " + v + " ";
                if (padded.find(" " + a.value + " ") == std::string::npos) return false;
                break;
            }
            default: break;
        }
    }
    for (auto& nc : c.nots)
        if (matchCompound(n, nc)) return false;
    return true;
}

/** Verifica la catena da destra verso sinistra partendo dall'indice idx. */
bool matchChain(const Node& n, const Chain& chain, int idx, const GumboNode* scope) {
    if (!matchCompound(n, chain[idx].compound)) return false;
    if (idx == 0) return true;
    char comb = chain[idx].combinator;
    (void)scope;  // come Jsoup, anche il nodo di partenza e i suoi antenati possono soddisfare la catena
    Node p = n.parent();
    if (comb == '>') {
        if (!p.valid()) return false;
        return matchChain(p, chain, idx - 1, scope);
    }
    while (p.valid()) {
        if (matchChain(p, chain, idx - 1, scope)) return true;
        p = p.parent();
    }
    return false;
}

void walk(GumboNode* n, const std::function<void(GumboNode*)>& fn) {
    if (n->type != GUMBO_NODE_ELEMENT && n->type != GUMBO_NODE_DOCUMENT) return;
    const GumboVector& ch = n->type == GUMBO_NODE_ELEMENT ? n->v.element.children : n->v.document.children;
    for (unsigned i = 0; i < ch.length; i++) {
        auto* c = (GumboNode*)ch.data[i];
        if (c->type == GUMBO_NODE_ELEMENT) {
            fn(c);
            walk(c, fn);
        }
    }
}

}  // namespace

std::vector<Node> Node::select(const std::string& selector) const {
    std::vector<Node> out;
    if (!n) return out;
    auto chains = Parser(selector).parseList();
    // i discendenti vanno cercati sotto questo nodo; per il documento lo "scope" e' nullptr
    const GumboNode* scope = n->type == GUMBO_NODE_DOCUMENT ? nullptr : n;
    walk(n, [&](GumboNode* c) {
        Node cand(c);
        for (auto& chain : chains) {
            if (chain.empty()) continue;
            if (matchChain(cand, chain, (int)chain.size() - 1, scope)) {
                out.push_back(cand);
                break;
            }
        }
    });
    return out;
}

Node Node::selectFirst(const std::string& selector) const {
    auto all = select(selector);
    return all.empty() ? Node() : all.front();
}

std::string textOf(const std::vector<Node>& nodes) {
    std::string out;
    for (auto& n : nodes) {
        std::string t = n.text();
        if (t.empty()) continue;
        if (!out.empty()) out += ' ';
        out += t;
    }
    return out;
}

// ------------------------------------------------------------------------------------ documento

Document::Document(const std::string& html, std::string url) : source(html), location(std::move(url)) {
    out = gumbo_parse_with_options(&kGumboDefaultOptions, source.c_str(), source.size());
}

Document::~Document() {
    if (out) gumbo_destroy_output(&kGumboDefaultOptions, out);
}

Node Document::root() const { return out ? Node(out->document) : Node(); }

std::string Document::absUrl(const Node& n, const std::string& attr) const {
    std::string v = n.attr(attr);
    if (v.empty()) return "";
    return http::resolve(location, v);
}

}  // namespace html
