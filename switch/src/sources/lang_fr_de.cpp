// Porting in C++ delle estensioni Aniyomi francesi (src/fr/*, esclusi i temi animestream/dooplay) e tedesche
// (src/de/*, escluso kinoking), piu' il tema multisrc "datalifeengine" (French Anime, Wiflix).
//
// Francese: Anime-Sama, Voiranime, FRAnime, OtakuFR, Vostfree, AnimeVostFr, AniSama, EmpireStreaming,
//           French Anime, Wiflix.
// Tedesco:  AniWorld, Serienstream, AnimeToast, Anime-Base, Anime-Stream, FilmPalast, Moflix-Stream, Movie4k, Kool.
//
// Estrattori privati (non presenti in extractors.hpp): Sibnet, Sendvid, VK, Uqload, YourUpload, MixDrop, Vudeo,
// Upstream, Lulu/"unpacker", Vido, StreamHub, StreamDav, Vidbm, Cdope, E-Player, VidCdn, Embed4Me, Vidoza, Metastream.

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <tuple>

#include "html/html.hpp"
#include "sources/extractors.hpp"
#include "sources/registry.hpp"
#include "util/crypto.hpp"
#include "util/unpacker.hpp"

namespace src {

namespace {

using json = nlohmann::json;
using namespace ext;
using DocPtr = std::unique_ptr<html::Document>;

const char* DESKTOP_UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/134.0.0.0 Safari/537.36";

// =============================================================================================== utilita'

std::string joinStr(const std::vector<std::string>& v, const std::string& sep = ", ") {
    std::string out;
    for (auto& s : v) {
        if (trim(s).empty()) continue;
        if (!out.empty()) out += sep;
        out += trim(s);
    }
    return out;
}

std::string joinText(const std::vector<html::Node>& nodes, const std::string& sep = ", ") {
    std::vector<std::string> v;
    for (auto& n : nodes) v.push_back(n.text());
    return joinStr(v, sep);
}

std::vector<std::string> split(const std::string& s, const std::string& sep) {
    std::vector<std::string> out;
    if (sep.empty()) return {s};
    size_t start = 0;
    while (true) {
        auto p = s.find(sep, start);
        out.push_back(s.substr(start, p == std::string::npos ? std::string::npos : p - start));
        if (p == std::string::npos) break;
        start = p + sep.size();
    }
    return out;
}

std::string substringAfterLast(const std::string& s, const std::string& d) {
    auto p = s.rfind(d);
    return p == std::string::npos ? s : s.substr(p + d.size());
}

std::string substringBeforeLast(const std::string& s, const std::string& d) {
    auto p = s.rfind(d);
    return p == std::string::npos ? s : s.substr(0, p);
}

std::string removeSurrounding(const std::string& s, char c) {
    if (s.size() >= 2 && s.front() == c && s.back() == c) return s.substr(1, s.size() - 2);
    return s;
}

std::string numStr(double n) {
    if (n == (long long)n) return std::to_string((long long)n);
    std::string s = std::to_string(n);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

/** Testo di uno script contenente tutte le stringhe indicate (come script:containsData(a):containsData(b)). */
std::string scriptAll(const html::Document& doc, std::initializer_list<const char*> needles) {
    for (auto& s : doc.select("script")) {
        std::string d = s.data();
        bool ok = true;
        for (auto* n : needles)
            if (d.find(n) == std::string::npos) ok = false;
        if (ok) return d;
    }
    return "";
}

/** Immagine con lazy-load (data-src, data-lazy-src, srcset, src), scartando i segnaposto data:image. */
std::string imgUrl(const html::Document& doc, const html::Node& img) {
    if (!img) return "";
    for (const char* a : {"data-src", "data-lazy-src", "srcset", "src"}) {
        if (!img.hasAttr(a)) continue;
        std::string v = trim(img.attr(a));
        if (v.empty() || startsWith(v, "data:image")) continue;
        if (std::string(a) == "srcset") v = substringBefore(v, " ");
        return http::resolve(doc.url(), v);
    }
    return "";
}

std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
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

/** Tutte le stringhe tra virgolette che iniziano con http(s) e contengono "needle". */
std::vector<std::string> quotedUrls(const std::string& text, const std::string& needle, char quote = '"') {
    std::vector<std::string> out;
    size_t pos = 0;
    while ((pos = text.find(quote, pos)) != std::string::npos) {
        auto end = text.find(quote, pos + 1);
        if (end == std::string::npos) break;
        std::string c = text.substr(pos + 1, end - pos - 1);
        if ((startsWith(c, "http") || startsWith(c, "//")) && contains(c, needle) && c.size() < 2048) {
            c = replaceAll(c, "\\/", "/");
            if (std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
            pos = end + 1;
        } else {
            pos = pos + 1;
        }
    }
    return out;
}

http::Headers mergeHeaders(http::Headers base, const http::Headers& extra) {
    for (auto& kv : extra) {
        bool found = false;
        for (auto& b : base)
            if (lower(b.first) == lower(kv.first)) {
                b.second = kv.second;
                found = true;
            }
        if (!found) base.push_back(kv);
    }
    return base;
}

Video simpleVideo(const std::string& url, const std::string& title, const std::string& referer = "",
                  const http::Headers& extra = {}) {
    Video v;
    v.url = url;
    v.title = title;
    v.quality = qualityOf(title);
    v.referer = referer;
    for (auto& kv : extra) {
        std::string k = lower(kv.first);
        if (k == "referer") v.referer = kv.second;
        else if (k == "user-agent") v.userAgent = kv.second;
        else v.headers.push_back(kv);
    }
    return v;
}

void append(std::vector<Video>& out, std::vector<Video> v) { out.insert(out.end(), v.begin(), v.end()); }

// =============================================================================================== hoster (copiati da lang_es.cpp)

std::vector<Video> yourUpload(const std::string& url, const std::string& prefix) {
    http::Headers h = {{"Referer", "https://www.yourupload.com/"}};
    html::Document doc(http::getText(url, h), url);
    std::string data = scriptWith(doc, {"jwplayerOptions"});
    if (data.empty()) return {};
    std::string file = substringBefore(substringAfter(data, "file: '"), "',");
    if (!startsWith(file, "http")) return {};
    return {simpleVideo(file, prefix + "YourUpload", "https://www.yourupload.com/")};
}

std::vector<Video> uqload(const std::string& url, const std::string& prefix) {
    const std::string base = "https://uqload.is/";
    std::string fixed = url;
    if (!startsWith(lower(url), base)) {
        auto p = url.find("://");
        auto slash = p == std::string::npos ? std::string::npos : url.find('/', p + 3);
        fixed = slash == std::string::npos ? url : base + url.substr(slash + 1);
    }
    html::Document doc(http::getText(fixed), fixed);
    std::string quality = trim(prefix).empty() ? "Uqload" : trim(prefix) + " Uqload";
    std::vector<std::string> sources;
    std::string script = scriptWith(doc, {"sources:"});
    if (!script.empty()) {
        std::string u = substringBefore(substringAfter(script, "sources: [\""), "\"");
        if (startsWith(u, "http") && u != script) sources.push_back(u);
    }
    if (sources.empty()) {
        std::string packed = scriptWith(doc, {"eval(function(p,a,c,k,e,d)"});
        if (packed.empty()) return {};
        std::string un = unpacker::unpackAndCombine(packed);
        for (auto& u : quotedUrls(un, ".m3u8")) sources.push_back(fixUrl(u));
        for (auto& u : quotedUrls(un, ".mp4")) sources.push_back(fixUrl(u));
    }
    std::vector<Video> out;
    for (auto& s : sources) {
        if (contains(s, ".m3u8")) append(out, hlsVideos(s, fixed, quality + " - "));
        else out.push_back(simpleVideo(s, quality, fixed));
    }
    return out;
}

std::vector<Video> mixDrop(const std::string& url, const std::string& prefix, const std::string& lang = "") {
    const std::string referer = "https://mixdrop.co/";
    http::Headers h = {{"Referer", referer}, {"User-Agent", DESKTOP_UA}};
    html::Document doc(http::getText(url, h), url);
    std::string packed = scriptAll(doc, {"eval", "MDCore"});
    if (packed.empty()) return {};
    std::string un = unpacker::unpackAndCombine(packed);
    if (un.empty()) return {};
    std::string videoUrl = "https:" + substringBefore(substringAfter(un, "Core.wurl=\""), "\"");
    Video v = simpleVideo(videoUrl, prefix + "MixDrop" + (lang.empty() ? "" : "(" + lang + ")"), referer);
    v.userAgent = DESKTOP_UA;
    if (contains(un, "Core.remotesub=\"")) {
        std::string sub = substringBefore(substringAfter(un, "Core.remotesub=\""), "\"");
        if (!trim(sub).empty()) v.subtitles.push_back({urlDecode(sub), "sub"});
    }
    return {v};
}

std::vector<Video> upstream(const std::string& url, const std::string& prefix) {
    html::Document doc(http::getText(url), url);
    std::string js = scriptWith(doc, {"eval"});
    if (js.empty()) return {};
    std::string un = unpacker::unpackAndCombine(js);
    std::string master = substringBefore(substringAfter(un, "{file:\""), "\"}");
    if (!startsWith(master, "http")) return {};
    return hlsVideos(master, "", prefix + "Upstream - ");
}

std::vector<Video> sendvid(const std::string& url, const std::string& prefix, const http::Headers& headers = {}) {
    html::Document doc(http::getText(url, headers), url);
    std::string master = doc.selectFirst("source#video_source").attr("src");
    if (master.empty()) return {};
    if (contains(master, ".m3u8")) return hlsVideos(master, url, prefix + "Sendvid:");
    std::string origin = "https://" + http::hostOf(url);
    return {simpleVideo(master, prefix + "Sendvid:default", origin + "/", {{"Origin", origin}})};
}

std::vector<Video> lulu(const std::string& url, const std::string& prefix) {
    http::Headers h = {{"Referer", "https://luluvdo.com/"}, {"Origin", "https://luluvdo.com"}};
    std::string html = http::getText(url, h);
    std::string m3u8;
    if (contains(html, "eval(function(p,a,c,k,e")) {
        html::Document doc(html, url);
        std::string un = unpacker::unpackAndCombine(scriptWith(doc, {"eval(function(p,a,c,k,e"}));
        if (contains(un, "sources:[{file:\"")) m3u8 = substringBefore(substringAfter(un, "sources:[{file:\""), "\"");
    } else if (contains(html, "sources: [{file:\"")) {
        m3u8 = substringBefore(substringAfter(html, "sources: [{file:\""), "\"");
    }
    if (!startsWith(m3u8, "http")) return {};
    // parametri: si mantengono quelli esistenti e si aggiungono i=0.3 e sp=0
    std::string baseUrl = substringBefore(m3u8, "?");
    std::string query = contains(m3u8, "?") ? substringAfter(m3u8, "?") : "";
    std::string fixed;
    for (auto& kv : split(query, "&")) {
        if (kv.empty()) continue;
        std::string k = substringBefore(kv, "=");
        if (k == "i" || k == "sp") continue;
        fixed += (fixed.empty() ? "" : "&") + kv;
    }
    fixed += std::string(fixed.empty() ? "" : "&") + "i=0.3&sp=0";
    std::string link = baseUrl + "?" + fixed;
    return hlsVideos(link, "https://luluvdo.com/", prefix + "Lulu - ");
}

std::vector<Video> vudeo(const std::string& url, const std::string& prefix) {
    html::Document doc(http::getText(url), url);
    std::string sources = scriptWith(doc, {"sources: ["});
    if (sources.empty()) return {};
    std::string referer = "https://" + http::hostOf(url) + "/";
    std::vector<Video> out;
    for (auto& u : split(replaceAll(substringBefore(substringAfter(sources, "sources: ["), "]"), "\"", ""), ",")) {
        std::string t = trim(u);
        if (startsWith(t, "https")) out.push_back(simpleVideo(t, prefix + "Vudeo", referer));
    }
    return out;
}

std::vector<Video> vk(const std::string& url, const std::string& prefix) {
    const std::string vkUrl = "https://vk.com";
    http::Headers doch = {{"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"},
                          {"Accept-Language", "en-US,en;q=0.9"}};
    http::Headers vh = {{"Origin", vkUrl}, {"Referer", vkUrl + "/"}};
    // id del video: oid=..&id=.., video-1_2, clip-1_2
    std::string videoId;
    {
        std::smatch m;
        static const std::regex r1("oid=(-?\\d+).*?id=(\\d+)"), r2("video(-?\\d+_\\d+)"), r3("clip(-?\\d+_\\d+)");
        std::string u = url.substr(0, std::min<size_t>(url.size(), 1024));
        if (std::regex_search(u, m, r1)) videoId = m[1].str() + "_" + m[2].str();
        else if (std::regex_search(u, m, r2)) videoId = m[1].str();
        else if (std::regex_search(u, m, r3)) videoId = m[1].str();
    }
    if (videoId.empty()) return {};
    auto parse = [&](const std::string& text) {
        std::vector<Video> out;
        std::set<int> seen;
        for (const std::string& key : {std::string("\"url"), std::string("\"mp4_")}) {
            size_t pos = 0;
            while ((pos = text.find(key, pos)) != std::string::npos) {
                size_t p = pos + key.size(), e = p;
                while (e < text.size() && std::isdigit((unsigned char)text[e])) e++;
                pos = p;
                if (e == p || text.compare(e, 3, "\":\"") != 0) continue;
                int q = std::atoi(text.substr(p, e - p).c_str());
                auto end = text.find('"', e + 3);
                if (end == std::string::npos) break;
                std::string u = replaceAll(text.substr(e + 3, end - e - 3), "\\/", "/");
                if (!startsWith(u, "http") || !seen.insert(q).second) continue;
                Video v = simpleVideo(u, prefix + std::to_string(q) + "p", vkUrl + "/", {{"Origin", vkUrl}});
                v.quality = q;
                out.push_back(v);
            }
        }
        std::stable_sort(out.begin(), out.end(), [](const Video& a, const Video& b) { return a.quality > b.quality; });
        return out;
    };
    try {
        http::Headers ah = doch;
        ah.push_back({"X-Requested-With", "XMLHttpRequest"});
        ah.push_back({"Content-Type", "application/x-www-form-urlencoded"});
        http::Response r = http::request("POST", vkUrl + "/al_video.php?act=show", ah, "act=show&al=1&video=" + videoId);
        if (r.status >= 200 && r.status < 300) {
            auto v = parse(substringAfter(r.body, "<!--"));
            if (!v.empty()) return v;
        }
    } catch (const std::exception&) {
    }
    std::string embed = url;
    if (!contains(url, "video_ext.php")) {
        auto parts = split(videoId, "_");
        if (parts.size() == 2) embed = vkUrl + "/video_ext.php?oid=" + parts[0] + "&id=" + parts[1] + "&autoplay=0";
    }
    http::Response r = http::request("GET", embed, doch);
    return parse(r.body);
}


/** Blocco JSON/JS bilanciato ({...} o [...]) che inizia al primo '{' o '[' dopo "marker" (rispetta le stringhe). */
std::string balancedAfter(const std::string& text, const std::string& marker, size_t from = 0) {
    size_t p = marker.empty() ? from : text.find(marker, from);
    if (p == std::string::npos) return "";
    p += marker.size();
    while (p < text.size() && text[p] != '{' && text[p] != '[') {
        if (!std::isspace((unsigned char)text[p]) && text[p] != '=' && text[p] != ':' && text[p] != '(') return "";
        p++;
    }
    if (p >= text.size()) return "";
    int depth = 0;
    char quote = 0;
    for (size_t i = p; i < text.size(); i++) {
        char c = text[i];
        if (quote) {
            if (c == '\\') i++;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') quote = c;
        else if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') {
            if (--depth == 0) return text.substr(p, i - p + 1);
        }
    }
    return "";
}

std::string trimChars(std::string s, const std::string& chars) {
    while (!s.empty() && chars.find(s.front()) != std::string::npos) s.erase(s.begin());
    while (!s.empty() && chars.find(s.back()) != std::string::npos) s.pop_back();
    return s;
}

/** Come json::value() ma senza eccezioni per campi null o di tipo diverso. */
template <typename T>
T jv(const json& j, const char* key, T def) {
    if (!j.is_object() || !j.contains(key) || j[key].is_null()) return def;
    try {
        return j[key].get<T>();
    } catch (const std::exception&) {
        return def;
    }
}
std::string jv(const json& j, const char* key, const char* def) { return jv<std::string>(j, key, std::string(def)); }

// =============================================================================================== utilita' (fr/de)

/** Elemento fratello successivo (Jsoup nextElementSibling). */
html::Node nextElement(const html::Node& n) {
    html::Node p = n.parent();
    if (!p) return html::Node();
    auto ch = p.children();
    for (size_t i = 0; i + 1 < ch.size(); i++)
        if (ch[i].raw() == n.raw()) return ch[i + 1];
    return html::Node();
}

/** Elemento fratello precedente (Jsoup previousElementSibling). */
html::Node prevElement(const html::Node& n) {
    html::Node p = n.parent();
    if (!p) return html::Node();
    auto ch = p.children();
    for (size_t i = 1; i < ch.size(); i++)
        if (ch[i].raw() == n.raw()) return ch[i - 1];
    return html::Node();
}

/** Esiste un fratello successivo che soddisfa "sel" (per selettori tipo "li.active ~ li"). */
bool hasFollowingSibling(const html::Node& n, const std::string& tag) {
    html::Node p = n.parent();
    if (!p) return false;
    bool after = false;
    for (auto& c : p.children()) {
        if (c.raw() == n.raw()) {
            after = true;
            continue;
        }
        if (after && (tag.empty() || c.tag() == tag)) return true;
    }
    return false;
}

/** Primo elemento di "sel" il cui testo contiene "needle" (sostituisce :contains()). */
html::Node firstWithText(const html::Node& root, const std::string& sel, const std::string& needle) {
    for (auto& n : root.select(sel))
        if (contains(n.text(), needle)) return n;
    return html::Node();
}

html::Node firstWithText(const html::Document& d, const std::string& sel, const std::string& needle) {
    return firstWithText(d.root(), sel, needle);
}

std::string lastChildText(const html::Node& n, const std::string& tag) {
    std::string out;
    for (auto& c : n.children())
        if (c.tag() == tag) out = c.text();
    return out;
}

std::string upper(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

/**
 * Ordina per preferenze (in ordine di priorita', confronto senza maiuscole) e poi per qualita' decrescente.
 * prefQuality: stringa cercata nel titolo (es. "1080").
 */
void sortByPrefs(std::vector<Video>& v, const std::vector<std::string>& prefs, const std::string& prefQuality = "") {
    auto key = [&](const Video& x) {
        std::vector<int> k;
        for (auto& p : prefs) k.push_back(!p.empty() && containsCI(x.title, p) ? 1 : 0);
        k.push_back(!prefQuality.empty() && contains(x.title, prefQuality) ? 1 : 0);
        k.push_back(x.quality ? x.quality : qualityOf(x.title));
        return k;
    };
    std::stable_sort(v.begin(), v.end(), [&](const Video& a, const Video& b) { return key(a) > key(b); });
}

void dedupeVideos(std::vector<Video>& v) {
    std::set<std::string> seen;
    std::vector<Video> out;
    for (auto& x : v)
        if (seen.insert(x.title + "|" + substringBefore(x.url, "?")).second) out.push_back(x);
    v.swap(out);
}

std::string statusText(const std::string& s) {
    std::string t = lower(s);
    if (contains(t, "en cours") || contains(t, "ongoing") || contains(t, "laufend") || contains(t, "airing"))
        return "In corso";
    if (contains(t, "termin") || contains(t, "complet") || contains(t, "abgeschlossen") || contains(t, "fin"))
        return "Completato";
    return "";
}

std::string formEncode(const std::vector<std::pair<std::string, std::string>>& kv) {
    std::string out;
    for (auto& p : kv) out += (out.empty() ? "" : "&") + http::urlEncode(p.first) + "=" + http::urlEncode(p.second);
    return out;
}

// =============================================================================================== hoster (nuovi)

std::vector<Video> sibnet(const std::string& url, const std::string& prefix) {
    html::Document doc(http::getText(url), url);
    std::string script = scriptWith(doc, {"player.src"});
    if (script.empty()) return {};
    std::string slug = substringBefore(substringAfter(substringAfter(substringAfter(script, "player.src"), "src:"), "\""), "\"");
    if (slug.empty()) return {};
    std::string videoUrl = contains(slug, "http") ? slug : "https://" + http::hostOf(url) + slug;
    return {simpleVideo(videoUrl, prefix + "Sibnet", url)};
}

std::vector<Video> vido(const std::string& url, const std::string& prefix) {
    std::string body = http::getText(url);
    auto p = body.find("master|");
    if (p == std::string::npos) return {};
    std::string id = substringBefore(body.substr(p + 7), "|");
    return hlsVideos("https://pink.vido.lol/hls/" + id + "/master.m3u8", "", prefix + "Vido - ");
}

std::vector<Video> streamHub(const std::string& url, const std::string& prefix) {
    std::string body = http::getText(url);
    auto p = body.find("urlset|"), q = body.find("width|");
    if (p == std::string::npos || q == std::string::npos) return {};
    std::string id = substringBefore(body.substr(p + 7), "|");
    std::string sub = substringBefore(body.substr(q + 6), "|");
    return hlsVideos("https://" + sub + ".streamhub.ink/hls/," + id + ",.urlset/master.m3u8", "", prefix + "StreamHub - ");
}

std::vector<Video> streamDav(const std::string& url, const std::string& prefix) {
    html::Document doc(http::getText(url), url);
    std::vector<Video> out;
    for (auto& s : doc.select("source")) {
        std::string src = doc.absUrl(s, "src");
        if (!startsWith(src, "http")) continue;
        out.push_back(simpleVideo(src, prefix + "StreamDav - (" + s.attr("label") + ")", url));
    }
    return out;
}

std::vector<Video> vidbm(const std::string& url, const std::string& prefix) {
    html::Document doc(http::getText(url), url);
    std::string js = scriptWith(doc, {"m3u8"});
    if (js.empty()) js = scriptWith(doc, {"mp4"});
    if (js.empty()) return {};
    std::string afterSource = substringAfter(js, "source");
    std::string master = substringBefore(substringAfter(afterSource, "file:\""), "\"");
    std::string quality = substringBefore(substringAfter(substringBefore(substringAfter(afterSource, "file"), "]"), "label:\""), "\"");
    if (!startsWith(master, "http")) return {};
    if (contains(master, "m3u8")) return hlsVideos(master, url, prefix + "Vidbm - ");
    return {simpleVideo(master, prefix + "Vidbm - " + quality, url)};
}

/** Pagina con script "eval(function(p,a,c,k,e,d)..." che contiene file:"...m3u8" (Lulustream, VTube, Upstream...). */
std::vector<Video> unpackerHls(const std::string& url, const std::string& name, const http::Headers& headers = {}) {
    html::Document doc(http::getText(url, headers), url);
    std::string script = scriptWith(doc, {"eval"});
    if (script.empty()) return {};
    std::string un = unpacker::unpackAndCombine(script);
    std::string playlist = substringBefore(substringAfter(un, "file:\""), "\"");
    if (!startsWith(playlist, "http")) return {};
    return hlsVideos(playlist, playlist, name + " - ");
}

std::vector<Video> cdope(const std::string& url) {
    const char* UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:109.0) Gecko/20100101 Firefox/109.0";
    std::string id = substringAfter(url, "/v/");
    http::Headers h = {{"Accept", "*/*"},
                       {"Content-Type", "application/x-www-form-urlencoded; charset=UTF-8"},
                       {"Origin", "https://cdopetimes.xyz"},
                       {"Referer", url},
                       {"User-Agent", UA},
                       {"X-Requested-With", "XMLHttpRequest"}};
    http::Response r = http::request("POST", "https://cdopetimes.xyz/api/source/" + id, h, "r=&d=cdopetimes.xyz");
    json j = json::parse(r.body, nullptr, false);
    std::vector<Video> out;
    if (!j.is_object() || !j.contains("data") || !j["data"].is_array()) return out;
    for (auto& f : j["data"]) {
        std::string file = jv(f, "file", "");
        if (!startsWith(file, "http")) continue;
        Video v = simpleVideo(file, jv(f, "label", "") + " (Cdope - " + jv(f, "type", "") + ")", "https://cdopetimes.xyz/");
        v.userAgent = UA;
        out.push_back(v);
    }
    return out;
}

std::vector<Video> eplayer(const std::string& url) {
    const std::string host = "https://e-player-stream.app";
    std::string id = substringAfterLast(url, "/");
    http::Headers h = {{"X-Requested-With", "XMLHttpRequest"},
                       {"Referer", host},
                       {"Origin", host},
                       {"Content-Type", "application/x-www-form-urlencoded"}};
    http::Response r = http::request("POST", host + "/player/index.php?data=" + id + "&do=getVideo", h,
                                     "hash=" + http::urlEncode(id) + "&r=");
    std::string master = replaceAll(substringBefore(substringAfter(r.body, "videoSource\":\""), "\""), "\\", "");
    if (!startsWith(master, "http")) return {};
    return hlsVideos(master, host, "E-Player - ", {}, {{"X-Requested-With", "XMLHttpRequest"}});
}

std::vector<Video> vidCdn(const std::string& url, const std::string& prefix) {
    if (!contains(url, "embeds.html")) return {};  // il server "sendvid" di vidcdn non funziona (come nel Kotlin)
    std::string id = http::queryParam(url, "id"), epid = http::queryParam(url, "epid");
    std::string body = http::getText("https://cdn2.vidcdn.xyz/sib2/" + id + "?epid=" + epid,
                                     {{"Referer", "https://msb.toonanime.xyz"}});
    json j = json::parse(body, nullptr, false);
    std::vector<Video> out;
    if (!j.is_object() || !j.contains("sources") || !j["sources"].is_array()) return out;
    for (auto& s : j["sources"]) {
        std::string file = jv(s, "file", "");
        if (file.empty()) continue;
        if (!startsWith(file, "http")) file = "https:" + file;
        out.push_back(simpleVideo(file, prefix + "Sibnet CDN"));
    }
    return out;
}

std::vector<Video> vidoza(const std::string& url, const std::string& title) {
    html::Document doc(http::getText(url), url);
    std::string script = scriptWith(doc, {"window.pData = {"});
    if (script.empty()) return {};
    std::string videoUrl = substringBefore(substringAfter(script, "sourcesCode: [{ src: \""), "\", type:");
    if (!startsWith(videoUrl, "http")) return {};
    return {simpleVideo(videoUrl, title, url)};
}

std::vector<Video> metastream(const std::string& url, const std::string& title) {
    html::Document doc(http::getText(url), url);
    std::string script = scriptWith(doc, {"sources: [{src:"});
    if (script.empty()) return {};
    std::string videoUrl = substringBefore(substringAfter(script, "sources: [{src: \""), "\", type:");
    if (!startsWith(videoUrl, "http")) return {};
    return {simpleVideo(videoUrl, title, url)};
}

// ---- Embed4Me (lettore usato da Anime-Sama)

std::string appendParams(const std::string& url, const std::vector<std::pair<std::string, std::string>>& params) {
    if (params.empty()) return url;
    std::string frag;
    std::string u = url;
    auto h = u.find('#');
    if (h != std::string::npos) {
        frag = u.substr(h);
        u = u.substr(0, h);
    }
    u += contains(u, "?") ? "&" : "?";
    u += formEncode(params);
    return u + frag;
}

json objectOf(const json& v) {
    if (v.is_object()) return v;
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        if (startsWith(s, "{")) {
            json j = json::parse(s, nullptr, false);
            if (j.is_object()) return j;
        }
    }
    return json();
}

bool isHex(const std::string& s) {
    if (s.empty() || s.size() % 2) return false;
    for (char c : s)
        if (!std::isxdigit((unsigned char)c)) return false;
    return true;
}

std::vector<Video> embed4me(const std::string& url, const std::string& prefix) {
    std::string id = trim(substringBefore(substringBefore(substringBefore(after(url, "#", ""), "&"), "?"), "/"));
    if (id.empty()) return {};
    std::string origin = http::originOf(substringBefore(url, "#"));
    if (!startsWith(origin, "http")) origin = "https://lpayer.embed4me.com";
    std::string api = origin + "/api/v1/video?id=" + http::urlEncode(id) + "&w=1920&h=1080&r=anime-sama.to";
    http::Headers ah = {{"Referer", "https://anime-sama.to/"}, {"Origin", origin}, {"Accept", "*/*"}};
    std::string raw = trim(http::getText(api, ah));
    if (raw.empty()) return {};
    raw = removeSurrounding(raw, '"');
    std::string jsonStr;
    auto decrypt = [](const std::string& hex) {
        return crypto::aesCbcDecrypt(crypto::fromHex(hex), "kiemtienmua911ca", "1234567890oiuytr");
    };
    if (startsWith(raw, "{")) {
        jsonStr = raw;
    } else if (isHex(raw)) {
        jsonStr = decrypt(raw);
    } else {
        json o = json::parse(raw, nullptr, false);
        for (const char* k : {"data", "payload", "result"})
            if (o.is_object() && o.contains(k) && o[k].is_string() && isHex(o[k].get<std::string>())) {
                jsonStr = decrypt(o[k].get<std::string>());
                break;
            }
    }
    json root = json::parse(jsonStr, nullptr, false);
    if (!root.is_object()) return {};

    json cfg = objectOf(root.contains("streamingConfig") ? root["streamingConfig"] : json());
    std::vector<std::string> order;
    if (cfg.is_object() && cfg.contains("order") && cfg["order"].is_array())
        for (auto& o : cfg["order"])
            if (o.is_string()) order.push_back(o.get<std::string>());
    json adjust = cfg.is_object() && cfg.contains("adjust") && cfg["adjust"].is_object() ? cfg["adjust"] : json::object();
    json pk = objectOf(root.contains("pk") ? root["pk"] : (root.contains("PK") ? root["PK"] : json()));
    std::string pkK = pk.is_object() && pk.contains("k") && pk["k"].is_string() ? pk["k"].get<std::string>() : "";
    std::string pkKx = pk.is_object() && pk.contains("kx") && pk["kx"].is_string() ? pk["kx"].get<std::string>() : "";

    std::vector<std::pair<std::string, std::string>> sources;
    for (const char* k : {"cf", "cfNative", "hlsVideoTiktok", "hlsVideoGoogle", "source"})
        if (root.contains(k) && root[k].is_string() && !trim(root[k].get<std::string>()).empty())
            sources.push_back({k, root[k].get<std::string>()});
    if (order.empty() && sources.empty()) {
        for (auto it = root.begin(); it != root.end(); ++it) {
            if (!it.value().is_string()) continue;
            std::string s = it.value().get<std::string>();
            if (startsWith(s, "http") && (contains(s, "/hls/") || contains(s, ".m3u8") || contains(s, "/v4/")))
                sources.push_back({it.key(), s});
        }
    }
    if (order.empty())
        for (auto& s : sources) order.push_back(s.first);

    std::vector<std::string> candidates;
    for (auto& name : order) {
        std::string u;
        for (auto& s : sources)
            if (s.first == name) u = s.second;
        if (trim(u).empty()) continue;
        json adj = adjust.contains(name) && adjust[name].is_object() ? adjust[name] : json();
        if (adj.is_object() && jv(adj, "disabled", false)) continue;
        if (adj.is_object() && adj.contains("params") && adj["params"].is_object()) {
            std::vector<std::pair<std::string, std::string>> params;
            for (auto it = adj["params"].begin(); it != adj["params"].end(); ++it)
                if (it.value().is_string() && !it.value().get<std::string>().empty())
                    params.push_back({it.key(), it.value().get<std::string>()});
            u = appendParams(u, params);
        }
        if (adj.is_object() && adj.contains("domain") && adj["domain"].is_string() && contains(u, "/hls/"))
            u = replaceAll(u, "/hls/", "/hlsmod/" + adj["domain"].get<std::string>() + "/");
        if (!startsWith(u, "http")) u = http::resolve(origin + "/", u);
        if (contains(u, "/v4/") && !pkK.empty() && !pkKx.empty()) u = appendParams(u, {{"k", pkK}, {"kx", pkKx}});
        if (std::find(candidates.begin(), candidates.end(), u) == candidates.end()) candidates.push_back(u);
    }
    std::string p = trim(prefix);
    std::string title = p.empty() ? "Embed4Me - " : p + " Embed4Me - ";
    std::vector<Video> out;
    for (auto& c : candidates) {
        try {
            append(out, hlsVideos(c, "", title, {}, {{"Referer", "https://anime-sama.to/"}, {"Origin", origin}}));
        } catch (const std::exception&) {
        }
    }
    return out;
}

// =============================================================================================== risolutore generico

const std::vector<const char*> VIDHIDE_HOSTS = {"smoothpre", "movearnpre", "minochinos", "morencius", "vidhide",
                                                "streamhide", "guccihide", "streamvid", "dhtpre", "peytonepre",
                                                "earnvids", "ryderjet", "dintezuvio", "callistanise", "ahvsh"};
const std::vector<const char*> STREAMWISH_HOSTS = {"streamwish", "wishembed", "strwish", "swdyu", "embedwish",
                                                   "playerwish", "hlswish", "sfastwish", "filelions", "wishonly"};
const std::vector<const char*> DOOD_HOSTS = {"dood", "d000d", "d0000d", "ds2play", "ds2video", "dsvplay", "myvidplay"};

bool anyOf(const std::string& url, const std::vector<const char*>& hosts) {
    for (auto* h : hosts)
        if (containsCI(url, h)) return true;
    return false;
}

/**
 * Riconosce l'hoster dall'URL e chiama l'estrattore giusto. Sostituisce l'UniversalExtractor (WebView)
 * delle estensioni originali: gli hoster sconosciuti restituiscono una lista vuota.
 */
std::vector<Video> resolveHost(const std::string& rawUrl, const std::string& prefix, const std::string& siteUrl = "") {
    std::string url = fixUrl(trim(rawUrl));
    if (!startsWith(url, "http")) return {};
    try {
        if (containsCI(url, "sibnet.ru")) return sibnet(url, prefix);
        if (containsCI(url, "vk.com") || containsCI(url, "vkvideo") || containsCI(url, "vk.ru")) return vk(url, prefix + "VK - ");
        if (containsCI(url, "sendvid")) return sendvid(url, prefix);
        if (containsCI(url, "vidmoly") || containsCI(url, "ansembed")) return vidMoly(url, prefix);
        if (anyOf(url, VIDHIDE_HOSTS)) return vidHide(url, prefix);
        if (containsCI(url, "uqload")) return uqload(url, prefix);
        if (containsCI(url, "yourupload")) return yourUpload(url, prefix);
        if (anyOf(url, DOOD_HOSTS)) return dood(url, prefix);
        if (containsCI(url, "voe.sx") || startsWith(lower(url), "https://voe")) return voe(url, prefix);
        if (containsCI(url, "ok.ru") || containsCI(url, "odnoklassniki")) return okru(url, prefix);
        if (containsCI(url, "streamtape") || containsCI(url, "shavetape")) return streamtape(url, prefix);
        if (containsCI(url, "filemoon") || containsCI(url, "moonplayer")) return moon(url, siteUrl, prefix + "Filemoon - ");
        if (containsCI(url, "mp4upload")) return mp4upload(url, prefix);
        if (anyOf(url, STREAMWISH_HOSTS)) return streamWish(url, prefix);
        if (containsCI(url, "mixdrop") || containsCI(url, "mxdrop")) return mixDrop(url, prefix);
        if (containsCI(url, "vudeo")) return vudeo(url, prefix);
        if (containsCI(url, "upstream")) return upstream(url, prefix);
        if (containsCI(url, "streamhub")) return streamHub(url, prefix);
        if (containsCI(url, "streamdav")) return streamDav(url, prefix);
        if (containsCI(url, "luluvdo") || containsCI(url, "lulustream")) return lulu(url, prefix);
        if (containsCI(url, "embed4me")) return embed4me(url, prefix);
        if (containsCI(url, "vadbam") || containsCI(url, "vidbm")) return vidbm(url, prefix);
        if (containsCI(url, "vidoza")) return vidoza(url, prefix + "Vidoza");
        if (containsCI(url, "vido.lol") || containsCI(url, "//vido")) return vido(url, prefix);
        std::string path = lower(substringBefore(url, "?"));
        if (endsWith(path, ".m3u8")) return hlsVideos(url, siteUrl.empty() ? "" : siteUrl + "/", prefix + "HLS - ");
        if (endsWith(path, ".mp4")) return {simpleVideo(url, prefix + "Direct", siteUrl.empty() ? "" : siteUrl + "/")};
    } catch (const std::exception&) {
    }
    return {};
}

std::vector<Video> ensureVideos(std::vector<Video> v) {
    if (v.empty()) throw http::Error("Nessun video trovato");
    return v;
}

// =============================================================================================== base

struct Info {
    const char* id;
    const char* name;
    const char* url;
    const char* lang;
    bool nsfw;
};

class FdSource : public Source {
  public:
    explicit FdSource(Info i) : info(i) {}
    std::string id() const override { return info.id; }
    std::string name() const override { return info.name; }
    std::string defaultBaseUrl() const override { return info.url; }
    std::string lang() const override { return info.lang; }
    bool nsfw() const override { return info.nsfw; }

  protected:
    Info info;
    bool ddosGuard = false;  // gestisce la pagina di controllo DDoS-Guard (403 + cookie __ddg2_)

    virtual http::Headers baseHeaders() const { return {{"Referer", baseUrl() + "/"}}; }

    std::string abs(const std::string& u) const {
        if (startsWith(u, "http://") || startsWith(u, "https://")) return u;
        if (startsWith(u, "//")) return "https:" + u;
        return baseUrl() + (startsWith(u, "/") ? u : "/" + u);
    }

    static std::string rel(const std::string& u) {
        if (startsWith(u, "http://") || startsWith(u, "https://")) return http::pathOf(u);
        return u;
    }

    http::Response req(const std::string& method, const std::string& url, const http::Headers& extra = {},
                       const std::string& body = "", bool follow = true) const {
        http::Headers h = mergeHeaders(baseHeaders(), extra);
        http::Response r = http::request(method, url, h, body, 30, follow);
        if (ddosGuard && r.status == 403 && containsCI(r.header("server"), "ddos-guard")) {
            try {
                std::string js = http::getText("https://check.ddos-guard.net/check.js");
                std::string path = substringBefore(after(js, "'", ""), "'");
                if (!path.empty()) {
                    http::request("GET", http::originOf(url) + path, {{"Referer", url}});
                    r = http::request(method, url, h, body, 30, follow);
                }
            } catch (const std::exception&) {
            }
        }
        return r;
    }

    void check(const http::Response& r) const {
        if (r.status < 200 || r.status >= 300)
            throw http::Error(std::string(info.name) + " ha risposto HTTP " + std::to_string(r.status));
    }

    std::string fetch(const std::string& url, const http::Headers& extra = {}) const {
        http::Response r = req("GET", url, extra);
        check(r);
        return std::move(r.body);
    }

    std::string postForm(const std::string& url, const std::string& body, const http::Headers& extra = {}) const {
        http::Headers h = mergeHeaders({{"Content-Type", "application/x-www-form-urlencoded"}}, extra);
        http::Response r = req("POST", url, h, body);
        check(r);
        return std::move(r.body);
    }

    DocPtr doc(const std::string& url, const http::Headers& extra = {}) const {
        http::Response r = req("GET", url, extra);
        check(r);
        return std::make_unique<html::Document>(r.body, r.finalUrl.empty() ? url : r.finalUrl);
    }

    /** URL finale dopo i redirect (come response.request.url). */
    std::string finalUrl(const std::string& url, const http::Headers& extra = {}) const {
        http::Response r = req("GET", url, extra);
        return r.finalUrl.empty() ? url : r.finalUrl;
    }

    Page list(const html::Document& d, const std::string& sel, const std::function<void(const html::Node&, Anime&)>& fill,
              const std::string& nextSel = "") const {
        Page p;
        std::set<std::string> seen;
        for (auto& el : d.select(sel)) {
            Anime a;
            fill(el, a);
            a.url = rel(a.url);
            a.title = trim(a.title);
            if (a.url.empty() || a.title.empty() || !seen.insert(a.url).second) continue;
            p.animes.push_back(a);
        }
        if (!nextSel.empty()) p.hasNextPage = d.selectFirst(nextSel).valid();
        return p;
    }

    static Episode episode(const std::string& url, const std::string& name, double number) {
        Episode e;
        e.url = url;
        e.name = name;
        e.number = number;
        return e;
    }
};


// =============================================================================================== Anime-Sama

/** Rimuove i commenti JS a blocco e le righe commentate con "//" (all'inizio della riga). */
std::string stripJsComments(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    bool lineStart = true;
    while (i < s.size()) {
        if (s.compare(i, 2, "/*") == 0) {
            auto e = s.find("*/", i + 2);
            if (e == std::string::npos) break;
            i = e + 2;
            continue;
        }
        if (lineStart && s.compare(i, 2, "//") == 0) {
            auto e = s.find('\n', i);
            if (e == std::string::npos) break;
            i = e;
            continue;
        }
        char c = s[i];
        if (c == '\n') lineStart = true;
        else if (!std::isspace((unsigned char)c)) lineStart = false;
        out += c;
        i++;
    }
    return out;
}

/** Stringhe tra apici (singoli, doppi o backtick) contenute in un array JS. */
std::vector<std::string> jsStrings(const std::string& arr) {
    std::vector<std::string> out;
    for (size_t i = 0; i < arr.size(); i++) {
        char q = arr[i];
        if (q != '\'' && q != '"' && q != '`') continue;
        std::string cur;
        size_t j = i + 1;
        for (; j < arr.size() && arr[j] != q; j++) {
            if (arr[j] == '\\' && j + 1 < arr.size()) j++;
            cur += arr[j];
        }
        out.push_back(trim(cur));
        i = j;
    }
    return out;
}

/** Variabili globali "epsN = [...]" di episodes.js, ordinate per N (al posto di QuickJS). */
std::vector<std::vector<std::string>> parseEpisodesJs(const std::string& raw) {
    std::string js = stripJsComments(raw);
    std::map<int, std::vector<std::string>> found;
    size_t pos = 0;
    while ((pos = js.find("eps", pos)) != std::string::npos) {
        size_t start = pos;
        pos += 3;
        if (start > 0 && (std::isalnum((unsigned char)js[start - 1]) || js[start - 1] == '_' || js[start - 1] == '.')) continue;
        size_t d = pos;
        while (d < js.size() && std::isdigit((unsigned char)js[d])) d++;
        if (d == pos) continue;
        int n = std::atoi(js.substr(pos, d - pos).c_str());
        size_t e = d;
        while (e < js.size() && std::isspace((unsigned char)js[e])) e++;
        if (e >= js.size() || js[e] != '=') continue;
        e++;
        while (e < js.size() && std::isspace((unsigned char)js[e])) e++;
        if (e >= js.size() || js[e] != '[') continue;
        std::string arr = balancedAfter(js, "", e);
        if (arr.empty()) continue;
        found[n] = jsStrings(arr);
        pos = e + arr.size();
    }
    std::vector<std::vector<std::string>> out;
    for (auto& kv : found) out.push_back(kv.second);
    return out;
}

class AnimeSama : public FdSource {
  public:
    AnimeSama() : FdSource({"fr.animesama", "Anime-Sama", "https://anime-sama.to", "fr", false}) {}

    Page popular(int page) override {
        if (page > 1) return {};
        auto d = doc(baseUrl() + "/");
        return list(*d, "#containerPepites > div a", [&](const html::Node& a, Anime& an) { card(*d, a, an); });
    }

    Page latest(int page) override {
        if (page > 1) return {};
        auto d = doc(baseUrl() + "/");
        Page p;
        std::set<std::string> seen;
        for (auto& div : d->select("#containerAjoutsAnimes > div")) {
            html::Node a = div.selectFirst("a");
            if (!a) continue;
            Anime an;
            card(*d, a, an);
            if (an.url.empty() || an.title.empty() || !seen.insert(an.url).second) continue;
            p.animes.push_back(an);
        }
        return p;
    }

    Page search(const std::string& q, int page) override {
        std::string url = baseUrl() + "/catalogue/?search=" + http::urlEncode(q) + "&page=" + std::to_string(page);
        auto d = doc(url);
        Page p = list(*d, "#list_catalog > div a", [&](const html::Node& a, Anime& an) { card(*d, a, an); });
        auto pag = d->select("#list_pagination a");
        std::string last = pag.empty() ? "" : trim(pag.back().text());
        p.hasNextPage = !last.empty() && last != std::to_string(page);
        return p;
    }

    Details details(const std::string& url) override {
        auto segs = split(trimChars(substringBefore(url, "#"), "/"), "/");
        if (segs.size() < 2) throw http::Error("URL non valido");
        std::string animeUrl = baseUrl() + "/" + segs[0] + "/" + segs[1];
        std::string onlySeason = segs.size() > 2 ? segs[2] : "";
        auto d = doc(animeUrl + "/");

        Details det;
        std::string animeName = d->selectFirst("h1").text();
        det.title = animeName;
        for (auto& lbl : d->select(".info-lbl"))
            if (contains(lbl.text(), "État")) {
                html::Node v = nextElement(lbl);
                std::string t = v.text();
                if (containsCI(t, "En cours")) det.status = "In corso";
                else if (containsCI(t, "Terminé")) det.status = "Completato";
                break;
            }
        html::Node cover = d->selectFirst("#coverOeuvre");
        det.thumbnail = cover ? d->absUrl(cover, "src") : "";
        if (det.thumbnail.empty()) det.thumbnail = d->selectFirst("meta[property=og:image]").attr("content");
        det.description = d->selectFirst("#synopsisText").text();
        det.genre = joinText(d->select(".genre-pill"));

        // panneauAnime("nom", "chemin") negli script (esclusi i commenti)
        std::string scripts;
        for (auto& s : d->select("script")) scripts += s.data() + "\n";
        std::vector<std::pair<std::string, std::string>> seasons;
        for (auto& line : split(stripJsComments(scripts), "\n")) {
            std::string l = trim(line);
            if (!startsWith(l, "panneauAnime(\"")) continue;
            std::string rest = l.substr(14);
            auto sep = rest.find("\", \"");
            if (sep == std::string::npos) continue;
            std::string name = rest.substr(0, sep);
            std::string stem = substringBefore(rest.substr(sep + 4), "\")");
            if (stem.empty()) continue;
            seasons.push_back({name, stem});
        }
        bool reduced = onlySeason.empty() && seasons.size() > 2;  // limita le richieste per le serie lunghe
        bool single = seasons.size() == 1 || !onlySeason.empty();

        for (auto& s : seasons) {
            std::string stemSeason = substringBefore(s.second, "/");
            if (!onlySeason.empty() && stemSeason != onlySeason) continue;
            std::string seasonUrl = animeUrl + "/" + trimChars(s.second, "/");
            try {
                if (containsCI(s.second, "film")) {
                    std::vector<std::string> names;
                    try {
                        std::string page = fetch(seasonUrl + "/");
                        for (auto& line : split(page, "\n")) {
                            std::string l = trim(line);
                            if (startsWith(l, "newSPF(\"")) names.push_back(substringBefore(l.substr(8), "\");"));
                        }
                    } catch (const std::exception&) {
                    }
                    auto eps = seasonEpisodes(seasonUrl, reduced);
                    for (size_t i = 0; i < eps.size(); i++) {
                        std::string t = i < names.size() ? names[i] : (eps.size() == 1 ? "Film" : "Film " + std::to_string(i + 1));
                        eps[i].name = t;
                        det.episodes.push_back(eps[i]);
                    }
                } else {
                    std::string display = startsWith(stemSeason, "saison") ? "Saison " + substringBefore(stemSeason.substr(6), "/")
                                                                         : substringBefore(s.first, " (");
                    auto eps = seasonEpisodes(seasonUrl, reduced);
                    for (auto& e : eps) {
                        if (!single) e.name = display + " - " + e.name;
                        det.episodes.push_back(e);
                    }
                }
            } catch (const std::exception&) {
            }
        }
        std::reverse(det.episodes.begin(), det.episodes.end());
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        json j = json::parse(episodeUrl, nullptr, false);
        if (!j.is_object()) throw http::Error("Episodio non valido");
        std::vector<Video> out;
        std::set<std::string> done;
        auto voices = jv(j, "v", json::array());
        auto players = jv(j, "p", json::array());
        for (size_t i = 0; i < players.size(); i++) {
            std::string voice = i < voices.size() && voices[i].is_string() ? voices[i].get<std::string>() : "";
            std::string prefix = voice.empty() ? "" : "(" + voice + ") ";
            for (auto& u : players[i]) {
                if (!u.is_string()) continue;
                std::string url = trim(u.get<std::string>());
                if (url.empty() || !done.insert(url).second) continue;
                auto v = resolveHost(url, prefix, baseUrl());
                if (anyOf(url, VIDHIDE_HOSTS)) dedupeByTitle(v);
                append(out, v);
            }
        }
        dedupeVideos(out);
        sortByPrefs(out, {"vostfr", "sibnet"}, "1080");
        return ensureVideos(out);
    }

  private:
    static void dedupeByTitle(std::vector<Video>& v) {
        std::set<std::string> seen;
        std::vector<Video> out;
        for (auto& x : v)
            if (seen.insert(x.title).second) out.push_back(x);
        v.swap(out);
    }

    void card(const html::Document& d, const html::Node& a, Anime& an) const {
        std::string href = d.absUrl(a, "href");
        auto segs = split(trimChars(substringBefore(http::pathOf(href), "?"), "/"), "/");
        if (segs.size() < 2) return;
        an.url = "/" + segs[0] + "/" + segs[1];
        html::Node img = a.selectFirst("img");
        an.title = a.selectFirst("h1").text();
        if (an.title.empty()) an.title = a.selectFirst("h2").text();
        if (an.title.empty()) an.title = img.attr("alt");
        if (an.title.empty()) an.title = a.text();
        an.thumbnail = img ? imgUrl(d, img) : "";
    }

    std::vector<std::vector<std::string>> fetchPlayers(const std::string& url) {
        try {
            http::Response r = req("GET", trimChars(url, "/") + "/episodes.js");
            if (r.status < 200 || r.status >= 300) return {};
            return parseEpisodesJs(r.body);
        } catch (const std::exception&) {
            return {};
        }
    }

    /** Episodi di una stagione: prova tutte le versioni (vostfr, vf, ...) come l'estensione originale. */
    std::vector<Episode> seasonEpisodes(const std::string& seasonUrl, bool reduced) {
        static const std::vector<std::string> VOICES = {"vostfr", "vf", "vf1", "vf2", "va", "var", "vcn", "vj", "vkr", "vqc"};
        static const std::vector<std::string> FEW = {"vostfr", "vf", "va"};
        std::string clean = trimChars(seasonUrl, "/");
        std::string current = substringAfterLast(clean, "/");
        bool isVoice = std::find(VOICES.begin(), VOICES.end(), current) != VOICES.end();
        std::string parent = isVoice ? substringBeforeLast(clean, "/") : clean;
        std::vector<std::string> paths = {current};
        for (auto& v : (reduced ? FEW : VOICES))
            if (std::find(paths.begin(), paths.end(), v) == paths.end()) paths.push_back(v);

        std::vector<std::string> names;
        std::vector<std::vector<std::vector<std::string>>> byVoice;
        size_t count = 0;
        for (auto& p : paths) {
            auto pl = fetchPlayers(parent + "/" + p);
            if (pl.empty()) continue;
            for (auto& x : pl) count = std::max(count, x.size());
            names.push_back(upper(p));
            byVoice.push_back(pl);
        }
        std::vector<Episode> eps;
        for (size_t i = 0; i < count; i++) {
            json v = json::array(), pls = json::array();
            for (size_t k = 0; k < byVoice.size(); k++) {
                json urls = json::array();
                for (auto& player : byVoice[k])
                    if (i < player.size() && !player[i].empty()) urls.push_back(player[i]);
                if (urls.empty()) continue;
                v.push_back(names[k]);
                pls.push_back(urls);
            }
            if (pls.empty()) continue;
            json e = {{"v", v}, {"p", pls}};
            eps.push_back(episode(e.dump(), "Episode " + std::to_string(i + 1), (double)(i + 1)));
        }
        return eps;
    }
};

// =============================================================================================== Voiranime

class Voiranime : public FdSource {
  public:
    Voiranime() : FdSource({"fr.voiranime", "Voiranime", "https://voir-anime.to", "fr", true}) {}

    Page popular(int page) override { return listing(baseUrl() + "/page/" + std::to_string(page) + "/?s&post_type=wp-manga&m_orderby=trending"); }
    Page latest(int page) override { return listing(baseUrl() + "/page/" + std::to_string(page) + "/?s&post_type=wp-manga&m_orderby=new-manga"); }
    Page search(const std::string& q, int page) override {
        return listing(baseUrl() + "/page/" + std::to_string(page) + "/?s=" + http::urlEncode(q) + "&post_type=wp-manga");
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details det;
        std::map<std::string, std::string> infoMap;
        for (auto& item : d->select(".post-content_item"))
            infoMap[lower(trim(item.selectFirst(".summary-heading").text()))] = trim(item.selectFirst(".summary-content").text());
        html::Node h1 = d->selectFirst(".post-title h1");
        det.title = h1 ? trim(ownText(h1)) : d->selectFirst("h1").text();
        if (det.title.empty()) det.title = d->selectFirst("h1").text();
        html::Node img = d->selectFirst(".summary_image img");
        det.thumbnail = imgUrl(*d, img);
        det.description = d->selectFirst(".description-summary .summary__content, .manga-excerpt").text();
        det.genre = joinText(d->select(".genres-content a"));
        det.author = infoMap["studios"];
        std::string st = lower(infoMap["status"]);
        if (st == "en cours") det.status = "In corso";
        else if (st == "terminé" || st == "termine" || st == "complété" || st == "completed") det.status = "Completato";

        std::string chaptersUrl = abs(url);
        if (!endsWith(chaptersUrl, "/")) chaptersUrl += "/";
        http::Response r = req("POST", chaptersUrl + "ajax/chapters/", {{"X-Requested-With", "XMLHttpRequest"}});
        check(r);
        html::Document ed(r.body, chaptersUrl);
        for (auto& li : ed.select("li.wp-manga-chapter")) {
            html::Node a = li.selectFirst("a");
            if (!a) continue;
            std::string href = ed.absUrl(a, "href");
            std::string text = a.text();
            std::string slug = substringAfterLast(trimChars(href, "/"), "/");
            std::string num = episodeNumber(slug, text);
            std::string sub;
            if (containsCI(slug, "vostfr") || containsCI(text, "VOSTFR")) sub = "VOSTFR";
            else if (containsCI(slug, "-vf") || containsCI(text, "VF")) sub = "VF";
            std::string name = "Épisode";
            if (!num.empty()) name += " " + num;
            if (!sub.empty()) name += " " + sub;
            det.episodes.push_back(episode(rel(href), name, num.empty() ? 0 : std::atof(replaceAll(num, ",", ".").c_str())));
        }
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        auto d = doc(abs(episodeUrl));
        std::string script = scriptWith(*d, {"thisChapterSources"});
        std::string obj = balancedAfter(script, "thisChapterSources");
        json j = json::parse(obj, nullptr, false);
        if (!j.is_object()) throw http::Error("Nessun video trovato");
        std::vector<Video> out;
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (!it.value().is_string()) continue;
            std::string html = it.value().get<std::string>();
            std::string iframe;
            auto p = html.find("<iframe");
            if (p != std::string::npos) {
                std::string tag = substringBefore(html.substr(p), ">");
                for (const char* key : {" src=\"", " src='"}) {
                    auto s = tag.find(key);
                    if (s == std::string::npos) continue;
                    char q = key[5];
                    iframe = substringBefore(tag.substr(s + 6), std::string(1, q));
                    break;
                }
            }
            if (iframe.empty()) continue;
            std::string server = it.key();
            if (startsWith(server, "LECTEUR")) server = server.substr(7);
            append(out, resolveHost(iframe, trim(server) + " - ", baseUrl()));
        }
        return ensureVideos(out);
    }

  private:
    Page listing(const std::string& url) {
        auto d = doc(url);
        return list(*d, "div.c-tabs-item__content", [&](const html::Node& el, Anime& a) {
            html::Node link = el.selectFirst("a[href*=/anime/]");
            if (!link) link = el.selectFirst("a");
            a.url = d->absUrl(link, "href");
            html::Node img = el.selectFirst("img");
            a.title = link.attr("title");
            if (trim(a.title).empty()) a.title = img.attr("alt");
            if (trim(a.title).empty()) a.title = el.selectFirst(".post-title").text();
            if (trim(a.title).empty()) a.title = link.text();
            a.thumbnail = imgUrl(*d, img);
        }, "a.nextpostslink");
    }

    static std::string episodeNumber(const std::string& slug, const std::string& text) {
        std::smatch m;
        static const std::regex epNum("-(\\d+(?:[.,]\\d+)?)-(?:vostfr|vf)", std::regex::icase);
        static const std::regex number("\\d+(?:[.,]\\d+)?");
        std::string s = slug.substr(0, std::min<size_t>(slug.size(), 512));
        if (std::regex_search(s, m, epNum)) return m[1].str();
        std::string last;
        for (auto it = std::sregex_iterator(s.begin(), s.end(), number); it != std::sregex_iterator(); ++it) last = it->str();
        if (!last.empty()) return last;
        std::string t = text.substr(0, std::min<size_t>(text.size(), 512));
        if (std::regex_search(t, m, number)) return m[0].str();
        return "";
    }
};

// =============================================================================================== FRAnime

struct FaEpisode {
    std::string title;
    std::vector<std::string> vo, vf;
};

struct FaAnime {
    std::string id, title, titleO, en, enJp, jaJp, poster, description, status, stem;
    double note = 0;
    std::vector<std::string> genres;
    std::vector<std::vector<FaEpisode>> seasons;
};

class FrAnime : public FdSource {
  public:
    FrAnime() : FdSource({"fr.franime", "FRAnime", "https://franime.fr", "fr", true}) {}

    Page popular(int page) override {
        auto db = database();
        std::vector<const FaAnime*> v;
        for (auto& a : *db) v.push_back(&a);
        std::stable_sort(v.begin(), v.end(), [](const FaAnime* a, const FaAnime* b) { return a->note > b->note; });
        return toPage(v, page);
    }

    Page latest(int page) override {
        auto db = database();
        std::vector<const FaAnime*> v;
        for (auto it = db->rbegin(); it != db->rend(); ++it) v.push_back(&*it);
        return toPage(v, page);
    }

    Page search(const std::string& q, int page) override {
        auto db = database();
        std::string lq = lower(trim(q));
        std::vector<const FaAnime*> v;
        for (auto& a : *db)
            if (contains(lower(a.title), lq) || contains(lower(a.titleO), lq) || contains(lower(a.en), lq) ||
                contains(lower(a.enJp), lq) || contains(lower(a.jaJp), lq) || contains(a.stem, trim(q)))
                v.push_back(&a);
        return toPage(v, page);
    }

    Details details(const std::string& url) override {
        auto db = database();
        const FaAnime* a = find(*db, url);
        std::string language = qp(url, "lang", "vo");
        int season = std::max(1, std::atoi(qp(url, "s", "1").c_str()));
        Details d;
        d.title = entryTitle(*a, season - 1, language);
        d.thumbnail = a->poster;
        d.description = a->description;
        d.genre = joinStr(a->genres);
        d.status = statusOf(*a, season);
        if (season - 1 < (int)a->seasons.size()) {
            auto& eps = a->seasons[season - 1];
            for (size_t i = 0; i < eps.size(); i++) {
                auto& players = language == "vo" ? eps[i].vo : eps[i].vf;
                if (players.empty()) continue;
                std::string name = eps[i].title.empty() ? "Episode " + std::to_string(i + 1) : eps[i].title;
                d.episodes.push_back(episode(url + "&ep=" + std::to_string(i + 1), name, (double)(i + 1)));
            }
        }
        std::stable_sort(d.episodes.begin(), d.episodes.end(), [](const Episode& x, const Episode& y) { return x.number > y.number; });
        return d;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        auto db = database();
        const FaAnime* a = find(*db, episodeUrl);
        int season = std::max(1, std::atoi(qp(episodeUrl, "s", "1").c_str()));
        int ep = std::max(1, std::atoi(qp(episodeUrl, "ep", "1").c_str()));
        std::string language = qp(episodeUrl, "lang", "vo");
        if (season - 1 >= (int)a->seasons.size() || ep - 1 >= (int)a->seasons[season - 1].size())
            throw http::Error("Episodio non trovato");
        auto& e = a->seasons[season - 1][ep - 1];
        auto& players = language == "vo" ? e.vo : e.vf;
        std::string base = apiUrl() + "/anime/" + a->id + "/" + std::to_string(season - 1) + "/" + std::to_string(ep - 1);
        std::vector<Video> out;
        for (size_t i = 0; i < players.size(); i++) {
            try {
                std::string playerUrl = trim(fetch(base + "/" + language + "/" + std::to_string(i)));
                playerUrl = removeSurrounding(playerUrl, '"');
                std::string name = lower(players[i]);
                if (name == "sendvid") append(out, sendvid(playerUrl, ""));
                else if (name == "sibnet") append(out, sibnet(playerUrl, ""));
                else if (name == "vk") append(out, vk(playerUrl, "VK - "));
                else if (name == "vidmoly") append(out, vidMoly(playerUrl, ""));
                else append(out, resolveHost(playerUrl, ""));
            } catch (const std::exception&) {
            }
        }
        return ensureVideos(out);
    }

  protected:
    http::Headers baseHeaders() const override { return {{"Referer", baseUrl() + "/"}, {"Origin", baseUrl()}}; }

  private:
    std::mutex mtx;
    std::shared_ptr<std::vector<FaAnime>> cache;

    std::string apiUrl() const { return "https://api." + http::hostOf(baseUrl()) + "/api"; }

    static std::string qp(const std::string& url, const std::string& name, const std::string& def) {
        std::string v = http::queryParam(url, name);
        return v.empty() ? def : v;
    }

    static std::string titleToUrl(const std::string& t) {
        std::string out;
        for (unsigned char c : t) {
            if (std::isalnum(c) && c < 128) out += (char)std::tolower(c);
            else if (c == ' ') out += '-';
        }
        return out;
    }

    static std::string str(const json& j, const char* k) {
        if (!j.is_object() || !j.contains(k) || j[k].is_null()) return "";
        if (j[k].is_string()) return j[k].get<std::string>();
        return j[k].dump();
    }

    static std::vector<std::string> players(const json& lang) {
        std::vector<std::string> out;
        if (lang.is_object() && lang.contains("lecteurs") && lang["lecteurs"].is_array())
            for (auto& p : lang["lecteurs"])
                if (p.is_string()) out.push_back(p.get<std::string>());
        return out;
    }

    std::shared_ptr<std::vector<FaAnime>> database() {
        std::lock_guard<std::mutex> lock(mtx);
        if (cache) return cache;
        auto db = std::make_shared<std::vector<FaAnime>>();
        {
            json j = json::parse(fetch(apiUrl() + "/animes/"), nullptr, false);
            if (!j.is_array()) throw http::Error("FRAnime: risposta non valida");
            for (auto& it : j) {
                FaAnime a;
                a.id = str(it, "id");
                a.title = str(it, "title");
                a.titleO = str(it, "titleO");
                if (it.contains("titles") && it["titles"].is_object()) {
                    a.en = str(it["titles"], "en");
                    a.enJp = str(it["titles"], "en_jp");
                    a.jaJp = str(it["titles"], "ja_jp");
                }
                a.poster = str(it, "affiche");
                a.description = str(it, "description");
                a.status = str(it, "status");
                a.note = it.contains("note") && it["note"].is_number() ? it["note"].get<double>() : 0;
                if (it.contains("themes") && it["themes"].is_array())
                    for (auto& g : it["themes"])
                        if (g.is_string()) a.genres.push_back(g.get<std::string>());
                if (it.contains("saisons") && it["saisons"].is_array())
                    for (auto& s : it["saisons"]) {
                        std::vector<FaEpisode> eps;
                        if (s.contains("episodes") && s["episodes"].is_array())
                            for (auto& e : s["episodes"]) {
                                FaEpisode fe;
                                fe.title = str(e, "title");
                                if (e.contains("lang") && e["lang"].is_object()) {
                                    if (e["lang"].contains("vo")) fe.vo = players(e["lang"]["vo"]);
                                    if (e["lang"].contains("vf")) fe.vf = players(e["lang"]["vf"]);
                                }
                                eps.push_back(std::move(fe));
                            }
                        a.seasons.push_back(std::move(eps));
                    }
                a.stem = titleToUrl(a.titleO);
                db->push_back(std::move(a));
            }
        }
        cache = db;
        return cache;
    }

    const FaAnime* find(const std::vector<FaAnime>& db, const std::string& url) const {
        std::string stem = substringAfterLast(substringBefore(url, "?"), "/");
        for (auto& a : db)
            if (a.stem == stem) return &a;
        throw http::Error("Anime non trovato");
    }

    static std::string entryTitle(const FaAnime& a, size_t season, const std::string& language) {
        std::string t = a.title + (a.seasons.size() > 1 ? " S" + std::to_string(season + 1) : "");
        if (season < a.seasons.size()) {
            bool vo = false, vf = false;
            for (auto& e : a.seasons[season]) {
                vo = vo || !e.vo.empty();
                vf = vf || !e.vf.empty();
            }
            if (vo && vf) t += language == "vo" ? " (VOSTFR)" : " (VF)";
        }
        return t;
    }

    static std::string statusOf(const FaAnime& a, int season) {
        if (season < (int)a.seasons.size()) return "Completato";
        std::string s = trim(a.status);
        if (s == "EN COURS") return "In corso";
        if (s == "TERMINÉ") return "Completato";
        return "";
    }

    Page toPage(const std::vector<const FaAnime*>& v, int page) const {
        Page p;
        size_t start = (size_t)std::max(0, page - 1) * 50;
        for (size_t i = start; i < v.size() && i < start + 50; i++) {
            const FaAnime& a = *v[i];
            for (size_t s = 0; s < a.seasons.size(); s++) {
                bool vo = false, vf = false;
                for (auto& e : a.seasons[s]) {
                    vo = vo || !e.vo.empty();
                    vf = vf || !e.vf.empty();
                }
                for (const char* l : {"vo", "vf"}) {
                    if ((std::string(l) == "vo" && !vo) || (std::string(l) == "vf" && !vf)) continue;
                    Anime an;
                    an.title = entryTitle(a, s, l);
                    an.thumbnail = a.poster;
                    an.url = "/anime/" + a.stem + "?lang=" + l + "&s=" + std::to_string(s + 1);
                    p.animes.push_back(an);
                }
            }
        }
        p.hasNextPage = v.size() > start + 50;
        return p;
    }
};

// =============================================================================================== OtakuFR

class OtakuFR : public FdSource {
  public:
    OtakuFR() : FdSource({"fr.otakufr", "OtakuFR", "https://otakufr.cc", "fr", false}) {}
    bool supportsLatest() const override { return false; }

    Page popular(int page) override { return listing(withPage(baseUrl() + "/en-cours", page)); }
    Page latest(int page) override { return popular(page); }
    Page search(const std::string& q, int page) override {
        std::string url = baseUrl() + "/toute-la-liste-affiches/" + (page > 1 ? "page/" + std::to_string(page) + "/" : "") +
                          "?q=" + http::urlEncode(q);
        return listing(url);
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details det;
        html::Node info = d->selectFirst("article.card div.episode");
        det.title = info.selectFirst("h1").text();
        if (det.title.empty()) det.title = d->selectFirst("h1").text();
        det.thumbnail = imgUrl(*d, info.selectFirst("img"));
        auto li = [&](const std::string& t) { return firstWithText(info, "li", t); };
        det.status = statusText(ownText(li("Statut")));
        html::Node genreLi = li("Genre:");
        std::vector<std::string> genres;
        for (auto& g : genreLi.select("ul li")) genres.push_back(g.text());
        det.genre = joinStr(genres);
        det.author = trim(ownText(li("Studio d'animation")));
        std::vector<std::string> paras;
        for (auto& p : info.children())
            if (p.tag() == "p" && !p.selectFirst("strong") && !trim(p.text()).empty()) paras.push_back(p.text());
        std::string desc = joinStr(paras, "\n\n") + "\n";
        for (const char* k : {"Autre Nom", "Auteur", "Réalisateur", "Type", "Sortie initiale", "Durée"}) {
            html::Node n = li(k);
            if (n) desc += "\n" + n.text();
        }
        det.description = trim(desc);

        for (auto& a : d->select("div.list-episodes > a")) {
            std::string t = trim(ownText(a));
            double num = 1;
            for (const char* marker : {" Vostfr", " VF"}) {
                auto p = t.find(marker);
                if (p == std::string::npos) continue;
                std::string before = t.substr(0, p);
                auto sp = before.rfind(' ');
                std::string n = sp == std::string::npos ? before : before.substr(sp + 1);
                if (!n.empty() && std::isdigit((unsigned char)n[0])) {
                    num = std::atof(n.c_str());
                    break;
                }
            }
            det.episodes.push_back(episode(rel(d->absUrl(a, "href")), t, num));
        }
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        auto d = doc(abs(episodeUrl));
        std::vector<Video> out;
        for (auto& f : d->select("div.tab-content iframe[src]")) {
            std::string url = d->absUrl(f, "data-src");
            if (url.empty() || !f.hasAttr("data-src")) url = d->absUrl(f, "src");
            try {
                if (contains(url, "parisanime.com")) {
                    html::Document nd(fetch(url, {{"X-Requested-With", "XMLHttpRequest"}}), url);
                    std::string res = nd.selectFirst("div[data-url]").attr("data-url");
                    if (res.empty()) continue;
                    url = startsWith(res, "//") ? "https:" + res : res;
                }
                if (startsWith(url, "https://doo") || contains(url, "d0000d")) append(out, dood(url, ""));
                else if (contains(url, "streamwish")) append(out, streamWish(url, ""));
                else if (contains(url, "sibnet.ru")) append(out, sibnet(url, ""));
                else if (contains(url, "vadbam")) append(out, vidbm(url, ""));
                else if (contains(url, "sendvid.com")) append(out, sendvid(url, ""));
                else if (contains(url, "ok.ru")) append(out, okru(url, ""));
                else if (contains(url, "upstream")) append(out, upstream(url, ""));
                else if (startsWith(url, "https://voe")) append(out, voe(url, ""));
            } catch (const std::exception&) {
            }
        }
        // qualita' preferita, poi risoluzione, poi server preferito (Streamwish)
        std::stable_sort(out.begin(), out.end(), [](const Video& a, const Video& b) {
            auto key = [](const Video& v) {
                return std::make_tuple(contains(v.title, "1080"), v.quality ? v.quality : qualityOf(v.title), containsCI(v.title, "streamwish"));
            };
            return key(a) > key(b);
        });
        return ensureVideos(out);
    }

  private:
    static std::string withPage(const std::string& url, int page) {
        return page <= 1 ? url : url + "/page/" + std::to_string(page);
    }

    Page listing(const std::string& url) {
        auto d = doc(url);
        Page p = list(*d, "div.list > article.card", [&](const html::Node& el, Anime& a) {
            html::Node link = el.selectFirst("a.episode-name");
            a.url = d->absUrl(link, "href");
            a.title = link.text();
            a.thumbnail = d->absUrl(el.selectFirst("img"), "src");
        });
        html::Node active = d->selectFirst("ul.pagination > li.active");
        p.hasNextPage = active && hasFollowingSibling(active, "li");
        return p;
    }
};

// =============================================================================================== Vostfree

class Vostfree : public FdSource {
  public:
    Vostfree() : FdSource({"fr.vostfree", "Vostfree", "https://vostfree.ws", "fr", false}) {}
    bool supportsLatest() const override { return false; }

    Page popular(int page) override { return listing(baseUrl() + "/films-vf-vostfr/page/" + std::to_string(page) + "/"); }
    Page latest(int page) override { return listing(baseUrl() + "/animes-vostfr/page/" + std::to_string(page) + "/"); }

    Page search(const std::string& q, int page) override {
        if (page > 1) return {};
        std::string body = "do=search&subaction=search&search_start=0&full_search=0&result_from=1&story=" + http::urlEncode(q);
        html::Document d(postForm(baseUrl() + "/index.php?do=search", body), baseUrl() + "/");
        Page p = list(d, "div#dle-content div.search-result, div#dle-content div.movie-poster", [&](const html::Node& el, Anime& a) { fill(d, el, a); });
        p.hasNextPage = false;
        return p;
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details det;
        det.title = d->selectFirst("div.slide-middle h1").text();
        det.description = trim(ownText(d->selectFirst("div.slide-desc")));
        det.thumbnail = d->absUrl(d->selectFirst("div.slide-poster img"), "src");
        std::vector<std::string> genres;
        for (auto& li : d->select("li.right")) {
            bool afterIcon = false;
            for (auto& c : li.children()) {
                if (c.tag() == "b" && contains(c.attr("class"), "fa-bookmark-o")) afterIcon = true;
                else if (afterIcon && c.tag() == "a") genres.push_back(c.text());
            }
        }
        det.genre = joinStr(genres);
        std::string epUrl = d->url().empty() ? abs(url) : d->url();
        epUrl = substringBefore(epUrl, "?");
        for (auto& o : d->select("select.new_player_selector option")) {
            std::string t = trim(o.text());
            if (t == "Film") {
                det.episodes.push_back(episode(rel(epUrl) + "?episode=1", "Film", 1));
            } else {
                int n = std::atoi(substringAfter(t, " ").c_str());
                det.episodes.push_back(episode(rel(epUrl) + "?episode=" + std::to_string(n), "Épisode " + std::to_string(n), n));
            }
        }
        std::reverse(det.episodes.begin(), det.episodes.end());
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::string epNum = substringAfter(episodeUrl, "=");
        auto d = doc(abs(episodeUrl));
        std::vector<Video> out;
        for (auto& div : d->select("div#buttons_" + epNum + " div")) {
            std::string server = lower(trim(div.text()));
            std::string id = div.attr("id");
            html::Node c = d->selectFirst("div#content_" + id);
            if (!c) continue;
            std::string content = trim(c.text());
            try {
                if (server == "doodstream") append(out, dood(content, ""));
                else if (server == "mixdrop") append(out, mixDrop(content, ""));
                else if (server == "ok") append(out, okru("https://ok.ru/videoembed/" + content, ""));
                else if (server == "sibnet") append(out, sibnet("https://video.sibnet.ru/shell.php?videoid=" + content, ""));
                else if (server == "uqload") append(out, uqload("https://uqload.io/embed-" + content + ".html", ""));
                else if (server == "voe") append(out, voe(content, ""));
                else if (server == "vudeo") append(out, vudeo(content, ""));
            } catch (const std::exception&) {
            }
        }
        sortByPrefs(out, {"Vudeo"});
        return ensureVideos(out);
    }

  private:
    void fill(const html::Document& d, const html::Node& el, Anime& a) const {
        html::Node link = el.selectFirst("a");
        a.url = d.absUrl(link, "href");
        a.title = link.text();
        a.thumbnail = d.absUrl(el.selectFirst("span.image img"), "src");
    }

    Page listing(const std::string& url) {
        auto d = doc(url);
        return list(*d, "div#dle-content div.movie-poster", [&](const html::Node& el, Anime& a) { fill(*d, el, a); }, "span.next-page");
    }
};

// =============================================================================================== AnimeVostFr

class AnimeVostFr : public FdSource {
  public:
    AnimeVostFr() : FdSource({"fr.animevostfr", "AnimeVostFr", "https://animevostfr.tv", "fr", false}) {}

    Page popular(int page) override { return listing(baseUrl() + "/filter-advance/page/" + std::to_string(page) + "/"); }
    Page latest(int page) override { return listing(baseUrl() + "/filter-advance/page/" + std::to_string(page) + "/?status=ongoing"); }
    Page search(const std::string& q, int page) override {
        return listing(baseUrl() + (page > 1 ? "/page/" + std::to_string(page) + "/" : "/") + "?s=" + http::urlEncode(q));
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details det;
        det.title = d->selectFirst("h1[itemprop=name]").text();
        html::Node statusP = firstWithText(*d, "div.mvici-right > p", "Statut");
        std::string st = lastChildText(statusP, "a");
        det.status = st == "Fin" ? "Completato" : (st == "En cours" ? "In corso" : "");
        det.genre = trim(substringAfter(firstWithText(*d, "div.mvici-left > p", "Genres").text(), "Genres:"));
        det.thumbnail = d->selectFirst("div.thumb > img").attr("data-lazy-src");
        det.description = d->selectFirst("div[itemprop=description]").text();

        std::string type = lastChildText(firstWithText(*d, "div.mvici-right > p", "Type"), "a");
        if (type == "MOVIE") {
            det.episodes.push_back(episode(d->url().empty() ? abs(url) : d->url(), "Movie", 1));
            return det;
        }
        for (auto& a : d->select("div#seasonss > div.les-title > a")) {
            std::string href = d->absUrl(a, "href");
            std::string src = contains(a.text(), "-episode-") ? a.text() : href;
            std::string number = substringBefore(substringAfterLast(src, "-episode-"), "-");
            number = substringBefore(number, "/");
            det.episodes.push_back(episode(rel(href), "Épisode " + number, std::atof(number.c_str())));
        }
        std::reverse(det.episodes.begin(), det.episodes.end());
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        auto d = doc(abs(episodeUrl));
        if (contains(d->selectFirst("title").text(), "Warning")) throw http::Error(d->selectFirst("body").text());
        std::string epId = substringAfter(d->selectFirst("link[rel=shortlink]").attr("href"), "?p=");
        std::vector<Video> out;
        for (auto& o : d->select("div.list-server > select > option")) {
            try {
                std::string link = trim(fetch(baseUrl() + "/ajax-get-link-stream/?server=" + http::urlEncode(o.attr("value")) + "&filmId=" + epId,
                                              {{"X-Requested-With", "XMLHttpRequest"}}));
                // comedyshow.to richiede una WebView (Cloudflare): non supportato
                if (contains(link, "cdopetimes.xyz")) append(out, cdope(link));
            } catch (const std::exception&) {
            }
        }
        sortByPrefs(out, {}, "720");
        return ensureVideos(out);
    }

  private:
    Page listing(const std::string& url) {
        auto d = doc(url);
        Page p = list(*d, "div.ml-item", [&](const html::Node& el, Anime& a) {
            html::Node link;
            for (auto& x : el.select("a"))
                if (x.selectFirst("img")) {
                    link = x;
                    break;
                }
            a.url = d->absUrl(link, "href");
            a.title = link.selectFirst("span.mli-info > h2").text();
            a.thumbnail = link.selectFirst("img").attr("data-original");
        });
        // "ul.pagination li:not(.active):last-child"
        for (auto& li : d->select("ul.pagination li")) {
            auto siblings = li.parent().children();
            if (!siblings.empty() && siblings.back().raw() == li.raw() && !contains(li.attr("class"), "active")) p.hasNextPage = true;
        }
        return p;
    }
};

// =============================================================================================== AniSama

class AniSama : public FdSource {
  public:
    AniSama() : FdSource({"fr.anisama", "AniSama", "https://v1.animesz.xyz", "fr", false}) {}

    Page popular(int page) override { return listing(baseUrl() + "/most-popular/?page=" + std::to_string(page)); }
    Page latest(int page) override { return listing(baseUrl() + "/recently-added/?page=" + std::to_string(page)); }
    Page search(const std::string& q, int page) override {
        return listing(baseUrl() + "/filter?keyword=" + http::urlEncode(q) + "&page=" + std::to_string(page));
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details det;
        html::Node det0 = d->selectFirst(".anime-detail");
        std::string name = det0.selectFirst(".dynamic-name").text();
        det.title = contains(name, " ") ? substringBeforeLast(name, " ") : name;
        det.thumbnail = det0.selectFirst(".film-poster-img").attr("src");
        det.author = meta(det0, "Studio");
        std::string st = meta(det0, "Status");
        det.status = st == "Terminer" ? "Completato" : (st == "En cours" ? "In corso" : "");
        det.description = det0.selectFirst(".shorting").text();
        det.genre = meta(det0, "Genre");

        std::string id = substringAfterLast(trimChars(substringBefore(url, "?"), "/"), "-");
        std::string body = fetch(baseUrl() + "/ajax/episode/list/" + id, {{"Referer", abs(url)}});
        json j = json::parse(body, nullptr, false);
        html::Document ed(j.is_object() ? jv(j, "html", "") : "", baseUrl() + "/");
        for (auto& el : ed.select(".ep-item")) {
            std::string epId = substringAfterLast(el.attr("href"), "=");
            det.episodes.push_back(episode("/ajax/episode/servers?episodeId=" + epId, el.attr("title"),
                                           std::atof(el.attr("data-number").c_str())));
        }
        std::reverse(det.episodes.begin(), det.episodes.end());
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::string body = fetch(baseUrl() + episodeUrl, {{"Referer", baseUrl() + "/"}});
        json j = json::parse(body, nullptr, false);
        html::Document d(j.is_object() ? jv(j, "html", "") : "", baseUrl() + "/");
        std::string epid = substringAfterLast(episodeUrl, "=");
        std::vector<Video> out;
        for (auto& s : d.select(".server-item")) {
            try {
                json pj = json::parse(fetch(baseUrl() + "/ajax/episode/sources?id=" + s.attr("data-id") + "&epid=" + epid), nullptr, false);
                std::string link = pj.is_object() ? jv(pj, "link", "") : "";
                if (link.empty()) continue;
                std::string prefix = "(" + upper(s.attr("data-type")) + ") ";
                if (contains(link, "toonanime.xyz")) append(out, vidCdn(link, prefix));
                else if (contains(link, "filemoon.sx")) append(out, moon(link, baseUrl(), prefix + " Filemoon - "));
                else if (contains(link, "sibnet.ru")) append(out, sibnet(link, prefix));
                else if (contains(link, "sendvid.com")) append(out, sendvid(link, prefix));
                else if (contains(link, "voe.sx")) append(out, voe(link, prefix));
                else if (contains(link, "d000d") || contains(link, "dood")) append(out, dood(link, prefix));
                else if (contains(link, "vidhide")) append(out, vidHide(link, prefix));
            } catch (const std::exception&) {
            }
        }
        return ensureVideos(out);
    }

  private:
    static std::string meta(const html::Node& root, const std::string& name) {
        for (auto& item : root.select(".item"))
            if (contains(item.selectFirst(".item-title").text(), name)) {
                std::vector<std::string> v;
                for (auto& c : item.children())
                    if (contains(" " + c.attr("class") + " ", " item-content ")) v.push_back(c.text());
                return joinStr(v, " ");
            }
        return "";
    }

    Page listing(const std::string& url) {
        auto d = doc(url);
        return list(*d, ".film_list article", [&](const html::Node& el, Anime& a) {
            a.title = el.selectFirst(".dynamic-name").text();
            a.thumbnail = el.selectFirst(".film-poster-img").attr("data-src");
            a.url = d->absUrl(el.selectFirst(".film-poster-ahref"), "href");
        }, ".ap__-btn-next a:not(.disabled)");
    }
};

// =============================================================================================== EmpireStreaming

class EmpireStreaming : public FdSource {
  public:
    EmpireStreaming() : FdSource({"fr.empirestreaming", "EmpireStreaming", "https://empire-stream.net", "fr", false}) {}

    Page popular(int page) override { return block(page, "Les plus vus"); }
    Page latest(int page) override { return block(page, "Ajout récents"); }

    Page search(const std::string& q, int page) override {
        auto items = searchItems();
        std::vector<const Item*> m;
        std::string lq = lower(q);
        for (auto& it : *items)
            if (contains(lower(it.title), lq)) m.push_back(&it);
        std::stable_sort(m.begin(), m.end(), [](const Item* a, const Item* b) { return a->title < b->title; });
        Page p;
        size_t start = (size_t)std::max(0, page - 1) * 30;
        for (size_t i = start; i < m.size() && i < start + 30; i++) {
            Anime a;
            a.title = m[i]->title;
            a.url = "/" + m[i]->urlPath;
            a.thumbnail = baseUrl() + "/images/medias/" + m[i]->thumb;
            p.animes.push_back(a);
        }
        p.hasNextPage = m.size() > start + 30;
        return p;
    }

    Details details(const std::string& url) override {
        http::Response r = req("GET", abs(url));
        check(r);
        html::Document d(r.body, r.finalUrl.empty() ? abs(url) : r.finalUrl);
        Details det;
        det.title = d.selectFirst("h3#title_media").text();
        std::string thumb = substringBefore(after(r.body, "backdrop\":\"", ""), "\"");
        if (!thumb.empty()) det.thumbnail = replaceAll(baseUrl() + "/images/medias/" + thumb, "\\", "");
        det.genre = joinText(d.select("div > button.bc-w.fs-12.ml-1.c-b"));
        det.description = d.selectFirst("div.target-media-desc p.content").text();

        std::string script = scriptWith(d, {"window.empire", "data:"});
        std::string data = substringBeforeLast(substringBefore(substringAfter(script, "data:"), "countpremiumaccount:"), ",");
        json j = json::parse(data, nullptr, false);
        if (!j.is_object()) return det;
        if (contains(d.url(), "serie") && j.contains("Saison") && j["Saison"].is_object()) {
            for (auto it = j["Saison"].begin(); it != j["Saison"].end(); ++it) {
                if (!it.value().is_array()) continue;
                for (auto& e : it.value()) {
                    int ep = jv(e, "episode", 1), season = jv(e, "saison", 1);
                    std::string name = "Saison " + std::to_string(season) + " Épisode " + std::to_string(ep) + " : " + jv(e, "title", "");
                    det.episodes.push_back(episode(encode(jv(e, "video", json::array())), name, season * 100000.0 + ep));
                }
            }
            // ordinati per (stagione, episodio) decrescenti; poi il numero torna quello dell'episodio
            std::stable_sort(det.episodes.begin(), det.episodes.end(), [](const Episode& a, const Episode& b) {
                return a.number > b.number;
            });
            for (auto& e : det.episodes) e.number = std::fmod(e.number, 100000.0);
        } else {
            det.episodes.push_back(episode(encode(jv(j, "Iframe", json::array())), jv(j, "Titre", det.title), 1));
        }
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::vector<Video> out;
        for (auto& part : split(episodeUrl, ", ")) {
            auto f = split(part, "|");
            if (f.size() < 3) continue;
            try {
                std::string body = fetch(baseUrl() + "/player_submit/" + f[0] + "/" + f[1]);
                std::string url = substringBefore(substringAfter(body, "window.location.href = \""), "\"");
                if (f[2] == "doodstream") append(out, dood(url, ""));
                else if (f[2] == "voe") append(out, voe(url, ""));
                else if (f[2] == "Eplayer") append(out, eplayer(url));
            } catch (const std::exception&) {
            }
        }
        sortByPrefs(out, {"Voe"}, "720p");
        return ensureVideos(out);
    }

  private:
    struct Item {
        std::string urlPath, title, thumb;
    };
    std::mutex mtx;
    std::shared_ptr<std::vector<Item>> items;

    static std::string encode(const json& videos) {
        std::string out;
        if (!videos.is_array()) return out;
        for (auto& v : videos) {
            std::string id = v.contains("id") ? (v["id"].is_string() ? v["id"].get<std::string>() : v["id"].dump()) : "";
            if (!out.empty()) out += ", ";
            out += id + "|" + jv(v, "version", "") + "|" + jv(v, "property", "");
        }
        return out;
    }

    std::shared_ptr<std::vector<Item>> searchItems() {
        std::lock_guard<std::mutex> lock(mtx);
        if (items) return items;
        auto v = std::make_shared<std::vector<Item>>();
        json j = json::parse(fetch(baseUrl() + "/api/views/contenitem"), nullptr, false);
        if (j.is_object() && j.contains("contentItem") && j["contentItem"].is_object()) {
            for (const char* k : {"films", "series"}) {
                auto& arr = j["contentItem"][k];
                if (!arr.is_array()) continue;
                for (auto& e : arr) {
                    Item it;
                    it.urlPath = jv(e, "urlPath", "");
                    it.title = jv(e, "title", "");
                    if (e.contains("image") && e["image"].is_array() && !e["image"].empty())
                        it.thumb = jv(e["image"][0], "path", "");
                    v->push_back(it);
                }
            }
        }
        items = v;
        return items;
    }

    Page block(int page, const std::string& title) {
        if (page > 1) return {};
        auto d = doc(baseUrl() + "/");
        Page p;
        std::set<std::string> seen;
        for (auto& b : d->select("div.block-forme")) {
            bool ok = false;
            for (auto& pp : b.select("p"))
                if (contains(pp.text(), title)) ok = true;
            if (!ok) continue;
            for (auto& el : b.select("div.content-card")) {
                Anime a;
                a.url = rel(d->absUrl(el.selectFirst("a.play"), "href"));
                a.thumbnail = baseUrl() + el.selectFirst("picture img").attr("data-src");
                a.title = el.selectFirst("h3.line-h-s, p.line-h-s").text();
                if (a.url.empty() || a.title.empty() || !seen.insert(a.url).second) continue;
                p.animes.push_back(a);
            }
        }
        return p;
    }
};

// =============================================================================================== tema DataLifeEngine

class DataLifeEngine : public FdSource {
  public:
    DataLifeEngine(Info i, std::string popularPath) : FdSource(i), popularPath(std::move(popularPath)) {}
    bool supportsLatest() const override { return false; }

    Page popular(int page) override { return listing(*doc(baseUrl() + popularPath + "page/" + std::to_string(page) + "/")); }
    Page latest(int page) override { return popular(page); }

    Page search(const std::string& q, int page) override {
        if (trim(q).size() < 4) throw http::Error("La recherche doit contenir au moins 4 caractères");
        std::string clean = replaceAll(http::urlEncode(q), "%20", "+");
        http::Headers h = {{"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8"},
                           {"Origin", baseUrl()}};
        std::string body, url;
        if (page == 1) {
            url = baseUrl() + "/";
            body = "do=search&subaction=search&story=" + clean;
        } else {
            url = baseUrl() + "/index.php?do=search";
            body = "do=search&subaction=search&search_start=" + std::to_string(page) + "&full_search=0&result_from=11&story=" + clean;
        }
        html::Document d(postForm(url, body, h), url);
        return listing(d);
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details det;
        det.title = d->selectFirst("h1").text();
        if (det.title.empty()) det.title = d->selectFirst("meta[property=og:title]").attr("content");
        html::Node img = d->selectFirst("div.mov-img img[src], div.fposter img[src], img[itemprop=image]");
        det.thumbnail = img ? d->absUrl(img, "src") : d->selectFirst("meta[property=og:image]").attr("content");
        det.description = d->selectFirst("div.mov-desc span[itemprop=description]").text();
        det.genre = joinText(d->select("div.mov-desc span[itemprop=genre] a"));
        det.episodes = episodes(*d);
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::vector<Video> out;
        for (auto& u : split(episodeUrl, ",")) {
            std::string url = trim(u);
            if (url.empty()) continue;
            if (contains(url, "waaw1.tv")) continue;
            append(out, resolveHost(url, "", baseUrl()));
        }
        sortByPrefs(out, {"Upstream"}, "720");
        return ensureVideos(out);
    }

  protected:
    std::string popularPath;
    virtual std::vector<Episode> episodes(const html::Document& d) = 0;

    Page listing(const html::Document& d) {
        Page p = list(d, "div#dle-content > div.mov", [&](const html::Node& el, Anime& a) {
            html::Node link = el.selectFirst("a[href]");
            a.url = http::pathOf(d.absUrl(link, "href"));
            a.url = substringBefore(a.url, "?");
            a.thumbnail = d.absUrl(el.selectFirst("img[src]"), "src");
            a.title = trim(link.text() + " " + el.selectFirst("span.block-sai").text());
        });
        // "span.navigation > span:not(.nav_ext) + a"
        for (auto& nav : d.select("span.navigation")) {
            auto ch = nav.children();
            for (size_t i = 0; i + 1 < ch.size(); i++)
                if (ch[i].tag() == "span" && !contains(ch[i].attr("class"), "nav_ext") && ch[i + 1].tag() == "a")
                    p.hasNextPage = true;
        }
        return p;
    }
};

class FrenchAnime : public DataLifeEngine {
  public:
    FrenchAnime() : DataLifeEngine({"fr.frenchanime", "French Anime", "https://french-anime.com", "fr", false}, "/animes-vostfr/") {}

  protected:
    std::vector<Episode> episodes(const html::Document& d) override {
        std::vector<Episode> out;
        html::Node eps = d.selectFirst("div.eps");
        if (!eps) return out;
        for (auto& it : split(eps.text(), " ")) {
            if (trim(it).empty()) continue;
            auto p = it.find('!');
            if (p == std::string::npos) continue;
            std::string num = it.substr(0, p);
            out.push_back(episode(it.substr(p + 1), "Episode " + num, std::atof(num.c_str())));
        }
        std::reverse(out.begin(), out.end());
        return out;
    }
};

class Wiflix : public DataLifeEngine {
  public:
    Wiflix() : DataLifeEngine({"fr.wiflix", "Wiflix", "https://flemmix.art", "fr", false}, "/serie-en-streaming/") {}

  protected:
    std::vector<Episode> episodes(const html::Document& d) override {
        struct E {
            Episode e;
            std::string lang;
        };
        std::vector<E> v;
        for (auto& div : d.select(".hostsblock div")) {
            auto links = div.select("a[href*=https]");
            if (links.empty()) continue;
            std::string cls = div.attr("class");
            std::string digits = digitsOnly(cls);
            double n = digits.empty() ? 0 : std::atof(digits.c_str());
            std::string lang = contains(cls, "vf") ? "VF" : "VOSTFR";
            std::string url;
            for (auto& a : div.select("a")) {
                std::string h = a.attr("href");
                if (startsWith(h, "/vd.php?u=")) h = h.substr(10);
                url += (url.empty() ? "" : ",") + h;
            }
            v.push_back({episode(url, "Episode " + std::to_string((int)n) + " (" + lang + ")", n), lang});
        }
        // come sort(): per lingua e numero, poi al contrario
        std::stable_sort(v.begin(), v.end(), [](const E& a, const E& b) {
            if (a.lang != b.lang) return a.lang > b.lang;
            return a.e.number > b.e.number;
        });
        std::vector<Episode> out;
        for (auto& e : v) out.push_back(e.e);
        return out;
    }
};


// =============================================================================================== AniWorld

class AniWorld : public FdSource {
  public:
    AniWorld() : FdSource({"de.aniworld", "AniWorld", "https://aniworld.to", "de", false}) { ddosGuard = true; }

    Page popular(int page) override {
        if (page > 1) return {};
        return listing(baseUrl() + "/beliebte-animes");
    }
    Page latest(int page) override {
        if (page > 1) return {};
        return listing(baseUrl() + "/neu");
    }

    Page search(const std::string& q, int page) override {
        if (page > 1) return {};
        http::Headers h = {{"Referer", baseUrl() + "/search"}, {"Origin", baseUrl()}, {"X-Requested-With", "XMLHttpRequest"}};
        json j = json::parse(fetch(baseUrl() + "/ajax/seriesSearch?keyword=" + http::urlEncode(q), h), nullptr, false);
        Page p;
        if (!j.is_array()) return p;
        for (auto& o : j) {
            if (!o.is_object() || !o.contains("name") || !o.contains("link")) continue;
            Anime a;
            a.title = replaceAll(replaceAll(jv(o, "name", ""), "<em>", ""), "</em>", "");
            a.url = "/anime/stream/" + jv(o, "link", "");
            std::string cover = replaceAll(jv(o, "cover", ""), "150x225", "220x330");
            if (!cover.empty()) a.thumbnail = http::resolve(baseUrl() + "/", cover);
            p.animes.push_back(a);
        }
        return p;
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details det;
        det.title = d->selectFirst("div.series-title h1 span").text();
        det.thumbnail = d->absUrl(d->selectFirst("div.seriesCoverBox img"), "data-src");
        det.genre = joinText(d->select("div.genres ul li"));
        det.description = d->selectFirst("p.seri_des").attr("data-full-description");
        for (auto& li : d->select("div.cast li"))
            if (contains(li.text(), "Produzent:")) {
                html::Node ul = li.selectFirst("ul");
                if (ul) {
                    det.author = joinText(ul.select("li"));
                    break;
                }
            }

        html::Node firstUl;
        for (auto& ul : d->select("#stream > ul")) {
            firstUl = ul;
            break;
        }
        for (auto& a : firstUl.select("li > a")) {
            std::string seasonUrl = d->absUrl(a, "href");
            try {
                auto sd = doc(seasonUrl);
                for (auto& tr : sd->select("table.seasonEpisodesList tbody tr")) {
                    html::Node link = tr.selectFirst("td.seasonEpisodeTitle a");
                    if (!link) continue;
                    std::string num = tr.attr("data-episode-season-id");
                    std::string name = joinText(link.select("span"), " ");
                    std::string href = link.attr("href");
                    Episode e;
                    e.url = href;
                    if (contains(href, "/filme")) {
                        e.name = "Film " + num + " : " + name;
                        e.number = parseNumber(num, -1);
                    } else {
                        std::string season = substringBefore(substringAfter(href, "staffel-"), "/episode");
                        e.name = "Staffel " + season + " Folge " + num + " : " + name;
                        e.number = parseNumber(tr.selectFirst("td meta").attr("content"), -1);
                    }
                    det.episodes.push_back(e);
                }
            } catch (const std::exception&) {
            }
        }
        std::reverse(det.episodes.begin(), det.episodes.end());
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        static const std::vector<std::string> HOSTERS = {"VOE", "Doodstream", "Streamtape", "Vidoza", "Filemoon", "Vidmoly"};
        auto d = doc(abs(episodeUrl));
        std::vector<Video> out;
        for (auto& li : d->select("ul.row li")) {
            std::string lang = language(li.attr("data-lang-key"));
            std::string redirect = d->absUrl(li.selectFirst("a.watchEpisode"), "href");
            std::string hoster = joinText(li.select("a h4"), " ");
            std::string matched;
            for (auto& h : HOSTERS)
                if (containsCI(hoster, h)) {
                    matched = h;
                    break;
                }
            if (matched.empty() || redirect.empty()) continue;
            try {
                std::string url = finalUrl(redirect);
                std::string prefix = "(" + lang + ") ";
                if (matched == "VOE") append(out, voe(url, prefix));
                else if (matched == "Doodstream") append(out, dood(url, prefix));
                else if (matched == "Streamtape") append(out, streamtape(url, prefix));
                else if (matched == "Vidoza") append(out, vidoza(url, prefix + "Vidoza"));
                else if (matched == "Filemoon") append(out, moon(url, baseUrl(), prefix + "Filemoon - "));
                else if (matched == "Vidmoly") append(out, vidMoly(url, prefix));
            } catch (const std::exception&) {
            }
        }
        sortByPrefs(out, {"VOE", "Deutscher Sub"});
        return ensureVideos(out);
    }

  private:
    static std::string language(const std::string& key) {
        if (contains(key, "3")) return "Deutscher Sub";
        if (contains(key, "1")) return "Deutscher Dub";
        if (contains(key, "2")) return "Englischer Sub";
        return "?";
    }

    Page listing(const std::string& url) {
        auto d = doc(url);
        return list(*d, "div.seriesListContainer div", [&](const html::Node& el, Anime& a) {
            html::Node link = el.selectFirst("a");
            html::Node h3 = el.selectFirst("h3");
            if (!link || !h3) return;
            a.title = h3.text();
            a.url = d->absUrl(link, "href");
            a.thumbnail = d->absUrl(link.selectFirst("img"), "data-src");
        });
    }
};

// =============================================================================================== Serienstream

class Serienstream : public FdSource {
  public:
    Serienstream() : FdSource({"de.serienstream", "Serienstream", "https://serienstream.to", "de", false}) { ddosGuard = true; }

    Page popular(int page) override {
        if (page > 1) return {};
        auto d = doc(baseUrl() + "/beliebte-serien");
        return list(*d, "a.show-card", [&](const html::Node& el, Anime& a) {
            std::string href = el.attr("href");
            a.url = contains(href, "/staffel") ? substringBefore(href, "/staffel") : href;
            html::Node img = el.selectFirst("img");
            a.title = img.attr("alt");
            if (trim(a.title).empty()) a.title = el.text();
            a.thumbnail = thumbUrl(*d, img);
        });
    }

    Page latest(int page) override {
        if (page > 1) return {};
        auto d = doc(baseUrl() + "/neue-episoden");
        return list(*d, "table.new-episodes-table tbody tr", [&](const html::Node& el, Anime& a) {
            html::Node link = el.selectFirst("td a[href*=/serie/]");
            if (!link) return;
            std::string href = link.attr("href");
            a.url = contains(href, "/staffel") ? substringBefore(href, "/staffel") : substringBefore(href, "/episode");
            a.title = link.text();
        });
    }

    Page search(const std::string& q, int page) override {
        if (page > 1) return {};
        json j = json::parse(fetch(baseUrl() + "/api/search/suggest?term=" + http::urlEncode(q)), nullptr, false);
        Page p;
        const json* arr = nullptr;
        bool isObj = j.is_object() && j.contains("shows") && j["shows"].is_array();
        if (isObj) arr = &j["shows"];
        else if (j.is_array()) arr = &j;
        if (!arr) return p;
        for (auto& o : *arr) {
            if (!o.is_object()) continue;
            std::string link = isObj ? jv(o, "url", "") : jv(o, "link", "");
            std::string title = isObj ? jv(o, "name", "") : jv(o, "title", "");
            if (link.empty() || title.empty()) continue;
            Anime a;
            a.title = replaceAll(replaceAll(title, "<em>", ""), "</em>", "");
            a.url = rel(link);
            for (const char* k : {"image", "cover", "thumbnail"})
                if (o.contains(k) && o[k].is_string()) {
                    a.thumbnail = http::resolve(baseUrl() + "/", o[k].get<std::string>());
                    break;
                }
            p.animes.push_back(a);
        }
        return p;
    }

    Details details(const std::string& url) override {
        http::Response r = req("GET", abs(url));
        check(r);
        std::string pageUrl = r.finalUrl.empty() ? abs(url) : r.finalUrl;
        html::Document d(r.body, pageUrl);
        Details det;
        det.title = d.selectFirst("h1").text();
        if (det.title.empty()) det.title = trim(substringBefore(d.selectFirst("title").text(), " |"));
        html::Node img = d.selectFirst("div.show-cover-mobile img");
        if (!img) img = d.selectFirst("div.col-5 picture img");
        if (!img) img = d.selectFirst("img[data-src*=/media/images/channel/]");
        if (!img) img = d.selectFirst("picture img");
        det.thumbnail = thumbUrl(d, img);
        det.genre = joinText(d.select("a[href^=/genre/]"));
        det.description = d.selectFirst("div.series-description span.description-text").text();
        if (det.description.empty()) det.description = d.selectFirst("div.series-description").text();
        if (det.description.empty()) det.description = d.selectFirst("meta[name=description]").attr("content");
        for (const char* sel : {"[id^=pg-produzenten-] a", "[id^=pg-regisseure-] a", "[id^=pg-besetzung-] a"}) {
            det.author = joinText(d.select(sel));
            if (!det.author.empty()) break;
        }
        if (det.author.empty())
            for (const char* k : {"Produzent", "Regisseur", "Besetzung"}) {
                for (auto& li : d.select("li.series-group"))
                    if (contains(li.selectFirst("strong").text(), k)) det.author = joinText(li.select("a"));
                if (!det.author.empty()) break;
            }
        if (det.author.empty()) det.author = joinText(d.select("div.chips-wrap a"));
        std::string years = d.selectFirst("p.small.text-muted.mb-2").text();
        if (!years.empty() && years.size() < 1024) {
            std::smatch m;
            static const std::regex yr("((?:19|20)\\d{2})\\s*[-\xE2\x80\x93]+\\s*((?:19|20)\\d{2}|NA)");
            if (std::regex_search(years, m, yr)) det.status = m[2].str() == "NA" ? "In corso" : "Completato";
        }

        auto seasonLinks = d.select("nav#season-nav a[data-season-pill]");
        if (seasonLinks.empty()) seasonLinks = d.select("a[data-season-pill]");
        if (seasonLinks.empty()) {
            for (auto& ul : d.select("#stream > ul")) {
                seasonLinks = ul.select("li > a");
                break;
            }
        }
        if (seasonLinks.empty()) {
            for (auto& row : d.select("table.episode-table tbody tr.episode-row")) {
                Episode e;
                if (parseRow(row, pageUrl, e)) det.episodes.push_back(e);
            }
            std::reverse(det.episodes.begin(), det.episodes.end());
            return det;
        }
        for (auto& link : seasonLinks) {
            std::string seasonUrl = d.absUrl(link, "href");
            if (seasonUrl.empty()) continue;
            DocPtr owned;
            const html::Document* sd = &d;
            if (seasonUrl != pageUrl) {
                try {
                    owned = doc(seasonUrl);
                    sd = owned.get();
                } catch (const std::exception&) {
                    continue;
                }
            }
            auto rows = sd->select("table.episode-table tbody tr.episode-row");
            if (!rows.empty()) {
                for (auto& row : rows) {
                    Episode e;
                    if (parseRow(row, seasonUrl, e)) det.episodes.push_back(e);
                }
            } else {
                std::string seasonNum = substringBefore(after(seasonUrl, "/staffel-", ""), "/");
                if (seasonNum.empty()) seasonNum = "1";
                for (auto& a : sd->select("nav#episode-nav a[href*=/episode-]")) {
                    std::string t = trim(a.text());
                    int n = std::atoi(t.c_str());
                    if (n <= 0) n = 1;
                    std::string href = sd->absUrl(a, "href");
                    det.episodes.push_back(episode(rel(href.empty() ? a.attr("href") : href),
                                                   "Staffel " + seasonNum + " Folge " + std::to_string(n), n));
                }
            }
        }
        std::reverse(det.episodes.begin(), det.episodes.end());
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::string pageUrl = abs(episodeUrl);
        auto d = doc(pageUrl);
        std::string csrf = d->selectFirst("meta[name=csrf-token]").attr("content");
        html::Node tokenInput = d->selectFirst("input[name=_token]");
        std::string formToken = tokenInput ? tokenInput.attr("value") : csrf;
        auto buttons = d->select("button.link-box[data-play-url]");
        if (buttons.empty()) buttons = d->select("div.link-wrapper button[data-play-url]");
        if (buttons.empty()) buttons = d->select("button[data-play-url]");
        std::string altcha;
        try {
            altcha = fetchAltcha(pageUrl, csrf);
        } catch (const std::exception&) {
        }
        std::vector<Video> out;
        for (auto& btn : buttons) {
            std::string provider = trim(btn.attr("data-provider-name"));
            std::string langId = trim(btn.attr("data-language-id"));
            std::string langLabel = trim(btn.attr("data-language-label"));
            std::string playUrl = trim(btn.attr("data-play-url"));
            if (playUrl.empty()) continue;
            std::string language = lang(langId);
            if (language.empty()) language = lang(langLabel);
            if (language.empty()) language = langLabel;
            std::string rawT = contains(playUrl, "t=") ? substringBefore(substringAfter(playUrl, "t="), "&") : playUrl;
            if (rawT.empty()) continue;
            std::string t = urlDecode(replaceAll(rawT, "+", "%2B"));
            if (t.empty()) continue;
            try {
                std::string hoster = resolveHosterUrl(t, csrf, formToken, pageUrl, playUrl, altcha);
                if (hoster.empty() || contains(hoster, "/r?") || hoster == baseUrl() + "/r" || hoster == baseUrl()) continue;
                std::string prefix = language.empty() ? "" : language + " - ";
                if (containsCI(hoster, "voe")) append(out, voe(hoster, prefix));
                else if (containsCI(hoster, "dood") || containsCI(hoster, "myvidplay")) append(out, dood(hoster, prefix));
                else if (containsCI(hoster, "streamtape")) append(out, streamtape(hoster, prefix));
                else if (lower(provider) == "voe") append(out, voe(hoster, prefix));
            } catch (const std::exception&) {
            }
        }
        sortByPrefs(out, {"streamtape", "Deutscher Sub"});
        return ensureVideos(out);
    }

  private:
    std::string thumbUrl(const html::Document& d, const html::Node& img) const {
        if (!img) return "";
        std::string u = d.absUrl(img, "data-src");
        if (trim(img.attr("data-src")).empty()) u = d.absUrl(img, "src");
        if (trim(img.attr("data-src")).empty() && trim(img.attr("src")).empty()) return "";
        return u;
    }

    static std::string lang(const std::string& key) {
        static const std::vector<std::pair<std::string, std::string>> MAP = {
            {"1", "Deutscher Dub"}, {"Deutsch", "Deutscher Dub"}, {"3", "Deutscher Sub"}, {"Ger-Sub", "Deutscher Sub"},
            {"2", "Englischer Sub"}, {"Englisch", "Englischer Sub"}, {"English", "Englischer Sub"}};
        std::string k = trim(key);
        for (auto& m : MAP)
            if (m.first == k) return m.second;
        if (!k.empty() && digitsOnly(k) == k) return "";
        for (auto& m : MAP)
            if (containsCI(k, m.first)) return m.second;
        return "";
    }

    bool parseRow(const html::Node& row, const std::string& seasonUrl, Episode& e) const {
        std::string onclick = row.attr("onclick");
        std::string href = substringBefore(after(onclick, "window.location='", ""), "'");
        if (href.empty()) href = row.selectFirst("a[href*=/episode-]").attr("href");
        if (href.empty()) return false;
        e.url = rel(href);
        std::string sAfter = after(seasonUrl, "/staffel-", "");
        std::string seasonNum;
        for (char c : sAfter) {
            if (!std::isdigit((unsigned char)c)) break;
            seasonNum += c;
        }
        if (seasonNum.empty()) seasonNum = "1";
        bool film = seasonNum == "0" || contains(seasonUrl, "/staffel-0");
        std::string epText = trim(row.selectFirst("th.episode-number-cell").text());
        if (epText.empty()) {
            std::string a = after(href, "episode-", "");
            std::string n;
            for (char c : a) {
                if (!std::isdigit((unsigned char)c)) break;
                n += c;
            }
            epText = n.empty() ? "1" : n;
        }
        int num = std::atoi(epText.c_str());
        if (num <= 0) num = 1;
        std::string ger = trim(row.selectFirst("strong.episode-title-ger").text());
        if (ger.empty()) ger = trim(row.selectFirst("td.episode-title-cell strong").text());
        std::string eng = trim(row.selectFirst("span.episode-title-eng").text());
        std::string title = !ger.empty() ? ger : (!eng.empty() ? eng : "Episode " + std::to_string(num));
        e.name = film ? "Film " + std::to_string(num) + " : " + title
                      : "Staffel " + seasonNum + " Folge " + std::to_string(num) + " : " + title;
        e.number = num;
        return true;
    }

    std::string resolveHosterUrl(const std::string& t, const std::string& csrf, const std::string& formToken,
                                 const std::string& referer, const std::string& playUrl, const std::string& altcha) {
        std::vector<std::pair<std::string, std::string>> form = {{"_token", formToken.empty() ? csrf : formToken}, {"t", t}};
        if (!altcha.empty()) form.push_back({"altcha", altcha});
        http::Headers h = {{"Referer", referer},
                           {"X-CSRF-TOKEN", csrf},
                           {"X-Requested-With", "XMLHttpRequest"},
                           {"Origin", baseUrl()},
                           {"Content-Type", "application/x-www-form-urlencoded"}};
        http::Response r = req("POST", baseUrl() + "/r", h, formEncode(form), false);
        std::string loc = r.header("location");
        if (!loc.empty()) return http::resolve(baseUrl() + "/", loc);
        std::string meta = substringBefore(after(r.body, "url='", ""), "'");
        if (meta.empty()) meta = substringBefore(after(r.body, "URL='", ""), "'");
        loc = meta.empty() ? baseUrl() + "/r" : meta;
        if (contains(loc, "/r?") || loc == baseUrl() + "/r") {
            std::string play = fixUrl(playUrl, baseUrl() + "/");
            http::Response g = req("GET", play);
            loc = g.finalUrl.empty() ? play : g.finalUrl;
            if (contains(loc, "/r?")) {
                std::string m2 = substringBefore(after(g.body, "url='", ""), "'");
                if (!m2.empty()) loc = m2;
            }
        }
        return loc;
    }

    std::string fetchAltcha(const std::string& referer, const std::string& csrf) {
        http::Response r = req("GET", baseUrl() + "/api/inline/verify-init",
                               {{"Referer", referer}, {"X-CSRF-TOKEN", csrf}, {"X-Requested-With", "XMLHttpRequest"}});
        if (r.status < 200 || r.status >= 300) return "";
        json j = json::parse(r.body, nullptr, false);
        if (!j.is_object()) return "";
        auto s = [&](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); };
        std::string algorithm = s("algorithm"), challenge = s("challenge"), salt = s("salt"), signature = s("signature");
        if (algorithm.empty() || challenge.empty() || salt.empty() || signature.empty()) return "";
        long maxnumber = 100000;
        if (j.contains("maxnumber")) {
            if (j["maxnumber"].is_number()) maxnumber = j["maxnumber"].get<long>();
            else if (j["maxnumber"].is_string()) maxnumber = std::atol(j["maxnumber"].get<std::string>().c_str());
        }
        std::string alg = lower(replaceAll(algorithm, "-", ""));
        std::function<std::string(const std::string&)> hash;
        if (alg == "sha256") hash = crypto::sha256;
        else if (alg == "sha1") hash = crypto::sha1;
        else return "";
        std::string target = lower(challenge);
        long number = -1;
        for (long i = 0; i <= maxnumber; i++)
            if (crypto::toHex(hash(salt + std::to_string(i))) == target) {
                number = i;
                break;
            }
        if (number < 0) return "";
        std::string payload = "{\"algorithm\":" + json(algorithm).dump() + ",\"challenge\":" + json(challenge).dump() +
                              ",\"salt\":" + json(salt).dump() + ",\"signature\":" + json(signature).dump() +
                              ",\"number\":" + std::to_string(number) + "}";
        return crypto::base64Encode(payload);
    }
};

// =============================================================================================== AnimeToast

class AnimeToast : public FdSource {
  public:
    AnimeToast() : FdSource({"de.animetoast", "AnimeToast", "https://www.animetoast.cc", "de", false}) {}
    bool supportsLatest() const override { return false; }

    Page popular(int page) override {
        if (page > 1) return {};
        auto d = doc(baseUrl() + "/");
        return list(*d, "div.row div.col-md-4 div.video-item", [&](const html::Node& el, Anime& a) {
            html::Node link = el.selectFirst("div.item-thumbnail a");
            a.url = d->absUrl(link, "href");
            a.thumbnail = link.selectFirst("img").attr("src");
            a.title = link.attr("title");
        });
    }
    Page latest(int page) override { return popular(page); }

    Page search(const std::string& q, int page) override {
        auto d = doc(baseUrl() + "/page/" + std::to_string(page) + "/?s=" + http::urlEncode(q));
        return list(*d, "div.item-thumbnail a[href]", [&](const html::Node& el, Anime& a) {
            a.url = d->absUrl(el, "href");
            a.thumbnail = el.selectFirst("img").attr("src");
            a.title = el.attr("title");
        }, ".nextpostslink");
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details det;
        det.thumbnail = d->selectFirst(".item-content p img").attr("src");
        det.title = d->selectFirst("h1.light-title.entry-title").text();
        det.genre = joinText(d->select("a[rel=tag]"));
        std::vector<std::string> desc;
        for (auto& p : d->select("div.item-content p")) {
            html::Node prev = prevElement(p);
            if (prev && prev.tag() == "div") desc.push_back(p.text());
        }
        det.description = joinStr(desc, " ");
        std::string cat = joinText(d->select("a[rel=\"category tag\"]"), " ");
        det.status = containsCI(cat, "Airing") ? "In corso" : "Completato";

        if (contains(cat, "Serie")) {
            std::string sel = d->selectFirst("#multi_link_tab0") ? "#multi_link_tab0" : "#multi_link_tab1";
            auto links = d->select(sel + " a");
            std::string epT = joinText(links, " ");
            if (!links.empty() && (contains(epT, ":") || contains(epT, "-"))) {
                auto first = doc(d->absUrl(links[0], "href"));
                std::string nUrl = first->absUrl(first->selectFirst("#player-embed a"), "href");
                auto nd = doc(nUrl);
                for (auto& a : nd->select("div.tab-pane a")) det.episodes.push_back(fromLink(*nd, a));
            } else {
                for (auto& a : links) det.episodes.push_back(fromLink(*d, a));
            }
        } else {
            std::string canon = d->selectFirst("link[rel=canonical]").attr("href");
            det.episodes.push_back(episode(rel(canon.empty() ? abs(url) : canon), d->selectFirst("h1.light-title").text(), 1));
        }
        std::reverse(det.episodes.begin(), det.episodes.end());
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        auto d = doc(abs(episodeUrl));
        std::vector<Video> out;
        auto panes = d->select("div.tab-pane");
        std::string all = joinText(panes, " ");
        if (contains(all, ":") || contains(all, "-")) {
            for (auto& pane : panes) {
                std::string t = pane.text();
                if (!contains(t, ":") && !contains(t, "-")) continue;
                auto sd = doc(d->absUrl(pane.selectFirst("a"), "href"));
                std::string nUrl = sd->absUrl(sd->selectFirst("#player-embed a"), "href");
                auto nd = doc(nUrl);
                collect(*nd, out);
                break;
            }
        } else {
            collect(*d, out);
        }
        std::reverse(out.begin(), out.end());
        return ensureVideos(out);
    }

  private:
    static bool strictFloat(const std::string& raw, double& out) {
        std::string n = trim(raw);
        if (n.empty()) return false;
        char* end = nullptr;
        out = std::strtod(n.c_str(), &end);
        return end && *end == '\0';
    }

    static double epNumber(const std::string& text) {
        double v;
        return strictFloat(substringAfter(text, "Ep."), v) ? v : -1e9;
    }

    Episode fromLink(const html::Document& d, const html::Node& a) const {
        std::string t = a.text();
        double num;
        if (!strictFloat(replaceAll(t, "Ep. ", ""), num)) num = 100;
        return episode(rel(d.absUrl(a, "href")), t, num);
    }

    /** Pagina di un episodio: per ogni link (hoster) con lo stesso numero dell'episodio corrente estrae i video. */
    void collect(const html::Document& d, std::vector<Video>& out) {
        double cur = epNumber(d.selectFirst("div.tab-pane a.current-link").text());
        if (cur == -1e9) cur = 100;
        for (auto& a : d.select("div.tab-pane a")) {
            if (epNumber(a.text()) != cur) continue;
            try {
                auto nd = doc(d.absUrl(a, "href"));
                std::string link = nd->absUrl(nd->selectFirst("#player-embed a"), "href");
                if (contains(link, "https://voe.sx")) append(out, voe(link, ""));
                std::string frame = nd->absUrl(nd->selectFirst("#player-embed iframe"), "src");
                if (contains(frame, "https://dood") || contains(frame, "https://ds2play")) append(out, dood(frame, ""));
                else if (contains(frame, "https://filemoon.sx")) append(out, moon(frame, baseUrl(), "Filemoon - "));
                else if (contains(frame, "mp4upload")) append(out, mp4upload(frame, ""));
            } catch (const std::exception&) {
            }
        }
    }
};

// =============================================================================================== Anime-Base

class AnimeBase : public FdSource {
  public:
    AnimeBase() : FdSource({"de.animebase", "Anime-Base", "https://anime-base.net", "de", true}) {}

    Page popular(int page) override {
        if (page > 1) return {};
        auto d = doc(baseUrl() + "/favorites");
        return list(*d, "div.table-responsive > a", [&](const html::Node& el, Anime& a) { fill(*d, el, a); });
    }

    Page latest(int page) override {
        if (page > 1) return {};
        auto d = doc(baseUrl() + "/updates");
        Page p;
        std::set<std::string> seen;
        for (auto& el : d->select("div.box-body > a")) {
            html::Node prev = prevElement(el.parent());
            if (!prev || prev.tag() != "div" || !contains(" " + prev.attr("class") + " ", " box-header ")) continue;
            Anime a;
            fill(*d, el, a);
            a.url = rel(a.url);
            if (a.url.empty() || a.title.empty() || !seen.insert(a.url).second) continue;
            p.animes.push_back(a);
        }
        return p;
    }

    Page search(const std::string& q, int page) override {
        if (page > 1) return {};
        auto sd = doc(baseUrl() + "/searching");
        std::string token = sd->selectFirst("form > input[name=_token]").attr("value");
        std::string body = formEncode({{"_token", token}, {"_token", token}, {"name_serie", q}, {"jahr", ""}});
        html::Document d(postForm(baseUrl() + "/searching", body), baseUrl() + "/searching");
        return list(d, "div.col-lg-9.col-md-8 div.box-body > a", [&](const html::Node& el, Anime& a) { fill(d, el, a); });
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details det;
        html::Node box = d->selectFirst("div.box-body.box-profile > center");
        det.title = box.selectFirst("h3").text();
        det.thumbnail = d->absUrl(box.selectFirst("img"), "src");
        html::Node infos = d->selectFirst("div.box-body > div.col-md-9");
        std::string st = info(infos, "Status");
        det.status = st == "Laufend" ? "In corso" : (st == "Abgeschlossen" ? "Completato" : "");
        html::Node gp = infoNode(infos, "Genre");
        det.genre = joinText(gp.select("a"));
        std::string desc = info(infos, "Beschreibung");
        std::string orig = info(infos, "Originalname"), year = info(infos, "Erscheinungsjahr");
        if (!orig.empty()) desc += "\nOriginal name: " + orig;
        if (!year.empty()) desc += "\nErscheinungsjahr: " + year;
        det.description = trim(desc);

        std::string page = rel(d->url().empty() ? abs(url) : d->url());
        for (auto& panel : d->select("div.tab-content > div > div.panel")) {
            std::string name = panel.selectFirst("h3").text();
            if (name.empty()) name = "Episode 1";
            std::string lang = panel.selectFirst("button").attr("data-dubbed") == "0" ? "Subbed" : "Dubbed";
            std::string cls;
            for (auto& c : split(panel.attr("class"), " "))
                if (startsWith(c, "episode-div")) {
                    cls = c;
                    break;
                }
            if (cls.empty()) continue;
            std::string n = substringAfter(substringBefore(name, ":"), " ");
            double num = !n.empty() && std::isdigit((unsigned char)n[0]) ? std::atof(n.c_str()) : 0;
            det.episodes.push_back(episode(page + "|" + cls, name + " (" + lang + ")", num));
        }
        std::stable_sort(det.episodes.begin(), det.episodes.end(), [](const Episode& a, const Episode& b) {
            auto key = [](const Episode& e) {
                return std::make_tuple(startsWith(e.name, "Film "), startsWith(e.name, "Special "), e.number);
            };
            return key(a) > key(b);
        });
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::string page = substringBefore(episodeUrl, "|"), cls = substringAfter(episodeUrl, "|");
        auto d = doc(abs(page));
        static const std::map<std::string, std::string> HOSTERS = {
            {"Streamwish", "https://streamwish.to/e/"}, {"Voe.SX", "https://voe.sx/e/"},
            {"Lulustream", "https://lulustream.com/e/"}, {"VTube", "https://vtbe.to/embed-"}};
        std::vector<Video> out;
        for (auto& b : d->select("div.panel." + cls + " div.panel-body > button")) {
            std::string hoster = trim(b.text());
            auto it = HOSTERS.find(hoster);
            if (it == HOSTERS.end()) continue;  // VidGuard: richiede un motore JS, non supportato
            std::string lang = b.attr("data-dubbed") == "0" ? "SUB " : "DUB ";
            std::string url = it->second + b.attr("data-streamlink");
            try {
                if (hoster == "Streamwish") append(out, streamWish(url, lang));
                else if (hoster == "Voe.SX") append(out, voe(url, lang));
                else append(out, unpackerHls(url, lang + hoster));
            } catch (const std::exception&) {
            }
        }
        sortByPrefs(out, {"SUB"}, "720p");
        return ensureVideos(out);
    }

  private:
    void fill(const html::Document& d, const html::Node& el, Anime& a) const {
        a.url = replaceAll(d.absUrl(el, "href"), "/link/", "/anime/");
        a.thumbnail = d.absUrl(el.selectFirst("div.thumbnail img"), "src");
        a.title = el.selectFirst("div.caption h3").text();
    }

    static html::Node infoNode(const html::Node& root, const std::string& label) {
        for (auto& s : root.select("strong"))
            if (contains(s.text(), label)) {
                html::Node n = nextElement(s);
                if (n && n.tag() == "p") return n;
            }
        return html::Node();
    }

    static std::string info(const html::Node& root, const std::string& label) { return trim(infoNode(root, label).text()); }
};

// =============================================================================================== Anime-Stream (de)

class AnimeStreamDe : public FdSource {
  public:
    AnimeStreamDe() : FdSource({"de.animestream", "Anime-Stream", "https://anime-stream.to", "de", false}) {}
    bool supportsLatest() const override { return false; }

    Page popular(int page) override { return listing(baseUrl() + "/series/page/" + std::to_string(page) + "/"); }
    Page latest(int page) override { return popular(page); }
    Page search(const std::string& q, int page) override {
        return listing(baseUrl() + "/page/" + std::to_string(page) + "/?s=" + http::urlEncode(q));
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details det;
        html::Node img = d->selectFirst("div.thumb img");
        det.thumbnail = img.attr("src");
        det.title = img.attr("alt");
        if (det.title.empty()) det.title = d->selectFirst("h3").text();
        det.description = d->selectFirst("div.desc p.f-desc").text();
        det.genre = joinText(d->select("div.mvici-left p a[rel=\"category tag\"]"));
        auto links = d->select("div.les-content a");
        for (size_t i = 0; i < links.size(); i++)
            det.episodes.push_back(episode(rel(d->absUrl(links[i], "href")), links[i].text(), (double)(i + 1)));
        std::reverse(det.episodes.begin(), det.episodes.end());
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        auto d = doc(abs(episodeUrl));
        std::string url = d->absUrl(d->selectFirst("div a.lnk-lnk"), "href");
        if (url.empty()) throw http::Error("Nessun video trovato");
        return ensureVideos(metastream(url, "Metastream"));
    }

  private:
    Page listing(const std::string& url) {
        auto d = doc(url);
        Page p = list(*d, "div.movies-list div.ml-item", [&](const html::Node& el, Anime& a) {
            a.url = d->absUrl(el.selectFirst("a"), "href");
            html::Node img = el.selectFirst("a img");
            a.thumbnail = img.attr("data-original");
            a.title = img.attr("alt");
        });
        html::Node active = d->selectFirst("li.active");
        p.hasNextPage = active && hasFollowingSibling(active, "li");
        return p;
    }
};

// =============================================================================================== FilmPalast

class FilmPalast : public FdSource {
  public:
    FilmPalast() : FdSource({"de.filmpalast", "FilmPalast", "https://filmpalast.to", "de", false}) {}

    Page popular(int page) override { return listing(baseUrl() + "/movies/top/page/" + std::to_string(page)); }
    Page latest(int page) override { return listing(baseUrl() + "/page/" + std::to_string(page)); }
    Page search(const std::string& q, int page) override {
        return listing(baseUrl() + "/search/title/" + http::urlEncode(q) + "/" + std::to_string(page));
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details det;
        det.thumbnail = baseUrl() + d->selectFirst("img.cover2").attr("src");
        det.title = d->selectFirst("h2.bgDark").text();
        auto items = d->selectFirst("#detail-content-list").children();
        auto nth = [&](size_t i) -> std::vector<html::Node> {
            if (i >= items.size() || items[i].tag() != "li") return {};
            std::vector<html::Node> v;
            for (auto& c : items[i].children())
                if (c.tag() == "span") v.push_back(c);
            return v;
        };
        det.genre = joinText(nth(1));
        det.description = joinText(nth(2), " ");
        det.author = joinText(nth(3));
        det.status = "Completato";
        std::string canon = d->selectFirst("link[rel=canonical]").attr("href");
        det.episodes.push_back(episode(rel(canon.empty() ? abs(url) : canon), "Film", 1));
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        auto d = doc(abs(episodeUrl));
        std::vector<Video> out;
        for (auto& a : d->select("ul.currentStreamLinks > li > a")) {
            std::string url = d->absUrl(a, "href");
            if (url.empty() || !a.hasAttr("href")) url = d->absUrl(a, "data-player-url");
            try {
                if (contains(url, "voe")) append(out, voe(url, ""));
                else if (contains(url, "streamtape")) append(out, streamtape(url, ""));
                else if (contains(url, "wolfstream")) append(out, wolfstream(url, ""));
                // evoload: servizio chiuso, non supportato
            } catch (const std::exception&) {
            }
        }
        std::stable_sort(out.begin(), out.end(), [](const Video& x, const Video& y) {
            return containsCI(x.title, "voe") && !containsCI(y.title, "voe");
        });
        return ensureVideos(out);
    }

  private:
    Page listing(const std::string& url) {
        auto d = doc(url);
        Page p = list(*d, "article.liste > a", [&](const html::Node& el, Anime& a) {
            a.url = d->absUrl(el, "href");
            a.thumbnail = baseUrl() + el.selectFirst("img").attr("src");
            a.title = el.attr("title");
        });
        p.hasNextPage = firstWithText(*d, "a.pageing", "vorw").valid();
        return p;
    }
};

// =============================================================================================== Moflix-Stream

class MoflixStream : public FdSource {
  public:
    MoflixStream() : FdSource({"de.moflixstream", "Moflix-Stream", "https://moflix-stream.xyz", "de", false}) {}
    bool supportsLatest() const override { return false; }

    Page popular(int page) override {
        std::string url = api() + "/channel/345?returnContentOnly=true&restriction=&order=rating:desc&paginate=simple&perPage=50&query=&page=" +
                          std::to_string(page);
        json j = json::parse(fetch(url, {{"Referer", baseUrl() + "/movies?order=rating%3Adesc"}}), nullptr, false);
        Page p;
        if (!j.is_object() || !j.contains("pagination")) return p;
        auto& pg = j["pagination"];
        p.animes = items(jv(pg, "data", json::array()));
        int cur = pg.contains("current_page") && pg["current_page"].is_number() ? pg["current_page"].get<int>() : page;
        int next = pg.contains("next_page") && pg["next_page"].is_number() ? pg["next_page"].get<int>() : 1;
        p.hasNextPage = cur < next;
        return p;
    }
    Page latest(int page) override { return popular(page); }

    Page search(const std::string& q, int page) override {
        if (page > 1) return {};
        std::string e = http::urlEncode(q);
        json j = json::parse(fetch(api() + "/search/" + e + "?query=" + e, {{"Referer", baseUrl() + "/search/" + e}}), nullptr, false);
        Page p;
        if (j.is_object()) p.animes = items(jv(j, "results", json::array()));
        return p;
    }

    Details details(const std::string& url) override {
        json j = json::parse(fetch(abs(url)), nullptr, false);
        if (!j.is_object() || !j.contains("title")) throw http::Error("Moflix-Stream: risposta non valida");
        auto& t = j["title"];
        Details d;
        d.title = jv(t, "name", "");
        d.thumbnail = thumb(t);
        std::vector<std::string> genres;
        if (t.contains("genres") && t["genres"].is_array())
            for (auto& g : t["genres"]) genres.push_back(jv(g, "display_name", ""));
        d.genre = joinStr(genres);
        d.description = t.contains("description") && t["description"].is_string() ? t["description"].get<std::string>() : "";

        std::string id = idOf(t);
        if (!j.contains("seasons") || !j["seasons"].is_object()) {
            d.episodes.push_back(episode(url, "Film", 1));
            return d;
        }
        std::string seasonsUrl = api() + "/titles/" + id + "/seasons";
        std::vector<int> seasons;
        json sl = j["seasons"];
        for (int guard = 0; guard < 50; guard++) {
            if (sl.contains("data") && sl["data"].is_array())
                for (auto& s : sl["data"])
                    if (s.contains("number") && s["number"].is_number()) seasons.push_back(s["number"].get<int>());
            if (!sl.contains("next_page") || !sl["next_page"].is_number()) break;
            json nj = json::parse(fetch(seasonsUrl + "?perPage=8&query=&page=" + std::to_string(sl["next_page"].get<int>())), nullptr, false);
            if (!nj.is_object() || !nj.contains("pagination")) break;
            sl = nj["pagination"];
        }
        std::sort(seasons.begin(), seasons.end(), std::greater<int>());
        for (int s : seasons) {
            try {
                json ej = json::parse(fetch(seasonsUrl + "/" + std::to_string(s) + "?load=episodes,primaryVideo"), nullptr, false);
                if (!ej.is_object() || !ej.contains("episodes") || !ej["episodes"].contains("data")) continue;
                std::vector<Episode> eps;
                for (auto& e : ej["episodes"]["data"]) {
                    int n = jv(e, "episode_number", 0);
                    eps.push_back(episode(rel(seasonsUrl) + "/" + std::to_string(s) + "/episodes/" + std::to_string(n) +
                                              "?load=videos,compactCredits,primaryVideo",
                                          "Staffel " + std::to_string(s) + " Folge " + std::to_string(n) + " : " + jv(e, "name", ""), n));
                }
                std::stable_sort(eps.begin(), eps.end(), [](const Episode& a, const Episode& b) { return a.number > b.number; });
                d.episodes.insert(d.episodes.end(), eps.begin(), eps.end());
            } catch (const std::exception&) {
            }
        }
        return d;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        json j = json::parse(fetch(abs(episodeUrl)), nullptr, false);
        json data = j.is_object() && j.contains("episode") && j["episode"].is_object() ? j["episode"]
                                                                                       : (j.is_object() && j.contains("title") ? j["title"] : json());
        std::vector<Video> out;
        if (!data.is_object() || !data.contains("videos") || !data["videos"].is_array()) throw http::Error("Nessun video trovato");
        for (auto& v : data["videos"]) {
            std::string name = jv(v, "name", ""), src = jv(v, "src", "");
            if (src.empty()) continue;
            try {
                if (contains(name, "Streamtape")) append(out, streamtape(src, ""));
                else if (contains(name, "Streamvid")) append(out, vidHide(src, ""));
                else if (contains(name, "Highstream")) append(out, vidHide(src, "Highstream - "));
                else if (contains(name, "Filelions")) append(out, streamWish(src, "FileLions - "));
                else if (contains(name, "LuluStream")) append(out, unpackerHls(src, "LuluStream"));
                // VidGuard: richiede un motore JS, non supportato
            } catch (const std::exception&) {
            }
        }
        return ensureVideos(out);
    }

  private:
    std::string api() const { return baseUrl() + "/api/v1"; }

    static std::string idOf(const json& t) {
        if (!t.contains("id")) return "";
        return t["id"].is_string() ? t["id"].get<std::string>() : t["id"].dump();
    }

    static std::string thumb(const json& t) {
        for (const char* k : {"poster", "backdrop"})
            if (t.contains(k) && t[k].is_string()) return t[k].get<std::string>();
        return "";
    }

    std::vector<Anime> items(const json& arr) const {
        std::vector<Anime> out;
        if (!arr.is_array()) return out;
        for (auto& it : arr) {
            if (!it.is_object()) continue;
            Anime a;
            a.title = jv(it, "name", "");
            a.url = "/api/v1/titles/" + idOf(it) +
                    "?load=images,genres,productionCountries,keywords,videos,primaryVideo,seasons,compactCredits";
            a.thumbnail = thumb(it);
            if (!a.title.empty()) out.push_back(a);
        }
        return out;
    }
};

// =============================================================================================== Movie4k

class Movie4k : public FdSource {
  public:
    Movie4k() : FdSource({"de.movie4k", "Movie4k", "https://movie4k.stream", "de", false}) {}
    bool supportsLatest() const override { return false; }

    Page popular(int page) override {
        return browse("/data/browse/?lang=2&keyword=&year=&rating=&votes=&genre=&country=&cast=&directors=&type=movies&order_by=trending&page=" +
                          std::to_string(page),
                      false);
    }
    Page latest(int page) override { return popular(page); }
    Page search(const std::string& q, int page) override {
        return browse("/data/browse/?lang=2&keyword=" + http::urlEncode(q) +
                          "&year=&rating=&votes=&genre=&country=&cast=&directors=&type=&order_by=&page=" + std::to_string(page),
                      true);
    }

    Details details(const std::string& url) override {
        json j = json::parse(fetch(apiUrl() + url), nullptr, false);
        if (!j.is_object()) throw http::Error("Movie4k: risposta non valida");
        Details d;
        d.title = jv(j, "title", "");
        d.description = j.contains("storyline") && j["storyline"].is_string() ? j["storyline"].get<std::string>() : "";
        for (const char* k : {"poster_path_season", "poster_path"})
            if (j.contains(k) && j[k].is_string() && !j[k].get<std::string>().empty()) {
                d.thumbnail = "https://image.tmdb.org/t/p/w300" + j[k].get<std::string>();
                break;
            }
        std::string id = str(j, "_id");
        if (num(j, "tv") == 1) {
            std::string season = str(j, "s");
            std::set<double> seen;
            std::vector<double> eps;
            if (j.contains("streams") && j["streams"].is_array())
                for (auto& s : j["streams"]) {
                    double e = num(s, "e");
                    if (seen.insert(e).second) eps.push_back(e);
                }
            std::sort(eps.begin(), eps.end());
            for (double e : eps) {
                std::string n = numStr(e);
                d.episodes.push_back(episode("/data/watch/?_id=" + id + "&e=" + n, "Staffel " + season + " Folge " + n, e));
            }
            std::reverse(d.episodes.begin(), d.episodes.end());
        } else {
            d.episodes.push_back(episode("/data/watch/?_id=" + id, "Film", 1));
        }
        return d;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::string e = http::queryParam(episodeUrl, "e");
        std::string url = substringBefore(episodeUrl, "&e=");
        json j = json::parse(fetch(apiUrl() + url), nullptr, false);
        if (!j.is_object() || !j.contains("streams") || !j["streams"].is_array()) throw http::Error("Nessun video trovato");
        bool tv = num(j, "tv") == 1;
        std::vector<Video> out;
        for (auto& item : j["streams"]) {
            if (out.size() > 15) break;
            if (tv && str(item, "e") != e && numStr(num(item, "e")) != e) continue;
            std::string link = str(item, "stream");
            if (link.empty()) continue;
            std::string full = contains(link, "https:") ? link : "https:" + link;
            try {
                if (contains(link, "//streamtape")) append(out, streamtape(full, ""));
                else if (contains(link, "vidoza")) append(out, vidoza(full, "Vidoza"));
                else if (contains(link, "//voe.sx") || contains(link, "//launchreliantcleaverriver") ||
                         contains(link, "//fraudclatterflyingcar") || contains(link, "//uptodatefinishconferenceroom") ||
                         contains(link, "//realfinanceblogcenter"))
                    append(out, voe(full, ""));
                // StreamZ / streamcrypt: il link finale si ottiene solo scaricando il video (range), non supportato
            } catch (const std::exception&) {
            }
        }
        std::reverse(out.begin(), out.end());
        return ensureVideos(out);
    }

  protected:
    http::Headers baseHeaders() const override { return {{"Referer", baseUrl() + "/"}}; }

  private:
    std::string apiUrl() const { return "https://api." + http::hostOf(baseUrl()); }

    static std::string str(const json& j, const char* k) {
        if (!j.is_object() || !j.contains(k) || j[k].is_null()) return "";
        if (j[k].is_string()) return j[k].get<std::string>();
        if (j[k].is_number()) return numStr(j[k].get<double>());
        return j[k].dump();
    }

    static double num(const json& j, const char* k) {
        if (!j.is_object() || !j.contains(k)) return -1;
        if (j[k].is_number()) return j[k].get<double>();
        if (j[k].is_string()) return std::atof(j[k].get<std::string>().c_str());
        return -1;
    }

    Page browse(const std::string& path, bool searchMode) {
        json j = json::parse(fetch(apiUrl() + path), nullptr, false);
        Page p;
        if (!j.is_object()) return p;
        if (j.contains("pager") && j["pager"].is_object())
            p.hasNextPage = num(j["pager"], "currentPage") < num(j["pager"], "endPage");
        if (!j.contains("movies") || !j["movies"].is_array()) return p;
        for (auto& it : j["movies"]) {
            Anime a;
            a.title = str(it, "title");
            a.url = "/data/watch/?_id=" + str(it, "_id");
            std::string poster = searchMode && num(it, "tv") == 1 ? str(it, "poster_path_season") : str(it, "poster_path");
            a.thumbnail = "https://image.tmdb.org/t/p/w300" + poster;
            if (!a.title.empty()) p.animes.push_back(a);
        }
        return p;
    }
};

// =============================================================================================== Kool

class Kool : public FdSource {
  public:
    Kool() : FdSource({"de.kool", "Kool", "https://www.kool.to", "de", false}) {}
    bool supportsLatest() const override { return false; }

    Page popular(int page) override {
        json body = {{"language", "de"}, {"region", "DE"}, {"catalogId", "tmdb.movie"}, {"id", "movie/popular"}, {"adult", false},
                     {"search", ""}, {"sort", "popularity"}, {"filter", json::object()}, {"cursor", cursor(page)}, {"clientVersion", "1.1.3"}};
        return parseCatalog(post("/kool/mediahubmx-catalog.json", body));
    }
    Page latest(int page) override { return popular(page); }

    Page search(const std::string& q, int page) override {
        Page out;
        for (const char* cat : {"tmdb.movie", "tmdb.series"}) {
            json body = {{"language", "de"}, {"region", "DE"}, {"catalogId", cat}, {"id", cat}, {"adult", false}, {"search", q},
                         {"sort", ""}, {"filter", json::object()}, {"cursor", cursor(page)}, {"clientVersion", "1.1.3"}};
            try {
                Page p = parseCatalog(post("/kool/mediahubmx-catalog.json", body));
                out.animes.insert(out.animes.end(), p.animes.begin(), p.animes.end());
                out.hasNextPage = out.hasNextPage || p.hasNextPage;
            } catch (const std::exception&) {
            }
        }
        return out;
    }

    Details details(const std::string& url) override {
        std::string id = http::queryParam(url, "_id"), type = http::queryParam(url, "type");
        std::string name = urlDecode(http::queryParam(url, "name"));
        json j = json::parse(post("/kool/mediahubmx-item.json", itemBody(type, id, name)), nullptr, false);
        Details d;
        d.title = j.is_object() ? jv(j, "name", name) : name;
        if (j.is_object() && j.contains("description") && j["description"].is_string()) d.description = j["description"].get<std::string>();
        if (j.is_object() && j.contains("images") && j["images"].is_object())
            for (const char* k : {"poster", "backdrop"})
                if (j["images"].contains(k) && j["images"][k].is_string()) {
                    d.thumbnail = j["images"][k].get<std::string>();
                    break;
                }
        if (type == "series") {
            if (j.is_object() && j.contains("episodes") && j["episodes"].is_array())
                for (auto& e : j["episodes"]) {
                    int season = jv(e, "season", 0), ep = jv(e, "episode", 0);
                    std::string epName = jv(e, "name", "");
                    std::string epId;
                    if (e.contains("ids") && e["ids"].contains("tmdb_episode_id")) {
                        auto& v = e["ids"]["tmdb_episode_id"];
                        epId = v.is_string() ? v.get<std::string>() : v.dump();
                    }
                    json ref = {{"type", type}, {"id", id}, {"name", name}, {"epid", epId}, {"season", season}, {"ep", ep}, {"epname", epName}};
                    d.episodes.push_back(episode(ref.dump(), "Staffel " + std::to_string(season) + " Folge " + std::to_string(ep) + " : " + epName, ep));
                }
            std::reverse(d.episodes.begin(), d.episodes.end());
        } else {
            json ref = {{"type", type}, {"id", id}, {"name", name}};
            d.episodes.push_back(episode(ref.dump(), "Film", 1));
        }
        return d;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        json ref = json::parse(episodeUrl, nullptr, false);
        if (!ref.is_object()) throw http::Error("Episodio non valido");
        json body = itemBody(jv(ref, "type", "movie"), jv(ref, "id", ""), jv(ref, "name", ""));
        if (jv(ref, "type", "") == "series")
            body["episode"] = {{"name", jv(ref, "epname", "")},
                               {"ids", {{"tmdb_episode_id", jv(ref, "epid", "")}}},
                               {"season", jv(ref, "season", 0)},
                               {"episode", jv(ref, "ep", 0)}};
        json arr = json::parse(post("/kool-cluster/mediahubmx-source.json", body), nullptr, false);
        std::vector<Video> out;
        if (!arr.is_array()) throw http::Error("Nessun video trovato");
        for (auto& it : arr) {
            std::string u = it.is_object() ? jv(it, "url", "") : "";
            if (u.empty()) continue;
            try {
                if (contains(u, "https://voe") || contains(u, "scatch176duplicities")) append(out, voe(u, ""));
                else if (contains(u, "https://clipboard")) out.push_back(simpleVideo(u, "Clipboard"));
                else if (contains(u, "https://streamtape")) append(out, streamtape(u, ""));
                else if (contains(u, "https://vidoza")) append(out, vidoza(u, "Vidoza"));
                else if (contains(u, "https://filemoon.sx")) append(out, moon(u, baseUrl(), "Filemoon - "));
            } catch (const std::exception&) {
            }
        }
        std::reverse(out.begin(), out.end());
        sortByPrefs(out, {"StreamTape"});
        return ensureVideos(out);
    }

  private:
    static json cursor(int page) {
        int t = page - 1;
        if (t <= 0) return nullptr;
        if (t == 1) return 8;
        return t * 8 - (t - 1);
    }

    static json itemBody(const std::string& type, const std::string& id, const std::string& name) {
        return {{"language", "de"}, {"region", "DE"}, {"type", type}, {"ids", {{"tmdb_id", id}}}, {"name", name},
                {"episode", json::object()}, {"clientVersion", "1.1.3"}};
    }

    /** Firma "mediahubmx-signature" ottenuta dal servizio dezor.net come fa l'app Android. */
    std::string signature() {
        static const char* PING = R"({"reason":"ping","locale":"de","theme":"dark","metadata":{"device":{"type":"Tablet","brand":"google","model":"Pixel 5","name":"Pixel 5","uniqueId":"17623a364c1eab4b"},"os":{"name":"android","version":"12","abis":["x86_64","arm64-v8a","x86","armeabi-v7a","armeabi"],"host":"2e977b6bc000001"},"app":{"platform":"android","version":"1.1.2","buildId":"97245000","engine":"hbc85","signatures":["43c308d52a6d51a07092ecd410963f26baae6a0e47d57fd718663a55e3d2d5e4"],"installer":"com.android.vending"},"version":{"package":"net.dezor.browser","binary":"1.1.2","js":"1.1.2"}},"appFocusTime":120169,"playDuration":0,"devMode":true,"hasMhub":true,"castConnected":false,"package":"net.dezor.browser","version":"1.1.2","process":"app","firstAppStart":1677833802384,"lastAppStart":1677833802384,"ipLocation":{"ip":"0.0.0.0","country":"DE","city":"Berlin"},"adblockEnabled":false,"proxy":{"supported":true,"enabled":false}})";
        http::Response r = http::request("POST", "https://www.dezor.net/api/app/ping",
                                         {{"Content-Type", "application/json; charset=utf-8"}, {"Accept", "application/json"}}, PING);
        json j = json::parse(r.body, nullptr, false);
        if (!j.is_object() || !j.contains("mhub") || !j["mhub"].is_string()) throw http::Error("Kool: firma non disponibile");
        return j["mhub"].get<std::string>();
    }

    std::string post(const std::string& path, const json& body) {
        http::Headers h = {{"Content-Type", "application/json; charset=utf-8"},
                           {"mediahubmx-signature", signature()},
                           {"User-Agent", "MediaHubMX/2"}};
        http::Response r = http::request("POST", baseUrl() + path, h, body.dump());
        check(r);
        return std::move(r.body);
    }

    Page parseCatalog(const std::string& body) {
        json j = json::parse(body, nullptr, false);
        Page p;
        if (!j.is_object()) return p;
        p.hasNextPage = j.contains("nextCursor") && !j["nextCursor"].is_null();
        if (!j.contains("items") || !j["items"].is_array()) return p;
        for (auto& it : j["items"]) {
            std::string type = jv(it, "type", "");
            if (type != "movie" && type != "series") continue;  // IPTV non supportata
            std::string id;
            if (it.contains("ids") && it["ids"].is_object())
                for (const char* k : {"tmdb_id", "urlId"})
                    if (it["ids"].contains(k)) {
                        auto& v = it["ids"][k];
                        id = v.is_string() ? v.get<std::string>() : v.dump();
                        break;
                    }
            if (id.empty()) continue;
            Anime a;
            a.title = jv(it, "name", "");
            a.url = "/data/watch/?_id=" + id + "&type=" + type + "&name=" + http::urlEncode(a.title);
            if (it.contains("images") && it["images"].is_object())
                for (const char* k : {"poster", "backdrop"})
                    if (it["images"].contains(k) && it["images"][k].is_string()) {
                        a.thumbnail = it["images"][k].get<std::string>();
                        break;
                    }
            p.animes.push_back(a);
        }
        return p;
    }
};

}  // namespace

std::vector<std::shared_ptr<Source>> makeFrenchGermanSources() {
    std::vector<std::shared_ptr<Source>> out;
    // francese (anime prima)
    out.push_back(std::make_shared<AnimeSama>());
    out.push_back(std::make_shared<Voiranime>());
    out.push_back(std::make_shared<FrAnime>());
    out.push_back(std::make_shared<OtakuFR>());
    out.push_back(std::make_shared<Vostfree>());
    out.push_back(std::make_shared<AnimeVostFr>());
    out.push_back(std::make_shared<AniSama>());
    out.push_back(std::make_shared<FrenchAnime>());
    out.push_back(std::make_shared<Wiflix>());
    out.push_back(std::make_shared<EmpireStreaming>());
    // tedesco
    out.push_back(std::make_shared<AniWorld>());
    out.push_back(std::make_shared<AnimeToast>());
    out.push_back(std::make_shared<AnimeBase>());
    out.push_back(std::make_shared<AnimeStreamDe>());
    out.push_back(std::make_shared<Serienstream>());
    out.push_back(std::make_shared<FilmPalast>());
    out.push_back(std::make_shared<MoflixStream>());
    out.push_back(std::make_shared<Movie4k>());
    out.push_back(std::make_shared<Kool>());
    return out;
}

}  // namespace src
