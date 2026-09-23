// Porting in C++ dell'estensione Aniyomi "KickAssAnime" (en.kickassanime, v61), estrattore incluso.

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <ctime>

#include "sources/registry.hpp"
#include "util/crypto.hpp"

using json = nlohmann::json;

namespace src {

namespace {

const char* const SEARCH_BASE_URL = "https://kaa.lt";
const char* const PREFIX_SEARCH = "slug:";
const char* const VIDEO_UA =
    "Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/129.0.0.0 Mobile Safari/537.36";

// preferenze predefinite dell'estensione
const char* const PREF_SERVER = "VidStreaming";
const int PREF_QUALITY = 1080;
const char* const PREF_LANG = "ja-JP";
const char* const PREF_LANG_2ND = "en-US";

std::string jstr(const json& j, const char* key) {
    if (!j.is_object()) return "";
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return "";
    if (it->is_string()) return it->get<std::string>();
    return it->dump();
}

bool startsWith(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }

std::string localeName(const std::string& code) {
    static const std::pair<const char*, const char*> LOCALE[] = {
        {"ja-JP", "Japanese"},        {"en-US", "English"},
        {"es-ES", "Spanish (España)"}, {"ko-KR", "Korean"},
        {"zh-CN", "Chinese (Simplified)"},
    };
    for (auto& p : LOCALE)
        if (code == p.first) return p.second;
    return "";
}

http::Headers baseHeaders() { return {{"User-Agent", http::DEFAULT_UA}}; }

json getJson(const std::string& url, const http::Headers& h = baseHeaders()) {
    return json::parse(http::getText(url, h));
}

// ======================= Estrattore (KickAssAnimeExtractor) =======================

http::Headers videoHeaders(const std::string& url) {
    return {
        {"Accept", "*/*"},
        {"Accept-Language", "en-US,en;q=0.9"},
        {"Origin", "https://" + http::hostOf(url)},
        {"Sec-Fetch-Dest", "empty"},
        {"Sec-Fetch-Mode", "cors"},
        {"Sec-Fetch-Site", "same-site"},
    };
}

/** Normalizza gli URL (https:////host, //host, /percorso). */
std::string fixUrl(const std::string& rawUrl, const std::string& baseUrl) {
    std::string t = trim(rawUrl);
    if (startsWith(t, "https://") || startsWith(t, "http://")) {
        size_t colon = t.find(':');
        size_t p = colon + 1;
        while (p < t.size() && t[p] == '/') p++;
        return t.substr(0, colon + 1) + "//" + t.substr(p);
    }
    if (startsWith(t, "//")) return "https://" + t.substr(2);
    if (startsWith(t, "/")) return "https://" + http::hostOf(baseUrl) + t;
    return t;
}

/** Valore di un attributo in una riga #EXT-X-... (es. RESOLUTION=1920x1080 o URI="..."). */
std::string m3uAttr(const std::string& line, const std::string& name) {
    size_t p = 0;
    while ((p = line.find(name + "=", p)) != std::string::npos) {
        if (p == 0 || line[p - 1] == ',' || line[p - 1] == ':') break;
        p += name.size();
    }
    if (p == std::string::npos) return "";
    p += name.size() + 1;
    if (p < line.size() && line[p] == '"') {
        auto e = line.find('"', p + 1);
        return line.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1);
    }
    auto e = line.find(',', p);
    return line.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

/**
 * Versione semplificata di PlaylistUtils.extractFromHls: restituisce le singole qualita'
 * (con le eventuali tracce audio separate) seguite dalla playlist principale.
 */
std::vector<Video> videosFromHls(const std::string& masterUrl, const std::string& name,
                                 const std::vector<Video::Track>& subs, const http::Headers& vh) {
    Video master;
    master.title = name + " - Auto";
    master.url = masterUrl;
    master.userAgent = VIDEO_UA;
    master.headers = vh;
    master.subtitles = subs;

    std::vector<Video> out;
    try {
        http::Headers h = vh;
        h.push_back({"User-Agent", VIDEO_UA});
        std::string pl = http::getText(masterUrl, h);
        if (pl.find("#EXT-X-STREAM-INF") == std::string::npos) return {master};

        std::vector<std::string> lines;
        size_t pos = 0;
        while (pos < pl.size()) {
            auto nl = pl.find('\n', pos);
            lines.push_back(trim(pl.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos)));
            pos = nl == std::string::npos ? pl.size() : nl + 1;
        }

        // tracce audio separate per gruppo
        std::vector<std::pair<std::string, Video::Track>> audio;
        for (auto& l : lines) {
            if (!startsWith(l, "#EXT-X-MEDIA:") || m3uAttr(l, "TYPE") != "AUDIO") continue;
            std::string uri = m3uAttr(l, "URI");
            if (uri.empty()) continue;
            std::string label = m3uAttr(l, "NAME");
            if (label.empty()) label = m3uAttr(l, "LANGUAGE");
            audio.push_back({m3uAttr(l, "GROUP-ID"), {http::resolve(masterUrl, uri), label}});
        }

        for (size_t i = 0; i < lines.size(); i++) {
            if (!startsWith(lines[i], "#EXT-X-STREAM-INF")) continue;
            std::string inf = lines[i];
            std::string uri;
            for (size_t k = i + 1; k < lines.size(); k++) {
                if (lines[k].empty() || lines[k][0] == '#') continue;
                uri = lines[k];
                break;
            }
            if (uri.empty()) continue;
            std::string res = m3uAttr(inf, "RESOLUTION");
            int height = res.find('x') != std::string::npos ? std::atoi(substringAfter(res, "x").c_str()) : 0;
            Video v;
            v.url = http::resolve(masterUrl, uri);
            v.quality = height;
            v.title = name + " - " + (height > 0 ? std::to_string(height) + "p" : std::string("Video"));
            v.userAgent = VIDEO_UA;
            v.headers = vh;
            v.subtitles = subs;
            std::string group = m3uAttr(inf, "AUDIO");
            for (auto& a : audio)
                if (group.empty() || a.first == group) v.audio.push_back(a.second);
            out.push_back(v);
        }
        std::stable_sort(out.begin(), out.end(), [](const Video& a, const Video& b) {
            bool pa = a.quality == PREF_QUALITY, pb = b.quality == PREF_QUALITY;
            if (pa != pb) return pa;
            return a.quality > b.quality;
        });
    } catch (const std::exception&) {
    }
    out.push_back(master);  // riserva: mpv sceglie da solo la qualita'
    return out;
}

std::vector<Video> videosFromManifest(const std::string& manifestUrl, const std::string& name,
                                      const std::vector<Video::Track>& subs, const http::Headers& vh) {
    if (manifestUrl.find(".m3u8") != std::string::npos) return videosFromHls(manifestUrl, name, subs, vh);
    // DASH (.mpd): passato direttamente a mpv
    Video v;
    v.title = name + " - DASH";
    v.url = manifestUrl;
    v.userAgent = VIDEO_UA;
    v.headers = vh;
    v.subtitles = subs;
    return {v};
}

/** Legge un valore "chiave":[n,"valore"] a partire da pos (entro limit). */
bool readTagged(const std::string& s, const std::string& key, size_t from, size_t limit, std::string& value,
                size_t& endPos) {
    std::string marker = "\"" + key + "\":[";
    size_t p = s.find(marker, from);
    if (p == std::string::npos || p >= limit) return false;
    p += marker.size();
    size_t d = p;
    while (d < s.size() && std::isdigit((unsigned char)s[d])) d++;
    if (d == p || s.compare(d, 2, ",\"") != 0) return false;
    d += 2;
    size_t e = s.find('"', d);
    if (e == std::string::npos || e >= limit || e == d || s.compare(e, 2, "\"]") != 0) return false;
    value = s.substr(d, e - d);
    endPos = e + 2;
    return true;
}

/** Nuovo player (dati serializzati in stile Astro: "manifest":[0,"//..."]). */
std::vector<Video> parseNewPlayer(const std::string& cleanHtml, const std::string& url, const std::string& name) {
    http::Headers vh = videoHeaders(url);
    std::string marker = "manifest\":[0,\"";
    size_t p = cleanHtml.find(marker);
    if (p == std::string::npos) return {};
    p += marker.size();
    if (cleanHtml.compare(p, 6, "https:") == 0)
        p += 6;
    else if (cleanHtml.compare(p, 5, "http:") == 0)
        p += 5;
    if (cleanHtml.compare(p, 2, "//") != 0) return {};
    size_t e = cleanHtml.find('"', p);
    if (e == std::string::npos || cleanHtml.compare(e, 2, "\"]") != 0) return {};
    std::string manifestUrl = fixUrl(cleanHtml.substr(p, e - p), url);

    std::vector<Video::Track> subs;
    size_t pos = 0;
    const std::string langMarker = "\"language\":[";
    while ((pos = cleanHtml.find(langMarker, pos)) != std::string::npos) {
        size_t objEnd = cleanHtml.find('}', pos);
        if (objEnd == std::string::npos) objEnd = cleanHtml.size();
        std::string lang, subName, src;
        size_t after = pos;
        if (readTagged(cleanHtml, "language", pos, objEnd, lang, after) &&
            readTagged(cleanHtml, "name", after, objEnd, subName, after) &&
            readTagged(cleanHtml, "src", after, objEnd, src, after)) {
            std::string subUrl = fixUrl(replaceAll(src, "\\/", "/"), url);
            if (startsWith(subUrl, "http")) subs.push_back({subUrl, subName + " (" + lang + ")"});
        }
        pos += langMarker.size();
    }

    try {
        return videosFromManifest(manifestUrl, name, subs, vh);
    } catch (const std::exception&) {
        return {};
    }
}

std::vector<Video> extractVideos(const std::string& url, const std::string& name) {
    std::string finalUrl = url;
    if (url.find("/vast") != std::string::npos) {
        std::string query;
        auto q = url.find('?');
        if (q != std::string::npos) query = substringBefore(url.substr(q), "#");
        finalUrl = http::originOf(url) + "/cat-player/player" + query;
    }

    std::string html = http::getText(finalUrl, baseHeaders());
    std::string cleanHtml = replaceAll(html, "&quot;", "\"");
    if (cleanHtml.find("manifest\":[0,\"") != std::string::npos) return parseNewPlayer(cleanHtml, finalUrl, name);

    if (html.find("cid: '") == std::string::npos) return {};

    std::string host = http::hostOf(finalUrl);
    std::string mid = name == "DuckStream" ? "mid" : "id";
    bool isBird = name == "BirdStream";
    std::string query = http::queryParam(finalUrl, mid);
    if (query.empty() || host.empty()) return {};

    std::string key;
    std::vector<std::string> order;
    if (name == "VidStreaming") {
        key = "e13d38099bf562e8b9851a652d2043d3";
        order = {"IP", "USERAGENT", "ROUTE", "MID", "TIMESTAMP", "KEY"};
    } else if (name == "DuckStream") {
        key = "4504447b74641ad972980a6b8ffd7631";
        order = {"IP", "USERAGENT", "ROUTE", "MID", "TIMESTAMP", "KEY"};
    } else if (name == "BirdStream") {
        key = "4b14d0ff625163e3c9c7a47926484bf2";
        order = {"IP", "USERAGENT", "ROUTE", "MID", "KEY"};
    } else {
        return {};  // CatStream (vecchio player) non ha chiavi: come l'estensione, nessuna firma possibile
    }

    // firma (getSignature)
    std::string cidHex = substringBefore(substringAfter(html, "cid: '"), "'");
    std::string cidRaw = crypto::fromHex(cidHex);
    std::vector<std::string> cid;
    {
        size_t s = 0;
        while (true) {
            auto b = cidRaw.find('|', s);
            cid.push_back(cidRaw.substr(s, b == std::string::npos ? std::string::npos : b - s));
            if (b == std::string::npos) break;
            s = b + 1;
        }
    }
    if (cid.size() < 2) return {};
    std::string timeStamp = std::to_string((long long)std::time(nullptr) + 60);
    std::string route = replaceAll(cid[1], "player.php", "source.php");
    std::string sigData;
    for (auto& o : order) {
        if (o == "IP") sigData += cid[0];
        else if (o == "USERAGENT") sigData += http::DEFAULT_UA;
        else if (o == "ROUTE") sigData += route;
        else if (o == "MID") sigData += query;
        else if (o == "TIMESTAMP") sigData += timeStamp;
        else if (o == "KEY") sigData += key;
    }
    std::string sig = crypto::toHex(crypto::sha1(sigData));

    std::string sourceUrl = "https://" + host + route + "?" + mid + "=" + query;
    if (!isBird) sourceUrl += "&e=" + timeStamp;
    sourceUrl += "&s=" + sig;

    http::Headers sh = baseHeaders();
    sh.push_back({"Referer", finalUrl});
    sh.push_back({"Origin", "https://" + host});
    std::string response = http::getText(sourceUrl, sh);

    std::string payload = replaceAll(substringBefore(substringAfter(response, ":\""), "\""), "\\", "");
    auto colon = payload.find(':');
    if (colon == std::string::npos) return {};
    std::string encrypted = payload.substr(0, colon);
    std::string ivHex = substringBefore(payload.substr(colon + 1), ":");

    json videoObj;
    try {
        std::string dec = crypto::aesCbcDecrypt(base64Decode(encrypted), key, crypto::fromHex(ivHex));
        videoObj = json::parse(dec);
    } catch (const std::exception&) {
        return {};
    }

    http::Headers vh = videoHeaders(finalUrl);
    std::vector<Video::Track> subs;
    for (auto& s : videoObj.value("subtitles", json::array())) {
        std::string src = jstr(s, "src");
        if (src.empty()) continue;
        subs.push_back({fixUrl(src, finalUrl), jstr(s, "name") + " (" + jstr(s, "language") + ")"});
    }

    std::string hls = jstr(videoObj, "hls");
    std::string dash = jstr(videoObj, "dash");
    std::string playlistUrl = fixUrl(hls.empty() ? dash : hls, finalUrl);
    if (playlistUrl.empty()) return {};
    if (!hls.empty()) return videosFromHls(playlistUrl, name, subs, vh);
    Video v;
    v.title = name + " - DASH";
    v.url = playlistUrl;
    v.userAgent = VIDEO_UA;
    v.headers = vh;
    v.subtitles = subs;
    return {v};
}

// ================================== Fonte ==================================

class KickAssAnime : public Source {
  public:
    std::string id() const override { return "en.kickassanime"; }
    std::string name() const override { return "KickAssAnime"; }
    std::string defaultBaseUrl() const override { return "https://kaa.lt"; }
    std::string lang() const override { return "en"; }

    Page popular(int page) override {
        json data = getJson(apiUrl() + "/trending?page=" + std::to_string(page));
        Page p = parseList(data);
        p.hasNextPage = data.value("page_count", 0) > page;
        return p;
    }

    Page latest(int page) override {
        json data = getJson(apiUrl() + "/recent?type=all&page=" + std::to_string(page));
        Page p = parseList(data);
        p.hasNextPage = data.value("hadNext", false);
        return p;
    }

    Page search(const std::string& rawQuery, int page) override {
        std::string query = trim(rawQuery);
        if (startsWith(query, "https://")) {
            if (http::hostOf(query) != http::hostOf(SEARCH_BASE_URL)) throw http::Error("URL non supportato");
            std::string slug = substringBefore(substringBefore(http::pathOf(query).substr(1), "/"), "?");
            if (slug.empty()) throw http::Error("URL non supportato");
            query = PREFIX_SEARCH + slug;
        }
        if (startsWith(query, PREFIX_SEARCH)) {
            std::string slug = query.substr(std::string(PREFIX_SEARCH).size());
            json info = getJson(std::string(SEARCH_BASE_URL) + "/api/show/" + slug);
            Page p;
            p.animes.push_back(fromObject(info));
            return p;
        }

        http::Headers h = baseHeaders();
        h.push_back({"Accept", "application/json, text/plain, */*"});
        h.push_back({"Content-Type", "application/json"});
        h.push_back({"Referer", std::string(SEARCH_BASE_URL) + "/search?q=" + http::urlEncode(query)});

        http::Response r;
        if (query.empty()) {
            r = http::request("GET", std::string(SEARCH_BASE_URL) + "/api/anime?page=" + std::to_string(page), h);
        } else {
            json body = {{"page", page}, {"query", query}};
            r = http::request("POST", std::string(SEARCH_BASE_URL) + "/api/fsearch", h, body.dump());
        }
        if (r.status < 200 || r.status >= 300)
            throw http::Error("KickAssAnime: ricerca non riuscita (HTTP " + std::to_string(r.status) + ")");
        json data = json::parse(r.body);
        Page p = parseList(data);
        p.hasNextPage = page < data.value("maxPage", 0);
        return p;
    }

    Details details(const std::string& animeUrl) override {
        json info = getJson(apiUrl() + animeUrl);
        std::vector<std::string> languages = fetchLanguages(animeUrl);

        Details d;
        Anime a = fromObject(info);
        d.title = a.title;
        d.thumbnail = a.thumbnail;
        std::string genres;
        for (auto& g : info.value("genres", json::array())) {
            if (!g.is_string()) continue;
            if (!genres.empty()) genres += ", ";
            genres += g.get<std::string>();
        }
        d.genre = genres;
        std::string status = jstr(info, "status");
        d.status = status == "finished_airing" ? "Completato" : status == "currently_airing" ? "In corso" : "";

        std::string desc;
        std::string synopsis = jstr(info, "synopsis");
        if (!synopsis.empty()) desc += synopsis + "\n\n";
        std::string langs;
        for (auto& l : languages) {
            if (!langs.empty()) langs += ", ";
            langs += localeName(l);
        }
        desc += "Available Dub Languages: " + langs + "\n";
        std::string season = jstr(info, "season");
        if (!season.empty()) {
            season[0] = (char)std::toupper((unsigned char)season[0]);
            desc += "Season: " + season + "\n";
        }
        std::string year = jstr(info, "year");
        if (!year.empty()) desc += "Year: " + year;
        d.description = desc;

        d.episodes = fetchEpisodes(animeUrl, languages);
        return d;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::string url = apiUrl() + replaceAll(episodeUrl, "/ep-", "/episode/ep-");
        json data = getJson(url);
        std::vector<Video> preferred, others;
        for (auto& s : data.value("servers", json::array())) {
            std::string name = jstr(s, "name");
            std::string src = jstr(s, "src");
            if (src.empty()) continue;
            try {
                auto vids = extractVideos(src, name);
                auto& dest = name == PREF_SERVER ? preferred : others;
                dest.insert(dest.end(), vids.begin(), vids.end());
            } catch (const std::exception&) {
            }
        }
        preferred.insert(preferred.end(), others.begin(), others.end());
        if (preferred.empty()) throw http::Error("Nessun video trovato");
        return preferred;
    }

    http::Headers imageHeaders() const override { return {{"Referer", baseUrl() + "/"}}; }

  private:
    std::string apiUrl() const { return baseUrl() + "/api/show"; }

    Anime fromObject(const json& o) const {
        Anime a;
        // preferenza "Use English titles" disattivata per default
        a.title = jstr(o, "title");
        if (a.title.empty()) a.title = jstr(o, "title_en");
        a.url = "/" + jstr(o, "slug");
        std::string hq;
        auto it = o.find("poster");
        if (it != o.end()) hq = jstr(*it, "hq");
        if (!hq.empty()) a.thumbnail = baseUrl() + "/image/poster/" + hq + ".webp";
        return a;
    }

    Page parseList(const json& data) const {
        Page p;
        for (auto& o : data.value("result", json::array())) {
            Anime a = fromObject(o);
            if (a.url.size() > 1 && !a.title.empty()) p.animes.push_back(a);
        }
        return p;
    }

    std::vector<std::string> fetchLanguages(const std::string& animeUrl) const {
        std::vector<std::string> out;
        try {
            json j = getJson(apiUrl() + animeUrl + "/language");
            for (auto& l : j.value("result", json::array()))
                if (l.is_string()) out.push_back(l.get<std::string>());
        } catch (const std::exception&) {
        }
        return out;
    }

    json episodePage(const std::string& animeUrl, int page, const std::string& lang) const {
        return getJson(apiUrl() + animeUrl + "/episodes?page=" + std::to_string(page) + "&lang=" + lang);
    }

    std::vector<Episode> fetchEpisodes(const std::string& animeUrl, std::vector<std::string> languages) const {
        std::stable_sort(languages.begin(), languages.end(), [](const std::string& a, const std::string& b) {
            auto rank = [](const std::string& l) { return l == PREF_LANG ? 0 : l == PREF_LANG_2ND ? 1 : 2; };
            return rank(a) < rank(b);
        });

        for (auto& lang : languages) {
            json first;
            try {
                first = episodePage(animeUrl, 1, lang);
            } catch (const std::exception&) {
                continue;
            }
            json results = first.value("result", json::array());
            if (!results.is_array() || results.empty()) continue;

            size_t pages = first.value("pages", json::array()).size();
            for (size_t idx = 2; idx <= pages; idx++) {
                try {
                    json pj = episodePage(animeUrl, (int)idx, lang);
                    for (auto& e : pj.value("result", json::array())) results.push_back(e);
                } catch (const std::exception&) {
                }
            }

            std::vector<Episode> eps;
            std::string locale = localeName(lang);
            for (auto& e : results) {
                std::string num = jstr(e, "episode_string");
                std::string title = jstr(e, "title");
                Episode ep;
                ep.name = "Ep. " + num + (title.empty() ? "" : " - " + title);
                if (!locale.empty()) ep.name += " [" + locale + "]";
                ep.url = animeUrl + "/ep-" + num + "-" + jstr(e, "slug");
                ep.number = parseNumber(num, -1);
                eps.push_back(ep);
            }
            std::reverse(eps.begin(), eps.end());
            return eps;
        }
        return {};
    }
};

}  // namespace

std::shared_ptr<Source> makeKickAssAnime() { return std::make_shared<KickAssAnime>(); }

}  // namespace src
