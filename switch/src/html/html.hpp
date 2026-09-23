#pragma once

#include <memory>
#include <string>
#include <vector>

struct GumboInternalNode;
struct GumboInternalOutput;

namespace html {

/**
 * Nodo HTML con un sottoinsieme dei selettori CSS usati dalle estensioni (stile Jsoup):
 * tag, *, .classe, #id, [attr], [attr=v], [attr*=v], [attr^=v], [attr$=v], [attr~=v],
 * :not(...), combinatori discendente (spazio) e figlio (>), liste separate da virgola.
 */
class Node {
  public:
    Node() = default;
    explicit Node(GumboInternalNode* n) : n(n) {}

    bool valid() const { return n != nullptr; }
    explicit operator bool() const { return valid(); }

    std::string tag() const;
    std::string attr(const std::string& name) const;
    bool hasAttr(const std::string& name) const;
    /** Testo di tutti i discendenti con spazi normalizzati (come Element.text() di Jsoup). */
    std::string text() const;
    /** Contenuto grezzo dei nodi di testo figli (per <script>). */
    std::string data() const;
    std::vector<Node> children() const;
    Node parent() const;

    std::vector<Node> select(const std::string& selector) const;
    Node selectFirst(const std::string& selector) const;

    GumboInternalNode* raw() const { return n; }

  private:
    GumboInternalNode* n = nullptr;
};

class Document {
  public:
    explicit Document(const std::string& html, std::string url = "");
    ~Document();
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    Node root() const;
    std::vector<Node> select(const std::string& s) const { return root().select(s); }
    Node selectFirst(const std::string& s) const { return root().selectFirst(s); }
    /** Risolve un attributo URL rispetto all'indirizzo della pagina (come "abs:href"). */
    std::string absUrl(const Node& n, const std::string& attr) const;
    const std::string& url() const { return location; }

  private:
    std::string source;  // gumbo punta dentro a questa stringa
    std::string location;
    GumboInternalOutput* out = nullptr;
};

/** Testo di una lista di nodi unito da uno spazio (Elements.text()). */
std::string textOf(const std::vector<Node>& nodes);

}  // namespace html
