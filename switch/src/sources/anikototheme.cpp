// Porting in C++ del tema multisrc Aniyomi "AnikotoTheme" (lib-multisrc/anikototheme, AnikotoExtractor.kt)
// e dei siti che lo usano: Anichi, Anikoto, AniWave (Unoriginal), AnimeSogo, AnimeKai (Unoriginal).

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <set>

#include "html/html.hpp"
#include "sources/registry.hpp"
#include "util/crypto.hpp"

using json = nlohmann::json;

namespace src {

namespace {

// ============================== Utilita' ===============================

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool icontains(const std::string& hay, const std::string& needle) {
    return lower(hay).find(lower(needle)) != std::string::npos;
}

bool iequals(const std::string& a, const std::string& b) { return lower(a) == lower(b); }

bool startsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }

bool endsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

bool hasClass(const html::Node& n, const std::string& cls) {
    std::string c = " " + n.attr("class") + " ";
    for (auto& ch : c)
        if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
    return c.find(" " + cls + " ") != std::string::npos;
}

std::string trimEndChars(std::string s, const std::string& chars) {
    while (!s.empty() && chars.find(s.back()) != std::string::npos) s.pop_back();
    return s;
}

std::string jstr(const json& j, const char* key) {
    if (!j.is_object()) return "";
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return "";
    if (it->is_string()) return it->get<std::string>();
    return it->dump();
}

/** Rimuove il suffisso "/ep-N" finale (EP_URL_SUFFIX_REGEX). */
std::string stripEpSuffix(const std::string& path) {
    auto p = path.rfind("/ep-");
    if (p == std::string::npos) return path;
    std::string rest = path.substr(p + 4);
    if (rest.empty()) return path;
    for (char c : rest)
        if (!std::isdigit((unsigned char)c)) return path;
    return path.substr(0, p);
}

/** Percorso (senza query) di un href assoluto o relativo. */
std::string encodedPath(const std::string& href) {
    std::string h = substringBefore(substringBefore(href, "#"), "?");
    if (h.find("://") != std::string::npos) return http::pathOf(h);
    return h;
}

/** "a" o "b" di un JSON di stato: {"result": "..."} */
std::string resultHtml(const std::string& body) {
    json j = json::parse(body);
    auto it = j.find("result");
    if (it == j.end() || !it->is_string()) throw http::Error("Risposta del server non valida");
    return it->get<std::string>();
}

/** Figli diretti con un certo tag. */
std::vector<html::Node> childrenByTag(const html::Node& n, const std::string& tag) {
    std::vector<html::Node> out;
    for (auto& c : n.children())
        if (c.tag() == tag) out.push_back(c);
    return out;
}

/** Emula Jsoup "div:contains(label) > span > a" (o "> span" se leafTag e' vuoto). */
std::vector<html::Node> containsSelect(const html::Document& doc, const std::string& label, const std::string& leafTag) {
    std::vector<html::Node> out;
    std::set<void*> seen;
    for (auto& div : doc.select("div")) {
        auto spans = childrenByTag(div, "span");
        if (spans.empty()) continue;
        if (!icontains(div.text(), label)) continue;
        for (auto& sp : spans) {
            if (leafTag.empty()) {
                if (seen.insert((void*)sp.raw()).second) out.push_back(sp);
            } else {
                for (auto& a : childrenByTag(sp, leafTag))
                    if (seen.insert((void*)a.raw()).second) out.push_back(a);
            }
        }
    }
    return out;
}

std::string joinText(const std::vector<html::Node>& nodes, const std::string& sep = ", ") {
    std::string out;
    for (auto& n : nodes) {
        std::string t = n.text();
        if (t.empty()) continue;
        if (!out.empty()) out += sep;
        out += t;
    }
    return out;
}

std::string fancyScore(const std::string& score) {
    double v = parseNumber(score, 0);
    if (v <= 0) return "";
    int stars = (int)(v / 2.0 + 0.5);
    stars = std::max(0, std::min(5, stars));
    std::string s;
    for (int i = 0; i < stars; i++) s += "\xE2\x98\x85";      // ★
    for (int i = stars; i < 5; i++) s += "\xE2\x98\x86";      // ☆
    std::string num = trim(score);
    if (num.find('.') != std::string::npos) {
        while (!num.empty() && num.back() == '0') num.pop_back();
        if (!num.empty() && num.back() == '.') num.pop_back();
    }
    return s + " " + num;
}

std::string mapStatus(const std::string& raw) {
    std::string s = lower(trim(raw));
    if (s == "ongoing" || s == "ongoing anime" || s == "currently airing") return "In corso";
    if (s == "finished airing" || s == "completed") return "Completato";
    return trim(raw);
}

// ================================ VRF =================================

std::string vrfExchange(const std::string& in, const std::string& k1, const std::string& k2) {
    std::string out = in;
    for (auto& c : out) {
        auto idx = k1.find(c);
        if (idx != std::string::npos) c = k2[idx];
    }
    return out;
}

std::string vrfRc4(const std::string& key, const std::string& in) {
    return crypto::base64Encode(crypto::rc4(key, in), true, true);
}

/** AnikotoUtils.vrfEncrypt: gia' passato per URLEncoder (solo '=' diventa %3D). */
std::string vrfEncrypt(const std::string& input) {
    std::string v = input;
    v = vrfExchange(v, "AP6GeR8H0lwUz1", "UAz8Gwl10P6ReH");
    v = vrfRc4("ItFKjuWokn4ZpB", v);
    v = vrfRc4("fOyt97QWFB3", v);
    v = vrfExchange(v, "1majSlPQd2M5", "da1l2jSmP5QM");
    v = vrfExchange(v, "CPYvHj09Au3", "0jHA9CPYu3v");
    std::reverse(v.begin(), v.end());
    v = vrfRc4("736y1uTJpBLUX", v);
    v = crypto::base64Encode(v, true, true);
    return http::urlEncode(v);
}

// ================================ HLS =================================

std::string hlsAttr(const std::string& line, const std::string& key) {
    auto p = line.find(key + "=");
    while (p != std::string::npos && p > 0 && line[p - 1] != ',' && line[p - 1] != ':') p = line.find(key + "=", p + 1);
    if (p == std::string::npos) return "";
    p += key.size() + 1;
    if (p < line.size() && line[p] == '"') {
        auto e = line.find('"', p + 1);
        return line.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1);
    }
    auto e = line.find(',', p);
    return line.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

/**
 * Equivalente di PlaylistUtils.extractFromHls: una voce per ogni variante della master playlist.
 * Se la master non si scarica restituisce comunque la master (mpv sceglie la qualita').
 */
std::vector<Video> extractFromHls(const std::string& masterUrl, const std::string& namePrefix, const std::string& referer,
                                  const http::Headers& extraHeaders, const std::vector<Video::Track>& subs) {
    auto make = [&](const std::string& url, const std::string& q, int height) {
        Video v;
        v.url = url;
        v.title = namePrefix + " - " + q;
        v.quality = height;
        v.referer = referer;
        v.headers = extraHeaders;
        v.subtitles = subs;
        return v;
    };

    std::vector<Video> out;
    std::string body;
    try {
        http::Headers h = extraHeaders;
        h.push_back({"Referer", referer});
        http::Response r = http::request("GET", masterUrl, h);
        if (r.status >= 200 && r.status < 300) body = r.body;
    } catch (const std::exception&) {
    }
    if (body.find("#EXT-X-STREAM-INF") == std::string::npos) {
        out.push_back(make(masterUrl, "Auto", 0));
        return out;
    }

    std::vector<Video::Track> audio;
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < body.size()) {
        auto nl = body.find('\n', pos);
        lines.push_back(trim(body.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos)));
        pos = nl == std::string::npos ? body.size() : nl + 1;
    }
    for (auto& l : lines) {
        if (startsWith(l, "#EXT-X-MEDIA:") && hlsAttr(l, "TYPE") == "AUDIO") {
            std::string uri = hlsAttr(l, "URI");
            if (uri.empty()) continue;
            std::string name = hlsAttr(l, "NAME");
            if (name.empty()) name = hlsAttr(l, "LANGUAGE");
            audio.push_back({http::resolve(masterUrl, uri), name});
        }
    }
    std::set<std::string> seen;
    for (size_t i = 0; i < lines.size(); i++) {
        if (!startsWith(lines[i], "#EXT-X-STREAM-INF")) continue;
        std::string res = hlsAttr(lines[i], "RESOLUTION");
        std::string uri;
        for (size_t k = i + 1; k < lines.size(); k++) {
            if (lines[k].empty() || lines[k][0] == '#') continue;
            uri = lines[k];
            break;
        }
        if (uri.empty()) continue;
        std::string url = http::resolve(masterUrl, uri);
        if (!seen.insert(url).second) continue;
        int height = std::atoi(substringAfter(res, "x").c_str());
        std::string q = height > 0 ? std::to_string(height) + "p" : "Video";
        Video v = make(url, q, height);
        v.audio = audio;
        out.push_back(v);
    }
    if (out.empty()) out.push_back(make(masterUrl, "Auto", 0));
    return out;
}

// ============================== Tema ==================================

enum class Flavor { Default, Anichi, Sogo };

struct SiteConfig {
    std::string id;
    std::string name;
    std::string baseUrl;
    Flavor flavor;
};

struct VideoData {
    std::string type;
    std::string serverId;
    std::string serverName;
};

const char* MAPPER_URL = "https://mapper.nekostream.site/api";
const char* PREF_SERVER = "HD-1";  // hosterNames.first()
const char* MEGAPLAY_AES_KEY = "i?LMTAx0Q6,:}50U";
const char* MEGAPLAY_AES_IV = "W0;27ToaUpl_P%'c";
const char* MEGAPLAY_TOKEN_SECRET = "MpCdnT0k3n!9f2K#xQ7vL5mR8wN1pY4s";

class AnikotoTheme : public Source {
  public:
    explicit AnikotoTheme(SiteConfig c) : cfg(std::move(c)) {}

    std::string id() const override { return cfg.id; }
    std::string name() const override { return cfg.name; }
    std::string defaultBaseUrl() const override { return cfg.baseUrl; }
    std::string lang() const override { return "en"; }

    Page popular(int page) override { return listing(baseUrl() + "/most-viewed/?page=" + std::to_string(page)); }

    Page latest(int page) override { return listing(baseUrl() + "/latest-updated/?page=" + std::to_string(page)); }

    Page search(const std::string& query, int page) override {
        // come okhttp addQueryParameter: il vrf (gia' codificato) viene codificato di nuovo
        std::string vrf = query.empty() ? "" : vrfEncrypt(query);
        std::string url = baseUrl() + "/filter?keyword=" + http::urlEncode(query) + "&page=" + std::to_string(page) +
                          "&vrf=" + http::urlEncode(vrf);
        return listing(url);
    }

    Details details(const std::string& animeUrl) override {
        std::string path = substringBefore(animeUrl, "#");
        std::string url = baseUrl() + path;
        http::Response r = http::request("GET", url, docHeaders());
        if (r.status >= 400) throw http::Error(cfg.name + ": HTTP " + std::to_string(r.status));
        std::string finalUrl = r.finalUrl.empty() ? url : r.finalUrl;
        std::string body = resolveSearchAnime(r.body, finalUrl);
        html::Document doc(body, finalUrl);

        Details d;
        if (cfg.flavor == Flavor::Anichi)
            parseAnichiDetails(doc, d);
        else
            parseDefaultDetails(doc, d);

        std::string animeId = animeUrl.find('#') != std::string::npos ? substringAfter(animeUrl, "#") : "";
        if (animeId.empty()) animeId = doc.selectFirst("[data-id]").attr("data-id");
        if (animeId.empty() && cfg.flavor != Flavor::Anichi) animeId = doc.selectFirst("[data-tip]").attr("data-tip");
        if (animeId.empty()) throw http::Error(cfg.name + ": ID dell'anime non trovato");

        d.episodes = episodeList(animeId, path);
        return d;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::string ids = substringBefore(episodeUrl, "&");
        std::string epUrl = substringBefore(substringAfter(episodeUrl, "epurl="), "&");

        http::Headers h = ajaxHeaders(baseUrl() + epUrl);
        std::string body = http::getText(baseUrl() + "/ajax/server/list?servers=" + ids, h);
        std::vector<VideoData> servers;
        try {
            html::Document doc(resultHtml(body));
            servers = parseServerListData(doc);
        } catch (const std::exception&) {
        }
        auto mapper = fetchMapperServers(episodeUrl);
        servers.insert(servers.end(), mapper.begin(), mapper.end());
        if (servers.empty()) throw http::Error("Nessun server trovato");

        struct Ranked {
            Video v;
            int typeRank;
            int serverRank;
        };
        std::vector<Ranked> all;
        for (auto& s : servers) {
            std::vector<Video> vs;
            try {
                vs = extractVideo(s, epUrl);
            } catch (const std::exception&) {
                continue;
            }
            int typeRank = (iequals(s.type, "Sub") || iequals(s.type, "H-Sub") || iequals(s.type, "HSub")) ? 1 : 0;
            int serverRank = iequals(s.serverName, PREF_SERVER)                       ? 2
                             : iequals(extractBaseServerName(s.serverName), PREF_SERVER) ? 1
                                                                                        : 0;
            for (auto& v : vs) all.push_back({v, typeRank, serverRank});
        }
        if (all.empty()) throw http::Error("Nessun video trovato");

        std::stable_sort(all.begin(), all.end(), [](const Ranked& a, const Ranked& b) {
            if (a.typeRank != b.typeRank) return a.typeRank > b.typeRank;
            if (a.serverRank != b.serverRank) return a.serverRank > b.serverRank;
            auto qa = a.v.quality == 1080 ? 1 : 0, qb = b.v.quality == 1080 ? 1 : 0;
            if (qa != qb) return qa > qb;
            return a.v.quality > b.v.quality;
        });
        std::vector<Video> out;
        std::set<std::string> seen;
        for (auto& r : all)
            if (seen.insert(r.v.title + "|" + r.v.url).second) out.push_back(r.v);
        return out;
    }

  private:
    SiteConfig cfg;

    // ------------------------------------------------------- selettori
    std::string listingThumbnailSelector() const {
        switch (cfg.flavor) {
            case Flavor::Anichi: return "div.ani.poster img";
            case Flavor::Sogo: return "a.poster img";
            default: return "div.poster img";
        }
    }
    std::string detailThumbnailSelector() const {
        switch (cfg.flavor) {
            case Flavor::Anichi: return ".series-intro__poster img";
            case Flavor::Sogo: return "section#w-info div.poster img";
            default: return "";
        }
    }
    std::string metaContainerSelector() const { return cfg.flavor == Flavor::Sogo ? "div.bl-meta" : "div.bmeta"; }
    std::string scoreLabelName() const { return cfg.flavor == Flavor::Sogo ? "Scores" : "MAL"; }
    std::string aliasContainerSelector() const {
        return cfg.flavor == Flavor::Sogo ? "div.alias" : "div.names.font-italic";
    }
    std::vector<std::string> metaExclusionLabels() const {
        if (cfg.flavor == Flavor::Sogo) return {"Genres", "Status", "Studios", "Producers", "Scores"};
        return {"Genres", "Status", "Studios", "Producers", "MAL"};
    }
    std::string synopsisContentSelector() const {
        return cfg.flavor == Flavor::Sogo ? "div.synopsis > div.content" : "div.synopsis > div.shorting > div.content";
    }
    std::string episodeListSelector() const {
        return cfg.flavor == Flavor::Sogo ? "ul.episodes > li > a" : "div.episodes ul > li > a";
    }
    bool useEpisodeTitles() const { return cfg.flavor != Flavor::Sogo; }

    // ------------------------------------------------------- intestazioni
    http::Headers docHeaders() const { return {{"Referer", baseUrl() + "/"}}; }

    http::Headers ajaxHeaders(const std::string& referer) const {
        return {
            {"Accept", "application/json, text/javascript, */*; q=0.01"},
            {"Referer", referer},
            {"X-Requested-With", "XMLHttpRequest"},
        };
    }

    // ------------------------------------------------------- elenchi
    static std::string getTitle(const html::Node& el) {
        std::string t = el.text();
        if (!t.empty()) return t;
        return trim(el.attr("data-jp"));
    }

    static std::string imgUrl(const html::Document& doc, const html::Node& img) {
        if (!img) return "";
        std::string u = img.attr("data-src");
        if (u.empty()) u = img.attr("src");
        if (u.empty()) return "";
        return http::resolve(doc.url(), u);
    }

    Page listing(const std::string& url) {
        http::Response r = http::request("GET", url, docHeaders());
        if (r.status >= 400) throw http::Error(cfg.name + ": HTTP " + std::to_string(r.status));
        html::Document doc(r.body, r.finalUrl.empty() ? url : r.finalUrl);
        Page p;
        for (auto& el : doc.select("div.ani.items > div.item")) {
            html::Node a = el.selectFirst("a.name");
            if (!a) continue;
            Anime an;
            an.url = stripEpSuffix(encodedPath(a.attr("href")));
            an.title = getTitle(a);
            an.thumbnail = imgUrl(doc, el.selectFirst(listingThumbnailSelector()));
            if (!an.url.empty()) p.animes.push_back(an);
        }
        // "nav > ul.pagination > li.active ~ li"
        for (auto& ul : doc.select("nav > ul.pagination")) {
            bool afterActive = false;
            for (auto& li : childrenByTag(ul, "li")) {
                if (afterActive) {
                    p.hasNextPage = true;
                    break;
                }
                if (hasClass(li, "active")) afterActive = true;
            }
            if (p.hasNextPage) break;
        }
        return p;
    }

    /** Se la pagina e' una ricerca (/filter?keyword=) apre il primo risultato. */
    std::string resolveSearchAnime(const std::string& body, std::string& location) {
        if (!startsWith(location, baseUrl() + "/filter?keyword=")) return body;
        html::Document doc(body, location);
        html::Node item = doc.selectFirst("div.ani.items > div.item");
        std::string href = item.selectFirst("a[href]").attr("href");
        if (href.empty()) throw http::Error(cfg.name + ": anime non trovato");
        std::string url = baseUrl() + stripEpSuffix(encodedPath(href));
        http::Response r = http::request("GET", url, docHeaders());
        if (r.status >= 400) throw http::Error(cfg.name + ": HTTP " + std::to_string(r.status));
        location = r.finalUrl.empty() ? url : r.finalUrl;
        return r.body;
    }

    // ------------------------------------------------------- dettagli
    void parseDefaultDetails(const html::Document& doc, Details& d) {
        html::Node titleEl = doc.selectFirst("h1.title, h2.title");
        if (titleEl) d.title = getTitle(titleEl);
        d.genre = joinText(containsSelect(doc, "Genres", "a"));
        d.author = joinText(containsSelect(doc, "Studios", "a"));
        d.status = mapStatus(joinText(containsSelect(doc, "Status", ""), " "));
        std::string thumbSel = detailThumbnailSelector();
        if (!thumbSel.empty()) d.thumbnail = imgUrl(doc, doc.selectFirst(thumbSel));

        // descrizione (buildDescription)
        std::string enTitle = titleEl.text();
        std::string jpTitle = trim(titleEl.attr("data-jp"));
        auto metaDivs = doc.select(metaContainerSelector() + " div.meta > div");
        auto ownLabel = [](const html::Node& div) {
            std::string full = div.text();
            std::string span = div.selectFirst("span").text();
            std::string label = span.empty() ? full : substringBefore(full, span);
            label = trim(label);
            if (endsWith(label, ":")) label.pop_back();
            return trim(label);
        };
        std::string score;
        for (auto& div : metaDivs)
            if (iequals(ownLabel(div), scoreLabelName())) {
                score = html::textOf(div.select("span"));
                break;
            }

        std::string desc;
        std::string fs = fancyScore(score);
        if (!fs.empty()) desc += fs + "\n\n";
        std::string synopsis = doc.selectFirst(synopsisContentSelector()).text();
        if (!synopsis.empty()) desc += synopsis + "\n\n";

        auto excl = metaExclusionLabels();
        std::string meta;
        for (auto& div : metaDivs) {
            std::string label = ownLabel(div);
            std::string value = html::textOf(div.select("span"));
            if (iequals(label, "Duration")) {
                std::string dg = digitsOnly(value);
                if (!dg.empty()) value = dg + " min";
            }
            if (label.empty() || value.empty() || std::find(excl.begin(), excl.end(), label) != excl.end()) continue;
            if (!meta.empty()) meta += " | ";
            meta += label + ": " + value;
        }
        if (!meta.empty()) desc += meta + "\n\n";

        std::string producers = joinText(containsSelect(doc, "Producers", "a"));
        desc += studioLine(d.author, producers);

        std::vector<std::string> alt;
        if (!jpTitle.empty()) alt.push_back(jpTitle);
        std::string names = doc.selectFirst(aliasContainerSelector()).text();
        if (!names.empty()) {
            size_t p = 0;
            while (p <= names.size()) {
                auto e = names.find(';', p);
                std::string n = trim(names.substr(p, e == std::string::npos ? std::string::npos : e - p));
                if (!n.empty() && n != jpTitle && n != enTitle) alt.push_back(n);
                if (e == std::string::npos) break;
                p = e + 1;
            }
        }
        desc += altLine(alt);
        d.description = trim(desc);
    }

    void parseAnichiDetails(const html::Document& doc, Details& d) {
        html::Node titleEl = doc.selectFirst("h1.series-title");
        if (!titleEl) titleEl = doc.selectFirst("h1");
        d.title = getTitle(titleEl);
        d.genre = joinText(doc.select(".series-genres .series-genre"));

        std::string studios, producers, status;
        for (auto& f : doc.select(".series-fact")) {
            std::string t = f.text();
            if (icontains(t, "Studios") && studios.empty()) studios = joinText(f.select(".series-fact__value a"));
            if (icontains(t, "Producers") && producers.empty())
                producers = joinText(f.select(".series-fact__value a"));
            if (icontains(t, "Status") && status.empty()) status = html::textOf(f.select(".series-fact__value"));
        }
        d.author = studios;
        d.status = mapStatus(status);
        d.thumbnail = imgUrl(doc, doc.selectFirst(detailThumbnailSelector()));

        std::string enTitle = titleEl.text();
        std::string jpTitle = trim(titleEl.attr("data-jp"));
        std::string desc;
        std::string fs = fancyScore(html::textOf(doc.select(".series-intro__poster .series-score b")));
        if (!fs.empty()) desc += fs + "\n\n";
        html::Node syn = doc.selectFirst(".series-blurb__full");
        if (!syn) syn = doc.selectFirst(".series-blurb__short");
        if (syn) desc += syn.text() + "\n\n";

        auto excl = metaExclusionLabels();
        std::string meta;
        for (auto& div : doc.select(".series-facts__grid .series-fact")) {
            std::string label = div.selectFirst(".series-fact__label").text();
            if (endsWith(label, ":")) label.pop_back();
            std::string value = div.selectFirst(".series-fact__value").text();
            if (label.empty() || value.empty() || std::find(excl.begin(), excl.end(), label) != excl.end()) continue;
            if (!meta.empty()) meta += " | ";
            meta += label + ": " + value;
        }
        if (!meta.empty()) desc += meta + "\n\n";
        desc += studioLine(studios, producers);

        std::vector<std::string> alt;
        if (!jpTitle.empty()) alt.push_back(jpTitle);
        std::string native = doc.selectFirst(".series-native").text();
        if (!native.empty() && native != jpTitle && native != enTitle) alt.push_back(native);
        desc += altLine(alt);
        d.description = trim(desc);
    }

    static std::string studioLine(const std::string& studios, const std::string& producers) {
        if (!studios.empty() && !producers.empty()) return "Studio: " + studios + " (Producers: " + producers + ")\n\n";
        if (!studios.empty()) return "Studio: " + studios + "\n\n";
        if (!producers.empty()) return "Producers: " + producers + "\n\n";
        return "";
    }

    static std::string altLine(const std::vector<std::string>& alt) {
        if (alt.empty()) return "";
        std::string s = "Other name(s): ";
        for (size_t i = 0; i < alt.size(); i++) s += (i ? ", " : "") + alt[i];
        return s + "\n\n";
    }

    // ------------------------------------------------------- episodi
    std::vector<Episode> episodeList(const std::string& animeId, const std::string& animePath) {
        std::string body = http::getText(baseUrl() + "/ajax/episode/list/" + animeId + "?vrf=" + vrfEncrypt(animeId),
                                         ajaxHeaders(baseUrl() + animePath));
        html::Document doc(resultHtml(body));
        std::string base = stripEpSuffix(animePath);
        std::vector<Episode> eps;
        for (auto& el : doc.select(episodeListSelector())) {
            html::Node parent = el.parent();
            std::string tooltip = parent.attr("title");
            std::string epNum = el.attr("data-num");
            std::string ids = el.attr("data-ids");
            std::string name = html::textOf(parent.select("span.d-title"));
            if (useEpisodeTitles() && name.empty() && !tooltip.empty())
                name = trim(substringBefore(substringBefore(tooltip, "Release:"), "Softsub"));
            std::string malId = el.attr("data-mal");
            std::string slug = el.attr("data-slug");
            std::string ts = digitsOnly(el.attr("data-timestamp"));

            Episode e;
            std::string epTitle = "Episode " + epNum;
            e.name = epTitle + (!name.empty() && name != epTitle ? ": " + name : "") +
                     (hasClass(el, "filler") ? " [Filler]" : "");
            e.url = ids + "&epurl=" + base + "/ep-" + epNum;
            if (!malId.empty()) e.url += "&mal=" + malId;
            if (!slug.empty()) e.url += "&slug=" + slug;
            if (!ts.empty()) e.url += "&ts=" + ts;
            e.number = parseNumber(epNum, 0);
            eps.push_back(e);
        }
        std::reverse(eps.begin(), eps.end());
        return eps;
    }

    // ------------------------------------------------------- server
    std::string extractBaseServerName(const std::string& raw) const {
        std::string s = raw;
        // Regex("-\\*\\d+\\s*$")
        std::string t = trimEndChars(s, " \t\r\n");
        size_t p = t.size();
        while (p > 0 && std::isdigit((unsigned char)t[p - 1])) p--;
        if (p < t.size() && p >= 2 && t[p - 1] == '*' && t[p - 2] == '-') s = t.substr(0, p - 2);
        std::string base = trim(trimEndChars(s, "- "));
        if (cfg.flavor == Flavor::Sogo && base == "Server") return "Kiwi-Stream";
        return base;
    }

    std::string getServerDisplayName(const std::string& serverName) const {
        if (cfg.flavor == Flavor::Sogo && startsWith(lower(serverName), "server")) {
            std::string suffix = trim(serverName.substr(6));
            return "Kiwi-Stream" + (suffix.empty() ? "" : " " + suffix);
        }
        return trimEndChars(serverName, "- ");
    }

    static std::string resolveTypeLabel(const html::Node& typeElem) {
        std::string labelText = typeElem.selectFirst("label").text();
        std::string dataType = typeElem.attr("data-type");
        std::string l = lower(labelText);
        if (l == "sub") return "Sub";
        if (l == "h-sub") return "H-Sub";
        if (l == "hsub") return "HSub";
        if (l == "dub") return "Dub";
        if (l == "a-dub" || l == "adub") return "A-Dub";
        if (l == "s-sub") return "S-Sub";
        std::string dt = lower(dataType);
        if (dt == "sub") return "Sub";
        if (dt == "hsub") return "HSub";
        if (dt == "dub") return "Dub";
        if (dt == "adub") return "A-Dub";
        auto cap = [](std::string s) {
            if (!s.empty()) s[0] = (char)std::toupper((unsigned char)s[0]);
            return s;
        };
        if (dt.empty()) return labelText.empty() ? "Unknown" : cap(labelText);
        return cap(dataType);
    }

    std::vector<VideoData> parseServerListData(const html::Document& doc) const {
        std::vector<VideoData> out;
        if (cfg.flavor == Flavor::Sogo) {
            for (auto& elem : doc.select("div.type")) {
                std::string label = resolveTypeLabel(elem);
                for (auto& s : elem.select("a.server")) {
                    std::string id = s.attr("data-link-id");
                    if (trim(id).empty()) continue;
                    std::string raw = trim(s.selectFirst("span").text());
                    if (raw.empty()) continue;
                    out.push_back({label, id, getServerDisplayName(raw)});
                }
            }
            return out;
        }
        for (auto& elem : doc.select("div.servers > div.type")) {
            std::string label = resolveTypeLabel(elem);
            for (auto& li : elem.select("li")) {
                if (hasClass(li, "download-icon")) continue;
                std::string id = li.attr("data-link-id");
                if (id.empty()) continue;
                out.push_back({label, id, li.text()});
            }
        }
        return out;
    }

    static std::string mapMapperServerName(const std::string& key) {
        if (iequals(key, "gogoanime")) return "Vidstream";
        if (iequals(key, "anivibe")) return "Vibe-Stream";
        if (iequals(key, "animepahe")) return "Kiwi-Stream";
        if (startsWith(lower(key), "kiwi-stream")) return "Kiwi-Stream";
        std::string s = key;
        if (!s.empty()) s[0] = (char)std::toupper((unsigned char)s[0]);
        return s;
    }

    static std::string urlParam(const std::string& epUrl, const std::string& key) {
        auto p = epUrl.find("&" + key + "=");
        if (p == std::string::npos) return "";
        return substringBefore(epUrl.substr(p + key.size() + 2), "&");
    }

    std::vector<VideoData> fetchMapperServers(const std::string& epUrl) const {
        std::string malId = urlParam(epUrl, "mal"), slug = urlParam(epUrl, "slug"), ts = urlParam(epUrl, "ts");
        if (malId.empty() || slug.empty() || ts.empty()) return {};
        std::vector<VideoData> out;
        try {
            http::Headers h = {
                {"Accept", "application/json, text/javascript, */*; q=0.01"},
                {"Referer", baseUrl() + "/"},
                {"Origin", baseUrl()},
            };
            json j = json::parse(http::getText(std::string(MAPPER_URL) + "/mal/" + malId + "/" + slug + "/" + ts, h));
            if (!j.is_object()) return {};
            for (auto it = j.begin(); it != j.end(); ++it) {
                if (iequals(it.key(), "status") || !it.value().is_object()) continue;
                std::string serverName = mapMapperServerName(it.key());
                for (auto& tk : {std::make_pair("sub", "H-Sub"), std::make_pair("dub", "A-Dub")}) {
                    auto link = it.value().find(tk.first);
                    if (link == it.value().end() || !link->is_object()) continue;
                    std::string linkId = trim(jstr(*link, "url"));
                    if (linkId.empty()) continue;
                    out.push_back({tk.second, linkId, serverName});
                }
            }
        } catch (const std::exception&) {
            return {};
        }
        return out;
    }

    std::string getEmbedLink(const std::string& serverId, const std::string& epUrl) const {
        std::string body = http::getText(baseUrl() + "/ajax/server?get=" + serverId, ajaxHeaders(baseUrl() + epUrl));
        json j = json::parse(body);
        std::string url = j.contains("result") ? jstr(j["result"], "url") : "";
        if (url.empty()) throw http::Error("Link del server non trovato");
        return url;
    }

    std::string namePrefix(const VideoData& s) const {
        std::string n = getServerDisplayName(s.serverName);
        if (!s.type.empty()) n += " - " + s.type;
        return n;
    }

    std::vector<Video> extractVideo(const VideoData& s, const std::string& epUrl) const {
        std::string embed = startsWith(s.serverId, "http") ? s.serverId : getEmbedLink(s.serverId, epUrl);
        if (isMegaPlayServer(s.serverName) || isMegaPlayUrl(embed)) return extractFromMegaPlay(embed, s);
        if (embed.find("mewcdn.online/player/plyr.php") != std::string::npos) return extractFromMewcdn(embed, s);
        if (endsWith(embed, ".m3u8") ||
            (embed.find(".m3u8") != std::string::npos && embed.find("/stream/") == std::string::npos))
            return extractFromHls(embed, namePrefix(s), baseUrl() + "/", {}, {});
        return {};
    }

    // ------------------------------------------------------- MegaPlay
    static bool isMegaPlayServer(const std::string& serverName) {
        std::string n;
        for (char c : lower(serverName))
            if (c != ' ' && c != '-') n += c;
        return n.find("vidstream") != std::string::npos || n.find("hd1") != std::string::npos ||
               n.find("hd2") != std::string::npos;
    }

    static bool isMegaPlayUrl(const std::string& url) {
        // Regex("megaplay\\.[^/]+/stream/", IGNORE_CASE)
        std::string u = lower(url);
        size_t p = 0;
        while ((p = u.find("megaplay.", p)) != std::string::npos) {
            size_t q = p + 9;
            size_t slash = u.find('/', q);
            if (slash != std::string::npos && slash > q && u.compare(slash, 8, "/stream/") == 0) return true;
            p = q;
        }
        return false;
    }

    static std::string parseMegaPlayMediaId(const std::string& html) {
        // data-id=["']([^"']+)["']
        size_t p = 0;
        while ((p = html.find("data-id=", p)) != std::string::npos) {
            p += 8;
            if (p < html.size() && (html[p] == '"' || html[p] == '\'')) {
                size_t s = p + 1, e = s;
                while (e < html.size() && html[e] != '"' && html[e] != '\'') e++;
                if (e < html.size() && e > s) {
                    std::string id = trim(html.substr(s, e - s));
                    if (!id.empty()) return id;
                }
            }
        }
        // File\s+(\d+)
        std::string low = lower(html);
        p = 0;
        while ((p = low.find("file", p)) != std::string::npos) {
            size_t q = p + 4, ws = q;
            while (q < html.size() && std::isspace((unsigned char)html[q])) q++;
            size_t d = q;
            while (d < html.size() && std::isdigit((unsigned char)html[d])) d++;
            if (q > ws && d > q) return html.substr(q, d - q);
            p += 4;
        }
        return "";
    }

    static std::string sourcesField(const json& j) {
        auto it = j.find("sources");
        if (it == j.end() || it->is_null()) return "";
        const json& e = *it;
        if (e.is_object()) return jstr(e, "file");
        if (e.is_array()) {
            if (e.empty()) return "";
            if (e[0].is_object()) return jstr(e[0], "file");
            if (e[0].is_string()) return e[0].get<std::string>();
            return "";
        }
        if (e.is_string()) return e.get<std::string>();
        return "";
    }

    static std::string processMegaPlaySource(const std::string& enc, const std::string& source) {
        std::string m3u8;
        bool decrypted = false;
        if (!enc.empty()) {
            try {
                std::string key(MEGAPLAY_AES_KEY);
                key.resize(32, '\0');
                std::string data = base64Decode(enc);
                if (!data.empty() && data.size() % 16 == 0) {
                    std::string js = crypto::aesCbcDecrypt(data, key, MEGAPLAY_AES_IV);
                    // "file"\s*:\s*"([^"]+)"
                    auto p = js.find("\"file\"");
                    if (p != std::string::npos) {
                        p += 6;
                        while (p < js.size() && std::isspace((unsigned char)js[p])) p++;
                        if (p < js.size() && js[p] == ':') {
                            p++;
                            while (p < js.size() && std::isspace((unsigned char)js[p])) p++;
                            if (p < js.size() && js[p] == '"') {
                                auto e = js.find('"', p + 1);
                                if (e != std::string::npos && e > p + 1) {
                                    m3u8 = replaceAll(js.substr(p + 1, e - p - 1), "\\/", "/");
                                    decrypted = true;
                                }
                            }
                        }
                    }
                }
            } catch (const std::exception&) {
            }
        }
        if (m3u8.empty()) m3u8 = source;
        if (m3u8.empty()) return "";
        std::string lm = lower(m3u8);
        if (!decrypted || lm.find("?token=") != std::string::npos || lm.find("&token=") != std::string::npos)
            return m3u8;

        // /([a-f0-9]{32})/([a-f0-9]{32})/
        auto isHex32 = [&](size_t pos) {
            if (pos + 32 > lm.size()) return false;
            for (size_t i = pos; i < pos + 32; i++)
                if (!std::isxdigit((unsigned char)lm[i])) return false;
            return true;
        };
        std::string pathKey;
        for (size_t i = 0; i + 67 <= lm.size(); i++) {
            if (lm[i] == '/' && isHex32(i + 1) && lm[i + 33] == '/' && isHex32(i + 34) && lm[i + 66] == '/') {
                pathKey = lm.substr(i + 1, 32) + "/" + lm.substr(i + 34, 32);
                break;
            }
        }
        if (pathKey.empty()) return m3u8;

        long long expiry = (long long)std::time(nullptr) + 90;
        std::string payload = std::to_string(expiry) + "|" + pathKey;
        std::string sig = crypto::base64Encode(crypto::hmacSha256(MEGAPLAY_TOKEN_SECRET, payload), true, false);
        std::string token = crypto::base64Encode(payload, true, false) + "." + sig;
        std::string frag;
        std::string url = m3u8;
        if (url.find('#') != std::string::npos) {
            frag = url.substr(url.find('#'));
            url = url.substr(0, url.find('#'));
        }
        url += (url.find('?') != std::string::npos ? "&" : "?");
        return url + "token=" + http::urlEncode(token) + frag;
    }

    std::vector<Video> extractFromMegaPlay(const std::string& embedUrl, const VideoData& s) const {
        http::Headers ph = {
            {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"},
            {"X-Requested-With", "XMLHttpRequest"},
            {"Referer", baseUrl() + "/"},
        };
        std::string page = http::getText(embedUrl, ph);
        std::string mediaId = parseMegaPlayMediaId(page);
        if (mediaId.empty()) throw http::Error("MegaPlay: ID non trovato");

        std::string origin = http::originOf(embedUrl);
        std::string host = http::hostOf(embedUrl);
        if (origin.empty() || host.empty()) {
            origin = "https://megaplay.buzz";
            host = "megaplay.buzz";
        }
        std::string api = origin + "/stream/getSources?id=" + http::urlEncode(mediaId);
        std::string sParam = http::queryParam(embedUrl, "s");
        if (!sParam.empty()) api += "&s=" + http::urlEncode(sParam);

        http::Headers ah = {
            {"Accept", "application/json,*/*"},
            {"X-Requested-With", "XMLHttpRequest"},
            {"Referer", embedUrl},
        };
        json j = json::parse(http::getText(api, ah));
        std::string m3u8 = processMegaPlaySource(jstr(j, "enc"), sourcesField(j));
        if (m3u8.empty()) throw http::Error("MegaPlay: sorgente non trovata");

        std::vector<Video::Track> subs;
        auto tr = j.find("tracks");
        if (tr != j.end() && tr->is_array()) {
            for (auto& t : *tr) {
                std::string label = trim(jstr(t, "label"));
                std::string file = jstr(t, "file");
                if (label.empty() || file.empty()) continue;
                subs.push_back({file, label});
            }
        }
        std::string referer = "https://" + host + "/";
        http::Headers vh = {{"Origin", "https://" + host}};
        return extractFromHls(m3u8, namePrefix(s), referer, vh, subs);
    }

    // ------------------------------------------------------- mewcdn
    std::vector<Video> extractFromMewcdn(const std::string& serverUrl, const VideoData& s) const {
        std::string fragment = substringBefore(substringAfter(serverUrl, "#"), "#");
        if (fragment.empty() || serverUrl.find('#') == std::string::npos) throw http::Error("mewcdn: frammento assente");
        std::string raw = trim(base64Decode(fragment));
        if (!startsWith(raw, "http")) throw http::Error("mewcdn: URL non valido");

        std::string page;
        try {
            page = http::get(serverUrl, {{"Referer", baseUrl() + "/"}}).body;
        } catch (const std::exception&) {
        }
        // var HOST_MAP = { 'origine': 'proxy', ... }
        std::string m3u8 = raw;
        auto p = page.find("var HOST_MAP");
        if (p != std::string::npos) {
            auto ob = page.find('{', p);
            auto cb = ob == std::string::npos ? std::string::npos : page.find('}', ob);
            if (cb != std::string::npos) {
                std::string body = page.substr(ob + 1, cb - ob - 1);
                std::vector<std::string> quoted;
                size_t q = 0;
                while ((q = body.find('\'', q)) != std::string::npos) {
                    auto e = body.find('\'', q + 1);
                    if (e == std::string::npos) break;
                    quoted.push_back(body.substr(q + 1, e - q - 1));
                    q = e + 1;
                }
                for (size_t i = 0; i + 1 < quoted.size(); i += 2) {
                    if (!quoted[i].empty() && m3u8.find(quoted[i]) != std::string::npos) {
                        m3u8 = replaceAll(m3u8, quoted[i], quoted[i + 1]);
                        break;
                    }
                }
            }
        }
        http::Headers vh = {{"Origin", "https://mewcdn.online"}};
        return extractFromHls(m3u8, namePrefix(s), "https://mewcdn.online/", vh, {});
    }
};

}  // namespace

std::vector<std::shared_ptr<Source>> makeAnikotoThemeSources() {
    return {
        std::make_shared<AnikotoTheme>(SiteConfig{"en.anichi", "Anichi", "https://anichi.to", Flavor::Anichi}),
        std::make_shared<AnikotoTheme>(SiteConfig{"en.anikoto", "Anikoto", "https://anikototv.to", Flavor::Default}),
        std::make_shared<AnikotoTheme>(
            SiteConfig{"en.aniwave", "AniWave (Unoriginal)", "https://animewave.to", Flavor::Default}),
        std::make_shared<AnikotoTheme>(SiteConfig{"en.animesogo", "AnimeSogo", "https://animesogo.to", Flavor::Sogo}),
        std::make_shared<AnikotoTheme>(
            SiteConfig{"en.kotokai", "AnimeKai (Unoriginal)", "https://animekaitv.to", Flavor::Default}),
    };
}

}  // namespace src
