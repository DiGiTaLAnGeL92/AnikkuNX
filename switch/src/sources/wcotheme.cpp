// Porting in C++ del tema multisrc Aniyomi "wcotheme" (lib-multisrc/wcotheme, baseVersionCode 7)
// e delle estensioni che lo usano: Wcofun, WCOStream, WcoAnimeSub, WcoForever, WcoAnimeDub, WcoTv.

#include <gumbo.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <random>
#include <regex>
#include <set>
#include <thread>

#include "html/html.hpp"
#include "sources/registry.hpp"

namespace src {

namespace {

const char* WCO_UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/151.0.0.0 "
    "Safari/537.36";
const int WCO_ANTIBOT_DELAY_SECONDS = 12;  // PREF_DELAY_DEFAULT
const char* WCO_PREF_QUALITY = "720";      // PREF_QUALITY_DEFAULT

struct WcoConfig {
    std::string id;
    std::string name;
    std::string baseUrl;
    bool supportsLatest = true;
    bool useOldIframeExtractor = false;
    bool querySearchBroken = false;  // WcoAnimeSub / WcoAnimeDub: ricerca lato server rotta
    bool wcostreamLayout = false;    // WCOStream: selettori diversi
    std::string fallbackTitleSelector = ".video-title";  // WcoForever: ".baslikCell"
};

// ---------------------------------------------------------------- helper DOM

bool containsIgnoreCase(const std::string& hay, const std::string& needle) {
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(), [](char a, char b) {
        return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
    });
    return it != hay.end();
}

/** Element.ownText() di Jsoup: solo i nodi di testo figli diretti, spazi normalizzati. */
std::string ownText(const html::Node& n) {
    if (!n.valid() || n.raw()->type != GUMBO_NODE_ELEMENT) return "";
    std::string raw;
    const GumboVector& ch = n.raw()->v.element.children;
    for (unsigned i = 0; i < ch.length; i++) {
        auto* c = static_cast<GumboNode*>(ch.data[i]);
        if (c->type == GUMBO_NODE_TEXT || c->type == GUMBO_NODE_WHITESPACE || c->type == GUMBO_NODE_CDATA)
            raw += c->v.text.text;
    }
    std::string out;
    bool space = false;
    for (char c : raw) {
        if (std::isspace((unsigned char)c)) {
            space = true;
        } else {
            if (space && !out.empty()) out += ' ';
            space = false;
            out += c;
        }
    }
    return out;
}

/** Fratello elemento successivo (per il combinatore '+'). */
html::Node nextElementSibling(const html::Node& n) {
    html::Node p = n.parent();
    if (!p.valid()) return {};
    auto kids = p.children();
    for (size_t i = 0; i + 1 < kids.size(); i++)
        if (kids[i].raw() == n.raw()) return kids[i + 1];
    return {};
}

std::vector<html::Node> childrenByTag(const html::Node& n, const std::string& tag) {
    std::vector<html::Node> out;
    for (auto& c : n.children())
        if (c.tag() == tag) out.push_back(c);
    return out;
}

/** "X > ul > li" */
std::vector<html::Node> ulLi(const html::Node& n) {
    std::vector<html::Node> out;
    for (auto& ul : childrenByTag(n, "ul"))
        for (auto& li : childrenByTag(ul, "li")) out.push_back(li);
    return out;
}

/** "div.recent-release:contains(label) + div > ul > li" */
std::vector<html::Node> recentGrid(const html::Document& doc, const std::string& label) {
    std::vector<html::Node> out;
    for (auto& rr : doc.select("div.recent-release")) {
        if (!containsIgnoreCase(rr.text(), label)) continue;
        html::Node sib = nextElementSibling(rr);
        if (!sib.valid() || sib.tag() != "div") continue;
        for (auto& li : ulLi(sib)) out.push_back(li);
    }
    return out;
}

std::string utf8(unsigned cp) {
    std::string s;
    if (cp < 0x80) {
        s += (char)cp;
    } else if (cp < 0x800) {
        s += (char)(0xC0 | (cp >> 6));
        s += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += (char)(0xE0 | (cp >> 12));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    } else {
        s += (char)(0xF0 | (cp >> 18));
        s += (char)(0x80 | ((cp >> 12) & 0x3F));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    }
    return s;
}

std::string randomHex16() {
    static const char* hex = "0123456789abcdef";
    std::random_device rd;
    std::mt19937 gen(rd() ^ (unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
    std::uniform_int_distribution<int> dist(0, 255);
    std::string out;
    for (int i = 0; i < 16; i++) {
        int b = dist(gen);
        out += hex[b >> 4];
        out += hex[b & 15];
    }
    return out;
}

// ---------------------------------------------------------------- sorgente

class WcoThemeSource : public Source {
  public:
    explicit WcoThemeSource(WcoConfig c) : cfg(std::move(c)) {}

    std::string id() const override { return cfg.id; }
    std::string name() const override { return cfg.name; }
    std::string defaultBaseUrl() const override { return cfg.baseUrl; }
    std::string lang() const override { return "en"; }
    bool supportsLatest() const override { return cfg.supportsLatest; }

    http::Headers imageHeaders() const override {
        return {{"User-Agent", WCO_UA}, {"Referer", baseUrl() + "/"}};
    }

    // ============================== Popular ===============================
    Page popular(int page) override {
        Page p;
        if (page > 1) return p;  // nessuna paginazione
        std::string url = baseUrl();
        html::Document doc(http::getText(url, headers()), url);
        p.animes = popularFrom(doc);
        return p;
    }

    // =============================== Latest ===============================
    Page latest(int page) override {
        Page p;
        if (page > 2) return p;
        std::string url = baseUrl();
        html::Document doc(http::getText(url, headers()), url);
        if (page == 1) {
            // "Recent Releases"; la "pagina successiva" e' la griglia "Recently Added"
            for (auto& li : recentGrid(doc, "Recent Releases")) addAnime(p.animes, doc, li);
            p.hasNextPage = !recentGrid(doc, "Recently Added").empty();
        } else {
            for (auto& li : recentGrid(doc, "Recently Added")) addAnime(p.animes, doc, li);
        }
        return p;
    }

    // =============================== Search ===============================
    Page search(const std::string& query, int page) override {
        Page p;
        if (page > 1) return p;
        std::string q = trim(query);
        if (q.empty()) return popular(page);

        if (cfg.querySearchBroken) {
            // Il sito non supporta la ricerca (l'estensione restituisce i popolari):
            // qui si filtra almeno l'elenco dei popolari per titolo.
            Page all = popular(1);
            for (auto& a : all.animes)
                if (containsIgnoreCase(a.title, q)) p.animes.push_back(a);
            return p;
        }

        std::string url = baseUrl() + "/search";
        http::Headers h = headers();
        h.push_back({"Content-Type", "application/x-www-form-urlencoded"});
        std::string body = "catara=" + http::urlEncode(q) + "&konuara=series";
        http::Response r = http::request("POST", url, h, body);
        if (r.status < 200 || r.status >= 300)
            throw http::Error(cfg.name + " ha risposto HTTP " + std::to_string(r.status));
        std::string finalUrl = r.finalUrl.empty() ? url : r.finalUrl;
        html::Document doc(r.body, finalUrl);
        if (finalUrl.find("/search") == std::string::npos) {
            p.animes = popularFrom(doc);
            return p;
        }
        const char* sel = cfg.wcostreamLayout ? "div#blog div.iccerceve" : "div#sidebar_right2 li";
        for (auto& el : doc.select(sel)) addAnime(p.animes, doc, el);
        return p;
    }

    // =========================== Anime Details ============================
    Details details(const std::string& animeUrl) override {
        std::string url = absolute(animeUrl);
        http::Response r = http::request("GET", url, headers());
        if (r.status < 200 || r.status >= 300)
            throw http::Error(cfg.name + " ha risposto HTTP " + std::to_string(r.status));
        std::string finalUrl = r.finalUrl.empty() ? url : r.finalUrl;
        html::Document doc(r.body, finalUrl);

        Details d;
        const char* titleSels[] = {"div.video-title a", "div.header-tag h2 a", "div.video-title h1",
                                   "div.baslikCell h1"};
        int nTitle = cfg.wcostreamLayout ? 3 : 4;
        for (int i = 0; i < nTitle; i++) {
            html::Node n = doc.selectFirst(titleSels[i]);
            if (n.valid()) {
                d.title = n.text();
                break;
            }
        }

        if (cfg.wcostreamLayout) {
            d.genre = joinText(doc.select("div#cat-genre > div.wcobtn"));
            d.description = html::textOf(doc.select("div#content div.katcont div.iltext p"));
            html::Node img = doc.selectFirst("#cat-img-desc img");
            if (img.valid()) d.thumbnail = doc.absUrl(img, "src");
        } else {
            d.description = doc.selectFirst("div#sidebar_cat p").text();
            html::Node img = doc.selectFirst("div#sidebar_cat img");
            if (img.valid()) d.thumbnail = doc.absUrl(img, "src");
            d.genre = joinText(doc.select("div#sidebar_cat > a"));
        }

        d.episodes = episodesFrom(doc, finalUrl);
        return d;
    }

    // ============================ Video Links =============================
    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::string url = absolute(episodeUrl);
        http::Response r = http::request("GET", url, headers());
        if (r.status < 200 || r.status >= 300)
            throw http::Error(cfg.name + " ha risposto HTTP " + std::to_string(r.status));
        std::string referer = r.finalUrl.empty() ? url : r.finalUrl;
        // gli episodi piu' vecchi ora sono riservati agli abbonati (pagina senza lettore)
        if (!cfg.useOldIframeExtractor && r.body.find("cizgi-js-0\" src") == std::string::npos &&
            r.body.find("wcopremium.tv/wp-login") != std::string::npos)
            throw http::Error("Episodio disponibile solo per gli abbonati Premium del sito");
        html::Document doc(r.body, referer);

        std::vector<Video> out;
        if (cfg.useOldIframeExtractor) {
            std::string iframe = oldIframeUrl(doc);
            if (iframe.empty()) throw http::Error("Nessun iframe trovato nella pagina dell'episodio");
            try {
                out = iframeParse(iframe, referer);
            } catch (const std::exception&) {
            }
        } else {
            auto iframes = doc.select("iframe");
            if (iframes.empty()) throw http::Error("Nessun iframe trovato nella pagina dell'episodio");
            std::set<std::string> seen;
            for (auto& f : iframes) {
                std::string link = doc.absUrl(f, "src");
                if (link.empty() || !seen.insert(link).second) continue;
                try {
                    auto v = iframeParse(link, referer);
                    out.insert(out.end(), v.begin(), v.end());
                } catch (const std::exception&) {
                    // iframe non disponibile: si prova il successivo
                }
            }
        }
        if (out.empty()) throw http::Error("Nessun video trovato");

        // sortVideos(): prima la qualita' preferita (720), poi le altre dalla migliore
        std::stable_sort(out.begin(), out.end(), [](const Video& a, const Video& b) {
            bool pa = a.title.find(WCO_PREF_QUALITY) != std::string::npos;
            bool pb = b.title.find(WCO_PREF_QUALITY) != std::string::npos;
            if (pa != pb) return pa;
            return a.quality > b.quality;
        });
        return out;
    }

  private:
    WcoConfig cfg;

    http::Headers headers() const {
        return {{"User-Agent", WCO_UA}, {"Referer", baseUrl() + "/"}, {"Origin", baseUrl()}};
    }

    std::string absolute(const std::string& u) const {
        if (u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0) return u;
        return http::resolve(baseUrl() + "/", u);
    }

    static std::string joinText(const std::vector<html::Node>& nodes) {
        std::string out;
        for (auto& n : nodes) {
            std::string t = n.text();
            if (t.empty()) continue;
            if (!out.empty()) out += ", ";
            out += t;
        }
        return out;
    }

    /** gridItemToAnime() */
    static void addAnime(std::vector<Anime>& list, const html::Document& doc, const html::Node& el) {
        html::Node anchor = el.selectFirst(".recent-release-episodes a, .img a");
        if (!anchor.valid()) anchor = el.selectFirst("a");
        if (!anchor.valid() && el.tag() == "a") anchor = el;
        if (!anchor.valid()) return;
        std::string href = doc.absUrl(anchor, "href");
        if (href.empty()) return;
        Anime a;
        a.url = http::pathOf(href);
        if (a.url.empty() || a.url == "/") return;  // WcoTv: voci senza URL
        a.title = trim(ownText(anchor));
        if (a.title.empty()) a.title = trim(el.selectFirst("img[alt]").attr("alt"));
        if (a.title.empty()) a.title = trim(el.text());
        if (a.title.empty()) return;
        html::Node img = el.selectFirst("img[src]");
        if (img.valid()) a.thumbnail = doc.absUrl(img, "src");
        list.push_back(a);
    }

    std::vector<Anime> popularFrom(const html::Document& doc) const {
        std::vector<Anime> out;
        if (cfg.wcostreamLayout) {
            // div#content > div > div:has(div.recent-release:contains(Recent Releases)) > div > ul > li
            for (auto& d : doc.select("div#content > div > div")) {
                bool match = false;
                for (auto& rr : d.select("div.recent-release"))
                    if (containsIgnoreCase(rr.text(), "Recent Releases")) match = true;
                if (!match) continue;
                for (auto& inner : childrenByTag(d, "div"))
                    for (auto& li : ulLi(inner)) addAnime(out, doc, li);
            }
        } else {
            for (auto& li : doc.select("div#sidebar_right2 ul.items > li")) addAnime(out, doc, li);
        }
        return out;
    }

    // ============================== Episodes ==============================
    struct Ep {
        Episode e;
        bool dub = false;
    };

    /** episodeTitleFromElement(): "(Season N )?Episode N titolo" */
    static std::pair<std::string, double> parseEpisodeTitle(const std::string& title) {
        if (title.size() < 2000) {
            static const std::regex re("(Season (\\d+) )?Episode (\\d+) (.*)");
            std::smatch m;
            if (std::regex_search(title, m, re)) {
                int season = m[2].matched ? std::atoi(m[2].str().c_str()) : 0;
                int ep = std::atoi(m[3].str().c_str());
                double number = (double)(((season > 0 ? season : 1) - 1) * 100 + ep);
                std::string name;
                if (season > 0) name += "Season " + std::to_string(season) + " - ";
                name += "Episode " + std::to_string(ep) + ": ";
                name += trim(m[4].str());
                return {name, number};
            }
        }
        return {title, 1};
    }

    std::vector<Episode> episodesFrom(const html::Document& doc, const std::string& pageUrl) const {
        std::vector<html::Node> nodes;
        std::set<GumboNode*> seen;
        auto add = [&](const html::Node& n) {
            if (seen.insert(n.raw()).second) nodes.push_back(n);
        };
        if (cfg.wcostreamLayout) {
            for (auto& n : doc.select("div#catlist-listview > ul > li")) add(n);
            // table:has(> tbody > tr > td > h3:contains(Episode List)) div.menustyle > ul > li
            for (auto& t : doc.select("table")) {
                bool match = false;
                for (auto& tb : childrenByTag(t, "tbody"))
                    for (auto& tr : childrenByTag(tb, "tr"))
                        for (auto& td : childrenByTag(tr, "td"))
                            for (auto& h3 : childrenByTag(td, "h3"))
                                if (containsIgnoreCase(h3.text(), "Episode List")) match = true;
                if (!match) continue;
                for (auto& ms : t.select("div.menustyle"))
                    for (auto& li : ulLi(ms)) add(li);
            }
        } else {
            for (auto& n : doc.select("div.cat-eps")) add(n);
        }
        for (auto& n : doc.select("div#episodeList a.dark-episode-item, nav#sidebarEpisodeList a.sidebar-episode-item"))
            add(n);

        std::vector<Ep> eps;
        std::set<std::string> urls;
        for (auto& el : nodes) {
            html::Node anchor = el.tag() == "a" ? el : el.selectFirst("a");
            if (!anchor.valid()) continue;
            std::string href = doc.absUrl(anchor, "href");
            if (href.empty()) continue;
            std::string path = http::pathOf(href);
            if (!urls.insert(path).second) continue;  // stesso episodio in piu' liste
            html::Node span = anchor.selectFirst("span");
            std::string title = span.valid() ? span.text() : el.text();
            auto parsed = parseEpisodeTitle(title);
            Ep ep;
            ep.e.url = path;
            ep.e.name = parsed.first;
            ep.e.number = parsed.second;
            ep.dub = containsIgnoreCase(ep.e.name, "dub");
            eps.push_back(ep);
        }

        std::vector<Episode> out;
        if (eps.empty()) {
            // la pagina e' gia' un episodio
            Episode e;
            e.url = http::pathOf(pageUrl);
            e.name = parseEpisodeTitle(html::textOf(doc.select(cfg.fallbackTitleSelector))).first;
            if (e.name.empty()) e.name = "Episode";
            e.number = 1;
            out.push_back(e);
            return out;
        }
        // Prima i sottotitolati poi i doppiati; in ogni gruppo dal piu' recente al piu' vecchio
        std::stable_sort(eps.begin(), eps.end(), [](const Ep& a, const Ep& b) {
            if (a.dub != b.dub) return !a.dub;
            return a.e.number > b.e.number;
        });
        for (auto& ep : eps) out.push_back(ep.e);
        return out;
    }

    // ============================ Estrazione ==============================

    /** iframeOldExtractor(): array di stringhe base64 + spostamento dei codici carattere. */
    static std::string oldIframeUrl(const html::Document& doc) {
        std::string script;
        for (auto& s : doc.select("script")) {
            std::string d = s.data();
            if (d.find("decodeURIComponent") != std::string::npos) {
                script = d;
                break;
            }
        }
        if (script.empty()) throw http::Error("Nessuno script trovato nella pagina dell'episodio");

        std::string arr = trim(substringBefore(substringAfter(script, "["), "]"));
        while (!arr.empty() && arr.back() == ',') arr = trim(arr.substr(0, arr.size() - 1));
        nlohmann::json list = nlohmann::json::parse("[" + arr + "]", nullptr, false);
        if (!list.is_array()) throw http::Error("Script del lettore non riconosciuto");

        auto lastDash = script.rfind("- ");
        if (lastDash == std::string::npos) throw http::Error("Script del lettore non riconosciuto");
        std::string shiftStr = trim(substringBefore(script.substr(lastDash + 2), ");"));
        int shift = std::atoi(shiftStr.c_str());

        std::string iframeHtml;
        for (auto& item : list) {
            if (!item.is_string()) continue;
            std::string digits = digitsOnly(base64Decode(item.get<std::string>()));
            if (digits.empty()) continue;
            long code = std::atol(digits.c_str()) - shift;
            if (code > 0 && code < 0x110000) iframeHtml += utf8((unsigned)code);
        }
        html::Document frag(iframeHtml);
        std::string src = trim(frag.selectFirst("iframe").attr("src"));
        if (src.rfind("//", 0) == 0) src = "https:" + src;
        return src;
    }

    static http::Headers chromeHeaders() {
        return {
            {"User-Agent", WCO_UA},
            {"Accept-Language", "en-US,en;q=0.9"},
            {"Dnt", "1"},
            {"Sec-Ch-Ua", "\"Not=A?Brand\";v=\"99\", \"Google Chrome\";v=\"151\", \"Chromium\";v=\"151\""},
            {"Sec-Ch-Ua-Mobile", "?0"},
            {"Sec-Ch-Ua-Platform", "\"Windows\""},
        };
    }

    std::vector<Video> iframeParse(const std::string& iframeLink, const std::string& episodeReferer) {
        if (iframeLink.find("embed.wcostream") != std::string::npos) return embedWcostream(iframeLink, episodeReferer);
        if (iframeLink.find("vhs.watchanimesub") != std::string::npos) return premium(iframeLink);
        return {};
    }

    /** Doppiato / hard-sub: embed.wcostream con verifica anti-bot. */
    std::vector<Video> embedWcostream(const std::string& iframeLink, const std::string& episodeReferer) {
        std::string iframeDomain = "https://" + http::hostOf(iframeLink);

        // 1. index.php
        http::Headers nav = {
            {"User-Agent", WCO_UA},
            {"Accept",
             "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8,"
             "application/signed-exchange;v=b3;q=0.7"},
            {"Referer", episodeReferer},
            {"Sec-Fetch-Dest", "iframe"},
            {"Sec-Fetch-Site", "cross-site"},
        };
        http::request("GET", iframeLink, nav);

        // 1b/1c. sotto-risorse tracciate dal server
        try {
            http::request("GET", iframeDomain + "/inc/embed/pre-init.js?v2", nav);
        } catch (const std::exception&) {
        }
        try {
            http::request("GET", iframeDomain + "/assets/ads/advertisement.js", nav);
        } catch (const std::exception&) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // 2. beacon "clear"
        std::string pid = http::queryParam(iframeLink, "pid");
        if (pid.empty()) return {};
        std::string nonce = randomHex16();
        nlohmann::json beacon = {{"nonce", nonce}, {"status", "clear"}, {"id", pid}};
        http::Headers bh = chromeHeaders();
        bh.push_back({"Accept", "*/*"});
        bh.push_back({"Content-Type", "application/json"});
        bh.push_back({"Origin", iframeDomain});
        bh.push_back({"Referer", iframeLink});
        bh.push_back({"Sec-Fetch-Dest", "empty"});
        bh.push_back({"Sec-Fetch-Mode", "cors"});
        bh.push_back({"Sec-Fetch-Site", "same-origin"});
        http::request("POST", iframeDomain + "/ad-verify", bh, beacon.dump());

        // 3. attesa imposta dal server dopo il beacon
        std::this_thread::sleep_for(std::chrono::seconds(WCO_ANTIBOT_DELAY_SECONDS));

        // 4. pagina del lettore: prima video-js-old.php, poi video-js.php
        http::Headers eh = chromeHeaders();
        eh.push_back({"Accept",
                      "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;"
                      "q=0.8,application/signed-exchange;v=b3;q=0.7"});
        eh.push_back({"Priority", "u=0, i"});
        eh.push_back({"Referer", iframeLink});
        eh.push_back({"Sec-Fetch-Dest", "iframe"});
        eh.push_back({"Sec-Fetch-Mode", "navigate"});
        eh.push_back({"Sec-Fetch-Site", "same-origin"});
        eh.push_back({"Sec-Fetch-Storage-Access", "none"});
        eh.push_back({"Sec-Fetch-User", "?1"});
        eh.push_back({"Upgrade-Insecure-Requests", "1"});

        std::string query;
        auto q = iframeLink.find('?');
        if (q != std::string::npos) query = substringBefore(iframeLink.substr(q + 1), "#");
        auto playerUrl = [&](const std::string& path) {
            return iframeDomain + path + "?" + (query.empty() ? "" : query + "&") + "n=" + nonce;
        };

        std::string playerBody;
        http::Response oldR = http::request("GET", playerUrl("/inc/embed/video-js-old.php"), eh);
        if (oldR.status >= 200 && oldR.status < 300 && oldR.body.find("$.getJSON") != std::string::npos) {
            playerBody = std::move(oldR.body);
        } else {
            playerBody = http::getText(playerUrl("/inc/embed/video-js.php"), eh);
        }

        html::Document player(playerBody, iframeDomain);
        std::string script;
        for (auto& s : player.select("script")) {
            std::string d = s.data();
            if (d.find("getJSON") != std::string::npos) {
                script = d;
                break;
            }
        }
        if (script.empty()) return {};
        std::string link = substringBefore(substringAfter(script, "$.getJSON(\""), "\"");
        std::string requestUrl = iframeDomain + link;

        http::Headers rh = {
            {"User-Agent", WCO_UA},
            {"Referer", requestUrl},
            {"Origin", iframeDomain},
            {"X-Requested-With", "XMLHttpRequest"},
        };
        auto j = nlohmann::json::parse(http::getText(requestUrl, rh));
        std::string server = j.value("server", "");
        if (server.empty()) return {};

        std::vector<Video> out;
        auto addQ = [&](const char* key, const char* label, int height) {
            if (!j.contains(key) || !j[key].is_string()) return;
            std::string v = trim(j[key].get<std::string>());
            if (v.empty()) return;
            Video vid;
            vid.title = label;
            vid.url = server + "/getvid?evid=" + v;
            vid.quality = height;
            vid.referer = baseUrl() + "/";
            vid.userAgent = WCO_UA;
            vid.headers = {{"Origin", baseUrl()}};
            out.push_back(vid);
        };
        addQ("enc", "480p", 480);
        addQ("hd", "720p", 720);
        addQ("fhd", "1080p", 1080);
        return out;
    }

    /** Video "Premium" (vhs.watchanimesub): playlist HLS con sottotitoli/audio multipli. */
    std::vector<Video> premium(const std::string& iframeLink) {
        std::string body = http::getText(iframeLink, headers());
        const std::string marker = "getRedirectedUrl(\"";
        size_t pos = 0;
        while ((pos = body.find(marker, pos)) != std::string::npos) {
            pos += marker.size();
            auto end = body.find('"', pos);
            if (end == std::string::npos) break;
            std::string url = body.substr(pos, end - pos);
            const std::string suffix = "/index.m3u8";
            if (url.rfind("https://", 0) == 0 && url.size() > suffix.size() &&
                url.compare(url.size() - suffix.size(), suffix.size(), suffix) == 0) {
                Video v;
                // la playlist master viene passata intera a mpv (sceglie variante e tracce)
                v.title = "Premium - auto";
                v.url = url;
                std::string referer = iframeLink + "/";
                v.referer = referer;
                v.userAgent = WCO_UA;
                v.headers = {{"Origin", "https://" + http::hostOf(referer)}, {"Accept", "*/*"}};
                return {v};
            }
        }
        return {};
    }
};

std::shared_ptr<Source> make(WcoConfig c) { return std::make_shared<WcoThemeSource>(std::move(c)); }

}  // namespace

std::vector<std::shared_ptr<Source>> makeWcoThemeSources() {
    std::vector<std::shared_ptr<Source>> out;
    {
        WcoConfig c;
        c.id = "en.wcofun";
        c.name = "Wcofun";
        c.baseUrl = "https://www.wcoflix.tv";
        out.push_back(make(c));
    }
    {
        WcoConfig c;
        c.id = "en.wcostream";
        c.name = "WCOStream";
        c.baseUrl = "https://www.wcostream.tv";
        c.supportsLatest = false;
        c.wcostreamLayout = true;
        out.push_back(make(c));
    }
    {
        WcoConfig c;
        c.id = "en.wcoanimesub";
        c.name = "WcoAnimeSub";
        c.baseUrl = "https://www.wcoanimesub.tv";
        c.useOldIframeExtractor = true;
        c.querySearchBroken = true;
        out.push_back(make(c));
    }
    {
        WcoConfig c;
        c.id = "en.wcoforever";
        c.name = "WcoForever";
        c.baseUrl = "https://www.wcoforever.net";
        c.fallbackTitleSelector = ".baslikCell";
        out.push_back(make(c));
    }
    {
        WcoConfig c;
        c.id = "en.wcoanimedub";
        c.name = "WcoAnimeDub";
        c.baseUrl = "https://www.wcoanimedub.tv";
        c.useOldIframeExtractor = true;
        c.querySearchBroken = true;
        out.push_back(make(c));
    }
    {
        WcoConfig c;
        c.id = "en.wcotv";
        c.name = "WcoTv";
        c.baseUrl = "https://www.wco.tv";
        c.useOldIframeExtractor = true;
        out.push_back(make(c));
    }
    return out;
}

}  // namespace src
