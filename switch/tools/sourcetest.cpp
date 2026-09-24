// Prova approfondita delle fonti integrate (senza interfaccia): catalogo, ricerca, dettagli e
// riproducibilita' reale dei video, facendo gli stessi passaggi del player (playlist HLS, chiave AES,
// primo segmento, MP4, DASH, sottotitoli).
// Uso: sourcetest [id-fonte|nome|lingua (it/en/all)|18+|tutte] [ricerca] [cacert]
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include <cstring>
#include <iostream>
#include <sstream>
#include <vector>

#include "net/http.hpp"
#include "net/hls_proxy.hpp"
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

namespace {

struct Summary {
    std::string name;
    std::string popular = "-", latest = "-", search = "-", details = "-";
    int videos = 0, playable = 0;
};

std::string cut(const std::string& s, size_t n = 110) { return s.size() > n ? s.substr(0, n) + "..." : s; }

http::Headers playerHeaders(const src::Video& v) {
    http::Headers h;
    h.push_back({"User-Agent", v.userAgent.empty() ? http::DEFAULT_UA : v.userAgent});
    if (!v.referer.empty()) h.push_back({"Referer", v.referer});
    if (!v.cookie.empty()) h.push_back({"Cookie", v.cookie});
    for (auto& kv : v.headers) {
        std::string k = kv.first;
        for (auto& c : k) c = (char)tolower((unsigned char)c);
        if (k != "referer" && k != "user-agent") h.push_back(kv);
    }
    return h;
}

http::Response fetchRange(const std::string& url, http::Headers h, int bytes) {
    if (bytes > 0) h.push_back({"Range", "bytes=0-" + std::to_string(bytes - 1)});
    return http::request("GET", url, h, "", 25);
}

/** Tipo del contenuto di un segmento o file video. */
std::string classify(const std::string& b) {
    auto at = [&](size_t off, const char* sig) {
        size_t n = strlen(sig);
        return b.size() >= off + n && b.compare(off, n, sig) == 0;
    };
    if (b.empty()) return "vuoto";
    if ((unsigned char)b[0] == 0x47 && (b.size() < 189 || (unsigned char)b[188] == 0x47)) return "MPEG-TS";
    if (at(4, "ftyp") || at(4, "styp") || at(4, "moof") || at(4, "sidx")) return "MP4/fMP4";
    if (at(0, "\x1A\x45\xDF\xA3")) return "WebM/MKV";
    if (at(0, "ID3") || (b.size() > 1 && (unsigned char)b[0] == 0xFF && ((unsigned char)b[1] & 0xF0) == 0xF0))
        return "audio AAC/ID3";
    // segmenti camuffati da immagine: cerca il sync TS dopo l'intestazione finta
    if (at(0, "\x89PNG") || at(0, "\xFF\xD8\xFF") || at(0, "GIF8") || at(0, "BM")) {
        for (size_t i = 0; i + 376 < b.size() && i < 4096; i++)
            if ((unsigned char)b[i] == 0x47 && (unsigned char)b[i + 188] == 0x47 && (unsigned char)b[i + 376] == 0x47)
                return "MPEG-TS (camuffato da immagine)";
        return "immagine (non video?)";
    }
    std::string head = b.substr(0, 200);
    for (auto& c : head) c = (char)tolower((unsigned char)c);
    if (head.find("<html") != std::string::npos || head.find("<!doctype") != std::string::npos) return "HTML (errore/protezione)";
    return "dati binari (probabilmente cifrati)";
}

std::vector<std::string> lines(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string l;
    while (std::getline(in, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        out.push_back(l);
    }
    return out;
}

std::string attr(const std::string& line, const std::string& name) {
    auto p = line.find(name + "=");
    if (p == std::string::npos) return "";
    p += name.size() + 1;
    if (p < line.size() && line[p] == '"') {
        auto e = line.find('"', p + 1);
        return line.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1);
    }
    auto e = line.find(',', p);
    return line.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

/** Verifica che il video sia riproducibile come farebbe mpv. Ritorna "" se ok, altrimenti il motivo. */
std::string probe(const src::Video& v, std::string& detail) {
    http::Headers h = playerHeaders(v);
    // come mpv: le playlist si chiedono intere (alcuni CDN rifiutano il Range sulle .m3u8)
    bool looksPlaylist = v.url.find(".m3u8") != std::string::npos || v.url.find(".mpd") != std::string::npos ||
                         v.url.find("/manifest") != std::string::npos || v.url.find("playlist") != std::string::npos;
    http::Response r = fetchRange(v.url, h, looksPlaylist ? 0 : 65536);
    if (r.status >= 400) {
        detail += "url completo: " + v.url + " | risposta: " + cut(r.body, 200);
        return "HTTP " + std::to_string(r.status) + " sul link del video";
    }
    std::string body = r.body;
    std::string base = r.finalUrl.empty() ? v.url : r.finalUrl;

    if (body.find("#EXTM3U") < 16) {
        auto ls = lines(body);
        for (size_t i = 0; i < ls.size(); i++) {
            if (ls[i].rfind("#EXT-X-STREAM-INF", 0) != 0) continue;
            for (size_t j = i + 1; j < ls.size(); j++) {
                if (ls[j].empty() || ls[j][0] == '#') continue;
                std::string variant = http::resolve(base, ls[j]);
                r = fetchRange(variant, h, 0);
                if (r.status >= 400) {
                    detail += "variante: " + variant;
                    return "HTTP " + std::to_string(r.status) + " sulla playlist della qualita'";
                }
                base = r.finalUrl.empty() ? variant : r.finalUrl;
                ls = lines(r.body);
                detail += "master->variante; ";
                break;
            }
            break;
        }
        std::string segment, keyUri;
        int segCount = 0;
        for (auto& l : ls) {
            if (l.rfind("#EXT-X-KEY", 0) == 0 && keyUri.empty()) {
                if (attr(l, "METHOD") != "NONE") keyUri = attr(l, "URI");
            } else if (l.rfind("#EXT-X-MAP", 0) == 0 && segment.empty()) {
                segment = http::resolve(base, attr(l, "URI"));  // init fMP4
            } else if (!l.empty() && l[0] != '#') {
                segCount++;
                if (segment.empty()) segment = http::resolve(base, l);
            }
        }
        if (segment.empty()) return "playlist senza segmenti";
        detail += std::to_string(segCount) + " segmenti; ";
        if (!keyUri.empty()) {
            http::Response k = http::request("GET", http::resolve(base, keyUri), h, "", 20);
            if (k.status >= 400) return "HTTP " + std::to_string(k.status) + " sulla chiave AES";
            if (k.body.size() != 16) return "chiave AES di " + std::to_string(k.body.size()) + " byte (attesi 16)";
            detail += "chiave AES ok; ";
        }
        http::Response seg = fetchRange(segment, h, 4096);
        if (seg.status >= 400) return "HTTP " + std::to_string(seg.status) + " sul primo segmento";
        std::string type = classify(seg.body);
        detail += "segmento: " + type;
        if (type.find("camuffato") != std::string::npos) {
            // come nell'app: needsProxy + passaggio dal proxy locale, poi si rilegge il primo segmento
            bool need = hlsproxy::needsProxy(v.url, h);
            std::string local = need ? hlsproxy::wrap(v.url, h) : "";
            if (local.empty()) return need ? "proxy locale non avviato" : "segmento camuffato ma needsProxy=no";
            http::Response p = http::request("GET", local, {}, "", 60);
            auto pls = lines(p.body);
            std::string next;
            for (int depth = 0; depth < 2 && p.status == 200; depth++) {
                next.clear();
                bool master = p.body.find("#EXT-X-STREAM-INF") != std::string::npos;
                for (auto& l : pls)
                    if (!l.empty() && l[0] != '#') { next = l; break; }
                if (next.empty()) break;
                p = http::request("GET", next, {}, "", 60);
                if (!master) break;
                pls = lines(p.body);
            }
            std::string ptype = p.status == 200 ? classify(p.body) : "HTTP " + std::to_string(p.status);
            detail += " -> via proxy: " + ptype;
            if (ptype.find("camuffato") != std::string::npos || ptype.rfind("MPEG-TS", 0) != 0)
                return "il proxy non ripulisce il segmento (" + ptype + ")";
        }
        if (type.find("HTML") != std::string::npos || type == "vuoto" || type.find("non video") != std::string::npos)
            return "primo segmento non valido (" + type + ")";
        if (keyUri.empty() && type.find("cifrati") != std::string::npos) return "segmento non riconosciuto";
        return "";
    }
    std::string lower = body.substr(0, 400);
    for (auto& c : lower) c = (char)tolower((unsigned char)c);
    if (lower.find("<mpd") != std::string::npos) {
        detail += "DASH (.mpd)";
        return "";
    }
    std::string type = classify(body);
    detail += type;
    if (type == "MP4/fMP4" || type == "WebM/MKV" || type.rfind("MPEG-TS", 0) == 0) return "";
    return "contenuto non riproducibile (" + type + ")";
}

std::string probeSub(const src::Video::Track& t, const src::Video& v) {
    http::Response r = fetchRange(t.url, playerHeaders(v), 2048);
    if (r.status >= 400) return "HTTP " + std::to_string(r.status);
    if (r.body.find("WEBVTT") != std::string::npos || r.body.find("-->") != std::string::npos) return "ok (VTT/SRT)";
    if (r.body.find("[Script Info]") != std::string::npos) return "ok (ASS)";
    return "formato sconosciuto";
}

// Con ANX_DUMP=<cartella> salva ogni risposta (pagine HTML/JSON, non i video) per capire perche' una fonte fallisce.
std::string dumpDir;
std::mutex dumpMutex;
int dumpCount = 0;

void setDumpSource(const std::string& id) {
    const char* base = getenv("ANX_DUMP");
    if (!base || !*base) return;
    std::lock_guard<std::mutex> lock(dumpMutex);
    mkdir(base
#ifndef _WIN32
          , 0777
#endif
    );
    dumpDir = std::string(base) + "/" + id;
    mkdir(dumpDir.c_str()
#ifndef _WIN32
          , 0777
#endif
    );
    dumpCount = 0;
    http::debugHook = [](const std::string& method, const std::string& url, const std::string& reqBody,
                         const http::Response& r) {
        std::string ct = r.header("content-type");
        bool text = ct.find("text") != std::string::npos || ct.find("json") != std::string::npos ||
                    ct.find("javascript") != std::string::npos || ct.find("xml") != std::string::npos || ct.empty();
        if (!text || r.body.size() > 3000000) return;
        std::lock_guard<std::mutex> lock(dumpMutex);
        if (dumpCount >= 60) return;
        int n = ++dumpCount;
        char name[32];
        snprintf(name, sizeof(name), "/%02d.txt", n);
        std::ofstream f(dumpDir + name, std::ios::binary);
        f << method << " " << url << "\n";
        if (!reqBody.empty()) f << "BODY: " << reqBody.substr(0, 2000) << "\n";
        f << "STATUS " << r.status << " -> " << r.finalUrl << "\nCONTENT-TYPE " << ct << "\n\n" << r.body;
        std::ofstream idx(dumpDir + "/indice.txt", std::ios::app);
        idx << name + 1 << "\t" << r.status << "\t" << method << " " << url << "\n";
    };
}

Summary test(src::Source& s, const std::string& query) {
    setDumpSource(s.id());
    Summary sum;
    sum.name = s.name();
    std::cout << "\n===== " << s.name() << " (" << s.baseUrl() << ")\n";
    src::Page pop, r;
    try {
        pop = s.popular(1);
        sum.popular = std::to_string(pop.animes.size());
        std::cout << "popolari: " << pop.animes.size() << " (altre pagine: " << pop.hasNextPage << ")\n";
        for (size_t i = 0; i < pop.animes.size() && i < 2; i++)
            std::cout << "  - " << pop.animes[i].title << " | " << pop.animes[i].url << " | " << cut(pop.animes[i].thumbnail) << "\n";
    } catch (const std::exception& e) {
        sum.popular = "ERR";
        std::cout << "popolari ERRORE: " << e.what() << "\n";
    }
    if (s.supportsLatest()) {
        try {
            auto l = s.latest(1);
            sum.latest = std::to_string(l.animes.size());
            std::cout << "recenti: " << l.animes.size() << "\n";
        } catch (const std::exception& e) {
            sum.latest = "ERR";
            std::cout << "recenti ERRORE: " << e.what() << "\n";
        }
    }
    try {
        r = s.search(query, 1);
        sum.search = std::to_string(r.animes.size());
        std::cout << "ricerca '" << query << "': " << r.animes.size() << "\n";
        for (size_t i = 0; i < r.animes.size() && i < 2; i++) std::cout << "  - " << r.animes[i].title << " | " << r.animes[i].url << "\n";
    } catch (const std::exception& e) {
        sum.search = "ERR";
        std::cout << "ricerca ERRORE: " << e.what() << "\n";
    }

    // fino a 2 anime: dai risultati della ricerca, altrimenti dai popolari
    std::vector<src::Anime> picks;
    for (auto& a : r.animes)
        if (picks.size() < 2) picks.push_back(a);
    for (auto& a : pop.animes)
        if (picks.size() < 2) picks.push_back(a);
    int detailsOk = 0;
    for (auto& a : picks) {
        std::cout << "\n  >> " << a.title << "\n";
        src::Details d;
        try {
            d = s.details(a.url);
            detailsOk++;
            std::cout << "  dettagli: '" << d.title << "' stato=" << d.status << " episodi=" << d.episodes.size()
                      << " copertina=" << (d.thumbnail.empty() ? "NO" : "si") << " trama=" << (d.description.empty() ? "NO" : "si")
                      << "\n";
        } catch (const std::exception& e) {
            std::cout << "  dettagli ERRORE: " << e.what() << "\n";
            continue;
        }
        if (d.episodes.empty()) continue;
        const src::Episode& ep = d.episodes.front();  // il piu' recente, come quello che si guarda davvero
        std::cout << "  episodio: " << ep.name << " #" << ep.number << " | " << cut(ep.url) << "\n";
        std::vector<src::Video> vids;
        try {
            vids = s.videos(ep.url);
        } catch (const std::exception& e) {
            std::cout << "  video ERRORE: " << e.what() << "\n";
            continue;
        }
        std::cout << "  video trovati: " << vids.size() << "\n";
        int tested = 0;
        for (auto& v : vids) {
            if (tested++ >= 4) break;  // bastano i primi (quelli che il player prova per primi)
            sum.videos++;
            std::string detail, err;
            try {
                err = probe(v, detail);
            } catch (const std::exception& e) {
                err = e.what();
            }
            if (err.empty()) sum.playable++;
            std::cout << "   [" << (err.empty() ? "OK  " : "FAIL") << "] " << v.title << " | " << cut(v.url, 90) << "\n"
                      << "          " << (err.empty() ? detail : err + (detail.empty() ? "" : " | " + detail)) << "\n";
            if (!v.subtitles.empty()) {
                std::string sr;
                try {
                    sr = probeSub(v.subtitles[0], v);
                } catch (const std::exception& e) {
                    sr = e.what();
                }
                std::cout << "          sottotitoli: " << v.subtitles.size() << " (" << v.subtitles[0].lang << ": " << sr << ")\n";
            }
        }
    }
    sum.details = std::to_string(detailsOk) + "/" + std::to_string(picks.size());
    return sum;
}

}  // namespace

int main(int argc, char** argv) {
    http::globalInit(argc > 3 ? argv[3] : "");
    std::string which = argc > 1 ? argv[1] : "tutte";
    if (which.rfind("http", 0) == 0) {
        // modalita' diagnostica: mostra la risposta grezza di un URL
        http::Response r = http::request("GET", which, {}, "", 25);
        std::cout << "HTTP " << r.status << " -> " << r.finalUrl << "\n";
        for (auto& kv : r.headers) std::cout << kv.first << ": " << kv.second << "\n";
        std::cout << "\n" << r.body.substr(0, 3000) << "\n";
        return 0;
    }
    if (which == "sonda") {
        // sonda veloce della home di ogni fonte: stato, redirect, Cloudflare, titolo
        std::string langs = argc > 2 ? argv[2] : "";
        for (auto& s : src::all()) {
            if (!langs.empty() && ("," + langs + ",").find("," + s->lang() + ",") == std::string::npos) continue;
            std::string info;
            try {
                http::Response r = http::request("GET", s->baseUrl(), {}, "", 20);
                std::string b = r.body;
                bool cf = b.find("Just a moment") != std::string::npos || b.find("challenge-platform") != std::string::npos ||
                          b.find("cf-chl") != std::string::npos || b.find("DDoS-Guard") != std::string::npos;
                std::string title;
                auto t = b.find("<title");
                if (t != std::string::npos) {
                    t = b.find('>', t);
                    auto e = b.find("</title", t);
                    if (t != std::string::npos && e != std::string::npos) title = b.substr(t + 1, std::min<size_t>(e - t - 1, 70));
                }
                for (auto& c : title)
                    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
                info = std::to_string(r.status) + "\t" + (cf ? "CF" : "-") + "\t" + r.finalUrl + "\t" + title;
            } catch (const std::exception& e) {
                info = std::string("ERR\t-\t\t") + e.what();
            }
            std::cout << s->lang() << "\t" << s->id() << "\t" << s->baseUrl() << "\t" << info << "\n" << std::flush;
        }
        return 0;
    }
    if (which == "lista") {
        for (auto& s : src::all())
            std::cout << s->lang() << "\t" << (s->nsfw() ? "18+" : "") << "\t" << s->id() << "\t" << s->name() << "\t"
                      << s->baseUrl() << "\n";
        return 0;
    }
    std::string query = argc > 2 ? argv[2] : "naruto";
    std::vector<Summary> all;
    auto inList = [&](const std::string& name) {
        std::string item;
        std::istringstream in(which);
        while (std::getline(in, item, ','))
            if (item == name) return true;
        return false;
    };
    for (auto& s : src::all())
        if ((which == "tutte" && !s->nsfw()) || inList(s->id()) || inList(s->name()) ||
            (inList(s->lang()) && !s->nsfw()) || (which == "18+" && s->nsfw()))
            all.push_back(test(*s, query));

    std::cout << "\n\n=================== RIEPILOGO ===================\n";
    printf("%-24s %6s %6s %6s %8s %14s\n", "fonte", "popol", "ultime", "cerca", "dettagli", "video ok/prov");
    for (auto& s : all)
        printf("%-24s %6s %6s %6s %8s %8d/%-5d\n", s.name.substr(0, 24).c_str(), s.popular.c_str(), s.latest.c_str(),
               s.search.c_str(), s.details.c_str(), s.playable, s.videos);
    http::globalCleanup();
}
