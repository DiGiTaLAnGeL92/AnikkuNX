// Porting in C++ dell'estensione Aniyomi "AnimePahe" (en.animepahe, extVersionCode 54) con l'estrattore Kwik.
// Omessi: server HLS locale (nanohttpd), bypass Cloudflare via WebView, filtri di navigazione (genere/tema/...),
// link MP4 via pahe.win (disattivati di default nell'estensione: "Use HLS Links" = true).

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

#include "html/html.hpp"
#include "sources/registry.hpp"
#include "util/unpacker.hpp"

using json = nlohmann::json;

namespace src {

namespace {

std::string jstr(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return "";
    if (it->is_string()) return it->get<std::string>();
    return it->dump();
}

int jint(const json& j, const char* key, int def = 0) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    if (it->is_number()) return it->get<int>();
    if (it->is_string()) return std::atoi(it->get<std::string>().c_str());
    return def;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

bool containsCI(const std::string& s, const std::string& what) { return lower(s).find(lower(what)) != std::string::npos; }

/** Rimuove (senza distinzione maiuscole) tutte le occorrenze di `what`. */
std::string removeCI(const std::string& s, const std::string& what) {
    std::string out;
    std::string ls = lower(s), lw = lower(what);
    size_t i = 0;
    while (i < s.size()) {
        if (!lw.empty() && ls.compare(i, lw.size(), lw) == 0) {
            i += lw.size();
        } else {
            out += s[i++];
        }
    }
    return out;
}

std::string collapseSpaces(const std::string& s) {
    std::string out;
    bool sp = false;
    for (char c : s) {
        if (std::isspace((unsigned char)c)) {
            sp = true;
        } else {
            if (sp && !out.empty()) out += ' ';
            sp = false;
            out += c;
        }
    }
    return out;
}

/** "[^a-z0-9]+" -> "" su testo minuscolo (normalizeTitle). */
std::string normalizeTitle(const std::string& raw) {
    std::string out;
    for (unsigned char c : lower(raw))
        if (std::isalnum(c)) out += (char)c;
    return out;
}

/** "[^a-zA-Z0-9\\s]+" -> "", spazi compattati (normalizeSearchQuery). */
std::string normalizeSearchQuery(const std::string& raw) {
    std::string out;
    for (unsigned char c : raw)
        if (std::isalnum(c) || std::isspace(c)) out += (char)c;
    return collapseSpaces(out);
}

/** Altezza dal testo della qualita': "(\d+)p" altrimenti il primo numero. */
int parseHeight(const std::string& s) {
    std::string l = lower(s);
    for (size_t i = 0; i < l.size(); i++) {
        if (!std::isdigit((unsigned char)l[i])) continue;
        size_t j = i;
        while (j < l.size() && std::isdigit((unsigned char)l[j])) j++;
        if (j < l.size() && l[j] == 'p') return std::atoi(l.substr(i, j - i).c_str());
        i = j;
    }
    for (size_t i = 0; i < l.size(); i++)
        if (std::isdigit((unsigned char)l[i])) return std::atoi(l.c_str() + i);
    return 0;
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Cache anime_id -> session (la session della pagina /anime/{session} cambia ogni pochi giorni).
std::mutex cacheMutex;
std::map<std::string, std::string> sessionCache;

void saveSession(const std::string& id, const std::string& session) {
    if (id.empty() || session.empty()) return;
    std::lock_guard<std::mutex> lock(cacheMutex);
    sessionCache[id] = session;
}

std::string cachedSession(const std::string& id) {
    std::lock_guard<std::mutex> lock(cacheMutex);
    auto it = sessionCache.find(id);
    return it == sessionCache.end() ? "" : it->second;
}

/** Valore di un parametro nella nostra url "/a/{id}?s=...&t=..." (decodifica %XX). */
std::string urlParam(const std::string& url, const std::string& name) {
    std::string q = substringAfter(url, "?");
    if (q == url) return "";
    size_t pos = 0;
    while (pos <= q.size()) {
        size_t amp = q.find('&', pos);
        std::string kv = q.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        if (substringBefore(kv, "=") == name && kv.find('=') != std::string::npos) {
            std::string v = substringAfter(kv, "="), out;
            for (size_t i = 0; i < v.size(); i++) {
                if (v[i] == '%' && i + 2 < v.size()) {
                    out += (char)std::strtol(v.substr(i + 1, 2).c_str(), nullptr, 16);
                    i += 2;
                } else if (v[i] == '+') {
                    out += ' ';
                } else {
                    out += v[i];
                }
            }
            return out;
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return "";
}

// ============================== Kwik ==============================

const char* KWIK_ORIGIN = "https://kwik.cx";

struct HlsStream {
    std::string url;
    std::string referer;
};

/** KwikExtractor.getHlsStream: la pagina embed contiene uno script p.a.c.k.e.d con "const source='...m3u8'". */
HlsStream kwikHls(const std::string& kwikUrl, const std::string& referer) {
    http::Response r = http::request("GET", kwikUrl, {{"Referer", referer}, {"User-Agent", http::DEFAULT_UA}});
    if (r.status < 200 || r.status >= 300) throw http::Error("Kwik: HTTP " + std::to_string(r.status));
    std::string finalUrl = r.finalUrl.empty() ? kwikUrl : r.finalUrl;

    html::Document doc(r.body, finalUrl);
    std::string script;
    for (auto& s : doc.select("script")) {
        std::string d = s.data();
        if (d.find("eval(function(") != std::string::npos) script = d;  // come substringAfterLast: l'ultimo
    }
    if (script.empty()) throw http::Error("Kwik: script non trovato");
    std::string unpacked = unpacker::unpackAndCombine(script);
    if (unpacked.empty()) throw http::Error("Kwik: decodifica dello script non riuscita");

    std::string url;
    const char* openers[] = {"const source=\\'", "const source='", "const source=\""};
    const char* closers[] = {"\\';", "';", "\";"};
    for (int i = 0; i < 3 && url.empty(); i++) {
        auto p = unpacked.find(openers[i]);
        if (p == std::string::npos) continue;
        p += std::string(openers[i]).size();
        auto e = unpacked.find(closers[i], p);
        if (e == std::string::npos) continue;
        url = unpacked.substr(p, e - p);
    }
    if (url.empty() || url.find("http") != 0) throw http::Error("Kwik: link del video non trovato");
    return {url, finalUrl};
}

// ============================== Source ==============================

class AnimePahe : public Source {
  public:
    std::string id() const override { return "en.animepahe"; }
    std::string name() const override { return "AnimePahe"; }
    std::string defaultBaseUrl() const override { return "https://animepahe.pw"; }
    std::string lang() const override { return "en"; }
    bool supportsLatest() const override { return false; }

    http::Headers imageHeaders() const override { return {{"Referer", baseUrl() + "/"}}; }

    Page popular(int page) override {
        json data = apiJson(baseUrl() + "/api?m=airing&page=" + std::to_string(page));
        Page p;
        for (auto& a : data.value("data", json::array())) {
            std::string id = jstr(a, "anime_id"), session = jstr(a, "anime_session");
            saveSession(id, session);
            Anime an;
            an.title = jstr(a, "anime_title");
            an.thumbnail = jstr(a, "snapshot");
            an.url = makeUrl(id, session, an.title);
            p.animes.push_back(an);
        }
        p.hasNextPage = jint(data, "current_page") < jint(data, "last_page");
        return p;
    }

    Page latest(int page) override { return popular(page); }

    Page search(const std::string& query, int page) override {
        if (trim(query).empty()) return popular(page);
        json data = apiJson(searchUrl(query, page));
        Page p;
        for (auto& a : data.value("data", json::array())) {
            std::string id = jstr(a, "id"), session = jstr(a, "session");
            saveSession(id, session);
            Anime an;
            an.title = jstr(a, "title");
            an.thumbnail = jstr(a, "poster");
            an.url = makeUrl(id, session, an.title);
            p.animes.push_back(an);
        }
        p.hasNextPage = jint(data, "current_page") < jint(data, "last_page");
        return p;
    }

    Details details(const std::string& animeUrl) override {
        std::string animeId = animeIdOf(animeUrl);
        std::string title = urlParam(animeUrl, "t");
        std::string session = sessionOf(animeUrl);

        // ---- pagina dell'anime (se la session e' scaduta si cerca quella nuova per titolo/id)
        http::Response resp;
        bool ok = false;
        if (!session.empty()) {
            resp = safeGet(baseUrl() + "/anime/" + session);
            checkNotChallenge(resp);
            ok = resp.status >= 200 && resp.status < 300;
        }
        if (!ok) {
            std::string newSession = refreshSession(animeId, title);
            if (newSession.empty()) throw http::Error("AnimePahe: anime non trovato (sessione scaduta)");
            session = newSession;
            resp = safeGet(baseUrl() + "/anime/" + session);
            if (resp.status < 200 || resp.status >= 300)
                throw http::Error("AnimePahe: HTTP " + std::to_string(resp.status));
        }
        checkNotChallenge(resp);

        html::Document doc(resp.body, resp.finalUrl.empty() ? baseUrl() + "/anime/" + session : resp.finalUrl);
        Details d;
        d.title = doc.selectFirst("div.title-wrapper > h1 > span").text();
        if (d.title.empty()) d.title = title;
        if (animeId.empty()) animeId = doc.selectFirst("meta[name=id]").attr("content");
        saveSession(animeId, session);
        d.thumbnail = doc.selectFirst("div.anime-poster a").attr("href");

        std::vector<std::string> genres;
        for (auto& li : doc.select("div.anime-genre ul li")) genres.push_back(li.text());
        std::string synonyms, japanese, aired, season, links;
        for (auto& p : doc.select("div.col-sm-4.anime-info p")) {
            std::string t = p.text();
            if (t.find("Studios:") != std::string::npos) {
                d.author = trim(replaceAll(t, "Studios: ", ""));
            } else if (t.find("Status:") != std::string::npos) {
                std::string st = p.selectFirst("a").text();
                d.status = st == "Currently Airing" ? "In corso" : st == "Finished Airing" ? "Completato" : st;
            } else if (t.find("Demographic:") != std::string::npos || t.find("Theme:") != std::string::npos) {
                for (auto& a : p.select("a")) genres.push_back(a.text());
            } else if (t.find("Synonyms:") != std::string::npos) {
                synonyms = t;
            } else if (t.find("Japanese:") != std::string::npos) {
                japanese = t;
            } else if (t.find("Aired:") != std::string::npos) {
                aired = t;
            } else if (t.find("Season:") != std::string::npos) {
                season = t;
            }
        }
        for (size_t i = 0; i < genres.size(); i++) d.genre += (i ? ", " : "") + genres[i];
        d.description = html::textOf(doc.select("div.anime-summary"));
        for (auto* s : {&synonyms, &japanese, &aired, &season})
            if (!trim(*s).empty()) d.description += "\n\n" + *s;

        d.episodes = fetchEpisodes(session, animeId, title.empty() ? d.title : title);
        return d;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::string path = substringBefore(episodeUrl, "?");
        std::string pageUrl = path.find("://") != std::string::npos ? path : baseUrl() + path;
        http::Response r = safeGet(pageUrl);
        if (r.status < 200 || r.status >= 300) throw http::Error("AnimePahe: HTTP " + std::to_string(r.status));
        checkNotChallenge(r);
        html::Document doc(r.body, pageUrl);

        struct Item {
            Video v;
            int langRank;  // 0 = sub (preferita di default)
            bool av1;
        };
        std::vector<Item> items;
        std::string referer = baseUrl() + "/";
        for (auto& btn : doc.select("div#resolutionMenu > button")) {
            std::string kwikLink = btn.attr("data-src");
            if (kwikLink.empty()) continue;
            std::string text = btn.text();
            bool hasDot = text.find(" \xC2\xB7 ") != std::string::npos;  // " · "
            std::string provider = hasDot ? substringBefore(text, " \xC2\xB7 ") : text;
            std::string qualityText = hasDot ? substringAfter(text, " \xC2\xB7 ") : text;
            std::string audio = lower(btn.attr("data-audio"));

            std::string langName = "Sub";
            int rank = 0;
            if (containsCI(qualityText, "eng") || audio == "eng") {
                langName = "Dub";
                rank = 1;
            } else if (containsCI(qualityText, "kor") || audio == "kor") {
                langName = "Korean";
                rank = 2;
            } else if (containsCI(qualityText, "chi") || audio == "chi") {
                langName = "Chinese";
                rank = 3;
            }
            qualityText = trim(collapseSpaces(removeCI(removeCI(removeCI(qualityText, "eng"), "kor"), "chi")));

            HlsStream hls;
            try {
                hls = kwikHls(kwikLink, referer);
            } catch (const std::exception&) {
                continue;
            }
            Item it;
            it.v.url = hls.url;
            it.v.referer = hls.referer;
            it.v.userAgent = http::DEFAULT_UA;
            it.v.headers = {{"Origin", KWIK_ORIGIN}};
            it.v.quality = parseHeight(qualityText);
            if (it.v.quality == 0) it.v.quality = std::atoi(btn.attr("data-resolution").c_str());
            std::string q = qualityText.empty() ? std::to_string(it.v.quality) + "p" : qualityText;
            it.v.title = langName + " - " + q + (trim(provider).empty() || provider == text ? "" : " (" + trim(provider) + ")");
            it.langRank = rank;
            it.av1 = containsCI(text, "av1") || btn.attr("data-av1") == "1";
            items.push_back(it);
        }
        if (items.empty()) throw http::Error("Nessun video trovato");

        // preferenze predefinite: lingua "sub", qualita' 1080p, niente AV1, poi altezza decrescente
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            if (a.langRank != b.langRank) return a.langRank < b.langRank;
            bool pa = a.v.quality == 1080, pb = b.v.quality == 1080;
            if (pa != pb) return pa;
            if (a.av1 != b.av1) return !a.av1;
            return a.v.quality > b.v.quality;
        });
        std::vector<Video> out;
        for (auto& it : items) out.push_back(it.v);
        return out;
    }

  private:
    http::Headers siteHeaders() const {
        // Referer come headersBuilder(); il cookie __ddg2_ vuoto aiuta a passare DDoS-Guard senza browser
        return {{"Referer", baseUrl() + "/"}, {"User-Agent", http::DEFAULT_UA}, {"Cookie", "__ddg2_="}};
    }

    /** safeApiCall: su 429 attende 12 s e riprova una volta. */
    http::Response safeGet(const std::string& url) {
        http::Response r = http::request("GET", url, siteHeaders());
        if (r.status == 429) {
            sleepMs(12000);
            r = http::request("GET", url, siteHeaders());
        }
        return r;
    }

    static void checkNotChallenge(const http::Response& r) {
        if (r.status == 403 || r.status == 503)
            throw http::Error("AnimePahe: bloccato dalla protezione anti-bot del sito (HTTP " +
                              std::to_string(r.status) + ")");
    }

    json apiJson(const std::string& url) {
        http::Response r = safeGet(url);
        if (r.status == 429 || r.header("content-type").find("text/html") != std::string::npos)
            throw http::Error("AnimePahe: troppe richieste o protezione anti-bot, riprova tra qualche secondo");
        if (r.status < 200 || r.status >= 300) throw http::Error("AnimePahe: HTTP " + std::to_string(r.status));
        try {
            return json::parse(r.body);
        } catch (const std::exception&) {
            throw http::Error("AnimePahe: risposta non valida");
        }
    }

    std::string searchUrl(const std::string& query, int page) const {
        // suffisso temporale per aggirare la cache dell'API (il sito lo ignora nella ricerca)
        long long suffix = (long long)std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count() +
                           page * 3;
        return baseUrl() + "/api?m=search&q=" + http::urlEncode(query + " " + std::to_string(suffix)) +
               "&page=" + std::to_string(page);
    }

    static std::string makeUrl(const std::string& id, const std::string& session, const std::string& title) {
        return "/a/" + id + "?s=" + http::urlEncode(session) + "&t=" + http::urlEncode(title);
    }

    static std::string animeIdOf(const std::string& url) {
        auto p = url.find("/a/");
        if (p != std::string::npos) return digitsOnly(substringBefore(substringBefore(url.substr(p + 3), "?"), "/"));
        std::string q = urlParam(url, "anime_id");
        return digitsOnly(q);
    }

    static std::string sessionOf(const std::string& url) {
        std::string id = animeIdOf(url);
        std::string s = id.empty() ? "" : cachedSession(id);
        if (s.empty()) s = urlParam(url, "s");
        if (s.empty()) {
            auto p = url.find("/anime/");
            if (p != std::string::npos) s = substringBefore(substringBefore(url.substr(p + 7), "?"), "/");
        }
        return s;
    }

    /** fetchSessionAndId: cerca l'anime nell'API per titolo e lo abbina per id (o per titolo normalizzato). */
    std::string refreshSession(const std::string& animeId, const std::string& title) {
        if (trim(title).empty()) return "";
        std::string query = normalizeSearchQuery(title);
        std::string normTitle = normalizeTitle(title);
        std::vector<std::string> queries = {query};
        std::vector<std::string> words;
        {
            std::string cur;
            for (char c : query) {
                if (c == ' ') {
                    if (!cur.empty()) words.push_back(cur);
                    cur.clear();
                } else {
                    cur += c;
                }
            }
            if (!cur.empty()) words.push_back(cur);
        }
        for (size_t len : {4u, 3u}) {
            if (words.size() > len) {
                std::string q;
                for (size_t i = words.size() - len; i < words.size(); i++) q += (q.empty() ? "" : " ") + words[i];
                queries.push_back(q);
            }
        }
        for (auto& q : queries) {
            for (int page = 1; page <= 5; page++) {
                json data;
                try {
                    data = apiJson(searchUrl(q, page));
                } catch (const std::exception&) {
                    break;
                }
                for (auto& a : data.value("data", json::array())) {
                    std::string id = jstr(a, "id"), session = jstr(a, "session");
                    saveSession(id, session);
                    bool match;
                    if (!animeId.empty()) {
                        match = id == animeId;
                    } else {
                        std::string at = normalizeTitle(jstr(a, "title"));
                        match = !at.empty() && (at.find(normTitle) != std::string::npos ||
                                                normTitle.find(at) != std::string::npos);
                    }
                    if (match) return session;
                }
                if (jint(data, "current_page") >= jint(data, "last_page")) break;
                sleepMs(1500);
            }
        }
        return "";
    }

    std::string releaseUrl(const std::string& session, int page) const {
        return baseUrl() + "/api?m=release&id=" + http::urlEncode(session) + "&sort=episode_asc&page=" +
               std::to_string(page);
    }

    std::vector<Episode> fetchEpisodes(std::string session, const std::string& animeId, const std::string& title) {
        json data;
        try {
            data = apiJson(releaseUrl(session, 1));
        } catch (const std::exception&) {
            std::string ns = refreshSession(animeId, title);
            if (ns.empty() || ns == session) throw;
            session = ns;
            data = apiJson(releaseUrl(session, 1));
        }

        struct Raw {
            std::string url;
        };
        std::vector<Raw> raws;
        for (int guard = 0; guard < 500; guard++) {
            for (auto& e : data.value("data", json::array())) {
                std::string epSession = jstr(e, "session");
                if (epSession.empty()) continue;
                std::string aid = jstr(e, "anime_id");
                if (!aid.empty()) saveSession(aid, session);
                raws.push_back({"/play/" + session + "/" + epSession + (aid.empty() ? "" : "?anime_id=" + aid)});
            }
            int cur = jint(data, "current_page"), last = jint(data, "last_page");
            if (cur >= last) break;
            sleepMs(1000);  // l'estensione attende 3 s tra le pagine per evitare il limite di richieste
            data = apiJson(releaseUrl(session, cur + 1));
        }

        // come l'estensione (numero sito disattivato): "Episode {indice+1}", poi ordine inverso
        std::vector<Episode> eps;
        eps.reserve(raws.size());
        for (size_t i = 0; i < raws.size(); i++) {
            Episode ep;
            ep.url = raws[i].url;
            ep.number = (double)(i + 1);
            ep.name = "Episode " + std::to_string(i + 1);
            eps.push_back(ep);
        }
        std::reverse(eps.begin(), eps.end());
        return eps;
    }
};

}  // namespace

std::shared_ptr<Source> makeAnimePahe() { return std::make_shared<AnimePahe>(); }

}  // namespace src
