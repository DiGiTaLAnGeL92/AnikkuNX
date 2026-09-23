// Prova da riga di comando delle fonti integrate (senza interfaccia).
// Uso: sourcetest [id-fonte|nome|lingua (it/en/all)|18+|tutte] [ricerca] [cacert]
#include <cstdio>
#include <iostream>

#include "net/http.hpp"
#include "sources/registry.hpp"
#include "sources/source.hpp"

// senza interfaccia: i testi restano in italiano (sostituisce util/i18n.cpp che richiede borealis)
static std::string fillArg(std::string s, const std::string& a) {
    auto p = s.find("{}");
    if (p != std::string::npos) s.replace(p, 2, a);
    return s;
}
std::string tr(const std::string& s) { return s; }
std::string tr(const std::string& s, const std::string& a) { return fillArg(s, a); }
std::string tr(const std::string& s, const std::string& a, const std::string& b) { return fillArg(fillArg(s, a), b); }

static void test(src::Source& s, const std::string& query) {
    std::cout << "\n===== " << s.name() << " (" << s.baseUrl() << ")\n";
    try {
        auto p = s.popular(1);
        std::cout << "popolari: " << p.animes.size() << " (altre pagine: " << p.hasNextPage << ")\n";
        for (size_t i = 0; i < p.animes.size() && i < 3; i++)
            std::cout << "  - " << p.animes[i].title << " | " << p.animes[i].url << " | " << p.animes[i].thumbnail << "\n";
    } catch (const std::exception& e) { std::cout << "popolari ERRORE: " << e.what() << "\n"; }
    try {
        auto l = s.latest(1);
        std::cout << "recenti: " << l.animes.size() << "\n";
        for (size_t i = 0; i < l.animes.size() && i < 2; i++) std::cout << "  - " << l.animes[i].title << " | " << l.animes[i].url << "\n";
    } catch (const std::exception& e) { std::cout << "recenti ERRORE: " << e.what() << "\n"; }
    src::Page r;
    try {
        r = s.search(query, 1);
        std::cout << "ricerca '" << query << "': " << r.animes.size() << "\n";
        for (size_t i = 0; i < r.animes.size() && i < 3; i++) std::cout << "  - " << r.animes[i].title << " | " << r.animes[i].url << "\n";
    } catch (const std::exception& e) { std::cout << "ricerca ERRORE: " << e.what() << "\n"; }
    if (r.animes.empty()) return;
    try {
        auto d = s.details(r.animes[0].url);
        std::cout << "dettagli: " << d.title << " | stato=" << d.status << " | generi=" << d.genre << " | autore=" << d.author
                  << "\n  copertina=" << d.thumbnail << "\n  trama=" << d.description.substr(0, 120) << "\n  episodi: " << d.episodes.size() << "\n";
        for (size_t i = 0; i < d.episodes.size() && i < 3; i++)
            std::cout << "  - " << d.episodes[i].name << " #" << d.episodes[i].number << " | " << d.episodes[i].url << "\n";
        if (d.episodes.empty()) return;
        auto vids = s.videos(d.episodes.back().url);
        std::cout << "video (" << d.episodes.back().name << "): " << vids.size() << "\n";
        for (auto& v : vids) {
            std::cout << "  - " << v.title << " | " << v.url << " | referer=" << v.referer << "\n";
        }
        if (!vids.empty()) {
            http::Headers h;
            if (!vids[0].referer.empty()) h.push_back({"Referer", vids[0].referer});
            if (!vids[0].userAgent.empty()) h.push_back({"User-Agent", vids[0].userAgent});
            if (!vids[0].cookie.empty()) h.push_back({"Cookie", vids[0].cookie});
            for (auto& kv : vids[0].headers) h.push_back(kv);
            h.push_back({"Range", "bytes=0-2047"});
            auto resp = http::request("GET", vids[0].url, h, "", 30);
            std::cout << "  prova download: HTTP " << resp.status << " tipo=" << resp.header("content-type")
                      << " byte=" << resp.body.size() << " inizio=" << resp.body.substr(0, 40) << "\n";
        }
    } catch (const std::exception& e) { std::cout << "dettagli/video ERRORE: " << e.what() << "\n"; }
}

int main(int argc, char** argv) {
    http::globalInit(argc > 3 ? argv[3] : "");
    std::string which = argc > 1 ? argv[1] : "tutte";
    std::string query = argc > 2 ? argv[2] : "naruto";
    for (auto& s : src::all())
        if ((which == "tutte" && !s->nsfw()) || which == s->id() || which == s->name() ||
            (which == s->lang() && !s->nsfw()) || (which == "18+" && s->nsfw()))
            test(*s, query);
    http::globalCleanup();
}
