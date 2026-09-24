// Porting in C++ del tema multisrc Aniyomi "AnimeStream" (lib-multisrc/animestream) per i siti
// inglesi / multilingua: Animenosub, AnimeKhor, LuciferDonghua, DonghuaStream, AnimeXin, ChineseAnime, LMAnime.
// Gli estrattori degli hoster (lib/*extractor e quelli specifici delle estensioni) sono inclusi qui sotto.

#include <gumbo.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <random>
#include <set>

#include "html/html.hpp"
#include "sources/registry.hpp"
#include "util/crypto.hpp"
#include "util/unpacker.hpp"

namespace src {

namespace {

using json = nlohmann::json;

// =============================================================================================== utilita'

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool contains(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

bool containsCI(const std::string& s, const std::string& what) { return contains(lower(s), lower(what)); }

bool startsWith(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }

bool endsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

/** substringAfter con valore di ripiego se il delimitatore manca (come Kotlin substringAfter(d, missing)). */
std::string after(const std::string& s, const std::string& d, const std::string& missing) {
    auto p = s.find(d);
    return p == std::string::npos ? missing : s.substr(p + d.size());
}

/** Testo proprio del nodo, esclusi i figli elemento (Element.ownText() di Jsoup). */
std::string ownText(const html::Node& node) {
    GumboNode* n = reinterpret_cast<GumboNode*>(node.raw());
    if (!n || n->type != GUMBO_NODE_ELEMENT) return "";
    std::string raw;
    const GumboVector& ch = n->v.element.children;
    for (unsigned i = 0; i < ch.length; i++) {
        auto* c = (GumboNode*)ch.data[i];
        if (c->type == GUMBO_NODE_TEXT || c->type == GUMBO_NODE_WHITESPACE || c->type == GUMBO_NODE_CDATA)
            raw += c->v.text.text;
    }
    std::string out;
    bool space = false;
    for (unsigned char c : raw) {
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f') {
            space = true;
        } else {
            if (space && !out.empty()) out += ' ';
            space = false;
            out += (char)c;
        }
    }
    return out;
}

/** Primo <script> il cui contenuto contiene tutte le stringhe indicate (script:containsData(...)). */
std::string scriptWith(const html::Document& doc, std::initializer_list<const char*> needles) {
    for (auto& s : doc.select("script")) {
        std::string d = s.data();
        bool ok = true;
        for (const char* n : needles)
            if (!contains(d, n)) {
                ok = false;
                break;
            }
        if (ok) return d;
    }
    return "";
}

std::string fixUrl(const std::string& url, const std::string& base = "") {
    if (url.empty()) return "";
    if (startsWith(url, "http")) return url;
    if (startsWith(url, "//")) return "https:" + url;
    if (!base.empty()) return http::resolve(base, url);
    auto p = url.find("http");
    return p == std::string::npos ? url : url.substr(p);
}

/** Altezza in pixel da un'etichetta tipo "Server - 720p" (0 se assente). */
int qualityOf(const std::string& title) {
    for (size_t i = 0; i < title.size(); i++) {
        if (title[i] != 'p' || i == 0 || !std::isdigit((unsigned char)title[i - 1])) continue;
        size_t b = i;
        while (b > 0 && std::isdigit((unsigned char)title[b - 1])) b--;
        return std::atoi(title.substr(b, i - b).c_str());
    }
    return 0;
}

std::string randomHex(size_t len) {
    static thread_local std::mt19937 rng((unsigned)std::random_device{}() ^ (unsigned)std::time(nullptr));
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < len; i++) out += hex[rng() % 16];
    return out;
}

std::string randomAlnum(size_t len) {
    static thread_local std::mt19937 rng((unsigned)std::random_device{}() ^ (unsigned)std::time(nullptr) ^ 0x5bd1e995u);
    static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    std::string out;
    for (size_t i = 0; i < len; i++) out += chars[rng() % 62];
    return out;
}

/** Valore di un campo in un oggetto JS/JSON non rigoroso: file:"x", "file":'x' ... */
std::string jsField(const std::string& obj, const std::string& key) {
    size_t pos = 0;
    while ((pos = obj.find(key, pos)) != std::string::npos) {
        size_t p = pos + key.size();
        if (p < obj.size() && (obj[p] == '"' || obj[p] == '\'')) p++;
        while (p < obj.size() && std::isspace((unsigned char)obj[p])) p++;
        if (p < obj.size() && obj[p] == ':') {
            p++;
            while (p < obj.size() && std::isspace((unsigned char)obj[p])) p++;
            if (p < obj.size() && (obj[p] == '"' || obj[p] == '\'')) {
                char q = obj[p];
                auto e = obj.find(q, p + 1);
                if (e == std::string::npos) return "";
                return replaceAll(obj.substr(p + 1, e - p - 1), "\\/", "/");
            }
        }
        pos += key.size();
    }
    return "";
}

/** Tracce "captions" dal blocco tracks:[...] di JWPlayer. */
std::vector<Video::Track> captionTracks(const std::string& script, const std::string& base = "") {
    std::vector<Video::Track> out;
    if (!contains(script, "tracks")) return out;
    std::string block = substringBefore(substringAfter(substringAfter(script, "tracks"), "["), "]");
    size_t start = 0;
    while ((start = block.find('{', start)) != std::string::npos) {
        auto end = block.find('}', start);
        std::string obj = block.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? block.size() : end;
        if (!containsCI(jsField(obj, "kind"), "captions")) continue;
        std::string file = fixUrl(jsField(obj, "file"), base);
        if (!startsWith(file, "http")) continue;
        std::string label = jsField(obj, "label");
        out.push_back({file, label.empty() ? "Sub" : label});
    }
    return out;
}

/** Regex `sources\s*:\s*(.+?]),` + `file\s*:\s*["']([^"']+)["']` senza std::regex. */
std::vector<std::string> sourcesFiles(const std::string& script) {
    std::vector<std::string> out;
    size_t pos = 0;
    while ((pos = script.find("sources", pos)) != std::string::npos) {
        size_t p = pos + 7;
        while (p < script.size() && std::isspace((unsigned char)script[p])) p++;
        if (p >= script.size() || script[p] != ':') {
            pos += 7;
            continue;
        }
        p++;
        while (p < script.size() && std::isspace((unsigned char)script[p])) p++;
        auto end = script.find("],", p + 1);
        if (end == std::string::npos) return out;
        std::string block = script.substr(p, end + 1 - p);
        size_t f = 0;
        while ((f = block.find("file", f)) != std::string::npos) {
            size_t q = f + 4;
            f = q;
            while (q < block.size() && std::isspace((unsigned char)block[q])) q++;
            if (q >= block.size() || block[q] != ':') continue;
            q++;
            while (q < block.size() && std::isspace((unsigned char)block[q])) q++;
            if (q >= block.size() || (block[q] != '"' && block[q] != '\'')) continue;
            size_t s = q + 1;
            size_t e = s;
            while (e < block.size() && block[e] != '"' && block[e] != '\'') e++;
            if (e < block.size() && e > s) out.push_back(block.substr(s, e - s));
            f = e;
        }
        return out;
    }
    return out;
}

// =============================================================================================== HLS

/**
 * Equivalente semplificato di PlaylistUtils.extractFromHls: scarica la master playlist e restituisce
 * una voce per variante (qualita'). Se ci sono tracce audio separate o la playlist non e' una master,
 * restituisce la playlist originale (mpv gestisce da solo le varianti).
 */
std::vector<Video> hlsVideos(const std::string& master, const std::string& referer, const std::string& titlePrefix,
                             const std::vector<Video::Track>& subs = {}, const http::Headers& extra = {},
                             const std::string& userAgent = "") {
    http::Headers h = {{"Accept", "*/*"}};
    std::string origin = referer.empty() ? "" : http::originOf(referer);
    if (!referer.empty()) h.push_back({"Referer", referer});
    if (!origin.empty()) h.push_back({"Origin", origin});
    if (!userAgent.empty()) h.push_back({"User-Agent", userAgent});
    for (auto& kv : extra) h.push_back(kv);

    bool bare = false;  // il CDN accetta solo richieste senza Referer/Origin del sito
    auto make = [&](const std::string& url, int q) {
        Video v;
        v.url = url;
        v.quality = q;
        v.title = titlePrefix + (q > 0 ? std::to_string(q) + "p" : std::string("Auto"));
        v.referer = referer;
        v.userAgent = userAgent;
        if (!origin.empty()) v.headers.push_back({"Origin", origin});
        for (auto& kv : extra)
            if (lower(kv.first) != "referer" && lower(kv.first) != "user-agent") v.headers.push_back(kv);
        v.subtitles = subs;
        if (bare) {
            v.referer.clear();
            v.headers.clear();
        }
        return v;
    };

    std::string body;
    int status = 0;
    try {
        http::Response r = http::request("GET", master, h, "", 20);
        status = r.status;
        if (r.status >= 200 && r.status < 300) body = r.body;
    } catch (const std::exception&) {
    }
    if (status == 401 || status == 403) {
        // alcuni CDN rifiutano Referer/Origin del sito: si riprova senza; se va, il video si usa senza
        try {
            http::Headers plain = {{"Accept", "*/*"}};
            if (!userAgent.empty()) plain.push_back({"User-Agent", userAgent});
            http::Response r = http::request("GET", master, plain, "", 20);
            if (r.status >= 200 && r.status < 300) {
                bare = true;
                if (r.body.find("#EXT-X-STREAM-INF") == std::string::npos) return {make(master, 0)};
                body = r.body;
            } else {
                return {};  // playlist non raggiungibile: inutile proporla al player
            }
        } catch (const std::exception&) {
            return {};
        }
    }
    if (body.find("#EXT-X-STREAM-INF") == std::string::npos) return {make(master, 0)};

    std::vector<Video> out;
    std::set<std::string> seen;
    int maxQ = 0;
    size_t pos = 0;
    while ((pos = body.find("#EXT-X-STREAM-INF:", pos)) != std::string::npos) {
        auto eol = body.find('\n', pos);
        if (eol == std::string::npos) break;
        std::string info = body.substr(pos, eol - pos);
        int q = 0;
        auto rp = info.find("RESOLUTION=");
        if (rp != std::string::npos) {
            std::string res = info.substr(rp + 11);
            auto x = res.find('x');
            if (x != std::string::npos) q = std::atoi(res.substr(x + 1).c_str());
        }
        // prima riga non vuota e non di commento dopo l'intestazione
        size_t lp = eol + 1;
        std::string line;
        while (lp < body.size()) {
            auto le = body.find('\n', lp);
            line = trim(body.substr(lp, le == std::string::npos ? std::string::npos : le - lp));
            lp = le == std::string::npos ? body.size() : le + 1;
            if (!line.empty() && line[0] != '#') break;
            if (startsWith(line, "#EXT-X-STREAM-INF")) {
                line.clear();
                break;
            }
            line.clear();
        }
        pos = eol + 1;
        if (line.empty()) continue;
        std::string url = http::resolve(master, line);
        if (!seen.insert(url).second) continue;
        maxQ = std::max(maxQ, q);
        out.push_back(make(url, q));
    }
    bool separateAudio = contains(body, "TYPE=AUDIO") && contains(substringAfter(body, "TYPE=AUDIO"), "URI=");
    if (out.empty() || separateAudio) {
        return {make(master, maxQ)};
    }
    std::stable_sort(out.begin(), out.end(), [](const Video& a, const Video& b) { return a.quality > b.quality; });
    return out;
}

// =============================================================================================== estrattori

// ---- StreamWish (lib/streamwishextractor): anche filelions / embedwish / swdyu
std::vector<Video> streamWish(const std::string& url, const std::string& prefix, const http::Headers& headers = {}) {
    // Regex(".*/[efd]/([a-zA-Z0-9]+)"): ultima occorrenza
    std::string id = url;
    for (size_t p = url.size(); p-- > 0;) {
        if (url[p] != '/' || p + 3 >= url.size()) continue;
        char c = url[p + 1];
        if ((c != 'e' && c != 'f' && c != 'd') || url[p + 2] != '/') continue;
        size_t s = p + 3, e = s;
        while (e < url.size() && std::isalnum((unsigned char)url[e])) e++;
        if (e > s) {
            id = url.substr(s, e - s);
            break;
        }
    }
    bool absolute = startsWith(id, "http://") || startsWith(id, "https://");
    std::vector<std::string> domains = absolute ? std::vector<std::string>{""}
                                                : std::vector<std::string>{"streamwish.com", "niramirus.com", "medixiru.com"};
    for (auto& domain : domains) {
        std::string full = absolute ? id : "https://" + domain + "/" + id;
        try {
            http::Response r = http::request("GET", full, headers, "", 20);
            if (r.status < 200 || r.status >= 300 || trim(r.body).empty()) continue;
            html::Document doc(r.body, r.finalUrl.empty() ? full : r.finalUrl);
            // la variante con main.js offuscato (synchrony) richiede un deoffuscatore JS: non supportata
            std::string script = scriptWith(doc, {"m3u8"});
            if (script.empty()) continue;
            if (contains(script, "eval(function(p,a,c")) {
                std::string un = unpacker::unpackAndCombine(script);
                if (!un.empty()) script = un;
            }
            // Regex("https[^\"]*m3u8[^\"]*")
            std::string masterUrl;
            size_t p = 0;
            while ((p = script.find("https", p)) != std::string::npos) {
                auto q = script.find('"', p);
                std::string cand = script.substr(p, q == std::string::npos ? std::string::npos : q - p);
                if (contains(cand, "m3u8")) {
                    masterUrl = cand;
                    break;
                }
                p += 5;
            }
            if (masterUrl.empty()) continue;
            auto subs = captionTracks(script);
            return hlsVideos(masterUrl, http::originOf(masterUrl) + "/", prefix + "StreamWish - ", subs);
        } catch (const std::exception&) {
            if (absolute) return {};
        }
    }
    return {};
}

// ---- VidHide (lib/vidhideextractor)
std::vector<Video> vidHide(const std::string& url, const std::string& prefix, const http::Headers& headers = {}) {
    html::Document doc(http::getText(url, headers), url);
    std::string packed = scriptWith(doc, {"eval(function(p,a,c,k,e,d)"});
    if (packed.empty()) return {};
    std::string script = unpacker::unpackAndCombine(packed);
    if (script.empty()) return {};
    // Regex("\"((?:https?:/)?/[^\"]*m3u8[^\"]*)\"")
    std::vector<std::string> urls;
    size_t pos = 0;
    while ((pos = script.find('"', pos)) != std::string::npos) {
        auto end = script.find('"', pos + 1);
        if (end == std::string::npos) break;
        std::string cand = script.substr(pos + 1, end - pos - 1);
        bool okStart = startsWith(cand, "https://") || startsWith(cand, "http://") || startsWith(cand, "/");
        if (okStart && contains(cand, "m3u8")) {
            std::string fixed = fixUrl(cand, url);
            if (std::find(urls.begin(), urls.end(), fixed) == urls.end()) urls.push_back(fixed);
            pos = end + 1;
        } else {
            pos = pos + 1;
        }
    }
    auto subs = captionTracks(script, url);
    std::vector<Video> out;
    for (auto& u : urls) {
        auto v = hlsVideos(u, url, prefix + "VidHide - ", subs);
        out.insert(out.end(), v.begin(), v.end());
    }
    return out;
}

// ---- Ok.ru (lib/okruextractor)
std::vector<Video> okru(const std::string& rawUrl, const std::string& prefix) {
    std::string url = fixUrl(rawUrl);
    html::Document doc(http::getText(url), url);
    std::string options;
    for (auto& d : doc.select("div[data-options]")) {
        options = d.attr("data-options");
        break;
    }
    if (options.empty()) return {};
    // ok.ru a volte annida i metadati come stringa JSON (virgolette \") e a volte come oggetto (virgolette "):
    // si cercano entrambe le forme; se il campo manca si restituisce "" (non tutta la stringa)
    auto extractLink = [](const std::string& s, const std::string& attr) -> std::string {
        std::string v;
        for (const std::string& pat : {attr + R"(\":\")", attr + R"(":")"}) {
            auto p = s.find(pat);
            if (p == std::string::npos) continue;
            p += pat.size();
            auto e = s.find('"', p);
            if (e == std::string::npos) continue;
            v = s.substr(p, e - p);
            if (!v.empty() && v.back() == '\\') v.pop_back();
            break;
        }
        v = replaceAll(v, R"(\\u0026)", "&");
        v = replaceAll(v, R"(\u0026)", "&");
        v = replaceAll(v, R"(\/)", "/");
        return v.rfind("http", 0) == 0 ? v : "";
    };
    std::string namePrefix = prefix + "Okru - ";
    std::string hls = extractLink(options, "ondemandHls");
    if (!hls.empty()) return hlsVideos(hls, "", namePrefix);
    std::string hlsMaster = extractLink(options, "hlsManifestUrl");
    if (!hlsMaster.empty()) return hlsVideos(hlsMaster, "", namePrefix);
    std::string dash = extractLink(options, "ondemandDash");
    if (!dash.empty()) {
        Video v;
        v.url = dash;
        v.title = namePrefix + "DASH";
        return {v};
    }
    static const std::vector<std::pair<std::string, std::string>> qualities = {
        {"ultra", "2160p"}, {"quad", "1440p"}, {"full", "1080p"}, {"hd", "720p"},
        {"sd", "480p"},     {"low", "360p"},   {"lowest", "240p"}, {"mobile", "144p"},
    };
    std::string arrayData = substringBefore(substringAfter(options, R"(\"videos\":[{\"name\":\")"), "]");
    const std::string sep = R"({\"name\":\")";
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        auto p = arrayData.find(sep, start);
        parts.push_back(arrayData.substr(start, p == std::string::npos ? std::string::npos : p - start));
        if (p == std::string::npos) break;
        start = p + sep.size();
    }
    std::reverse(parts.begin(), parts.end());
    std::vector<Video> out;
    for (auto& data : parts) {
        std::string videoUrl = extractLink(data, "url");
        std::string q = substringBefore(data, R"(\")");
        for (auto& kv : qualities)
            if (kv.first == q) {
                q = kv.second;
                break;
            }
        if (!startsWith(videoUrl, "https://")) continue;
        Video v;
        v.url = videoUrl;
        v.title = namePrefix + q;
        v.quality = qualityOf(q);
        out.push_back(v);
    }
    std::stable_sort(out.begin(), out.end(), [](const Video& a, const Video& b) { return a.quality > b.quality; });
    return out;
}

// ---- Dailymotion (lib/dailymotionextractor), senza i video protetti da password
std::vector<Video> dailymotion(const std::string& url, const std::string& titlePrefix) {
    const std::string dm = "https://www.dailymotion.com";
    std::string htmlString = http::getText(url);
    std::string internal = substringBefore(after(htmlString, "\"dmInternalData\":", htmlString), "</script>");
    std::string ts = substringBefore(substringAfter(internal, "\"ts\":"), ",");
    std::string v1st = substringBefore(substringAfter(internal, "\"v1st\":\""), "\",");

    std::string videoQuery = http::queryParam(url, "video");
    if (videoQuery.empty()) {
        std::string path = substringBefore(substringBefore(http::pathOf(url), "?"), "#");
        while (!path.empty() && path.back() == '/') path.pop_back();
        auto slash = path.rfind('/');
        videoQuery = slash == std::string::npos ? path : path.substr(slash + 1);
    }
    std::string jsonUrl = dm + "/player/metadata/video/" + videoQuery + "?locale=en-US&dmV1st=" + v1st +
                          "&dmTs=" + ts + "&is_native_app=0";
    json j = json::parse(http::getText(jsonUrl));
    if (j.contains("error") && !j["error"].is_null()) return {};
    if (!j.contains("qualities") || !j["qualities"].is_object()) return {};
    auto& autoList = j["qualities"]["auto"];
    if (!autoList.is_array() || autoList.empty()) return {};
    std::string masterUrl = autoList[0].value("url", "");
    if (masterUrl.empty()) return {};

    std::vector<Video::Track> subs;
    if (j.contains("subtitles") && j["subtitles"].is_object()) {
        auto& data = j["subtitles"]["data"];
        if (data.is_object()) {
            for (auto it = data.begin(); it != data.end(); ++it) {
                auto& s = it.value();
                if (!s.is_object() || !s.contains("urls") || !s["urls"].is_array() || s["urls"].empty()) continue;
                subs.push_back({s["urls"][0].get<std::string>(), s.value("label", it.key())});
            }
        }
    }
    return hlsVideos(masterUrl, dm + "/", titlePrefix, subs);
}

// ---- Rumble: il vecchio /hls-vod/<id>/playlist.m3u8 dell'estensione ora restituisce una playlist vuota;
// si usa l'API del lettore incorporato (embedJS), che elenca i file MP4 per qualita' (e l'HLS se presente).
std::vector<Video> rumble(const std::string& url, const std::string& prefix) {
    auto p = url.find("rumble.com/embed/v");
    if (p == std::string::npos) return {};
    size_t s = p + 18, e = s;
    while (e < url.size() && std::isalnum((unsigned char)url[e])) e++;
    if (e == s) return {};
    std::string id = url.substr(s, e - s);
    std::vector<Video> out;
    try {
        json j = json::parse(http::getText("https://rumble.com/embedJS/u3/?request=video&ver=2&v=v" + id,
                                           {{"Referer", url}}));
        if (j.contains("ua") && j["ua"].is_object()) {
            for (const char* kind : {"mp4", "webm"}) {
                if (!j["ua"].contains(kind) || !j["ua"][kind].is_object()) continue;
                for (auto it = j["ua"][kind].begin(); it != j["ua"][kind].end(); ++it) {
                    std::string link = it.value().is_string() ? it.value().get<std::string>()
                                     : it.value().is_object() ? it.value().value("url", "") : "";
                    if (link.empty()) continue;
                    Video v;
                    v.url = link;
                    v.quality = std::atoi(it.key().c_str());
                    v.title = prefix + "Rumble - " + (v.quality > 0 ? it.key() + "p" : it.key());
                    v.referer = "https://rumble.com/";
                    out.push_back(v);
                }
            }
            if (j["ua"].contains("hls") && j["ua"]["hls"].is_object()) {
                for (auto it = j["ua"]["hls"].begin(); it != j["ua"]["hls"].end(); ++it) {
                    std::string link = it.value().is_string() ? it.value().get<std::string>()
                                     : it.value().is_object() ? it.value().value("url", "") : "";
                    if (!link.empty()) {
                        auto hv = hlsVideos(link, "https://rumble.com/", prefix + "Rumble - ");
                        out.insert(out.end(), hv.begin(), hv.end());
                        break;
                    }
                }
            }
        }
    } catch (const std::exception&) {
    }
    std::sort(out.begin(), out.end(), [](const Video& a, const Video& b) { return a.quality > b.quality; });
    return out;
}

// ---- StreamPlay (lib/streamplayextractor)
std::vector<Video> streamPlayServer(const std::string& url, const std::string& prefix) {
    html::Document doc(http::getText(url), url);
    std::string script = scriptWith(doc, {"function(p,a,c,k,e,d)"});
    std::string text = script.empty() ? scriptWith(doc, {"window.kaken"}) : unpacker::unpackAndCombine(script);
    // Regex("window\\.kaken ?= ?\"([^\"]+)\";")
    auto p = text.find("window.kaken");
    if (p == std::string::npos) return {};
    std::string rest = text.substr(p + 12, 4096);
    size_t i = 0;
    if (i < rest.size() && rest[i] == ' ') i++;
    if (i >= rest.size() || rest[i] != '=') return {};
    i++;
    if (i < rest.size() && rest[i] == ' ') i++;
    if (i >= rest.size() || rest[i] != '"') return {};
    auto end = rest.find('"', i + 1);
    if (end == std::string::npos) return {};
    std::string kaken = rest.substr(i + 1, end - i - 1);
    if (kaken.empty()) return {};

    http::Headers api = {
        {"Accept", "application/json, text/javascript, */*; q=0.01"},
        {"Origin", http::originOf(url)},
        {"Referer", url},
        {"X-Requested-With", "XMLHttpRequest"},
        {"Content-Type", "text/plain"},
    };
    http::Response r = http::request("POST", "https://play.streamplay.co.in/api/", api, kaken);
    if (r.status < 200 || r.status >= 300) return {};
    json j = json::parse(r.body);
    std::vector<Video::Track> subs;
    if (j.contains("tracks") && j["tracks"].is_array())
        for (auto& t : j["tracks"])
            if (t.is_object()) subs.push_back({t.value("file", ""), t.value("label", "Sub")});
    std::vector<Video> out;
    if (!j.contains("sources") || !j["sources"].is_array()) return out;
    for (auto& s : j["sources"]) {
        if (!s.is_object()) continue;
        std::string file = fixUrl(replaceAll(s.value("file", ""), "master.txt", "master.m3u8"));
        if (file.empty()) continue;
        if (s.value("type", "") == "hls" && endsWith(file, "master.m3u8")) {
            auto v = hlsVideos(file, url, prefix + "StreamPlay - ", subs);
            out.insert(out.end(), v.begin(), v.end());
        } else {
            Video v;
            v.url = file;
            v.title = prefix + "StreamPlay - Original";
            v.referer = url;
            v.subtitles = subs;
            out.push_back(v);
        }
    }
    return out;
}

std::vector<Video> streamPlay(const std::string& url, const std::string& prefix) {
    html::Document doc(http::getText(url), url);
    std::vector<Video> out;
    for (auto& a : doc.select("#servers a")) {
        try {
            auto v = streamPlayServer(doc.absUrl(a, "href"), prefix + a.text() + " - ");
            out.insert(out.end(), v.begin(), v.end());
        } catch (const std::exception&) {
        }
    }
    return out;
}

// ---- Mp4Upload (lib/mp4uploadextractor)
std::vector<Video> mp4upload(const std::string& url, const std::string& prefix) {
    const std::string referer = "https://mp4upload.com/";
    html::Document doc(http::getText(url, {{"Referer", referer}}), url);
    std::string script;
    std::string packed = scriptWith(doc, {"eval", "p,a,c,k,e,d"});
    if (!packed.empty()) script = unpacker::unpackAndCombine(packed);
    if (script.empty()) script = scriptWith(doc, {"player.src"});
    if (script.empty()) return {};
    std::string videoUrl = substringBefore(substringAfter(substringAfter(substringBefore(substringAfter(script, ".src("), ")"), "src:"), "\""), "\"");
    if (videoUrl.empty()) return {};
    // Regex("\\WHEIGHT=(\\d+)")
    int height = 0;
    size_t p = 0;
    while ((p = script.find("HEIGHT=", p)) != std::string::npos) {
        if (p > 0 && !std::isalnum((unsigned char)script[p - 1]) && script[p - 1] != '_') {
            height = std::atoi(script.c_str() + p + 7);
            if (height > 0) break;
        }
        p += 7;
    }
    Video v;
    v.url = videoUrl;
    v.quality = height;
    v.title = prefix + "Mp4Upload - " + (height > 0 ? std::to_string(height) + "p" : std::string("Unknown resolution"));
    v.referer = referer;
    return {v};
}

// ---- DoodStream (lib/doodextractor)
std::vector<Video> dood(const std::string& url, const std::string& prefix) {
    http::Response r = http::request("GET", url);
    if (r.status < 200 || r.status >= 300) return {};
    std::string newUrl = r.finalUrl.empty() ? url : r.finalUrl;
    std::string doodHost = http::originOf(newUrl);
    const std::string& content = r.body;
    if (!contains(content, "'/pass_md5/")) return {};
    std::string title = substringBefore(substringAfter(content, "<title>"), "</title>");
    int q = qualityOf(title);
    auto p = content.find("/pass_md5/");
    auto e = content.find('\'', p);
    std::string md5 = doodHost + content.substr(p, e == std::string::npos ? std::string::npos : e - p);
    std::string token = md5.substr(md5.rfind('/') + 1);
    std::string start = http::getText(md5, {{"Referer", newUrl}});
    long long expiry = (long long)std::time(nullptr) * 1000;
    Video v;
    v.url = start + randomAlnum(10) + "?token=" + token + "&expiry=" + std::to_string(expiry);
    v.quality = q;
    v.title = prefix + "Doodstream - " + (q > 0 ? std::to_string(q) + "p" : std::string("mirror"));
    v.referer = doodHost + "/";
    return {v};
}

// ---- StreamTape (lib/streamtapeextractor)
std::vector<Video> streamtape(const std::string& url, const std::string& prefix) {
    const std::string base = "https://streamtape.com/e/";
    std::string newUrl = url;
    if (!startsWith(url, base)) {
        std::vector<std::string> parts;
        size_t start = 0;
        while (true) {
            auto slash = url.find('/', start);
            parts.push_back(url.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        if (parts.size() < 5) return {};
        newUrl = base + parts[4];
    }
    html::Document doc(http::getText(newUrl), newUrl);
    const std::string target = "document.getElementById('robotlink')";
    std::string data = scriptWith(doc, {"document.getElementById('robotlink')"});
    if (data.empty()) return {};
    std::string script = substringAfter(data.substr(data.find(target)), target + ".innerHTML = '");
    Video v;
    v.url = "https:" + substringBefore(script, "'") + substringBefore(substringAfter(script, "+ ('xcd"), "'");
    v.title = prefix + "StreamTape";
    v.referer = "https://streamtape.com/";
    return {v};
}

// ---- Voe (lib/voeextractor), senza l'intercettore DDoS-Guard
std::vector<Video> voe(const std::string& url, const std::string& prefix) {
    std::string pageUrl = url;
    auto doc = std::make_unique<html::Document>(http::getText(url), url);
    std::string first = doc->selectFirst("script").data();
    auto rp = first.find("window.location.href");
    if (rp != std::string::npos) {
        std::string snippet = first.substr(rp, 1024);
        std::string target = substringBefore(substringAfter(snippet, "'"), "'");
        if (startsWith(target, "http")) {
            pageUrl = target;
            doc = std::make_unique<html::Document>(http::getText(target), target);
        }
    }
    std::string encoded;
    for (auto& s : doc->select("script[type=application/json]")) {
        encoded = trim(s.data());
        break;
    }
    if (encoded.empty()) return {};
    encoded = substringAfter(encoded, "[\"");
    auto last = encoded.rfind("\"]");
    if (last != std::string::npos) encoded = encoded.substr(0, last);

    // decryptF7: rot13 -> rimozione pattern -> base64 -> shift -3 -> reverse -> base64
    std::string s = encoded;
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = (char)('A' + (c - 'A' + 13) % 26);
        else if (c >= 'a' && c <= 'z') c = (char)('a' + (c - 'a' + 13) % 26);
    }
    for (const char* pat : {"@$", "^^", "~@", "%?", "*~", "!!", "#&"}) s = replaceAll(s, pat, "_");
    s = replaceAll(s, "_", "");
    s = base64Decode(s);
    for (auto& c : s) c = (char)((unsigned char)c - 3);
    std::reverse(s.begin(), s.end());
    s = base64Decode(s);
    json j = json::parse(s, nullptr, false);
    if (!j.is_object()) return {};

    std::vector<Video::Track> subs;
    if (j.contains("captions") && j["captions"].is_array())
        for (auto& c : j["captions"])
            if (c.is_object() && c.contains("file") && c["file"].is_string())
                subs.push_back({fixUrl(c["file"].get<std::string>(), pageUrl), c.value("label", "Subtitle")});

    std::vector<Video> out;
    if (j.contains("source") && j["source"].is_string()) out = hlsVideos(j["source"].get<std::string>(), "", prefix + "VOE - ", subs);
    if (j.contains("direct_access_url") && j["direct_access_url"].is_string()) {
        Video v;
        v.url = j["direct_access_url"].get<std::string>();
        v.title = prefix + "VOE - MP4";
        v.subtitles = subs;
        out.push_back(v);
    }
    return out;
}

// ---- VidMoly (lib/vidmolyextractor)
std::vector<Video> vidMoly(const std::string& iframeUrl, const std::string& prefix) {
    const std::string base = "https://vidmoly.biz";
    std::string fixed = iframeUrl;
    if (!startsWith(lower(iframeUrl), base)) {
        std::string path = http::pathOf(iframeUrl);
        fixed = base + (startsWith(path, "/") ? path : "/" + path);
    }
    http::Headers h = {{"Origin", base}, {"Referer", base + "/"}};
    html::Document doc(http::getText(fixed, h), fixed);
    std::string script = scriptWith(doc, {"sources"});
    std::vector<Video> out;
    for (auto& u : sourcesFiles(script)) {
        auto v = hlsVideos(u, base + "/", prefix + "VidMoly - ");
        out.insert(out.end(), v.begin(), v.end());
    }
    return out;
}

// ---- Vtube (animenosub/extractors/VtubeExtractor)
std::vector<Video> vtube(const std::string& url, const std::string& siteUrl, const std::string& prefix) {
    http::Headers h = {
        {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8"},
        {"Referer", siteUrl + "/"},
    };
    html::Document doc(http::getText(url, h), url);
    std::string packed = scriptWith(doc, {"function(p,a,c,k,e,d)"});
    std::string script = packed.empty() ? "" : unpacker::unpackAndCombine(packed);
    if (script.empty()) script = scriptWith(doc, {"sources"});
    std::vector<Video> out;
    for (auto& u : sourcesFiles(script)) {
        auto v = hlsVideos(u, url, prefix + "Vtube - ");
        out.insert(out.end(), v.begin(), v.end());
    }
    return out;
}

// ---- WolfStream (animenosub/extractors/WolfstreamExtractor)
std::vector<Video> wolfstream(const std::string& url, const std::string& prefix) {
    html::Document doc(http::getText(url), url);
    std::vector<Video> out;
    for (auto& u : sourcesFiles(scriptWith(doc, {"sources"}))) {
        Video v;
        v.url = u;
        v.title = prefix + "WolfStream";
        v.referer = url;
        out.push_back(v);
    }
    return out;
}

// ---- Moon / Filemoon "byse" (animenosub/extractors/MoonExtractor); AES-GCM decifrato come CTR (senza tag)
std::string b64url(const std::string& s) { return base64Decode(s); }

std::vector<Video> moon(const std::string& url, const std::string& siteUrl, const std::string& prefix) {
    const std::string ua = http::DEFAULT_UA;
    std::string host = http::hostOf(url);
    std::string path = substringBefore(substringBefore(http::pathOf(url), "?"), "#");
    while (!path.empty() && path.back() == '/') path.pop_back();
    std::string videoId = path.substr(path.rfind('/') + 1);
    if (host.empty() || videoId.empty()) return {};

    http::Headers dh = {{"Referer", siteUrl + "/"}, {"Origin", siteUrl}, {"User-Agent", ua}};
    http::Response dr = http::request("GET", "https://" + host + "/api/videos/" + videoId + "/embed/details", dh);
    json details = json::parse(dr.body, nullptr, false);
    if (!details.is_object()) return {};
    std::string embedUrl = details.value("embed_frame_url", "");
    if (embedUrl.empty()) return {};
    std::string embedHost = http::hostOf(embedUrl);

    std::string viewerId = randomHex(32), deviceId = randomHex(32);
    long long now = (long long)std::time(nullptr);
    std::string payload = "{\"viewer_id\":\"" + viewerId + "\",\"device_id\":\"" + deviceId +
                          "\",\"confidence\":0.93,\"iat\":" + std::to_string(now) + ",\"exp\":" + std::to_string(now + 600) + "}";
    std::string token = crypto::base64Encode(payload, true, false) + ".AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    json body = {{"fingerprint", {{"token", token}, {"viewer_id", viewerId}, {"device_id", deviceId}, {"confidence", 0.93}}}};

    std::string siteHost = siteUrl;
    if (startsWith(siteHost, "https://")) siteHost = siteHost.substr(8);
    http::Headers ph = {
        {"Referer", embedUrl},
        {"Origin", "https://" + embedHost},
        {"User-Agent", ua},
        {"X-Embed-Origin", siteHost},
        {"X-Embed-Parent", "https://" + host + "/e/" + videoId},
        {"X-Embed-Referer", siteUrl + "/"},
        {"Content-Type", "application/json; charset=utf-8"},
    };
    http::Response pr = http::request("POST", "https://" + embedHost + "/api/videos/" + videoId + "/embed/playback", ph, body.dump());
    json resp = json::parse(pr.body, nullptr, false);
    if (!resp.is_object()) return {};

    auto firstSource = [](const json& j) -> std::string {
        if (!j.contains("sources") || !j["sources"].is_array() || j["sources"].empty()) return "";
        const json& s = j["sources"][0];
        if (s.contains("url") && s["url"].is_string()) return s["url"].get<std::string>();
        if (s.contains("file") && s["file"].is_string()) return s["file"].get<std::string>();
        return "";
    };
    std::string masterUrl = firstSource(resp);
    if (masterUrl.empty() && resp.contains("playback") && resp["playback"].is_object()) {
        const json& pb = resp["playback"];
        std::vector<std::string> parts;
        if (pb.contains("key_parts") && pb["key_parts"].is_array())
            for (auto& k : pb["key_parts"])
                if (k.is_string()) parts.push_back(k.get<std::string>());
        std::string key;
        int ver = 0;
        if (pb.contains("version")) {
            if (pb["version"].is_string()) ver = std::atoi(pb["version"].get<std::string>().c_str());
            else if (pb["version"].is_number_integer()) ver = pb["version"].get<int>();
        }
        if (ver > 0 && (int)parts.size() >= ver) {
            key = b64url(parts[ver - 1]) + b64url(parts[parts.size() - ver]);
        } else if (parts.size() >= 2) {
            key = b64url(parts[0]) + b64url(parts[1]);
        } else {
            for (auto& k : parts) key += b64url(k);
        }
        std::string iv = b64url(pb.value("iv", ""));
        std::string data = b64url(pb.value("payload", ""));
        if ((key.size() == 16 || key.size() == 24 || key.size() == 32) && iv.size() == 12 && data.size() > 16) {
            // GCM con IV di 96 bit: il flusso cifrato parte dal contatore J0+1 = IV || 00000002
            std::string plain = crypto::aesCtr(data.substr(0, data.size() - 16), key, iv + std::string("\x00\x00\x00\x02", 4));
            json inner = json::parse(plain, nullptr, false);
            if (inner.is_object()) masterUrl = firstSource(inner);
        }
    }
    if (masterUrl.empty()) return {};
    std::string namePrefix = prefix + (containsCI(prefix, "moon") ? "" : "Moon - ");
    return hlsVideos(masterUrl, "https://" + embedHost + "/", namePrefix, {}, {}, ua);
}

// ---- Vatchus (chineseanime/extractors/VatchusExtractor)
std::vector<Video> vatchus(const std::string& url, const std::string& prefix) {
    html::Document doc(http::getText(url), url);
    std::string script = scriptWith(doc, {"document.write"});
    if (script.empty()) return {};
    std::string list = substringBefore(substringAfter(script, " = ["), "];");
    std::vector<int> numbers;
    size_t start = 0;
    while (start <= list.size()) {
        auto comma = list.find(',', start);
        std::string item = trim(replaceAll(list.substr(start, comma == std::string::npos ? std::string::npos : comma - start), "\"", ""));
        if (!item.empty()) {
            std::string d = digitsOnly(base64Decode(item));
            if (!d.empty()) numbers.push_back(std::atoi(d.c_str()));
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    if (numbers.empty()) return {};
    int offset = numbers.front() - 60;
    std::string decoded;
    for (int n : numbers) {
        int c = n - offset;
        if (c < 0x80) {
            decoded += (char)c;
        } else if (c < 0x800) {
            decoded += (char)(0xC0 | (c >> 6));
            decoded += (char)(0x80 | (c & 0x3F));
        } else {
            decoded += (char)(0xE0 | ((c >> 12) & 0x0F));
            decoded += (char)(0x80 | ((c >> 6) & 0x3F));
            decoded += (char)(0x80 | (c & 0x3F));
        }
    }
    decoded = trim(decoded);
    std::string playlist = substringBefore(substringAfter(decoded, "file:'"), "'");
    if (playlist.empty()) return {};
    std::vector<Video::Track> subs;
    std::string tracks = substringBefore(substringAfter(decoded, "tracks:["), "]");
    size_t p = 0;
    while ((p = tracks.find('{', p)) != std::string::npos) {
        auto e = tracks.find('{', p + 1);
        std::string obj = tracks.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = e == std::string::npos ? tracks.size() : e;
        if (!contains(obj, "\"kind\":\"captions\"")) continue;
        std::string file = substringBefore(substringAfter(obj, "file\":\""), "\"");
        if (!startsWith(file, "http")) continue;
        subs.push_back({file, substringBefore(substringAfter(obj, "label\":\""), "\"")});
    }
    return hlsVideos(playlist, url, prefix, subs);
}

// =============================================================================================== tema

enum class Site { Animenosub, AnimeKhor, LuciferDonghua, DonghuaStream, AnimeXin, ChineseAnime, LMAnime };

struct SiteInfo {
    Site site;
    const char* id;
    const char* name;
    const char* url;
    const char* lang;
    bool nsfw;
};

const char* MONTHS[] = {"january", "february", "march",     "april",   "may",      "june",
                        "july",    "august",   "september", "october", "november", "december"};

/** "MMMM d, yyyy" -> yyyymmdd (0 se non valido). */
long parseDate(const std::string& s) {
    std::string t = lower(trim(s));
    auto sp = t.find(' ');
    if (sp == std::string::npos) return 0;
    std::string m = t.substr(0, sp);
    int month = 0;
    for (int i = 0; i < 12; i++)
        if (m == MONTHS[i]) month = i + 1;
    if (!month) return 0;
    std::string rest = t.substr(sp + 1);
    int day = std::atoi(rest.c_str());
    int year = std::atoi(trim(substringAfter(rest, ",")).c_str());
    if (!day || !year) return 0;
    return (long)year * 10000 + month * 100 + day;
}

class AnimeStream : public Source {
  public:
    explicit AnimeStream(SiteInfo info) : info(info) {}

    std::string id() const override { return info.id; }
    std::string name() const override { return info.name; }
    std::string defaultBaseUrl() const override { return info.url; }
    std::string lang() const override { return info.lang; }
    bool nsfw() const override { return info.nsfw; }

    Page popular(int page) override {
        return listPage(animeListUrl() + "/?page=" + std::to_string(page) + "&order=popular", listNextSelector());
    }

    Page latest(int page) override {
        return listPage(animeListUrl() + "/?page=" + std::to_string(page) + "&order=update", listNextSelector());
    }

    Page search(const std::string& rawQuery, int page) override {
        std::string query = trim(rawQuery);
        // ricerca per URL diretto del sito (come PREFIX_SEARCH / URL https://...)
        if (startsWith(query, "https://") || startsWith(query, "http://")) {
            if (http::hostOf(query) != http::hostOf(baseUrl())) throw http::Error("URL non supportato");
            std::string path = http::pathOf(query);
            Details d = details(path);
            Page p;
            p.animes.push_back({path, d.title, d.thumbnail});
            return p;
        }
        if (query.empty())
            return listPage(animeListUrl() + "/?page=" + std::to_string(page) + "&status=&type=&sub=&order=", searchNextSelector());
        std::string url = info.site == Site::DonghuaStream
                              ? baseUrl() + "/pagg/" + std::to_string(page) + "/?s=" + http::urlEncode(query)
                              : baseUrl() + "/page/" + std::to_string(page) + "/?s=" + http::urlEncode(query);
        return listPage(url, searchNextSelector());
    }

    Details details(const std::string& animeUrl) override {
        std::string pageUrl = absolute(animeUrl);
        html::Document doc(http::getText(pageUrl), pageUrl);
        Details d;
        d.title = doc.selectFirst("h1.entry-title").text();
        if (d.title.empty()) throw http::Error("Pagina dell'anime non valida");
        html::Node img = doc.selectFirst("div.thumb > img, div.limage > img");
        if (img) d.thumbnail = imageUrl(doc, img);

        html::Node infos = doc.selectFirst("div.info-content, div.right ul.data");
        // genere: "div.genxed > a, li:contains(Genre:) a"
        std::vector<std::string> genres;
        auto addGenre = [&](const std::string& g) {
            if (!g.empty() && std::find(genres.begin(), genres.end(), g) == genres.end()) genres.push_back(g);
        };
        for (auto& a : infos.select("div.genxed > a")) addGenre(a.text());
        for (auto& li : infos.select("li"))
            if (containsCI(li.text(), "Genre:"))
                for (auto& a : li.select("a")) addGenre(a.text());
        for (auto& g : genres) d.genre += (d.genre.empty() ? "" : ", ") + g;

        std::string status = lower(trim(getInfo(infos, "Status")));
        if (status == "completed" || status == "completo") d.status = "Completato";
        else if (status == "ongoing" || status == "lançamento") d.status = "In corso";
        std::string fansub = trim(getInfo(infos, "Fansub"));
        std::string studio = trim(getInfo(infos, "Studio"));
        d.author = !fansub.empty() ? fansub : studio;

        std::string desc;
        std::string descSelector = info.site == Site::ChineseAnime ? ".entry-content" : ".entry-content[itemprop=description], .desc";
        auto descNodes = doc.select(descSelector);
        if (!descNodes.empty()) {
            std::string text = descNodes.back().text();
            if (info.site == Site::AnimeXin) text = animeXinDescription(text);
            desc += text + "\n\n";
        }
        std::string alt = doc.selectFirst(".alter").text();
        if (!trim(alt).empty()) desc += "Alternative name(s): " + alt + "\n";
        for (auto& sp : infos.select("div.spe > span")) desc += sp.text() + "\n";
        for (auto& li : infos.select("li"))
            if (li.selectFirst("b").valid()) desc += li.text() + "\n";
        d.description = trim(desc);

        d.episodes = episodes(doc);
        return d;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::string pageUrl = absolute(episodeUrl);
        html::Document doc(http::getText(pageUrl), pageUrl);
        std::vector<Video> out;
        static const char* allowedLangs[] = {"English", "Español", "Indonesian", "Portugués", "Türkçe", "العَرَبِيَّة", "ไทย"};
        for (auto& el : doc.select("select.mirror > option[data-index], ul.mirror a[data-em]")) {
            std::string name = el.text();
            if (info.site == Site::LMAnime) {
                bool ok = false;
                for (auto* l : allowedLangs)
                    if (contains(name, l)) ok = true;
                if (!ok) continue;
                name = substringBefore(name, " ");
            }
            try {
                std::string encoded = el.tag() == "option" ? el.attr("value") : el.attr("data-em");
                std::string hosterUrl = getHosterUrl(encoded);
                if (hosterUrl.empty()) continue;
                auto v = getVideoList(hosterUrl, name);
                out.insert(out.end(), v.begin(), v.end());
            } catch (const std::exception&) {
                // hoster non disponibile: si passa al successivo
            }
        }
        if (out.empty()) throw http::Error("Nessun video trovato");
        sortVideos(out);
        return out;
    }

  private:
    SiteInfo info;

    std::string animeListUrl() const { return baseUrl() + "/anime"; }

    std::string absolute(const std::string& url) const {
        if (startsWith(url, "http")) return url;
        return baseUrl() + (startsWith(url, "/") ? url : "/" + url);
    }

    std::string searchNextSelector() const {
        return info.site == Site::ChineseAnime ? "div.hpage > a.r" : "div.pagination a.next, div.hpage > a.r";
    }

    std::string listNextSelector() const {
        return info.site == Site::DonghuaStream ? "div.mrgn a.r" : searchNextSelector();
    }

    static std::string imageUrl(const html::Document& doc, const html::Node& img) {
        std::string u;
        if (img.hasAttr("data-src")) u = doc.absUrl(img, "data-src");
        else if (img.hasAttr("data-lazy-src")) u = doc.absUrl(img, "data-lazy-src");
        else if (img.hasAttr("srcset")) u = http::resolve(doc.url(), substringBefore(trim(img.attr("srcset")), " "));
        else u = doc.absUrl(img, "src");
        return substringBefore(u, "?resize");
    }

    Page listPage(const std::string& url, const std::string& nextSelector) {
        html::Document doc(http::getText(url), url);
        Page p;
        for (auto& a : doc.select("div.listupd article a.tip")) {
            Anime an;
            an.url = http::pathOf(doc.absUrl(a, "href"));
            html::Node tt = a.selectFirst("div.tt, div.ttl");
            an.title = ownText(tt);
            if (an.title.empty()) an.title = tt.selectFirst("h2").text();
            if (an.title.empty()) an.title = a.attr("title");
            html::Node img = a.selectFirst("img");
            if (img) an.thumbnail = imageUrl(doc, img);
            if (!an.url.empty() && !an.title.empty()) p.animes.push_back(an);
        }
        p.hasNextPage = doc.selectFirst(nextSelector).valid();
        return p;
    }

    /** Element.getInfo: primo span che contiene il testo -> testo del link oppure testo proprio. */
    static std::string getInfo(const html::Node& infos, const std::string& text) {
        for (auto& sp : infos.select("span")) {
            if (!containsCI(sp.text(), text)) continue;
            html::Node a = sp.selectFirst("a");
            return a.valid() ? a.text() : ownText(sp);
        }
        return "";
    }

    static std::string animeXinDescription(const std::string& description) {
        std::string low = lower(description);
        auto en = low.find("english");
        auto id = low.find("indonesia");
        if (en == std::string::npos || id == std::string::npos || en >= id) return description;
        // preferenza lingua predefinita "All Sub": sezione inglese
        std::string r = trim(description.substr(en + 7, id - en - 7));
        if (startsWith(r, ":")) r = trim(r.substr(1));
        return r;
    }

    std::vector<Episode> episodes(const html::Document& doc) {
        std::string selector = info.site == Site::LuciferDonghua ? "div.eplister > ul > li a" : "div.eplister > ul > li > a";
        struct Ep {
            Episode e;
            long date;
        };
        std::vector<Ep> list;
        for (auto& a : doc.select(selector)) {
            html::Node numNode = a.selectFirst(".epl-num");
            if (!numNode) continue;
            std::string epNum = numNode.text();
            Ep ep;
            ep.e.url = http::pathOf(doc.absUrl(a, "href"));
            if (info.site == Site::Animenosub) {
                std::string t = a.selectFirst("div.epl-title").text();
                ep.e.name = "Ep. " + epNum;
                if (!t.empty() && !containsCI(t, "Episode " + epNum)) ep.e.name += " " + t;
            } else {
                ep.e.name = "Episode " + epNum;
            }
            std::string numStr = epNum;
            if (info.site == Site::LuciferDonghua) numStr = trim(replaceAll(numStr, "[4K]", ""));
            ep.e.number = parseNumber(substringBefore(numStr, " "), 0);
            ep.date = parseDate(a.selectFirst(".epl-date").text());
            list.push_back(ep);
        }
        // opzione "Skip Preview episodes" attiva di default
        if (info.site == Site::LuciferDonghua && list.size() > 2) {
            std::stable_sort(list.begin(), list.end(), [](const Ep& x, const Ep& y) { return x.e.number > y.e.number; });
            if (list[0].date < list[1].date) list.erase(list.begin());
        }
        std::vector<Episode> out;
        for (auto& ep : list) {
            if (info.site == Site::DonghuaStream && containsCI(ep.e.name, "Preview")) continue;
            out.push_back(ep.e);
        }
        return out;
    }

    /** getHosterUrl: dato base64 con l'HTML dell'iframe, oppure URL di una pagina da scaricare. */
    std::string getHosterUrl(const std::string& encodedData) {
        std::string data = trim(encodedData);
        if (data.empty()) return "";
        bool isUrl = startsWith(data, "http://") || startsWith(data, "https://");
        std::string docUrl = isUrl ? data : "";
        std::string htmlText = isUrl ? http::getText(data) : base64Decode(data);
        html::Document doc(htmlText, docUrl);
        auto safeUrl = [&](const std::string& value) {
            if (startsWith(value, "http")) return value;
            if (startsWith(value, "//")) return "https:" + value;
            return http::resolve(docUrl.empty() ? baseUrl() + "/" : docUrl, value);
        };
        std::string iframeSelector = info.site == Site::LuciferDonghua ? "#embed_holder iframe" : "iframe";
        for (auto& f : doc.select(iframeSelector)) {
            std::string src = trim(f.attr("src"));
            if (!src.empty()) return safeUrl(src);
        }
        for (auto& m : doc.select("meta[itemprop=embedUrl]")) {
            std::string c = trim(m.attr("content"));
            if (!c.empty()) return safeUrl(c);
        }
        return "";
    }

    std::vector<Video> getVideoList(const std::string& url, const std::string& name) {
        std::string prefix = name + " - ";
        std::string site = baseUrl();
        auto any = [&](std::initializer_list<const char*> keys) {
            for (auto* k : keys)
                if (contains(url, k)) return true;
            return false;
        };
        switch (info.site) {
            case Site::Animenosub:
                if (any({"bysesayeveum", "filemoon", "fmoon", "moonembed"})) return moon(url, site, prefix);
                if (contains(url, "vidmoly")) return vidMoly(url, prefix);
                if (any({"streamwish", "swdyu"})) return streamWish(url, prefix, {{"Referer", site + "/"}});
                if (any({"vtbe", "vtube"})) return vtube(url, site, prefix);
                if (contains(url, "wolfstream")) return wolfstream(url, prefix);
                break;
            case Site::AnimeKhor:
                if (contains(url, "ahvsh.com") || lower(name) == "streamhide") return vidHide(url, prefix);
                if (contains(url, "ok.ru")) return okru(url, prefix);
                if (contains(url, "streamwish")) return streamWish(url, prefix, {{"Referer", site + "/"}});
                break;
            case Site::LuciferDonghua:
                if (contains(url, "ok.ru")) return okru(url, prefix);
                if (contains(url, "dailymotion")) return dailymotion(url, prefix + "Dailymotion - ");
                if (contains(url, "rumble")) return rumble(url, prefix);
                break;
            case Site::DonghuaStream:
                if (contains(url, "dailymotion")) return dailymotion(url, prefix + "Dailymotion - ");
                if (contains(url, "streamplay")) return streamPlay(url, prefix);
                if (contains(url, "ok.ru")) return okru(url, prefix);
                if (contains(url, "rumble")) return rumble(url, prefix);
                break;
            case Site::AnimeXin:
                if (contains(url, "ok.ru")) return okru(url, prefix);
                if (contains(url, "dailymotion")) return dailymotion(url, prefix + "Dailymotion - ");
                if (contains(url, "https://dood")) return dood(url, prefix);
                // gdriveplayer, youtube, vidstreaming: non supportati
                if (any({"gdriveplayer", "youtube.com", "vidstreaming"})) return {};
                break;
            case Site::ChineseAnime:
                if (contains(url, "dailymotion")) return dailymotion(url, prefix + "Dailymotion - ");
                if (contains(url, "embedwish")) return streamWish(url, prefix);
                if (contains(url, "vatchus")) return vatchus(url, prefix + "Vatchus - ");
                if (contains(url, "donghua.xyz/v/")) return vidHide(url, prefix);
                break;
            case Site::LMAnime: {
                std::string p = "(" + name + ") - ";
                if (contains(url, "dailymotion")) return dailymotion(url, "Dailymotion (" + name + ") - ");
                if (contains(url, "mp4upload")) return mp4upload(url, p);
                if (contains(url, "filelions")) return streamWish(url, p);
                return {};
            }
        }
        return genericHoster(url, prefix);
    }

    /** Hoster comuni non previsti dall'estensione originale (i siti cambiano spesso server). */
    std::vector<Video> genericHoster(const std::string& url, const std::string& prefix) {
        std::string u = lower(url);
        auto any = [&](std::initializer_list<const char*> keys) {
            for (auto* k : keys)
                if (contains(u, k)) return true;
            return false;
        };
        if (any({"ok.ru", "odnoklassniki"})) return okru(url, prefix);
        if (contains(u, "dailymotion")) return dailymotion(url, prefix + "Dailymotion - ");
        if (contains(u, "rumble.com/embed")) return rumble(url, prefix);
        if (contains(u, "mp4upload")) return mp4upload(url, prefix);
        if (any({"streamwish", "swdyu", "embedwish", "filelions", "wishembed", "strwish", "playerwish"}))
            return streamWish(url, prefix);
        if (any({"vidhide", "filelions", "streamhide", "ahvsh"})) return vidHide(url, prefix);
        if (any({"filemoon", "fmoon", "moonembed", "bysesayeveum"})) return moon(url, baseUrl(), prefix);
        if (contains(u, "vidmoly")) return vidMoly(url, prefix);
        if (any({"dood", "d0000d", "d000d", "ds2play", "dooood"})) return dood(url, prefix);
        if (any({"streamtape", "strtape", "stape"})) return streamtape(url, prefix);
        if (contains(u, "voe")) return voe(url, prefix);
        return {};
    }

    /** Ordinamento come sortVideos() con le preferenze predefinite. */
    void sortVideos(std::vector<Video>& list) const {
        std::vector<std::string> prefs;  // in ordine di priorita'
        switch (info.site) {
            case Site::Animenosub: prefs = {"SUB", "720p", "Moon"}; break;
            case Site::AnimeXin:
            case Site::ChineseAnime: prefs = {"720p", "All Sub"}; break;
            case Site::LMAnime: prefs = {"720p", "English"}; break;
            default: prefs = {"720p"}; break;
        }
        auto key = [&](const Video& v) {
            std::vector<int> k;
            for (auto& p : prefs) k.push_back(containsCI(v.title, p) ? 1 : 0);
            k.push_back(v.quality > 0 ? v.quality : qualityOf(v.title));
            return k;
        };
        std::stable_sort(list.begin(), list.end(), [&](const Video& a, const Video& b) { return key(a) > key(b); });
    }
};

}  // namespace

std::vector<std::shared_ptr<Source>> makeAnimeStreamSources() {
    static const SiteInfo sites[] = {
        {Site::Animenosub, "en.animenosub", "Animenosub", "https://animenosub.to", "en", true},
        {Site::AnimeKhor, "en.animekhor", "AnimeKhor", "https://animekhor.org", "en", false},
        {Site::LuciferDonghua, "en.luciferdonghua", "LuciferDonghua", "https://luciferdonghua.in", "en", false},
        {Site::DonghuaStream, "en.donghuastream", "DonghuaStream", "https://donghuastream.org", "en", false},
        {Site::AnimeXin, "all.animexin", "AnimeXin", "https://animexin.dev", "all", false},
        {Site::ChineseAnime, "all.chineseanime", "ChineseAnime", "https://www.chineseanime.in", "all", false},
        {Site::LMAnime, "all.lmanime", "LMAnime", "https://lmanime.com", "all", false},
    };
    std::vector<std::shared_ptr<Source>> out;
    for (auto& s : sites) out.push_back(std::make_shared<AnimeStream>(s));
    return out;
}

}  // namespace src
