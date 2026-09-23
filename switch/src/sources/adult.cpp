// Porting in C++ delle estensioni Aniyomi inglesi per adulti (18+, isNsfw = true):
//   - Hstream      (en.hstream, v12)
//   - HentaiHaven  (en.hentaihaven, v7)
//   - HentaiMama   (en.hentaimama, v8)
//   - Oppai Stream (en.oppaistream, v6)
// Tutte le fonti restituiscono nsfw() = true: l'app le nasconde finche' l'utente non le attiva.
// hanime.tv non e' portata: richiede la firma calcolata da WebView/WASM.

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <random>
#include <set>

#include "html/html.hpp"
#include "sources/registry.hpp"
#include "util/crypto.hpp"

using json = nlohmann::json;

namespace src {

namespace {

// ------------------------------------------------------------------------------------ utilita'

std::string jstr(const json& j, const char* key) {
    if (!j.is_object()) return "";
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return "";
    if (it->is_string()) return it->get<std::string>();
    return it->dump();
}

bool contains(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

bool startsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }

std::string toLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trimEndSlash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

std::string substringBeforeLast(const std::string& s, const std::string& d) {
    auto p = s.rfind(d);
    return p == std::string::npos ? s : s.substr(0, p);
}

std::string substringAfterLast(const std::string& s, const std::string& d) {
    auto p = s.rfind(d);
    return p == std::string::npos ? s : s.substr(p + d.size());
}

/** URL assoluto -> percorso + query (come setUrlWithoutDomain). */
std::string withoutDomain(const std::string& url) {
    if (startsWith(url, "http://") || startsWith(url, "https://") || startsWith(url, "//")) {
        std::string p = http::pathOf(url);
        return p.empty() ? "/" : p;
    }
    return url;
}

std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) &&
            std::isxdigit((unsigned char)s[i + 2])) {
            out += (char)std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

/** Primo numero decimale (\d+(\.\d+)?) che segue immediatamente la posizione data. */
std::string numberAt(const std::string& s, size_t pos) {
    size_t e = pos;
    while (e < s.size() && std::isdigit((unsigned char)s[e])) e++;
    if (e == pos) return "";
    if (e + 1 < s.size() && s[e] == '.' && std::isdigit((unsigned char)s[e + 1])) {
        e++;
        while (e < s.size() && std::isdigit((unsigned char)s[e])) e++;
    }
    return s.substr(pos, e - pos);
}

/** Elemento fratello successivo (per i selettori "A + B" non supportati). */
html::Node nextSibling(const html::Node& n) {
    html::Node p = n.parent();
    if (!p) return html::Node();
    auto ch = p.children();
    for (size_t i = 0; i + 1 < ch.size(); i++)
        if (ch[i].raw() == n.raw()) return ch[i + 1];
    return html::Node();
}

/** Primo <h2> il cui testo contiene `label` (":contains"). */
html::Node headingContaining(const html::Document& doc, const std::string& tag, const std::string& label) {
    for (auto& h : doc.select(tag))
        if (contains(toLower(h.text()), toLower(label))) return h;
    return html::Node();
}

std::string joinTexts(const std::vector<html::Node>& nodes, const std::string& sep = ", ") {
    std::string out;
    for (auto& n : nodes) {
        std::string t = n.text();
        if (t.empty()) continue;
        if (!out.empty()) out += sep;
        out += t;
    }
    return out;
}

http::Response checkedGet(const std::string& url, const http::Headers& h, const std::string& who) {
    http::Response r = http::request("GET", url, h);
    if (r.status < 200 || r.status >= 300)
        throw http::Error(who + ": errore HTTP " + std::to_string(r.status));
    return r;
}

/** Metti in testa (ordine stabile) i video il cui titolo contiene `pref`. */
void preferFirst(std::vector<Video>& v, const std::string& pref) {
    std::stable_partition(v.begin(), v.end(), [&](const Video& x) { return contains(x.title, pref); });
}

/**
 * Equivalente minimo di PlaylistUtils.extractFromHls: legge la master playlist e restituisce un video
 * per ogni variante (#EXT-X-STREAM-INF). Se non e' una master, restituisce la playlist stessa.
 * Le intestazioni `h` sono usate sia per scaricare la playlist sia dal player.
 */
std::vector<Video> extractHls(const std::string& masterUrl, const http::Headers& h, const std::string& referer,
                              const std::function<std::string(const std::string&)>& nameGen) {
    http::Response r = http::request("GET", masterUrl, h);
    if (r.status < 200 || r.status >= 300) throw http::Error("Playlist non disponibile");
    const std::string& body = r.body;
    if (!contains(body, "#EXTM3U")) throw http::Error("Playlist non valida");
    std::string base = r.finalUrl.empty() ? masterUrl : r.finalUrl;

    std::vector<Video> out;
    std::set<std::string> seen;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t nl = body.find('\n', pos);
        std::string line = trim(body.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        pos = nl == std::string::npos ? body.size() : nl + 1;
        if (!startsWith(line, "#EXT-X-STREAM-INF")) continue;
        int height = 0;
        auto rp = line.find("RESOLUTION=");
        if (rp != std::string::npos) {
            std::string res = line.substr(rp + 11);
            res = substringBefore(res, ",");
            auto x = res.find_first_of("xX");
            if (x != std::string::npos) height = std::atoi(res.substr(x + 1).c_str());
        }
        // la riga successiva non commentata e' l'URL della variante
        std::string uri;
        while (pos < body.size()) {
            size_t nl2 = body.find('\n', pos);
            std::string l2 = trim(body.substr(pos, nl2 == std::string::npos ? std::string::npos : nl2 - pos));
            pos = nl2 == std::string::npos ? body.size() : nl2 + 1;
            if (l2.empty() || l2[0] == '#') continue;
            uri = l2;
            break;
        }
        if (uri.empty()) continue;
        std::string url = http::resolve(base, uri);
        if (!seen.insert(url).second) continue;
        Video v;
        std::string q = height > 0 ? std::to_string(height) + "p" : "Video";
        v.title = nameGen(q);
        v.url = url;
        v.referer = referer;
        v.userAgent = http::DEFAULT_UA;
        v.quality = height;
        v.headers = h;
        out.push_back(v);
    }
    std::stable_sort(out.begin(), out.end(), [](const Video& a, const Video& b) { return a.quality > b.quality; });
    if (out.empty()) {
        Video v;
        v.title = nameGen("Video");
        v.url = masterUrl;
        v.referer = referer;
        v.userAgent = http::DEFAULT_UA;
        v.headers = h;
        out.push_back(v);
    }
    return out;
}

// ================================================================================== Hstream

class Hstream : public Source {
  public:
    std::string id() const override { return "en.hstream"; }
    std::string name() const override { return "Hstream"; }
    std::string defaultBaseUrl() const override { return "https://hstream.moe"; }
    std::string lang() const override { return "en"; }
    bool nsfw() const override { return true; }

    Page popular(int page) override {
        return parseList(baseUrl() + "/search?order=view-count&page=" + std::to_string(page));
    }

    Page latest(int page) override {
        return parseList(baseUrl() + "/search?order=recently-uploaded&page=" + std::to_string(page));
    }

    Page search(const std::string& query, int page) override {
        std::string q = trim(query);
        std::string url = baseUrl() + "/search?";
        if (!q.empty()) url += "search=" + http::urlEncode(q) + "&";
        url += "page=" + std::to_string(page) + "&order=view-count";
        return parseList(url);
    }

    Details details(const std::string& animeUrl) override {
        std::string pageUrl = http::resolve(baseUrl() + "/", animeUrl);
        http::Response r = checkedGet(pageUrl, {}, "Hstream");
        std::string loc = r.finalUrl.empty() ? pageUrl : r.finalUrl;
        html::Document doc(r.body, loc);

        Details d;
        d.status = "Completato";
        html::Node main = doc.selectFirst("div.flex-1");
        if (!main) throw http::Error("Hstream: pagina non riconosciuta");
        html::Node h1span = main.selectFirst("h1 span");
        d.title = h1span ? h1span.text() : main.selectFirst("h1").text();
        d.author = main.selectFirst("div.mt-4.flex.flex-wrap a").text();
        html::Node img = doc.selectFirst("div.hidden.shrink-0[class~=md:block] img");
        if (img) d.thumbnail = doc.absUrl(img, "src");

        html::Node gh = headingContaining(doc, "h2", "Genres");
        html::Node gdiv = gh ? nextSibling(gh) : html::Node();
        if (gdiv && gdiv.tag() == "div") d.genre = joinTexts(gdiv.select("a"));

        html::Node dh = headingContaining(doc, "h2", "Description");
        html::Node dp = dh ? nextSibling(dh) : html::Node();
        if (dp && dp.tag() == "p")
            d.description = dp.text();
        else
            d.description = doc.selectFirst("div.border-t p.leading-relaxed").text();

        // episodi: link "<percorso-serie>-<numero>"
        std::string showPath = trimEndSlash(substringBefore(http::pathOf(loc), "?"));
        std::set<std::string> seen;
        for (auto& a : doc.select("a[href*='" + showPath + "-']")) {
            std::string href = a.attr("href");
            std::string path = trimEndSlash(substringBefore(http::pathOf(doc.absUrl(a, "href")), "?"));
            if (!startsWith(path, showPath + "-")) continue;
            std::string numStr = path.substr(showPath.size() + 1);
            double num = parseNumber(numStr, -1e9);
            if (num == -1e9 || numStr != numberAt(numStr, 0)) continue;
            Episode ep;
            ep.url = withoutDomain(href);
            if (!seen.insert(ep.url).second) continue;
            ep.number = num;
            ep.name = "Episode " + numStr;
            d.episodes.push_back(ep);
        }
        std::stable_sort(d.episodes.begin(), d.episodes.end(),
                         [](const Episode& a, const Episode& b) { return a.number > b.number; });
        return d;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::string pageUrl = http::resolve(baseUrl() + "/", episodeUrl);
        http::Response r = checkedGet(pageUrl, {}, "Hstream");
        std::string loc = r.finalUrl.empty() ? pageUrl : r.finalUrl;
        html::Document doc(r.body, loc);

        // token XSRF dal cookie impostato dalla pagina (Laravel lo invia a ogni risposta)
        std::string token;
        auto range = r.headers.equal_range("set-cookie");
        for (auto it = range.first; it != range.second; ++it) {
            std::string c = trim(it->second);
            if (startsWith(c, "XSRF-TOKEN=")) {
                token = substringBefore(c.substr(11), ";");
                break;
            }
        }
        if (token.empty()) throw http::Error("Hstream: token XSRF mancante");

        std::string episodeId = doc.selectFirst("input#e_id").attr("value");
        if (episodeId.empty()) throw http::Error("Hstream: episodio non trovato");

        http::Headers h = {
            {"Referer", loc},
            {"Origin", baseUrl()},
            {"X-Requested-With", "XMLHttpRequest"},
            {"X-XSRF-TOKEN", urlDecode(token)},
            {"Content-Type", "application/json; charset=utf-8"},
            {"Accept", "application/json, text/plain, */*"},
        };
        json body = {{"episode_id", episodeId}};
        http::Response api = http::request("POST", baseUrl() + "/player/api", h, body.dump());
        if (api.status < 200 || api.status >= 300)
            throw http::Error("Hstream: player non disponibile (HTTP " + std::to_string(api.status) + ")");
        json data = json::parse(api.body);

        std::vector<std::string> domains;
        for (auto& d : data.value("stream_domains", json::array()))
            if (d.is_string()) domains.push_back(d.get<std::string>());
        std::string streamUrl = jstr(data, "stream_url");
        if (domains.empty() || streamUrl.empty()) throw http::Error("Nessun video trovato");
        std::random_device rd;
        std::string urlBase = domains[rd() % domains.size()] + "/" + streamUrl;

        std::string resolution = data.contains("resolution") ? jstr(data, "resolution") : "4k";
        int legacy = data.value("legacy", 0);

        std::vector<std::string> resolutions = {"720", "1080"};
        if (resolution == "4k") resolutions.push_back("2160");

        bool forceLegacy = legacy != 0;
        if (!forceLegacy) {
            try {
                http::Response m = http::request("GET", urlBase + "/720/manifest.mpd");
                if (contains(m.body, ".html")) forceLegacy = true;
            } catch (const std::exception&) {
            }
        }

        std::vector<Video> out;
        for (auto& res : resolutions) {
            std::string path;
            if (forceLegacy)
                path = res == "720" ? "/x264.720p.mp4" : "/av1." + res + ".webm";
            else
                path = "/" + res + "/manifest.mpd";
            Video v;
            v.url = urlBase + path;
            v.title = res + "p" + (forceLegacy ? " (Legacy)" : "");
            v.quality = std::atoi(res.c_str());
            v.userAgent = http::DEFAULT_UA;
            v.subtitles.push_back({urlBase + "/eng.ass", "English"});
            out.push_back(v);
        }
        // preferenza predefinita 720p, poi le altre in ordine inverso (come sortVideos)
        std::reverse(out.begin(), out.end());
        preferFirst(out, "720p");
        return out;
    }

  private:
    Page parseList(const std::string& url) {
        html::Document doc(checkedGet(url, {}, "Hstream").body, url);
        Page p;
        std::set<std::string> seen;
        for (auto& el : doc.select("div.items-center div.w-full > a")) {
            std::string href = trimEndSlash(el.attr("href"));
            html::Node img = el.selectFirst("img");
            if (!img) continue;
            Anime a;
            a.url = withoutDomain(substringBeforeLast(href, "-") + "/");
            std::string epNum = substringAfterLast(href, "-");
            a.title = cleanTitle(img.attr("alt"));
            std::string imgBase = substringBeforeLast(doc.absUrl(img, "src"), "/");
            a.thumbnail = imgBase + "/cover-ep-" + epNum + ".webp";
            if (!seen.insert(a.url).second) continue;
            p.animes.push_back(a);
        }
        // "span[aria-current] + a"
        for (auto& s : doc.select("span[aria-current]")) {
            html::Node n = nextSibling(s);
            if (n && n.tag() == "a") {
                p.hasNextPage = true;
                break;
            }
        }
        return p;
    }

    /** Rimuove il suffisso "\s*(?:[-–—]\s*|\bEpisode\s*)\d+(?:\.\d+)?\s*$" (EPISODE_PARSER). */
    static std::string cleanTitle(const std::string& raw) {
        std::string s = trim(raw);
        size_t e = s.size();
        size_t i = e;
        while (i > 0 && (std::isdigit((unsigned char)s[i - 1]) || s[i - 1] == '.')) i--;
        if (i == e || !std::isdigit((unsigned char)s[i])) return s;
        std::string num = s.substr(i);
        if (numberAt(num, 0) != num) return s;
        size_t j = i;
        while (j > 0 && std::isspace((unsigned char)s[j - 1])) j--;
        // trattini: "-", "–" (E2 80 93), "—" (E2 80 94)
        if (j >= 1 && s[j - 1] == '-') {
            j--;
        } else if (j >= 3 && (unsigned char)s[j - 3] == 0xE2 && (unsigned char)s[j - 2] == 0x80 &&
                   ((unsigned char)s[j - 1] == 0x93 || (unsigned char)s[j - 1] == 0x94)) {
            j -= 3;
        } else if (j >= 7 && toLower(s.substr(j - 7, 7)) == "episode" &&
                   (j == 7 || !std::isalnum((unsigned char)s[j - 8]))) {
            j -= 7;
        } else {
            return s;
        }
        return trim(s.substr(0, j));
    }
};

// ================================================================================== HentaiHaven

class HentaiHaven : public Source {
  public:
    std::string id() const override { return "en.hentaihaven"; }
    std::string name() const override { return "HentaiHaven"; }
    std::string defaultBaseUrl() const override { return "https://hentaihaven.xxx"; }
    std::string lang() const override { return "en"; }
    bool nsfw() const override { return true; }

    Page popular(int page) override {
        return parseList(baseUrl() + "/page/" + std::to_string(page) + "/?m_orderby=views");
    }

    Page latest(int page) override {
        return parseList(baseUrl() + "/page/" + std::to_string(page) + "/?m_orderby=latest");
    }

    Page search(const std::string& query, int page) override {
        std::string q = trim(query);
        // senza testo il Kotlin sfoglia l'elenco con l'ordinamento predefinito ("latest")
        if (q.empty()) return latest(page);
        std::string url = baseUrl() + "/?s=" + http::urlEncode(q) + "&post_type=wp-manga";
        html::Document doc(checkedGet(url, headers(), "HentaiHaven").body, url);
        Page p;
        std::set<std::string> seen;
        for (auto& el : doc.select("div.c-tabs-item, div.page-item-detail.video")) {
            html::Node link = el.selectFirst("a[href*='/watch/']");
            Anime a;
            a.url = withoutDomain(link.attr("href"));
            a.title = link.attr("title");
            if (trim(a.title).empty()) {
                html::Node t = el.selectFirst("div.post-title a, h3 a, h4 a");
                a.title = t.text();
            }
            html::Node img = el.selectFirst("img");
            if (img) a.thumbnail = doc.absUrl(img, "src");
            if (a.url.empty() || trim(a.title).empty()) continue;
            if (!seen.insert(a.url).second) continue;
            p.animes.push_back(a);
        }
        p.hasNextPage = (bool)doc.selectFirst("a.nextpostslink, div.wp-pagenavi a.next");
        return p;
    }

    Details details(const std::string& animeUrl) override {
        std::string pageUrl = baseUrl() + cleanUrl(animeUrl);
        html::Document doc(checkedGet(pageUrl, headers(), "HentaiHaven").body, pageUrl);

        Details d;
        html::Node t = doc.selectFirst("div.post-title h1");
        d.title = t ? t.text() : doc.selectFirst("h1.entry-title").text();
        html::Node img = doc.selectFirst("div.summary_image img, div.summary-image img");
        if (img) d.thumbnail = doc.absUrl(img, "src");
        d.description = doc.selectFirst("div.description-summary div.summary__content, div.entry-content p").text();

        d.author = doc.selectFirst("div.post-content_item.mg_author div.summary-content a").text();
        std::string statusText;
        for (auto& item : doc.select("div.post-content_item")) {
            std::string txt = item.text();
            if (d.author.empty() && contains(txt, "Studio")) d.author = item.selectFirst("div.summary-content a").text();
            if (statusText.empty() && contains(txt, "Status"))
                statusText = toLower(item.selectFirst("div.summary-content").text());
        }
        d.genre = joinTexts(doc.select("div.genres-content a, div.post-content_item.mg_genres a"));
        if (statusText == "ongoing")
            d.status = "In corso";
        else if (statusText == "completed")
            d.status = "Completato";

        auto elements = doc.select("li.wp-manga-chapter, ul.main.version-chap li");
        size_t size = elements.size();
        for (size_t index = 0; index < size; index++) {
            html::Node link = elements[index].selectFirst("a");
            if (!link) continue;
            Episode ep;
            ep.url = withoutDomain(link.attr("href"));
            ep.name = trim(link.text());
            double fallback = (double)(size - index);
            if (ep.name.empty()) ep.name = "Episode " + std::to_string(size - index);
            ep.number = fallback;
            // [Ee]pisode[- ](\d+(?:\.\d+)?)
            std::string lower = toLower(ep.name);
            size_t p = 0;
            while ((p = lower.find("episode", p)) != std::string::npos) {
                size_t after = p + 7;
                if (after < lower.size() && (lower[after] == '-' || lower[after] == ' ')) {
                    std::string n = numberAt(lower, after + 1);
                    if (!n.empty()) {
                        ep.number = parseNumber(n, fallback);
                        break;
                    }
                }
                p = after;
            }
            d.episodes.push_back(ep);
        }
        std::stable_sort(d.episodes.begin(), d.episodes.end(),
                         [](const Episode& a, const Episode& b) { return a.number > b.number; });
        return d;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::string pageUrl = http::resolve(baseUrl() + "/", episodeUrl);
        html::Document doc(checkedGet(pageUrl, headers(), "HentaiHaven").body, pageUrl);

        std::string playerSrc;
        if (html::Node n = doc.selectFirst("iframe[src*='player-logic/player.php']"))
            playerSrc = doc.absUrl(n, "src");
        else if (html::Node n2 = doc.selectFirst("script[src*='player-logic/player.php']"))
            playerSrc = doc.absUrl(n2, "src");
        else if (html::Node n3 = doc.selectFirst("[data-src*='player-logic/player.php']"))
            playerSrc = doc.absUrl(n3, "data-src");

        std::string dataB64;
        if (!playerSrc.empty()) {
            dataB64 = urlDecode(http::queryParam(playerSrc, "data"));
        } else {
            // player\.php\?data=([A-Za-z0-9+/=]+) negli script/iframe
            for (auto& el : doc.select("script, iframe")) {
                std::string content = el.attr("src");
                if (trim(content).empty()) content = el.data();
                auto p = content.find("player.php?data=");
                if (p == std::string::npos) continue;
                p += 16;
                size_t e = p;
                while (e < content.size() && (std::isalnum((unsigned char)content[e]) || content[e] == '+' ||
                                              content[e] == '/' || content[e] == '='))
                    e++;
                if (e > p) {
                    dataB64 = content.substr(p, e - p);
                    break;
                }
            }
        }
        if (dataB64.empty()) throw http::Error("Nessun video trovato");

        std::string apiUrl;
        for (auto& s : doc.select("script")) {
            std::string data = s.data();
            if (!contains(data, "player_logic")) continue;
            auto p = data.find("\"api_url\"");
            if (p != std::string::npos) {
                p = data.find(':', p + 9);
                if (p != std::string::npos) {
                    p = data.find('"', p);
                    if (p != std::string::npos) {
                        auto e = data.find('"', p + 1);
                        if (e != std::string::npos) apiUrl = replaceAll(data.substr(p + 1, e - p - 1), "\\/", "/");
                    }
                }
            }
            break;
        }
        if (apiUrl.empty()) apiUrl = baseUrl() + "/wp-content/plugins/player-logic/api.php";

        std::vector<Video> out = octopusVideos(apiUrl, dataB64, pageUrl);
        if (out.empty()) throw http::Error("Nessun video trovato");
        preferFirst(out, "1080p");
        return out;
    }

  private:
    http::Headers headers() const { return {{"Accept-Language", "en-US,en;q=0.9"}}; }

    static std::string cleanUrl(const std::string& url) {
        std::string u = trimEndSlash(url);
        auto p = u.rfind("/episode-");
        if (p != std::string::npos) {
            std::string num = u.substr(p + 9);
            if (!num.empty() && std::all_of(num.begin(), num.end(), [](char c) { return std::isdigit((unsigned char)c); }))
                u = u.substr(0, p);
        }
        return trimEndSlash(u) + "/";
    }

    Page parseList(const std::string& url) {
        html::Document doc(checkedGet(url, headers(), "HentaiHaven").body, url);
        Page p;
        for (auto& el : doc.select("div.page-item-detail.video")) {
            Anime a;
            a.url = withoutDomain(el.selectFirst("div.item-thumb a").attr("href"));
            a.title = el.selectFirst("div.post-title a, h3.h5 a").text();
            html::Node img = el.selectFirst("img");
            if (img) a.thumbnail = doc.absUrl(img, "src");
            p.animes.push_back(a);
        }
        p.hasNextPage = (bool)doc.selectFirst("a.nextpostslink, div.wp-pagenavi a.next");
        return p;
    }

    // ---- OctopusExtractor / MasterExtractor
    std::vector<Video> octopusVideos(const std::string& apiUrl, const std::string& dataB64,
                                     const std::string& episodeUrl) {
        const std::string origin = defaultBaseUrl();
        std::string decoded = base64Decode(dataB64);

        std::vector<std::string> parts;
        size_t start = 0;
        while (true) {
            auto p = decoded.find(":|:", start);
            parts.push_back(decoded.substr(start, p == std::string::npos ? std::string::npos : p - start));
            if (p == std::string::npos) break;
            start = p + 3;
        }
        std::string ciphertext = trim(parts[0]);
        if (ciphertext.empty()) return {};
        std::string bRaw;
        for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
            if (contains(*it, "=") || it->size() > 20) {
                bRaw = trim(*it);
                break;
            }
        }
        std::string bEncoded = crypto::base64Encode(bRaw);

        // multipart/form-data
        const std::string boundary = "----AnikkuNXBoundary7MA4YWxkTrZu0gW";
        std::string body;
        auto addPart = [&](const std::string& name, const std::string& value) {
            body += "--" + boundary + "\r\n";
            body += "Content-Disposition: form-data; name=\"" + name + "\"\r\n";
            body += "Content-Length: " + std::to_string(value.size()) + "\r\n\r\n";
            body += value + "\r\n";
        };
        addPart("action", "zarat_get_data_player_ajax");
        addPart("a", ciphertext);
        addPart("b", bEncoded);
        body += "--" + boundary + "--\r\n";

        http::Headers h = {
            {"Referer", episodeUrl},
            {"Origin", origin},
            {"Accept-Language", "en-US,en;q=0.9"},
            {"X-Requested-With", "XMLHttpRequest"},
            {"Content-Type", "multipart/form-data; boundary=" + boundary},
        };
        http::Response r = http::request("POST", apiUrl, h, body);
        if (r.status < 200 || r.status >= 300)
            throw http::Error("HentaiHaven: API del player non disponibile (HTTP " + std::to_string(r.status) + ")");

        json payload = json::parse(r.body, nullptr, false);
        if (payload.is_discarded() || !payload.is_object()) return {};
        auto dit = payload.find("data");
        if (dit == payload.end() || !dit->is_object()) return {};
        const json& data = *dit;
        auto sit = data.find("sources");
        if (sit == data.end() || !sit->is_array() || sit->empty()) return {};
        std::string sourceUrl = jstr((*sit)[0], "src");
        if (sourceUrl.empty()) return {};
        bool isOctopus = false;
        if (data.contains("isOctopus")) {
            const json& o = data["isOctopus"];
            isOctopus = o.is_boolean() ? o.get<bool>() : (o.is_string() && o.get<std::string>() == "true");
        }

        if (isOctopus) return octopusStream(sourceUrl, episodeUrl, payload);

        // MasterExtractor: HLS H.264 con audio incluso
        http::Headers mh = {{"Referer", episodeUrl}, {"Origin", origin}, {"Accept-Language", "en-US,en;q=0.9"}};
        try {
            auto v = extractHls(sourceUrl, mh, episodeUrl, [](const std::string& q) { return q; });
            return v;
        } catch (const std::exception&) {
            Video v;
            v.url = sourceUrl;
            v.title = "Master · Fallback";
            v.referer = episodeUrl;
            v.userAgent = http::DEFAULT_UA;
            v.headers = mh;
            return {v};
        }
    }

    std::vector<Video> octopusStream(const std::string& sourceUrl, const std::string& episodeUrl,
                                     const json& payload) {
        const std::string origin = defaultBaseUrl();
        // .../playlist.m3u8 -> .../playlist_vp9.m3u8
        std::string master = sourceUrl;
        {
            std::string noQuery = substringBefore(substringBefore(sourceUrl, "#"), "?");
            std::string rest = sourceUrl.substr(noQuery.size());
            if (substringAfterLast(noQuery, "/") == "playlist.m3u8")
                master = substringBeforeLast(noQuery, "/") + "/playlist_vp9.m3u8" + rest;
        }
        std::string octopusBase = substringBeforeLast(master, "/");

        http::Headers vh = {
            {"Referer", episodeUrl},
            {"Origin", origin},
            {"Accept-Language", "en-US,en;q=0.9"},
            {"Accept-Encoding", "identity"},
            {"Cache-Control", "no-transform"},
            {"Accept", "application/x-mpegURL, application/vnd.apple.mpegurl, */*;q=0.8"},
            {"Connection", "keep-alive"},
        };
        auto ait = payload.find("authorization");
        if (ait != payload.end() && ait->is_object()) {
            std::string token = jstr(*ait, "token"), expiration = jstr(*ait, "expiration"), ip = jstr(*ait, "ip");
            if (!trim(token).empty()) vh.push_back({"X-Video-Token", token});
            if (!trim(expiration).empty()) vh.push_back({"X-Video-Expiration", expiration});
            if (!trim(ip).empty()) vh.push_back({"X-Video-Ip", ip});
        }

        Video::Track subtitle{octopusBase + "/s/en.vtt", "English"};
        std::vector<int> heights;
        try {
            http::Response r = http::request("GET", master, vh);
            if (r.status >= 200 && r.status < 300 && !trim(r.body).empty()) {
                const std::string& b = r.body;
                bool subFound = false;
                std::set<int> hs;
                size_t pos = 0;
                while (pos < b.size()) {
                    size_t nl = b.find('\n', pos);
                    std::string line = b.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
                    pos = nl == std::string::npos ? b.size() : nl + 1;
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (!subFound && startsWith(line, "#EXT-X-MEDIA:") && contains(line, "TYPE=\"SUBTITLES\"")) {
                        subFound = true;
                        auto u = line.find("URI=\"");
                        if (u != std::string::npos) {
                            std::string uri = substringBefore(line.substr(u + 5), "\"");
                            if (!trim(uri).empty()) subtitle.url = http::resolve(master, uri);
                        }
                    }
                    auto rp = line.find("RESOLUTION=");
                    if (rp != std::string::npos) {
                        size_t e = rp + 11;
                        while (e < line.size() && (std::isdigit((unsigned char)line[e]) || line[e] == 'x' || line[e] == 'X'))
                            e++;
                        std::string res = line.substr(rp + 11, e - rp - 11);
                        res = substringAfterLast(substringAfterLast(res, "x"), "X");
                        if (!res.empty() && std::isdigit((unsigned char)res[0])) hs.insert(std::atoi(res.c_str()));
                    }
                }
                for (auto it = hs.rbegin(); it != hs.rend() && heights.size() < 6; ++it) heights.push_back(*it);
            }
        } catch (const std::exception&) {
        }

        auto make = [&](const std::string& title, int q) {
            Video v;
            v.url = master;
            v.title = title;
            v.quality = q;
            v.referer = episodeUrl;
            v.userAgent = http::DEFAULT_UA;
            v.headers = vh;
            v.subtitles.push_back(subtitle);
            return v;
        };
        std::vector<Video> out;
        if (heights.empty())
            out.push_back(make("Octopus · Auto", 0));
        else
            for (int hgt : heights) out.push_back(make("Octopus · " + std::to_string(hgt) + "p", hgt));
        return out;
    }
};

// ================================================================================== HentaiMama

class HentaiMama : public Source {
  public:
    std::string id() const override { return "en.hentaimama"; }
    std::string name() const override { return "HentaiMama"; }
    std::string defaultBaseUrl() const override { return "https://hentaimama.io"; }
    std::string lang() const override { return "en"; }
    bool nsfw() const override { return true; }

    Page popular(int page) override { return seriesPage("weekly", page); }

    Page latest(int page) override { return seriesPage("recent", page); }

    Page search(const std::string& query, int page) override {
        std::string url;
        if (!query.empty()) {
            // query.replace(Regex("[\\W]"), " ")
            std::string q = query;
            for (auto& c : q)
                if (!(std::isalnum((unsigned char)c) || c == '_')) c = ' ';
            url = baseUrl() + "/page/" + std::to_string(page) + "/?s=" + http::urlEncode(q);
        } else {
            std::string params = "&submit=Submit&filter=weekly";
            url = page == 1 ? baseUrl() + "/advance-search/?" + params
                            : baseUrl() + "/advance-search/page/" + std::to_string(page) + "/?" + params;
        }
        return parseList(url);
    }

    Details details(const std::string& animeUrl) override {
        std::string pageUrl = http::resolve(baseUrl() + "/", animeUrl);
        html::Document doc(checkedGet(pageUrl, headers(), "HentaiMama").body, pageUrl);

        Details d;
        d.thumbnail = doc.selectFirst("div.dsc-poster img").attr("src");
        d.title = textOf(doc.select("h1.dsc-title"));
        d.genre = joinTexts(doc.select("div.dsc-genres a"));
        d.description = textOf(doc.select("div.dsc-desc p"));
        for (auto& stat : doc.select("div.dsc-stats div.dsc-stat")) {
            if (textOf(stat.select("span")) != "Studio") continue;
            std::string studio = textOf(stat.select("b"));
            if (!trim(studio).empty() && studio != "—") d.author = studio;
            break;
        }
        d.status = doc.selectFirst("span.dsc-chip.is-airing") ? "In corso" : "Completato";

        for (auto& el : doc.select("div.dt-se-list a.dt-se-item")) {
            Episode ep;
            ep.url = withoutDomain(el.attr("href"));
            ep.name = textOf(el.select(".dt-se-title"));
            // Episode (\d+\.?\d*)
            auto p = ep.name.find("Episode ");
            ep.number = -1;
            if (p != std::string::npos) {
                std::string n = numberAt(ep.name, p + 8);
                if (!n.empty()) ep.number = parseNumber(n, -1);
            }
            d.episodes.push_back(ep);
        }
        std::reverse(d.episodes.begin(), d.episodes.end());
        return d;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::string pageUrl = http::resolve(baseUrl() + "/", episodeUrl);
        html::Document doc(checkedGet(pageUrl, headers(), "HentaiMama").body, pageUrl);
        std::string postId = doc.selectFirst("#post_report input[name=idpost]").attr("value");

        struct Hoster {
            std::string name, option;
        };
        std::vector<Hoster> hosters;
        for (auto& tab : doc.select(".dt-mi-tabs a")) {
            std::string href = tab.attr("href");
            std::string opt = startsWith(href, "#option-") ? href.substr(8) : href;
            if (trim(opt).empty()) continue;
            hosters.push_back({tab.text(), opt});
        }
        // server preferito predefinito "mi-2"
        std::stable_partition(hosters.begin(), hosters.end(), [](const Hoster& h) { return h.name == "mi-2"; });

        std::vector<Video> out;
        for (auto& h : hosters) {
            try {
                auto v = hosterVideos(postId, h.option, h.name);
                out.insert(out.end(), v.begin(), v.end());
            } catch (const std::exception&) {
            }
        }
        if (out.empty()) throw http::Error("Nessun video trovato");
        preferFirst(out, "1080p");
        return out;
    }

  private:
    http::Headers headers() const { return {{"Referer", baseUrl()}}; }

    Page seriesPage(const std::string& filter, int page) {
        std::string url = page == 1 ? baseUrl() + "/hentai-series/?filter=" + filter
                                    : baseUrl() + "/hentai-series/page/" + std::to_string(page) + "/?filter=" + filter;
        return parseList(url);
    }

    Page parseList(const std::string& url) {
        html::Document doc(checkedGet(url, headers(), "HentaiMama").body, url);
        Page p;
        for (auto& el : doc.select("article.series-card")) {
            Anime a;
            a.url = withoutDomain(el.selectFirst("a.sc-poster").attr("href"));
            a.title = textOf(el.select("h3.sc-title a"));
            a.thumbnail = el.selectFirst("a.sc-poster img").attr("src");
            p.animes.push_back(a);
        }
        p.hasNextPage = (bool)doc.selectFirst("div.pagination.dt-pg a.dt-pg-next");
        return p;
    }

    std::vector<Video> hosterVideos(const std::string& postId, const std::string& option, const std::string& hoster) {
        std::string body = "action=get_player_contents&a=" + http::urlEncode(postId) + "&i=" + http::urlEncode(option);
        http::Headers h = {
            {"Referer", baseUrl() + "/"},
            {"Content-Type", "application/x-www-form-urlencoded"},
        };
        http::Response r = http::request("POST", baseUrl() + "/wp-admin/admin-ajax.php", h, body);
        if (r.status < 200 || r.status >= 300) return {};

        int optionIndex = std::atoi(option.c_str());
        if (optionIndex <= 0) return {};
        json arr = json::parse(r.body, nullptr, false);
        if (!arr.is_array() || (int)arr.size() < optionIndex || !arr[optionIndex - 1].is_string()) return {};
        std::string fragment = arr[optionIndex - 1].get<std::string>();
        if (trim(fragment).empty()) return {};

        html::Document frag(fragment, baseUrl() + "/");
        html::Node iframe = frag.selectFirst("iframe");
        if (!iframe) return {};
        std::string iframeSrc = frag.absUrl(iframe, "src");
        if (iframeSrc.empty()) return {};

        std::string player = checkedGet(iframeSrc, headers(), "HentaiMama").body;
        // sources:\s*(\[.+?\])
        std::string sourcesJson;
        size_t p = 0;
        while ((p = player.find("sources:", p)) != std::string::npos) {
            size_t s = p + 8;
            while (s < player.size() && std::isspace((unsigned char)player[s])) s++;
            if (s < player.size() && player[s] == '[') {
                size_t e = player.find(']', s + 2);
                if (e != std::string::npos) sourcesJson = player.substr(s, e - s + 1);
                break;
            }
            p = s;
        }
        if (sourcesJson.empty()) return {};
        json sources = json::parse(sourcesJson, nullptr, false);
        if (!sources.is_array()) return {};

        std::vector<Video> out;
        for (auto& src : sources) {
            std::string file = replaceAll(jstr(src, "file"), "\\/", "/");
            if (file.empty()) continue;
            std::string label = jstr(src, "label");
            std::string type = jstr(src, "type");

            Video fallback;
            fallback.url = file;
            fallback.title = !label.empty() ? hoster + " - " + label : hoster;
            fallback.quality = std::atoi(digitsOnly(label).c_str());
            fallback.referer = baseUrl();
            fallback.userAgent = http::DEFAULT_UA;

            if (type == "hls" || contains(file, ".m3u8")) {
                try {
                    auto v = extractHls(file, {{"Referer", baseUrl()}}, baseUrl(),
                                        [&](const std::string& q) { return hoster + " - " + q; });
                    if (!v.empty()) {
                        out.insert(out.end(), v.begin(), v.end());
                        continue;
                    }
                } catch (const std::exception&) {
                }
            }
            out.push_back(fallback);
        }
        return out;
    }

    static std::string textOf(const std::vector<html::Node>& nodes) { return html::textOf(nodes); }
};

// ================================================================================== Oppai Stream

class OppaiStream : public Source {
  public:
    std::string id() const override { return "en.oppaistream"; }
    std::string name() const override { return "Oppai Stream"; }
    std::string defaultBaseUrl() const override { return "https://oppai.stream"; }
    std::string lang() const override { return "en"; }
    bool nsfw() const override { return true; }

    static constexpr int SEARCH_LIMIT = 36;

    Page popular(int page) override {
        return parseSearch(baseUrl() + "/actions/search.php?order=views&page=" + std::to_string(page) +
                           "&limit=" + std::to_string(SEARCH_LIMIT));
    }

    Page latest(int page) override {
        return parseSearch(baseUrl() + "/actions/search.php?order=uploaded&page=" + std::to_string(page) +
                           "&limit=" + std::to_string(SEARCH_LIMIT));
    }

    Page search(const std::string& query, int page) override {
        std::string url = baseUrl() + "/actions/search.php?text=" + http::urlEncode(trim(query)) +
                          "&order=az&genres=&blacklist=&studio=&page=" + std::to_string(page) +
                          "&limit=" + std::to_string(SEARCH_LIMIT);
        return parseSearch(url);
    }

    Details details(const std::string& animeUrl) override {
        std::string pageUrl = http::resolve(baseUrl() + "/", animeUrl);
        html::Document doc(checkedGet(pageUrl, headers(), "Oppai Stream").body, pageUrl);

        Details d;
        html::Node h1 = doc.selectFirst("div.episode-info > h1");
        if (!h1) throw http::Error("Oppai Stream: pagina non riconosciuta");
        std::string name = substringBefore(h1.text(), " Ep ");
        d.title = name;
        html::Node desc = doc.selectFirst("div.description");
        if (desc) d.description = substringBeforeLast(desc.text(), " Watch ");
        d.genre = joinTexts(doc.select("div.tags a"));
        std::vector<std::string> studios;
        for (auto& a : doc.select("div.episode-info a.red")) studios.push_back(a.text());
        for (auto& s : studios) d.author += (d.author.empty() ? "" : ", ") + s;
        d.status = "";

        // copertina da AniList (predefinito attivo) solo se lo studio corrisponde
        std::string newTitle = name;
        for (auto& c : newTitle) {
            unsigned char u = (unsigned char)c;
            if (!(std::isalnum(u) || std::isspace(u) || c == '!' || c == '.' || c == ':' || c == '"')) c = ' ';
        }
        try {
            std::string cover;
            std::vector<std::string> alStudios;
            if (anilistCover(newTitle, cover, alStudios)) {
                bool match = std::any_of(alStudios.begin(), alStudios.end(), [&](const std::string& s) {
                    return std::find(studios.begin(), studios.end(), s) != studios.end();
                });
                if (match) d.thumbnail = cover;
            }
        } catch (const std::exception&) {
        }
        if (d.thumbnail.empty()) d.thumbnail = doc.selectFirst("video#episode").attr("poster");

        for (auto& el : doc.select("div.more-same-eps .in-main-gr > a")) {
            Episode ep;
            std::string href = el.attr("exur");
            if (href.empty()) href = el.attr("href");
            ep.url = withoutDomain(fixLink(href));
            html::Node epn = el.selectFirst("font.ep");
            std::string num = epn ? epn.text() : "1";
            ep.name = "Episode " + num;
            ep.number = parseNumber(num, 1);
            if (num != numberAt(num, 0)) ep.number = 1;
            d.episodes.push_back(ep);
        }
        std::reverse(d.episodes.begin(), d.episodes.end());
        return d;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::string pageUrl = http::resolve(baseUrl() + "/", episodeUrl);
        html::Document doc(checkedGet(pageUrl, headers(), "Oppai Stream").body, pageUrl);

        std::string script;
        for (auto& s : doc.select("script")) {
            std::string data = s.data();
            if (contains(data, "var availableres")) {
                script = data;
                break;
            }
        }
        if (script.empty()) throw http::Error("Nessun video trovato");

        std::vector<Video::Track> subs;
        for (auto& t : doc.select("track[kind=captions], track[kind=subtitles]"))
            subs.push_back({t.attr("src"), t.attr("label")});

        std::string obj = substringBefore(substringAfter(script, "var availableres = {"), "}");
        std::vector<Video> out;
        size_t start = 0;
        while (start <= obj.size()) {
            auto comma = obj.find(',', start);
            std::string item = obj.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            start = comma == std::string::npos ? obj.size() + 1 : comma + 1;
            item = replaceAll(replaceAll(item, "\"", ""), "\\", "");
            auto colon = item.find(':');
            if (colon == std::string::npos) continue;
            std::string resolution = trim(item.substr(0, colon));
            std::string url = trim(item.substr(colon + 1));
            if (url.empty()) continue;
            std::string fixed = resolution == "4k" ? "2160p" : resolution + "p";
            Video v;
            v.url = url;
            v.title = fixed;
            v.quality = resolution == "4k" ? 2160 : std::atoi(resolution.c_str());
            v.referer = baseUrl();
            v.userAgent = http::DEFAULT_UA;
            v.subtitles = subs;
            out.push_back(v);
        }
        if (out.empty()) throw http::Error("Nessun video trovato");
        // sortedWith(compareBy { contains(pref) }).reversed(): preferito in testa, poi ordine inverso
        std::reverse(out.begin(), out.end());
        preferFirst(out, "1080p");
        return out;
    }

  private:
    http::Headers headers() const { return {{"Referer", baseUrl()}}; }

    Page parseSearch(const std::string& url) {
        html::Document doc(checkedGet(url, headers(), "Oppai Stream").body, url);
        auto elements = doc.select("div.episode-shown > div > a");
        Page p;
        std::set<std::string> titles;
        for (auto& el : elements) {
            Anime a;
            html::Node img = el.selectFirst("img.cover-img-in");
            if (img) a.thumbnail = doc.absUrl(img, "src");
            a.title = stripTrailingNumber(el.selectFirst(".title-ep").text());
            std::string href = el.attr("exur");
            if (href.empty()) href = el.attr("href");
            a.url = withoutDomain(fixLink(href));
            if (!titles.insert(a.title).second) continue;
            p.animes.push_back(a);
        }
        p.hasNextPage = (int)elements.size() >= SEARCH_LIMIT;
        return p;
    }

    /** Regex("""\s+\d+$""") -> "" */
    static std::string stripTrailingNumber(const std::string& s) {
        size_t e = s.size(), i = e;
        while (i > 0 && std::isdigit((unsigned char)s[i - 1])) i--;
        if (i == e) return s;
        size_t j = i;
        while (j > 0 && std::isspace((unsigned char)s[j - 1])) j--;
        if (j == i) return s;
        return s.substr(0, j);
    }

    /** Codifica il valore tra "?e=" e "&f=" (fixLink). */
    static std::string fixLink(const std::string& s) {
        auto a = s.find("?e=");
        if (a == std::string::npos) return s;
        a += 3;
        auto b = s.find("&f=", a);
        if (b == std::string::npos) return s;
        return s.substr(0, a) + http::urlEncode(s.substr(a, b - a)) + s.substr(b);
    }

    bool anilistCover(const std::string& title, std::string& cover, std::vector<std::string>& studios) {
        std::string query = "query {\n    Media(search: \"" + title +
                            "\", type: ANIME, isAdult: true) {\n        coverImage {\n            extraLarge\n"
                            "            large\n        }\n        studios {\n            nodes {\n"
                            "                name\n            }\n        }\n    }\n}";
        http::Headers h = {
            {"Referer", baseUrl()},
            {"Content-Type", "application/x-www-form-urlencoded"},
        };
        http::Response r = http::request("POST", "https://graphql.anilist.co", h, "query=" + http::urlEncode(query));
        if (r.status < 200 || r.status >= 300) return false;
        json j = json::parse(r.body, nullptr, false);
        if (!j.is_object() || !j.contains("data") || !j["data"].is_object()) return false;
        const json& media = j["data"].value("Media", json());
        if (!media.is_object()) return false;
        cover = jstr(media.value("coverImage", json::object()), "extraLarge");
        for (auto& n : media.value("studios", json::object()).value("nodes", json::array()))
            studios.push_back(jstr(n, "name"));
        return !cover.empty();
    }
};

}  // namespace

std::vector<std::shared_ptr<Source>> makeAdultSources() {
    return {
        std::make_shared<Hstream>(),
        std::make_shared<HentaiHaven>(),
        std::make_shared<HentaiMama>(),
        std::make_shared<OppaiStream>(),
    };
}

}  // namespace src
