// Porting in C++ delle estensioni Aniyomi arabe, turche, russe e polacche (src/ar, src/tr, src/ru, src/pl).
// Arabo:   Anime4Up e WIT ANIME (stesso tema "anime-list"), Animerco, AnimeLek, Okanime, Anime Blkom, Animeiat,
//          ArabAnime, Arab Seed, Asia2TV, Egy Dead, Tuktuk Cinema.
// Turco:   Türk Anime TV, Anizm, Animeler, TR Anime Izle, HentaiZM (18+), HDFilmCehennemi (18+).
// Russo:   Animevost (+ mirror), YummyAnime, Animelib (18+).
// Polacco: Docchi (18+), OgladajAnime (18+).
// Gli estrattori degli hoster non presenti in extractors.hpp sono privati in questo file (vedi sotto).

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
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
using ojson = nlohmann::ordered_json;
using namespace ext;
using DocPtr = std::unique_ptr<html::Document>;

const char* DESKTOP_UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/134.0.0.0 Safari/537.36";

// =============================================================================================== utilita'

std::string joinStr(const std::vector<std::string>& v, const std::string& sep = ", ") {
    std::string out;
    for (auto& s : v) {
        std::string t = trim(s);
        if (t.empty()) continue;
        if (!out.empty()) out += sep;
        out += t;
    }
    return out;
}

std::string joinText(const std::vector<html::Node>& nodes, const std::string& sep = ", ") {
    std::vector<std::string> v;
    for (auto& n : nodes) v.push_back(n.text());
    return joinStr(v, sep);
}

std::vector<std::string> splitStr(const std::string& s, const std::string& sep) {
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

std::string afterLast(const std::string& s, const std::string& d) {
    auto p = s.rfind(d);
    return p == std::string::npos ? s : s.substr(p + d.size());
}

std::string beforeLast(const std::string& s, const std::string& d) {
    auto p = s.rfind(d);
    return p == std::string::npos ? s : s.substr(0, p);
}

/** substringBefore(d, missing): se il delimitatore manca restituisce "missing". */
std::string beforeOr(const std::string& s, const std::string& d, const std::string& missing) {
    auto p = s.find(d);
    return p == std::string::npos ? missing : s.substr(0, p);
}

std::string removeSuffix(const std::string& s, const std::string& suf) {
    return endsWith(s, suf) ? s.substr(0, s.size() - suf.size()) : s;
}

/** toFloatOrNull() di Kotlin (accetta solo un numero intero/decimale completo). */
double toNumber(const std::string& raw, double def = -1) {
    std::string s = trim(raw);
    if (s.empty()) return def;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (!end || *end != '\0') return def;
    return v;
}

std::string numStr(double n) {
    if (n == (long long)n) return std::to_string((long long)n);
    std::string s = std::to_string(n);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

/** Float.toString() di Kotlin: 1 -> "1.0", 12.5 -> "12.5". */
std::string kotlinFloatStr(double n) {
    if (n == (long long)n) return std::to_string((long long)n) + ".0";
    return numStr(n);
}

/** Testo di uno script contenente tutte le stringhe indicate. */
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

bool hasClass(const html::Node& n, const std::string& cls) {
    std::string c = " " + n.attr("class") + " ";
    for (auto& ch : c)
        if (ch == '\t' || ch == '\n') ch = ' ';
    return c.find(" " + cls + " ") != std::string::npos;
}

html::Node nextElement(const html::Node& n) {
    html::Node p = n.parent();
    if (!p) return {};
    auto ch = p.children();
    for (size_t i = 0; i + 1 < ch.size(); i++)
        if (ch[i].raw() == n.raw()) return ch[i + 1];
    return {};
}

bool isLastChild(const html::Node& n) {
    html::Node p = n.parent();
    if (!p) return false;
    auto ch = p.children();
    return !ch.empty() && ch.back().raw() == n.raw();
}

/** n-esimo figlio (1-based) se ha il tag indicato (come tag:nth-child(n)). */
html::Node nthChild(const html::Node& parent, size_t n, const std::string& tag) {
    auto ch = parent.children();
    if (n == 0 || n > ch.size()) return {};
    if (!tag.empty() && ch[n - 1].tag() != tag) return {};
    return ch[n - 1];
}

/** Primo nodo del selettore il cui testo contiene "text" (come sel:contains(text)). */
html::Node firstContaining(const std::vector<html::Node>& nodes, const std::string& text) {
    for (auto& n : nodes)
        if (contains(n.text(), text)) return n;
    return {};
}

std::vector<html::Node> allContaining(const std::vector<html::Node>& nodes, const std::string& text) {
    std::vector<html::Node> out;
    for (auto& n : nodes)
        if (contains(n.text(), text)) out.push_back(n);
    return out;
}

/** Tra i nodi che contengono il testo, quello piu' interno (testo piu' corto). */
html::Node innermostContaining(const std::vector<html::Node>& nodes, const std::string& text) {
    html::Node best;
    size_t bestLen = std::string::npos;
    for (auto& n : nodes) {
        std::string t = n.text();
        if (!contains(t, text)) continue;
        if (t.size() < bestLen) {
            bestLen = t.size();
            best = n;
        }
    }
    return best;
}

std::string urlDecode(const std::string& s) {
    std::string out;
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

/** Codifica percentuale dei byte non ASCII e degli spazi (percorsi arabi/turchi/cirillici). */
std::string encodeUrl(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (c >= 0x80 || c == ' ' || c == '"' || c == '<' || c == '>') {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        } else {
            out += (char)c;
        }
    }
    return out;
}

using Form = std::vector<std::pair<std::string, std::string>>;

std::string formBody(const Form& f) {
    std::string out;
    for (auto& kv : f) {
        if (!out.empty()) out += '&';
        out += http::urlEncode(kv.first) + "=" + http::urlEncode(kv.second);
    }
    return out;
}

std::string headerValue(const http::Headers& h, const std::string& name) {
    for (auto& kv : h)
        if (lower(kv.first) == lower(name)) return kv.second;
    return "";
}

http::Headers withHeaders(http::Headers base, const http::Headers& extra) {
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

void checkStatus(const http::Response& r, const std::string& url) {
    if (r.status < 200 || r.status >= 300)
        throw http::Error("Il sito ha risposto HTTP " + std::to_string(r.status) + " (" + http::hostOf(url) + ")");
}

std::string httpGet(const std::string& url, const http::Headers& h = {}) {
    http::Response r = http::request("GET", encodeUrl(url), h);
    checkStatus(r, url);
    return std::move(r.body);
}

http::Response httpPostRaw(const std::string& url, const std::string& body, http::Headers h = {},
                           const std::string& contentType = "application/x-www-form-urlencoded") {
    if (headerValue(h, "Content-Type").empty() && !contentType.empty()) h.push_back({"Content-Type", contentType});
    return http::request("POST", encodeUrl(url), h, body);
}

std::string httpPost(const std::string& url, const std::string& body, const http::Headers& h = {},
                     const std::string& contentType = "application/x-www-form-urlencoded") {
    http::Response r = httpPostRaw(url, body, h, contentType);
    checkStatus(r, url);
    return std::move(r.body);
}

DocPtr getDoc(const std::string& url, const http::Headers& h = {}) {
    http::Response r = http::request("GET", encodeUrl(url), h);
    checkStatus(r, url);
    return std::make_unique<html::Document>(r.body, r.finalUrl.empty() ? url : r.finalUrl);
}

DocPtr docOf(const std::string& body, const std::string& url) { return std::make_unique<html::Document>(body, url); }

json parseJson(const std::string& s) {
    json j = json::parse(s, nullptr, false);
    if (j.is_discarded()) throw http::Error("Risposta JSON non valida");
    return j;
}

/** Valore stringa (o numero convertito) di un campo JSON, "" se assente/null. */
std::string jstr(const json& j, const char* key) {
    if (!j.is_object()) return "";
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return "";
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    if (it->is_number()) return numStr(it->get<double>());
    if (it->is_boolean()) return it->get<bool>() ? "true" : "false";
    return "";
}

std::string jval(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number()) return numStr(v.get<double>());
    return "";
}

const json& jarr(const json& j, const char* key) {
    static const json empty = json::array();
    if (!j.is_object()) return empty;
    auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return empty;
    return *it;
}

const json& jobj(const json& j, const char* key) {
    static const json empty = json::object();
    if (!j.is_object()) return empty;
    auto it = j.find(key);
    if (it == j.end() || !it->is_object()) return empty;
    return *it;
}

/** Base64 (anche senza padding, con a capo) -> testo. */
std::string b64(const std::string& in) {
    std::string s;
    for (char c : in)
        if (!std::isspace((unsigned char)c)) s += c;
    while (s.size() % 4) s += '=';
    try {
        return base64Decode(s);
    } catch (const std::exception&) {
        return "";
    }
}

Video mkVideo(const std::string& url, const std::string& title, const std::string& referer = "",
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

std::vector<Video> prefixed(std::vector<Video> v, const std::string& prefix) {
    for (auto& x : v) x.title = prefix + x.title;
    return v;
}

/** Ordina mettendo per primi i video il cui titolo contiene "pref" (poi qualita' decrescente). */
void sortPref(std::vector<Video>& v, const std::string& pref) {
    std::stable_sort(v.begin(), v.end(), [&](const Video& a, const Video& b) {
        int pa = !pref.empty() && contains(a.title, pref), pb = !pref.empty() && contains(b.title, pref);
        if (pa != pb) return pa > pb;
        int qa = a.quality ? a.quality : qualityOf(a.title), qb = b.quality ? b.quality : qualityOf(b.title);
        return qa > qb;
    });
}

void finish(std::vector<Video>& v, const std::string& pref) {
    // rimuove i duplicati per URL
    std::set<std::string> seen;
    std::vector<Video> out;
    for (auto& x : v)
        if (!x.url.empty() && seen.insert(x.url).second) out.push_back(x);
    v.swap(out);
    if (v.empty()) throw http::Error("Nessun video trovato");
    sortPref(v, pref);
}

/** Episodi dal piu' recente: se la lista e' crescente la inverte. */
void newestFirst(std::vector<Episode>& e) {
    if (e.size() > 1 && e.front().number >= 0 && e.back().number >= 0 && e.front().number < e.back().number)
        std::reverse(e.begin(), e.end());
}

/** Esegue f ignorando gli errori (come runCatching / parallelCatchingFlatMap). */
template <class F>
void tryAppend(std::vector<Video>& out, F&& f) {
    try {
        append(out, f());
    } catch (const std::exception&) {
    }
}

/** Valore di un'assegnazione JS "<key> = 'valore'" o "<key>='valore'" (anche senza virgolette). */
std::string jsAssign(const std::string& text, const std::string& key) {
    size_t pos = 0;
    while ((pos = text.find(key, pos)) != std::string::npos) {
        size_t p = pos + key.size();
        pos = p;
        while (p < text.size() && std::isspace((unsigned char)text[p])) p++;
        if (p >= text.size() || text[p] != '=') continue;
        p++;
        if (p < text.size() && text[p] == '=') continue;  // confronto ==
        while (p < text.size() && std::isspace((unsigned char)text[p])) p++;
        if (p >= text.size()) break;
        if (text[p] == '\'' || text[p] == '"') {
            char q = text[p];
            auto e = text.find(q, p + 1);
            if (e == std::string::npos) break;
            std::string v = text.substr(p + 1, e - p - 1);
            if (!v.empty() && v.size() < 512) return v;
        } else {
            size_t e = p;
            while (e < text.size() && std::isalnum((unsigned char)text[e])) e++;
            if (e > p) return text.substr(p, e - p);
        }
    }
    return "";
}

/** Primo URL http(s) che contiene "needle" (terminato da virgolette, spazi o backslash). */
std::string findUrlWith(const std::string& text, const std::string& needle) {
    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        size_t s = text.rfind("http", pos);
        size_t e = pos + needle.size();
        while (e < text.size() && !std::strchr("\"' \t\r\n\\<>", text[e])) e++;
        if (s != std::string::npos && pos - s < 2048) {
            std::string u = text.substr(s, e - s);
            if ((startsWith(u, "http://") || startsWith(u, "https://")) &&
                u.find_first_of("\"' \t\r\n\\<>") == std::string::npos)
                return u;
        }
        pos = e;
    }
    return "";
}

bool regexIn(const std::string& s, const std::regex& re) {
    if (s.size() > 2048) return false;  // solo stringhe corte (stack dello Switch)
    return std::regex_search(s, re);
}

bool isVidbom(const std::string& url) {
    static const std::regex re(
        "(v[aie]d[bp][aoe]?m|myvii?d|segavid|v[aei]{1,2}dshar[er]?)\\.(com|net|org|xyz)");
    return regexIn(url, re);
}

bool isDood(const std::string& url) {
    static const std::regex re(
        "do*d(stream)?\\.(com?|watch|to|s[ho]|cx|la|w[sf]|pm|re|yt|stream)/[de]/[0-9a-zA-Z]+");
    return regexIn(url, re);
}

bool anyIn(const std::string& s, std::initializer_list<const char*> list) {
    for (auto* x : list)
        if (contains(s, x)) return true;
    return false;
}

// =============================================================================================== hoster

// ---- VidBom / VidShare (lib/vidbomextractor)
std::vector<Video> vidBom(const std::string& url, const http::Headers& h = {}) {
    DocPtr doc = getDoc(url, h);
    std::string script = scriptWith(*doc, {"sources"});
    if (script.empty()) return {};
    std::string data = substringBefore(substringAfter(script, "sources: ["), "],");
    auto parts = splitStr(data, "file:\"");
    std::vector<Video> out;
    for (size_t i = 1; i < parts.size(); i++) {
        std::string src = substringBefore(parts[i], "\"");
        std::string quality = "Vidbom: " + substringBefore(substringAfter(parts[i], "label:\""), "\"");
        if (quality.size() > 15) quality = "Vidshare: 480p";
        if (startsWith(src, "http")) out.push_back(mkVideo(src, quality));
    }
    return out;
}

// ---- Uqload (lib/uqloadextractor)
std::vector<Video> uqload(const std::string& url, const std::string& prefix = "") {
    const std::string base = "https://uqload.is/";
    std::string fixed = url;
    if (!startsWith(lower(url), base)) {
        auto p = url.find("://");
        auto slash = p == std::string::npos ? std::string::npos : url.find('/', p + 3);
        fixed = slash == std::string::npos ? url : base + url.substr(slash + 1);
    }
    DocPtr doc = getDoc(fixed);
    std::string quality = trim(prefix).empty() ? "Uqload" : trim(prefix) + " Uqload";
    std::vector<std::string> sources;
    std::string script = scriptWith(*doc, {"sources:"});
    if (!script.empty()) {
        std::string u = substringBefore(substringAfter(script, "sources: [\""), "\"");
        if (startsWith(u, "http")) sources.push_back(u);
    }
    if (sources.empty()) {
        std::string packed = scriptWith(*doc, {"eval(function(p,a,c,k,e,d)"});
        if (packed.empty()) return {};
        std::string un = unpacker::unpackAndCombine(packed);
        for (const char* ext : {".m3u8", ".mp4"}) {
            size_t pos = 0;
            while ((pos = un.find(ext, pos)) != std::string::npos) {
                size_t s = un.rfind('"', pos), e = un.find('"', pos);
                pos += 4;
                if (s == std::string::npos || e == std::string::npos) continue;
                std::string link = un.substr(s + 1, e - s - 1);
                if (startsWith(link, "//")) link = "https:" + link;
                else if (startsWith(link, "/")) link = "https://uqload.is" + link;
                if (startsWith(link, "http") && std::find(sources.begin(), sources.end(), link) == sources.end())
                    sources.push_back(link);
            }
        }
    }
    std::vector<Video> out;
    for (auto& s : sources) {
        if (contains(s, ".m3u8")) append(out, hlsVideos(s, fixed, quality + " - "));
        else out.push_back(mkVideo(s, quality, fixed));
    }
    return out;
}

// ---- YourUpload (lib/youruploadextractor)
std::vector<Video> yourUpload(const std::string& url, const std::string& prefix = "") {
    http::Headers h = {{"Referer", "https://www.yourupload.com/"}};
    DocPtr doc = getDoc(url, h);
    std::string data = scriptWith(*doc, {"jwplayerOptions"});
    if (data.empty()) return {};
    std::string file = substringBefore(substringAfter(data, "file: '"), "',");
    if (!startsWith(file, "http") || contains(file, "/novideo.mp4")) return {};
    return {mkVideo(file, prefix + "YourUpload", "https://www.yourupload.com/")};
}

// ---- Sibnet (lib/sibnetextractor)
std::vector<Video> sibnet(const std::string& url, const std::string& prefix = "") {
    DocPtr doc = getDoc(url);
    std::string script = scriptWith(*doc, {"player.src"});
    if (script.empty()) return {};
    std::string slug = substringBefore(substringAfter(substringAfter(substringAfter(script, "player.src"), "src:"), "\""), "\"");
    if (slug.empty()) return {};
    std::string videoUrl = contains(slug, "http") ? slug : "https://" + http::hostOf(url) + slug;
    return {mkVideo(videoUrl, prefix + "Sibnet", url)};
}

// ---- Sendvid (lib/sendvidextractor)
std::vector<Video> sendvid(const std::string& url, const std::string& prefix = "") {
    DocPtr doc = getDoc(url);
    std::string master = doc->selectFirst("source#video_source").attr("src");
    if (master.empty()) return {};
    if (contains(master, ".m3u8")) return hlsVideos(master, url, prefix + "Sendvid:");
    std::string origin = http::originOf(url);
    return {mkVideo(master, prefix + "Sendvid:default", origin + "/", {{"Origin", origin}})};
}

// ---- Vudeo (lib/vudeoextractor)
std::vector<Video> vudeo(const std::string& url, const std::string& prefix = "") {
    DocPtr doc = getDoc(url);
    std::string sources = scriptWith(*doc, {"sources: ["});
    if (sources.empty()) return {};
    std::string referer = "https://" + http::hostOf(url) + "/";
    std::vector<Video> out;
    for (auto& u : splitStr(replaceAll(substringBefore(substringAfter(sources, "sources: ["), "]"), "\"", ""), ","))
        if (startsWith(trim(u), "https")) out.push_back(mkVideo(trim(u), prefix + "Vudeo", referer));
    return out;
}

// ---- Google Drive (lib/googledriveextractor)
std::vector<Video> googleDrive(const std::string& itemId, const std::string& name = "Video") {
    if (itemId.empty()) return {};
    std::string url = "https://drive.usercontent.google.com/download?id=" + itemId;
    http::Headers h = {
        {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8"}};
    // si chiede solo l'inizio: se non e' una pagina HTML e' gia' il file video
    http::Headers ranged = h;
    ranged.push_back({"Range", "bytes=0-4095"});
    http::Response r = http::request("GET", url, ranged);
    if (r.status >= 400) return {};
    if (lower(r.body.substr(0, 15)) != "<!doctype html>") return {mkVideo(url, name, "", h)};
    std::string page = r.body;
    if (!contains(page, "</html>")) page = httpGet(url, h);
    DocPtr doc = docOf(page, url);
    std::string size = trim(ownText(doc->selectFirst("span.uc-name-size")));
    std::string videoUrl = url;
    std::map<std::string, std::string> params;
    std::vector<std::string> order;
    for (auto& in : doc->select("input[type=hidden]")) {
        std::string n = in.attr("name");
        if (!params.count(n)) order.push_back(n);
        params[n] = in.attr("value");
    }
    std::string query;
    bool hasId = false;
    for (auto& n : order) {
        if (n == "id") hasId = true;
        query += (query.empty() ? "" : "&") + http::urlEncode(n) + "=" + http::urlEncode(params[n]);
    }
    if (!query.empty()) {
        videoUrl = "https://drive.usercontent.google.com/download?" + (hasId ? "" : "id=" + itemId + "&") + query;
    }
    return {mkVideo(videoUrl, name + (size.empty() ? "" : " " + size + " "), "", h)};
}

/** Tutti i valori Set-Cookie che iniziano con "name" (solo "nome=valore"). */
std::string cookieFrom(const http::Response& r, const std::string& name) {
    auto range = r.headers.equal_range("set-cookie");
    for (auto it = range.first; it != range.second; ++it)
        if (startsWith(lower(it->second), lower(name))) return substringBefore(it->second, ";");
    return "";
}

// ---- Mail.ru (tr/turkanime/extractors/MailRuExtractor)
std::vector<Video> mailRu(const std::string& url, const std::string& prefix = "") {
    DocPtr doc = getDoc(url);
    std::string script = scriptWith(*doc, {"metadataUrl"});
    if (script.empty()) return {};
    std::string metaUrl = fixUrl(substringBefore(substringAfter(script, "metadataUrl\":\""), "\""));
    http::Headers mh = {{"Accept", "application/json, text/javascript, */*; q=0.01"}, {"Referer", url}};
    http::Response r = http::request("GET", metaUrl, mh);
    checkStatus(r, metaUrl);
    std::string videoKey = cookieFrom(r, "video_key");
    json j = parseJson(r.body);
    std::string host = http::hostOf(url);
    std::vector<Video> out;
    for (auto& v : jarr(j, "videos")) {
        std::string u = replaceAll(fixUrl(jstr(v, "url")), ".mp4", ".mp4/stream.mpd");
        if (u.empty()) continue;
        http::Headers vh = {{"Origin", "https://" + host}, {"Referer", "https://" + host + "/"}};
        if (!videoKey.empty()) vh.push_back({"Cookie", videoKey});
        out.push_back(mkVideo(u, prefix + "Mail.ru " + jstr(v, "key"), "", vh));
    }
    return out;
}

// ---- MVidoo (tr/turkanime/extractors/MVidooExtractor)
std::vector<Video> mvidoo(const std::string& url, const std::string& prefix = "") {
    std::string body = httpGet(url);
    size_t p = body.find("{var");
    while (p != std::string::npos) {
        size_t eq = body.find('=', p), lb = eq == std::string::npos ? eq : body.find('[', eq);
        size_t rb = lb == std::string::npos ? lb : body.find(']', lb);
        if (rb == std::string::npos || lb - eq > 8) {
            p = body.find("{var", p + 4);
            continue;
        }
        json arr = json::parse(replaceAll(body.substr(lb, rb - lb + 1), "\\x", ""), nullptr, false);
        if (!arr.is_array()) break;
        std::string joined;
        for (auto& t : arr) {
            std::string hex = jval(t);
            try {
                joined += crypto::fromHex(hex);
            } catch (const std::exception&) {
            }
        }
        std::reverse(joined.begin(), joined.end());
        std::string video = substringBefore(substringAfter(joined, "src=\""), "\"");
        if (startsWith(video, "http")) return {mkVideo(video, prefix + "MVidoo")};
        break;
    }
    return {};
}

// ---- Embedgram (tr/turkanime/extractors/EmbedgramExtractor)
std::vector<Video> embedgram(const std::string& url, const std::string& prefix = "") {
    http::Response r = http::request("GET", url);
    checkStatus(r, url);
    std::string xsrf = cookieFrom(r, "XSRF-TOKEN");
    html::Document doc(r.body, url);
    std::string src;
    for (auto& s : doc.select("video#my-video > source[src]"))
        if (!s.attr("src").empty()) {
            src = s.attr("src");
            break;
        }
    if (src.empty()) return {};
    std::string videoUrl = fixUrl(src);
    http::Headers vh = {{"Referer", "https://" + http::hostOf(url) + "/"}};
    if (!xsrf.empty()) vh.push_back({"Cookie", xsrf});
    return {mkVideo(videoUrl, prefix + "Embedgram", "", vh)};
}

// ---- VK (lib/vkextractor)
void vkParse(const std::string& text, std::vector<std::pair<int, std::string>>& found) {
    for (const char* key : {"\"url", "\"mp4_"}) {
        size_t pos = 0;
        std::string k = key;
        while ((pos = text.find(k, pos)) != std::string::npos) {
            size_t p = pos + k.size(), s = p;
            pos = p;
            while (p < text.size() && std::isdigit((unsigned char)text[p])) p++;
            if (p == s || text.compare(p, 3, "\":\"") != 0) continue;
            int q = std::atoi(text.substr(s, p - s).c_str());
            size_t vs = p + 3, ve = text.find('"', vs);
            if (ve == std::string::npos) break;
            std::string u = replaceAll(text.substr(vs, ve - vs), "\\/", "/");
            if (!startsWith(u, "http")) continue;
            bool dup = false;
            for (auto& f : found)
                if (f.first == q) dup = true;
            if (!dup) found.push_back({q, u});
        }
    }
}

std::vector<Video> vk(const std::string& url, const std::string& prefix = "") {
    const std::string VK = "https://vk.com";
    http::Headers docH = {
        {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8"},
        {"Accept-Language", "en-US,en;q=0.9"}};
    http::Headers videoH = {{"Origin", VK}};
    // id del video: oid=..&id=.. | video-123_456 | clip-123_456
    std::string videoId;
    std::string oid = http::queryParam(url, "oid"), id = http::queryParam(url, "id");
    if (!oid.empty() && !id.empty()) videoId = oid + "_" + id;
    for (const char* marker : {"video", "clip"}) {
        if (!videoId.empty()) break;
        size_t p = 0;
        while ((p = url.find(marker, p)) != std::string::npos) {
            size_t s = p + std::strlen(marker), e = s;
            if (e < url.size() && url[e] == '-') e++;
            size_t d1 = e;
            while (e < url.size() && std::isdigit((unsigned char)url[e])) e++;
            if (e > d1 && e < url.size() && url[e] == '_') {
                size_t d2 = ++e;
                while (e < url.size() && std::isdigit((unsigned char)url[e])) e++;
                if (e > d2) {
                    videoId = url.substr(s, e - s);
                    break;
                }
            }
            p = s;
        }
    }
    if (videoId.empty()) return {};
    std::vector<std::pair<int, std::string>> found;
    try {
        http::Headers ah = docH;
        ah.push_back({"X-Requested-With", "XMLHttpRequest"});
        std::string body = httpPost(VK + "/al_video.php?act=show", formBody({{"act", "show"}, {"al", "1"}, {"video", videoId}}), ah);
        vkParse(substringAfter(body, "<!--"), found);
    } catch (const std::exception&) {
    }
    if (found.empty()) {
        std::string embed = url;
        if (!contains(url, "video_ext.php")) {
            auto parts = splitStr(videoId, "_");
            if (parts.size() == 2) embed = VK + "/video_ext.php?oid=" + parts[0] + "&id=" + parts[1] + "&autoplay=0";
        }
        http::Response r = http::request("GET", embed, docH);
        if (contains(r.finalUrl, "429.html") || r.status == 429) {
            std::string c = cookieFrom(r, "hash429");
            if (c.empty()) return {};
            std::string hash = crypto::toHex(crypto::md5(substringAfter(c, "=")));
            std::string target = r.finalUrl.empty() ? embed : r.finalUrl;
            http::request("GET", target + (contains(target, "?") ? "&" : "?") + "key=" + hash, docH);
            r = http::request("GET", embed, docH);
        }
        vkParse(r.body, found);
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<Video> out;
    for (auto& f : found) {
        Video v = mkVideo(f.second, prefix + std::to_string(f.first) + "p", VK + "/", videoH);
        v.quality = f.first;
        out.push_back(v);
    }
    return out;
}

// ---- CDA.pl (lib/cdaextractor)
std::string cdaDecrypt(std::string a) {
    for (const char* p : {"_XDDD", "_CDA", "_ADC", "_CXD", "_QWE", "_Q5", "_IKSDE"}) a = replaceAll(a, p, "");
    a = urlDecode(a);
    for (auto& c : a) {
        int f = (unsigned char)c;
        if (f >= 33 && f <= 126) c = (char)(33 + (f + 14) % 94);
    }
    a = replaceAll(a, ".cda.mp4", "");
    a = replaceAll(replaceAll(a, ".2cda.pl", ".cda.pl"), ".3cda.pl", ".cda.pl");
    if (contains(a, "/upstream")) return "https://" + replaceAll(a, "/upstream", ".mp4/upstream");
    return "https://" + a + ".mp4";
}

std::vector<Video> cda(const std::string& url, const std::string& prefix = "") {
    http::Headers h = {
        {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8"}};
    std::string body = httpGet(url, h);
    if (contains(body, "został usunięty przez jego właściciela lub Administratora")) return {};
    html::Document doc(body, url);
    json data = parseJson(doc.selectFirst("div[player_data]").attr("player_data"));
    const json& video = jobj(data, "video");
    std::string cur = jstr(video, "quality");
    std::vector<Video> out;
    if (!jobj(video, "qualities").is_object()) return {};
    for (auto& q : jobj(video, "qualities").items()) {
        std::string key = q.key(), value = jval(q.value());
        try {
            std::string videoUrl;
            if (value == cur && value != "lq") {
                videoUrl = cdaDecrypt(jstr(video, "file"));
            } else {
                std::string rpc = "{\"jsonrpc\":\"2.0\",\"method\":\"videoGetLink\",\"id\":1,\"params\":[\"" +
                                  jstr(video, "id") + "\",\"" + value + "\"," + jstr(video, "ts") + ",\"" +
                                  jstr(video, "hash2") + "\",{}]}";
                json res = parseJson(httpPost("https://www.cda.pl/", rpc, {{"X-Requested-With", "XMLHttpRequest"}},
                                              "application/json"));
                videoUrl = jstr(jobj(res, "result"), "resp");
            }
            if (startsWith(videoUrl, "http")) out.push_back(mkVideo(videoUrl, prefix + "cda.pl - " + key));
        } catch (const std::exception&) {
        }
    }
    return out;
}

// ---- LuluStream (lib/luluextractor)
std::vector<Video> lulu(const std::string& url, const std::string& prefix = "") {
    http::Headers h = {{"Referer", "https://luluvdo.com/"}, {"Origin", "https://luluvdo.com"}};
    std::string html = httpGet(url, h);
    std::string link;
    if (contains(html, "eval(function(p,a,c,k,e")) {
        html::Document doc(html, url);
        std::string packed = scriptWith(doc, {"eval(function(p,a,c,k,e"});
        std::string un = packed.empty() ? "" : unpacker::unpackAndCombine(packed);
        link = substringBefore(substringAfter(un, "sources:[{file:\""), "\"");
        if (link == un) link.clear();
    } else {
        std::string after = substringAfter(html, "sources: [{file:\"");
        if (after != html) link = substringBefore(after, "\"");
    }
    if (!startsWith(link, "http")) return {};
    // riordina i parametri come fa l'estensione (t, s, e, f + i=0.3&sp=0)
    std::string base = substringBefore(link, "?");
    std::string query = substringAfter(link, "?");
    const char* order[] = {"t", "s", "e", "f"};
    std::vector<std::pair<std::string, std::string>> named, positional;
    if (query != link) {
        size_t idx = 0;
        for (auto& kv : splitStr(query, "&")) {
            std::string k = substringBefore(kv, "="), v = substringAfter(kv, "=");
            if (!contains(kv, "=")) continue;
            if (k.empty()) {
                if (idx < 4) positional.push_back({order[idx], v});
            } else {
                named.push_back({k, v});
            }
            idx++;
        }
    }
    named.push_back({"i", "0.3"});
    named.push_back({"sp", "0"});
    std::string fixed = base + "?";
    bool first = true;
    for (auto& kv : positional) {
        fixed += (first ? "" : "&") + kv.first + "=" + kv.second;
        first = false;
    }
    for (auto& kv : named) {
        fixed += (first ? "" : "&") + kv.first + "=" + kv.second;
        first = false;
    }
    return hlsVideos(fixed, "https://luluvdo.com/", prefix + "Lulu - ");
}

// ---- Streamup (lib/streamupextractor)
std::vector<Video> streamup(const std::string& url, const std::string& prefix = "") {
    http::Response r = http::request("GET", url);
    checkStatus(r, url);
    const std::string& body = r.body;
    std::string base = http::originOf(r.finalUrl.empty() ? url : r.finalUrl);
    std::string stream;
    if (contains(body, "decodePrintable95(\"")) {
        std::string enc = substringBefore(substringAfter(body, "decodePrintable95(\""), "\"");
        std::string shiftStr = trim(substringAfter(substringAfter(body, "__enc_shift"), "="));
        int shift = std::atoi(shiftStr.c_str());
        std::string raw;
        try {
            raw = crypto::fromHex(enc);
        } catch (const std::exception&) {
        }
        for (size_t i = 0; i < raw.size(); i++) {
            int s = (unsigned char)raw[i] - 32;
            int v = (s - shift - (int)i) % 95;
            if (v < 0) v += 95;
            stream += (char)(v + 32);
        }
        if (!startsWith(stream, "http")) stream.clear();
    }
    if (stream.empty()) {
        // '([a-f0-9]{32})' e '([A-Za-z0-9+/=]{200,})'
        std::string session, encrypted;
        size_t pos = 0;
        while ((pos = body.find('\'', pos)) != std::string::npos) {
            size_t e = body.find('\'', pos + 1);
            if (e == std::string::npos) break;
            std::string c = body.substr(pos + 1, e - pos - 1);
            pos = e + 1;
            bool hex = c.size() == 32, b64s = c.size() >= 200;
            for (char ch : c) {
                if (!(std::isdigit((unsigned char)ch) || (ch >= 'a' && ch <= 'f'))) hex = false;
                if (!(std::isalnum((unsigned char)ch) || ch == '+' || ch == '/' || ch == '=')) b64s = false;
            }
            if (hex && session.empty()) session = c;
            if (b64s && encrypted.empty()) encrypted = c;
        }
        if (!session.empty() && !encrypted.empty()) {
            std::string key = b64(trim(httpGet(base + "/ajax/stream?session=" + session,
                                               {{"Referer", url}, {"X-Requested-With", "XMLHttpRequest"}})));
            std::string data = b64(encrypted);
            if (data.size() > 16) {
                std::string dec = crypto::aesCbcDecrypt(data.substr(16), key, data.substr(0, 16));
                stream = jstr(parseJson(dec), "streaming_url");
            }
        } else {
            std::string mediaId = afterLast(url, "/");
            stream = jstr(parseJson(httpGet(base + "/ajax/stream?filecode=" + mediaId, {{"Referer", url}})),
                          "streaming_url");
        }
    }
    if (stream.empty()) return {};
    return {mkVideo(stream, prefix + "Streamup", base + "/", {{"Origin", base}})};
}

// ---- Streamlare (lib/streamlareextractor)
std::vector<Video> streamlare(const std::string& url, const std::string& prefix = "") {
    std::string id = afterLast(url, "/");
    std::string playlist = httpPost("https://slwatch.co/api/video/stream/get", "{\"id\":\"" + id + "\"}", {},
                                    "application/json");
    std::string label = trim(prefix).empty() ? "" : trim(prefix) + " ";
    std::string type = substringBefore(substringAfter(playlist, "\"type\":\""), "\"");
    std::vector<Video> out;
    if (type == "hls") {
        std::string master = replaceAll(substringBefore(substringAfter(playlist, "\"file\":\""), "\""), "\\/", "/");
        return hlsVideos(master, "", label + "Streamlare:");
    }
    auto parts = splitStr(playlist, "\"label\":\"");
    for (size_t i = 1; i < parts.size(); i++) {
        std::string q = substringBefore(parts[i], "\",");
        std::string api = replaceAll(substringBefore(substringAfter(parts[i], "\"file\":\""), "\","), "\\", "");
        try {
            http::Response r = http::request("POST", api);
            std::string final = r.finalUrl.empty() ? api : r.finalUrl;
            out.push_back(mkVideo(final, label + "Streamlare:" + q));
        } catch (const std::exception&) {
        }
    }
    return out;
}

// ---- MixDrop (lib/mixdropextractor)
std::vector<Video> mixDrop(const std::string& url, const std::string& prefix = "", const std::string& lang = "") {
    const std::string referer = "https://mixdrop.co/";
    http::Headers h = {{"Referer", referer}, {"User-Agent", DESKTOP_UA}};
    DocPtr doc = getDoc(url, h);
    std::string packed = scriptAll(*doc, {"eval", "MDCore"});
    if (packed.empty()) return {};
    std::string un = unpacker::unpackAndCombine(packed);
    if (un.empty()) return {};
    std::string videoUrl = "https:" + substringBefore(substringAfter(un, "Core.wurl=\""), "\"");
    Video v = mkVideo(videoUrl, prefix + "MixDrop" + (lang.empty() ? "" : "(" + lang + ")"), referer);
    v.userAgent = DESKTOP_UA;
    if (contains(un, "Core.remotesub=\"")) {
        std::string sub = substringBefore(substringAfter(un, "Core.remotesub=\""), "\"");
        if (!trim(sub).empty()) v.subtitles.push_back({urlDecode(sub), "sub"});
    }
    return {v};
}

// ---- 4shared (estrattore "SharedExtractor" delle estensioni arabe)
std::vector<Video> shared4(const std::string& url, const std::string& quality = "mirror") {
    DocPtr doc = getDoc(url);
    std::string src = doc->selectFirst("source").attr("src");
    if (src.empty()) return {};
    return {mkVideo(src, "4Shared: " + quality)};
}

// ---- VidYard (ar/anime4up/extractors/VidYardExtractor)
std::vector<Video> vidYard(const std::string& url) {
    const std::string VY = "https://play.vidyard.com";
    std::string id = substringBefore(substringAfter(url, "com/"), "?");
    std::string body = httpGet(VY + "/player/" + id + ".json", {{"Referer", VY}});
    std::string data = substringBefore(substringAfter(body, "hls\":["), "]");
    auto parts = splitStr(data, "profile\":\"");
    std::vector<Video> out;
    for (size_t i = 1; i < parts.size(); i++) {
        std::string src = substringBefore(substringAfter(parts[i], "url\":\""), "\"");
        std::string quality = substringBefore(parts[i], "\"");
        if (startsWith(src, "http")) out.push_back(mkVideo(src, quality, VY));
    }
    return out;
}

// ---- SoraPlay (ar/witanime/extractors/SoraPlayExtractor)
std::vector<Video> soraPlay(const std::string& url) {
    const std::string ref = "https://yonaplay.org/";
    DocPtr doc = getDoc(url, {{"Referer", ref}});
    std::string script = scriptWith(*doc, {"sources"});
    if (script.empty()) return {};
    std::string data = substringBefore(substringAfter(script, "sources: ["), "],");
    auto parts = splitStr(data, "\"file\":\"");
    std::vector<Video> out;
    for (size_t i = 1; i < parts.size(); i++) {
        std::string src = substringBefore(parts[i], "\"");
        std::string q = "Soraplay: " + substringBefore(substringAfter(parts[i], "\"label\":\""), "\"");
        out.push_back(mkVideo(src, q, ref));
    }
    return out;
}

// ---- Videa (tr/hentaizm/extractors/VideaExtractor)
std::vector<Video> videa(const std::string& url) {
    const std::string KEY = "xHb0ZvME5q8CBcoQi6AngerDu3FGO9fkUlwPmLVY_RTzj2hJIS4NasXWKy1td7p";
    std::string body = httpGet(url);
    std::string nonce;
    size_t p = 0;
    while ((p = body.find("_xt", p)) != std::string::npos) {
        size_t q = p + 3;
        while (q < body.size() && std::isspace((unsigned char)body[q])) q++;
        if (q < body.size() && body[q] == '=') {
            q++;
            while (q < body.size() && std::isspace((unsigned char)body[q])) q++;
            if (q < body.size() && body[q] == '"') {
                nonce = substringBefore(body.substr(q + 1), "\"");
                break;
            }
        }
        p += 3;
    }
    if (nonce.size() < 64) return {};
    std::string l = nonce.substr(0, 32), s = nonce.substr(32), result;
    for (int i = 0; i < 32; i++) {
        long idx = i - ((long)KEY.find(l[i]) - 31);
        if (idx < 0 || idx >= (long)s.size()) return {};
        result += s[idx];
    }
    std::string seed = randomAlnum(8);
    std::string v = http::queryParam(url, "v");
    std::string req = "https://videa.hu/player/xml?platform=desktop&_s=" + seed + "&_t=" + result.substr(0, 16) +
                      "&v=" + http::urlEncode(v);
    http::Headers h = {{"Referer", url}, {"Origin", "https://videa.hu"}};
    http::Response r = http::request("GET", req, h);
    checkStatus(r, req);
    std::string xml = r.body;
    if (!startsWith(xml, "<?xml")) {
        std::string key = result.substr(16) + seed + r.header("x-videa-xs");
        xml = crypto::rc4(key, b64(xml));
    }
    html::Document doc(xml, req);
    std::vector<Video> out;
    for (auto& src : doc.select("video_source")) {
        std::string name = src.attr("name");
        html::Node hash = doc.selectFirst("hash_value_" + name);
        if (!hash) continue;
        std::string videoUrl = "https:" + trim(src.text()) + "?md5=" + trim(hash.text()) + "&expires=" + src.attr("exp");
        out.push_back(mkVideo(videoUrl, "Videa - " + name, url, {{"Origin", "https://videa.hu"}}));
    }
    return out;
}

// ---- Kodik (ru/yummyanime, ru/animelib). Il player codifica i link con uno spostamento ROT-N + base64 che
// l'estensione decifra eseguendo il JS con QuickJS: qui si prova lo spostamento finche' il risultato e' un URL.
std::string kodikDecode(const std::string& src) {
    if (startsWith(src, "//") || startsWith(src, "http")) return fixUrl(src);
    std::vector<int> shifts = {13, 18};
    for (int i = 0; i < 26; i++)
        if (i != 13 && i != 18) shifts.push_back(i);
    for (int shift : shifts) {
        std::string t = src;
        for (auto& c : t) {
            if (c >= 'A' && c <= 'Z') c = (char)('A' + (c - 'A' + shift) % 26);
            else if (c >= 'a' && c <= 'z') c = (char)('a' + (c - 'a' + shift) % 26);
        }
        std::string d = b64(t);
        if (startsWith(d, "//") || startsWith(d, "http")) return fixUrl(d);
    }
    return "";
}

std::vector<Video> kodik(const std::string& rawIframe, const std::string& siteReferer, const std::string& siteOrigin,
                         const std::string& label, const http::Headers& extra = {}, bool postToPd = false) {
    std::string iframe = fixUrl(rawIframe);
    http::Headers h = withHeaders({{"Referer", siteReferer}}, extra);
    std::string page = httpGet(iframe, h);
    html::Document doc(page, iframe);

    std::string rawParams;
    size_t up = page.find("urlParams");
    if (up != std::string::npos) {
        size_t q = page.find_first_of("'\"", up);
        if (q != std::string::npos && q - up < 16) {
            size_t e = page.find(page[q], q + 1);
            if (e != std::string::npos) rawParams = page.substr(q + 1, e - q - 1);
        }
    }
    json form = json::parse(rawParams, nullptr, false);
    if (!form.is_object() || jstr(form, "d_sign").empty()) return {};

    std::string type, hash, id;
    for (auto& s : doc.select("script")) {
        std::string d = s.data();
        std::string t = jsAssign(d, ".type"), hh = jsAssign(d, ".hash"), ii = jsAssign(d, ".id");
        if (!t.empty() && !hh.empty() && !ii.empty()) {
            type = t, hash = hh, id = ii;
            break;
        }
    }
    std::string noScheme = substringAfter(iframe, "://");
    auto parts = splitStr(noScheme, "/");
    if (type.empty() && parts.size() > 1) type = parts[1];
    if (id.empty() && parts.size() > 2) id = parts[2];
    if (hash.empty() && parts.size() > 3) hash = parts[3];
    if (type.empty() || id.empty() || hash.empty()) return {};

    Form f = {{"d", jstr(form, "d")},
              {"d_sign", urlDecode(jstr(form, "d_sign"))},
              {"pd", jstr(form, "pd")},
              {"pd_sign", urlDecode(jstr(form, "pd_sign"))},
              {"ref", urlDecode(jstr(form, "ref"))},
              {"ref_sign", urlDecode(jstr(form, "ref_sign"))},
              {"type", type},
              {"id", id},
              {"hash", hash},
              {"bad_user", "true"},
              {"cdn_is_working", "true"}};
    std::string host = parts.empty() || parts[0].empty() ? "kodikplayer.com" : parts[0];
    if (postToPd && !jstr(form, "pd").empty()) host = jstr(form, "pd");
    http::Headers ph = withHeaders({{"Referer", siteReferer}, {"Origin", siteOrigin}}, extra);
    json data = parseJson(httpPost("https://" + host + "/ftor", formBody(f), ph));
    const json& links = jobj(data, "links");
    std::vector<Video> out;
    for (const char* q : {"720", "480", "360"}) {
        const json& arr = jarr(links, q);
        if (arr.empty()) continue;
        std::string url = kodikDecode(jstr(arr[0], "src"));
        if (url.empty()) continue;
        Video v = mkVideo(url, label + " (" + std::string(q) + "p Kodik)", siteReferer, withHeaders({{"Origin", siteOrigin}}, extra));
        v.quality = std::atoi(q);
        out.push_back(v);
    }
    return out;
}

// ---- Megamax multi-server (lib/megamaxmultiserver)
struct Provider {
    std::string url, name, quality;
};

std::vector<Provider> megamax(const std::string& url, const http::Headers& headers) {
    std::string type = contains(url, "/iframe/") ? "mirror" : "leech";
    std::string page = httpGet(url, headers);
    std::string version = substringBefore(substringAfter(page, ",\"version\":\""), "\"");
    if (version == page) version.clear();
    http::Headers h = withHeaders(headers, {{"X-Inertia", "true"},
                                            {"X-Inertia-Partial-Component", "files/" + type + "/video"},
                                            {"X-Inertia-Partial-Data", "streams"},
                                            {"X-Inertia-Version", version}});
    json j = parseJson(httpGet(url, h));
    const json& data = jarr(jobj(jobj(j, "props"), "streams"), "data");
    std::vector<Provider> out;
    for (auto& d : data) {
        if (type == "mirror") {
            int res = std::atoi(substringAfter(jstr(d, "resolution"), "x").c_str());
            std::string q = jstr(d, "resolution");
            if (res > 0) {
                int best = 144;
                for (int s : {144, 240, 360, 480, 720, 1080})
                    if (std::abs(s - res) < std::abs(best - res)) best = s;
                q = std::to_string(best) + "p";
            }
            for (auto& m : jarr(d, "mirrors")) {
                std::string link = fixUrl(jstr(m, "link"));
                if (!link.empty()) out.push_back({link, jstr(m, "driver"), q});
            }
        } else {
            out.push_back({jstr(d, "file"), "Leech", substringBefore(jstr(d, "label"), " ")});
        }
    }
    return out;
}

// ---- VidLand / earnvids (lib/vidlandextractor)
std::vector<Video> vidLand(const std::string& url) {
    DocPtr doc = getDoc(url);
    std::string packed = scriptWith(*doc, {"eval"});
    if (packed.empty()) return {};
    std::string un = unpacker::unpackAndCombine(packed);
    for (const char* k : {"hls4\":", "hls3\":"}) {
        std::string after = substringAfter(un, k);
        if (after == un) continue;
        std::string link = substringBefore(substringAfter(after, "\""), "\"");
        if (!link.empty()) return hlsVideos(fixUrl(link, url), url, "VidLand: ");
    }
    return {};
}

// ---- Aincrad / anizmplayer (tr/anizm/extractors/AincradExtractor)
std::vector<Video> aincrad(const std::string& url) {
    const std::string DOMAIN = "https://anizmplayer.com";
    std::string hash = substringBefore(afterLast(url, "video/"), "/");
    http::Headers h = {{"Origin", DOMAIN}, {"Referer", url}, {"X-Requested-With", "XMLHttpRequest"}};
    json j = parseJson(httpPost(DOMAIN + "/player/index.php?data=" + hash + "&do=getVideo",
                                formBody({{"hash", hash}, {"r", "https://anizm.net/"}}), h));
    std::string link = jstr(j, "securedLink");
    if (link.empty()) return {};
    return hlsVideos(link, url, "Aincrad - ");
}

// ---- Rapidrame / Closeload / XBet (tr/hdfilmcehennemi/extractors)
std::string unmixBytes(const std::string& data) {
    std::string out;
    for (size_t i = 0; i < data.size(); i++) {
        int c = (signed char)data[i];
        c = ((c - (int)(399756995 % (i + 5)) + 256) % 256 + 256) % 256;
        out += (char)c;
    }
    return out;
}

std::string base64Rot13ReverseUnmix(const std::string& value) {
    std::string d = b64(value);
    for (auto& c : d) {
        if (c >= 'A' && c <= 'Z') c = (char)('A' + (c - 'A' + 13) % 26);
        else if (c >= 'a' && c <= 'z') c = (char)('a' + (c - 'a' + 13) % 26);
    }
    std::reverse(d.begin(), d.end());
    return unmixBytes(d);
}

/** Argomento array di "x = f([...])" nello script spacchettato, unito e senza virgolette. */
std::string partsValue(const std::string& script) {
    size_t pos = 0;
    while ((pos = script.find("([", pos)) != std::string::npos) {
        size_t p = pos;
        pos += 2;
        size_t e = p;
        while (e > 0 && (std::isalnum((unsigned char)script[e - 1]) || script[e - 1] == '_')) e--;
        if (e == p) continue;
        size_t b = e;
        while (b > 0 && std::isspace((unsigned char)script[b - 1])) b--;
        if (b == 0 || script[b - 1] != '=') continue;
        size_t end = script.find("])", p);
        if (end == std::string::npos) break;
        std::string inner = script.substr(p + 2, end - p - 2), joined;
        for (auto& part : splitStr(inner, ",")) {
            std::string t = trim(part);
            if (!t.empty() && t.front() == '"') t.erase(0, 1);
            if (!t.empty() && t.back() == '"') t.pop_back();
            joined += t;
        }
        return joined;
    }
    return "";
}

std::vector<Video> rapidrame(const std::string& url, const std::string& label, const http::Headers& headers) {
    DocPtr doc = getDoc(url, headers);
    std::string script = scriptWith(*doc, {"eval"});
    if (script.empty()) return {};
    std::string un = unpacker::unpackAndCombine(script);
    std::string parts = partsValue(un);
    if (parts.empty()) return {};
    std::string playlist = base64Rot13ReverseUnmix(parts);
    std::string host = "https://" + http::hostOf(url);
    std::vector<Video::Track> subs;
    size_t tp = script.find("tracks:");
    if (tp != std::string::npos) {
        size_t lb = script.find('[', tp), rb = lb == std::string::npos ? lb : script.find(']', lb);
        if (rb != std::string::npos) {
            json tracks = json::parse(script.substr(lb, rb - lb + 1), nullptr, false);
            if (tracks.is_array())
                for (auto& t : tracks)
                    if (jstr(t, "kind") == "captions")
                        subs.push_back({fixUrl(jstr(t, "file"), host), "[" + jstr(t, "language") + "] " + jstr(t, "label")});
        }
    }
    return hlsVideos(playlist, url, label + " - ", subs, {{"Origin", host}});
}

std::vector<Video> closeload(const std::string& url, const std::string& name, const http::Headers& headers) {
    DocPtr doc = getDoc(url, headers);
    std::string script = scriptAll(*doc, {"eval", "PlayerInit"});
    if (script.empty()) return {};
    std::string un = unpacker::unpackAndCombine(script);
    std::string parts = partsValue(un);
    if (parts.empty()) return {};
    std::string playlist = base64Rot13ReverseUnmix(parts);
    std::string host = "https://" + http::hostOf(url);
    try {
        std::string hash = substringBefore(substringAfter(un, "hash:\""), "\"");
        std::string ajax = fixUrl(substringBefore(substringAfter(un, "url:\""), "\""), host);
        if (startsWith(ajax, "http")) http::request("POST", ajax, withHeaders(headers, {{"Content-Type", "application/x-www-form-urlencoded"}}), formBody({{"hash", hash}}));
    } catch (const std::exception&) {
    }
    Video v = mkVideo(playlist, name, url, {{"Origin", host}});
    for (auto& t : doc->select("track[src]")) {
        std::string lang = t.attr("label").empty() ? t.attr("srclang") : t.attr("label");
        v.subtitles.push_back({doc->absUrl(t, "src"), lang});
    }
    return {v};
}

std::vector<Video> xbet(const std::string& url, const http::Headers& headers) {
    DocPtr doc = getDoc(url, headers);
    std::string script = scriptWith(*doc, {"playerConfigs ="});
    if (script.empty()) return {};
    std::string host = "https://" + http::hostOf(url);
    std::string postPath = replaceAll(substringBefore(substringAfter(script, "file\":\""), "\""), "\\", "");
    http::Headers ph = withHeaders(headers, {{"Referer", url}, {"Origin", host}});
    json list = parseJson(replaceAll(httpPost(host + postPath, "", ph), "[],", ""));
    std::vector<Video> out;
    if (!list.is_array()) return out;
    for (auto& item : list) {
        tryAppend(out, [&] {
            std::string path = "/playlist/" + removeSuffix(jstr(item, "file"), "~") + ".txt";
            std::string playlist = trim(httpPost(host + path, "", ph));
            return hlsVideos(playlist, url, "[" + jstr(item, "title") + "] XBet - ");
        });
    }
    return out;
}

// =============================================================================================== base comune

class Base : public Source {
  public:
    Base(const char* id, const char* name, const char* url, const char* lang, bool nsfw = false, bool latest = true)
        : id_(id), name_(name), url_(url), lang_(lang), nsfw_(nsfw), latest_(latest) {}
    std::string id() const override { return id_; }
    std::string name() const override { return name_; }
    std::string defaultBaseUrl() const override { return url_; }
    std::string lang() const override { return lang_; }
    bool nsfw() const override { return nsfw_; }
    bool supportsLatest() const override { return latest_; }
    Page latest(int) override { throw http::Error("Ultimi aggiornamenti non disponibili per questa fonte"); }

  protected:
    std::string id_, name_, url_, lang_;
    bool nsfw_, latest_;

    virtual http::Headers headers() const { return {}; }
    std::string full(const std::string& u) const {
        if (startsWith(u, "http://") || startsWith(u, "https://")) return u;
        if (startsWith(u, "//")) return "https:" + u;
        if (!u.empty() && u[0] != '/') return baseUrl() + "/" + u;
        return baseUrl() + u;
    }
    DocPtr page(const std::string& u, const http::Headers& extra = {}) const {
        return getDoc(full(u), withHeaders(headers(), extra));
    }
    std::string get(const std::string& u, const http::Headers& extra = {}) const {
        return httpGet(full(u), withHeaders(headers(), extra));
    }
    std::string post(const std::string& u, const std::string& body, const http::Headers& extra = {},
                     const std::string& ct = "application/x-www-form-urlencoded") const {
        return httpPost(full(u), body, withHeaders(headers(), extra), ct);
    }
    static std::string rel(const html::Document& doc, const html::Node& n, const char* attr = "href") {
        std::string a = doc.absUrl(n, attr);
        return a.empty() ? "" : http::pathOf(a);
    }
    static std::string relUrl(const std::string& url) { return startsWith(url, "http") ? http::pathOf(url) : url; }

    /** Elenco generico: selettore degli elementi, conversione, selettore della pagina successiva. */
    static Page listOf(const html::Document& doc, const std::string& sel,
                       const std::function<Anime(const html::Node&)>& f, const std::string& nextSel = "") {
        Page p;
        std::set<std::string> seen;
        for (auto& el : doc.select(sel)) {
            Anime a;
            try {
                a = f(el);
            } catch (const std::exception&) {
                continue;
            }
            if (a.url.empty() || !seen.insert(a.url).second) continue;
            if (trim(a.title).empty()) a.title = a.url;
            p.animes.push_back(a);
        }
        if (!nextSel.empty()) p.hasNextPage = doc.selectFirst(nextSel).valid();
        return p;
    }
};

std::string statusFrom(const std::string& text, const char* ongoing, const char* completed) {
    if (contains(text, ongoing)) return "In corso";
    if (contains(text, completed)) return "Completato";
    return "";
}

// =============================================================================================== ARABO

// ---- Tema "anime-list" condiviso da Anime4Up (ar.anime4up) e WIT ANIME (ar.witanime)
class AnimeListTheme : public Base {
  public:
    enum Kind { ANIME4UP, WITANIME };
    AnimeListTheme(Kind k, const char* id, const char* name, const char* url)
        : Base(id, name, url, "ar", false, k == WITANIME), kind(k) {}

    Page popular(int page) override {
        if (kind == ANIME4UP) return list("/anime-list-3/page/" + std::to_string(page) + "/");
        return list("/قائمة-الانمي/page/" + std::to_string(page));
    }
    Page latest(int page) override {
        if (kind == ANIME4UP) return Base::latest(page);
        return list("/episode/page/" + std::to_string(page) + "/");
    }
    Page search(const std::string& q, int) override {
        return list("/?search_param=animes&s=" + http::urlEncode(q));
    }

    Details details(const std::string& url) override {
        DocPtr doc = realDoc(page(url));
        Details d;
        d.thumbnail = doc->selectFirst("img.thumbnail").attr("src");
        d.title = doc->selectFirst("h1.anime-details-title").text();
        d.genre = joinText(doc->select("ul.anime-genres > li > a, div.anime-info > a"));
        std::string desc;
        for (auto& i : doc->select("div.anime-info")) desc += i.text() + "\n";
        std::string story = doc->selectFirst("p.anime-story").text();
        if (!story.empty()) desc += "\n" + story;
        d.description = trim(desc);
        std::string st = firstContaining(doc->select("div.anime-info"), "حالة الأنمي").text();
        d.status = statusFrom(st, "يعرض الان", "مكتمل");
        const char* sel = kind == ANIME4UP ? "div.ehover6 > div.episodes-card-title > h3 > a, ul.all-episodes-list li > a"
                                           : "div.ehover6 > div.episodes-card-title > h3 a";
        for (auto& a : doc->select(sel)) {
            Episode e;
            e.url = kind == ANIME4UP ? rel(*doc, a) : relUrl(encodedUrl(a));
            e.name = a.text();
            e.number = toNumber(afterLast(e.name, " "), 0);
            if (!e.url.empty()) d.episodes.push_back(e);
        }
        std::reverse(d.episodes.begin(), d.episodes.end());
        return d;
    }

    std::vector<Video> videos(const std::string& url) override {
        DocPtr doc = page(url);
        std::vector<Video> out;
        if (kind == ANIME4UP) {
            // nome -> link: come "fhd + hd + sd" (mappe unite, l'ultima vince)
            std::vector<std::string> names;
            std::map<std::string, std::string> links;
            for (const char* q : {"watch_fhd", "watch_hd", "watch_SD"}) {
                std::string v = doc->selectFirst(std::string(".WatchServersEmbed form input[name='") + q + "']").attr("value");
                if (v.empty()) continue;
                json arr = json::parse(b64(v), nullptr, false);
                if (!arr.is_array()) continue;
                for (auto& s : arr) {
                    std::string n = jstr(s, "name");
                    if (!links.count(n)) names.push_back(n);
                    links[n] = jstr(s, "link");
                }
            }
            std::set<std::string> seen;
            for (auto& n : names) {
                std::string link = links[n];
                if (link.empty() || !seen.insert(link).second) continue;
                tryAppend(out, [&] { return anime4upVideos(link); });
            }
            finish(out, "1080p");
        } else {
            std::set<std::string> seenNames;
            for (auto& a : doc->select("ul#episode-servers li a")) {
                std::string server = substringBefore(a.text(), " -");
                if (!seenNames.insert(server).second) continue;
                std::string dataUrl = a.attr("data-url");
                std::string link = !trim(dataUrl).empty() ? b64(dataUrl) : encodedUrl(a);
                tryAppend(out, [&] { return witVideos(link, 0); });
            }
            finish(out, "1080");
        }
        return out;
    }

  protected:
    http::Headers headers() const override { return {{"Referer", kind == ANIME4UP ? baseUrl() + "/" : baseUrl()}}; }

  private:
    Kind kind;

    static std::string encodedUrl(const html::Node& el) {
        return b64(substringBefore(substringAfter(el.attr("onclick"), "'"), "'"));
    }

    Page list(const std::string& path) {
        DocPtr doc = page(path);
        const char* sel = kind == ANIME4UP ? "div.anime-list-content div.anime-card-poster > div.hover"
                                           : "div.anime-list-content div.row div.anime-card-poster div.ehover6";
        const char* next = kind == ANIME4UP ? "ul.pagination > li > a.next" : "ul.pagination a.next";
        return listOf(*doc, sel, [&](const html::Node& el) {
            Anime a;
            html::Node link = el.selectFirst("a");
            std::string href = link.attr("href");
            if (kind == WITANIME && contains(href, "javascript:")) href = encodedUrl(link);
            a.url = relUrl(http::resolve(doc->url(), href));
            html::Node img = el.selectFirst("img");
            a.thumbnail = doc->absUrl(img, "src");
            a.title = img.attr("alt");
            return a;
        }, next);
    }

    /** WIT ANIME: se la pagina e' un episodio, porta alla pagina dell'anime. */
    DocPtr realDoc(DocPtr doc) {
        if (kind != WITANIME) return doc;
        std::string href = doc->selectFirst("div.anime-page-link a").attr("href");
        if (href.empty()) return doc;
        return page(href);
    }

    std::vector<Video> anime4upVideos(const std::string& url) {
        if (contains(url, "drive.google")) return {};  // GdrivePlayer non supportato
        if (contains(url, "vidyard")) return vidYard(url);
        if (contains(url, "ok.ru")) return okru(url, "");
        if (contains(url, "mp4upload")) return mp4upload(url, "");
        if (contains(url, "uqload")) return uqload(url);
        if (contains(url, "voe")) return voe(url, "");
        if (contains(url, "shared")) return shared4(url);
        if (isDood(url)) return dood(url, "Dood mirror ");
        if (isVidbom(url)) return vidBom(url);
        if (anyIn(url, {"streamwish.", "anime7u.", "animezd.", "ajmidyad.", "khadhnayad.", "yadmalik.", "hayaatieadhab."}))
            return streamWish(url, "Mirror: ");
        return {};
    }

    std::vector<Video> witVideos(const std::string& url, int depth) {
        if (depth > 2) return {};
        if (contains(url, "yonaplay")) return multi(url, depth);
        if (contains(url, "soraplay")) return contains(url, "/mirror") ? multi(url, depth) : soraPlay(url);
        if (contains(url, "dood")) return dood(url, "Dood mirror ");
        if (contains(url, "4shared")) return shared4(url);
        if (contains(url, "dropbox")) return {mkVideo(url, "Dropbox mirror")};
        if (contains(url, "dailymotion")) return dailymotion(url, "Dailymotion - ");
        if (contains(url, "ok.ru")) return okru(url, "");
        if (contains(url, "mp4upload.com")) return mp4upload(url, "");
        static const std::regex vb("//v[aie]d[bp][aoe]?m");
        if (regexIn(url, vb)) return vidBom(url);
        return {};
    }

    std::vector<Video> multi(const std::string& url, int depth) {
        http::Headers h = contains(url, "soraplay") ? http::Headers{{"Referer", "https://yonaplay.org"}} : headers();
        DocPtr doc = getDoc(url, h);
        std::vector<Video> out;
        for (auto& li : doc->select(".OD li")) {
            std::string v = substringBefore(substringAfter(li.attr("onclick"), "go_to_player('"), "')");
            if (v.empty()) continue;
            if (!startsWith(v, "https:")) v = "https:" + v;
            tryAppend(out, [&] { return witVideos(v, depth + 1); });
        }
        return out;
    }
};

// ---- Animerco (ar.animerco)
class Animerco : public Base {
  public:
    Animerco() : Base("ar.animerco", "Animerco", "https://zeta.animerco.org", "ar") {}

    Page popular(int page) override { return list("/trending/page/" + std::to_string(page) + "/"); }
    Page latest(int page) override { return list("/page/" + std::to_string(page) + "/?s="); }
    Page search(const std::string& q, int page) override {
        return list("/page/" + std::to_string(page) + "/?s=" + http::urlEncode(q));
    }

    Details details(const std::string& url) override {
        DocPtr doc = page(url);
        Details d;
        html::Node poster = doc->selectFirst("a.poster");
        d.thumbnail = poster.attr("data-src");
        d.title = poster.attr("title");
        if (d.title.empty()) d.title = doc->selectFirst("div.media-title h1").text();
        html::Node infos = doc->selectFirst("ul.media-info");
        std::vector<std::string> people;
        for (const char* label : {"الشبكات", "الأستوديو"})
            for (auto& li : allContaining(infos.select("li"), label)) people.push_back(joinText(li.select("a")));
        d.author = joinStr(people);
        std::vector<std::string> genres;
        for (auto& a : doc->select("nav.Nvgnrs a")) genres.push_back(a.text());
        for (auto& li : allContaining(doc->select("ul.media-info li"), "النوع"))
            for (auto& a : li.select("a")) genres.push_back(a.text());
        d.genre = joinStr(genres);
        std::string desc;
        std::string score = doc->selectFirst(".media-rating .score").text();
        double sc = toNumber(score);
        if (sc >= 0) {
            int stars = (int)(sc / 2 + 0.5);
            for (int i = 0; i < 5; i++) desc += i < stars ? "★" : "☆";
            desc += " " + score + "\n";
        }
        desc += doc->selectFirst("div.media-story p").text();
        std::string alt = doc->selectFirst("div.media-title > h3.alt-title").text();
        if (!alt.empty()) desc += "\n\nAlternative title: " + alt;
        d.description = desc;
        std::vector<std::string> badges;
        for (auto& b : doc->select("ul.chapters-list a.se-title > span.badge")) badges.push_back(b.text());
        bool all = !badges.empty(), any = false;
        for (auto& b : badges) {
            if (!contains(b, "مكتمل")) all = false;
            if (contains(b, "يعرض الأن")) any = true;
        }
        d.status = all ? "Completato" : any ? "In corso" : "";

        if (contains(doc->url(), "/movies/")) {
            d.episodes.push_back({http::pathOf(doc->url()), "فيلم", 1});
            return d;
        }
        std::vector<std::vector<Episode>> seasons;
        for (auto& a : episodeLinks(*doc)) {
            std::vector<Episode> eps;
            try {
                DocPtr sdoc = page(doc->absUrl(a, "href"));
                std::string seasonName = sdoc->selectFirst("div.media-title h1").text();
                if (seasonName.empty()) seasonName = "Season";
                double sn = toNumber(afterLast(seasonName, " "));
                int seasonNum = sn >= 0 ? (int)sn : 1;
                for (auto& el : episodeLinks(*sdoc)) {
                    Episode e;
                    e.url = rel(*sdoc, el);
                    std::string epText = ownText(el.selectFirst("h3"));
                    if (epText.empty()) epText = "Episode";
                    e.name = epText + " - " + seasonName;
                    std::string num = digitsOnly(epText);
                    while (num.size() < 3) num = "0" + num;
                    e.number = toNumber(std::to_string(seasonNum) + "." + num, 1);
                    eps.push_back(e);
                }
            } catch (const std::exception&) {
            }
            std::reverse(eps.begin(), eps.end());
            seasons.push_back(eps);
        }
        std::vector<Episode> flat;
        for (auto& s : seasons) flat.insert(flat.end(), s.begin(), s.end());
        std::reverse(flat.begin(), flat.end());
        d.episodes = flat;
        return d;
    }

    std::vector<Video> videos(const std::string& url) override {
        DocPtr doc = page(url);
        std::vector<Video> out;
        for (auto& p : doc->select("ul.server-list > li > a")) {
            tryAppend(out, [&]() -> std::vector<Video> {
                std::string body = post("/wp-admin/admin-ajax.php",
                                        formBody({{"action", "player_ajax"},
                                                  {"post", p.attr("data-post")},
                                                  {"nume", p.attr("data-nume")},
                                                  {"type", p.attr("data-type")}}));
                std::string u = replaceAll(substringBefore(substringAfter(body, "\"embed_url\":\""), "\","), "\\", "");
                if (trim(u).empty() || u == body) return {};
                std::string name = lower(p.selectFirst("span.server").text());
                if (contains(u, "ok.ru")) return okru(u, "");
                if (contains(u, "mp4upload")) return mp4upload(u, "");
                if (contains(name, "wish")) return streamWish(u, "");
                if (contains(u, "yourupload")) return yourUpload(u);
                if (contains(u, "dood")) return dood(u, "");
                if (contains(u, "drive.google")) return {};
                if (contains(u, "streamtape")) return streamtape(u, "");
                if (contains(u, "4shared")) return shared4(u);
                if (contains(u, "uqload")) return uqload(u);
                if (anyIn(u, {"vidbom.com", "vidbem.com", "vidbm.com", "vedpom.com", "vedbom.com", "vedbom.org",
                              "vadbom.com", "vidbam.org", "myviid.com", "myviid.net", "myvid.com", "vidshare.com",
                              "vedsharr.com", "vedshar.com", "vedshare.com", "vadshar.com", "vidshar.org"}))
                    return vidBom(u);
                return {};
            });
        }
        finish(out, "1080");
        return out;
    }

  private:
    Page list(const std::string& path) {
        DocPtr doc = page(path);
        Page p = listOf(*doc, "div.media-block > div > a.image", [&](const html::Node& el) {
            return Anime{rel(*doc, el), el.attr("title"), el.attr("data-src")};
        });
        // ul.pagination li:last-child a:has(svg)
        auto lis = doc->select("ul.pagination li");
        for (auto& li : lis)
            if (isLastChild(li))
                for (auto& a : li.select("a"))
                    if (a.selectFirst("svg")) p.hasNextPage = true;
        return p;
    }

    static std::vector<html::Node> episodeLinks(const html::Document& doc) {
        std::vector<html::Node> out;
        for (auto& a : doc.select("ul.episodes-lists li a"))
            if (a.selectFirst("h3")) out.push_back(a);
        return out;
    }
};

// ---- AnimeLek (ar.animelek)
class AnimeLek : public Base {
  public:
    AnimeLek() : Base("ar.animelek", "AnimeLek", "https://animelek.me", "ar") {}

    Page popular(int) override {
        DocPtr doc = page("/");
        return listOf(*doc, "div.slider-episode-container div.episodes-card-container", card(*doc));
    }
    Page latest(int page) override {
        DocPtr doc = this->page("/episode/?page=" + std::to_string(page));
        return listOf(*doc, "div.episodes-list-content div.episodes-card-container", card(*doc), "li.page-item a[rel=next]");
    }
    Page search(const std::string& q, int page) override {
        DocPtr doc = this->page("/search/?s=" + http::urlEncode(q) + "&page=" + std::to_string(page));
        return listOf(*doc, "div.anime-list-content div.anime-card-container", card(*doc), "li.page-item a[rel=next]");
    }

    Details details(const std::string& url) override {
        DocPtr doc = page(url);
        Details d;
        html::Node infos = doc->selectFirst("div.anime-container-infos");
        html::Node datas = doc->selectFirst("div.anime-container-data");
        d.thumbnail = infos.selectFirst("img").attr("src");
        d.title = datas.selectFirst("h1").text();
        d.genre = joinText(datas.select("ul li > a"));
        d.status = statusFrom(firstContaining(infos.select("div.full-list-info"), "حالة الأنمي").selectFirst("a").text(),
                              "يعرض الان", "مكتمل");
        d.author = innermostContaining(doc->select("div"), "المخرج").selectFirst("span.info").text();
        std::string desc = datas.selectFirst("p.anime-story").text() + "\n\n";
        for (auto& i : infos.select("div.full-list-info")) desc += i.text() + "\n";
        d.description = trim(desc);
        for (auto& a : doc->select("div.ep-card-anime-title-detail h3 a")) {
            Episode e;
            e.url = rel(*doc, a);
            e.name = a.text();
            std::string n = digitsOnly(e.name);
            e.number = n.empty() ? 1 : toNumber(n, 1);
            d.episodes.push_back(e);
        }
        std::reverse(d.episodes.begin(), d.episodes.end());
        return d;
    }

    std::vector<Video> videos(const std::string& url) override {
        DocPtr doc = page(url);
        std::vector<Video> out;
        for (auto& el : doc->select("ul#episode-servers li.watch a")) {
            std::string u = el.attr("data-ep-url"), q = el.text();
            tryAppend(out, [&]() -> std::vector<Video> {
                if (anyIn(u, {"vidbam", "vadbam", "vidbom", "vidbm"})) return vidBom(u);
                if (contains(u, "dood")) return dood(u, q + " ");
                if (contains(u, "ok.ru")) return okru(u, "");
                if (contains(u, "streamtape")) return streamtape(u, "");
                if (contains(u, "4shared")) return shared4(u, q);
                return {};
            });
        }
        finish(out, "720p");
        return out;
    }

  private:
    static std::function<Anime(const html::Node&)> card(const html::Document& doc) {
        return [&doc](const html::Node& el) {
            html::Node a = el.selectFirst("h3 a");
            return Anime{rel(doc, a), a.text(), el.selectFirst("img").attr("src")};
        };
    }
};

// ---- Okanime (ar.okanime)
class Okanime : public Base {
  public:
    Okanime() : Base("ar.okanime", "Okanime", "https://ww3.okanime.xyz", "ar") {}

    Page popular(int) override {
        DocPtr doc = page("/");
        return cards(*doc, true);
    }
    Page latest(int page) override {
        DocPtr doc = this->page("/recently-uploaded-episodes?page=" + std::to_string(page));
        return cards(*doc, false);
    }
    Page search(const std::string& q, int page) override {
        std::string path = "/search/?s=" + http::urlEncode(q) + (page > 1 ? "&page=" + std::to_string(page) : "");
        DocPtr doc = this->page(path);
        return cards(*doc, true);
    }

    Details details(const std::string& url) override {
        DocPtr doc = page(toAnimeUrl(url));
        Details d;
        // nuovo layout (2026): animepage-*
        if (doc->selectFirst("div.animepage-sidebar, dl.animepage-meta")) {
            for (auto& h : doc->select("h1.animepage-h1, h1"))
                if (!trim(h.text()).empty()) {
                    d.title = trim(h.text());
                    break;
                }
            if (d.title.empty()) d.title = doc->selectFirst(".animepage-h1").text();
            d.thumbnail = doc->absUrl(doc->selectFirst("img.animepage-poster"), "src");
            d.genre = joinText(doc->select("div.animepage-genres a"));
            d.description = trim(doc->selectFirst("div.synopsis-text").text());
            std::string info;
            for (auto& row : doc->select("dl.animepage-meta div.animepage-meta-row")) {
                std::string k = trim(row.selectFirst("dt").text()), v = trim(row.selectFirst("dd").text());
                if (k == "الحالة") d.status = contains(v, "يعرض") ? "In corso" : contains(v, "مكتمل") ? "Completato" : "";
                if (!k.empty()) info += "\n" + k + ": " + v;
            }
            if (!info.empty()) d.description = trim(d.description + "\n" + info);
            for (auto& a : doc->select("div.ep-compact-grid a, a.ep-compact-btn, div.ep-list a[href*='/episode/'], div.episode-card a[href*='/episode/']")) {
                Episode e;
                e.url = rel(*doc, a);
                if (e.url.empty()) continue;
                bool dup = false;
                for (auto& x : d.episodes)
                    if (x.url == e.url) dup = true;
                if (dup) continue;
                std::string t = trim(a.attr("title"));
                if (t.empty()) t = trim(a.text());
                std::string n = afterLast(e.url, "-episode-");
                e.number = toNumber(n, toNumber(digitsOnly(t), 1));
                e.name = t.empty() ? "الحلقة " + numStr(e.number) : t;
                d.episodes.push_back(e);
            }
            newestFirst(d.episodes);
            return d;
        }
        // vecchio layout
        d.title = doc->selectFirst("div.author-info-title > h1").text();
        d.genre = joinText(doc->select("div.review-author-info a"));
        html::Node infos = doc->selectFirst("div.text-right");
        d.thumbnail = infos.selectFirst("img").attr("src");
        std::string st = firstContaining(infos.select("div.full-list-info"), "حالة الأنمي").selectFirst("a").text();
        d.status = st == "يعرض الان" ? "In corso" : st == "مكتمل" ? "Completato" : "";
        std::string desc = doc->selectFirst("div.review-content").text();
        if (!desc.empty()) desc += "\n";
        for (auto& info : infos.select("div.full-list-info")) {
            std::vector<std::string> smalls;
            for (auto& s : info.select("small")) smalls.push_back(s.text());
            desc += "\n" + joinStr(smalls, ": ");
        }
        d.description = trim(desc);
        for (auto& a : doc->select("div.row div.episode-card div.anime-title a")) {
            Episode e;
            e.url = rel(*doc, a);
            e.name = a.text();
            e.number = toNumber(afterLast(e.name, " "), 1);
            d.episodes.push_back(e);
        }
        newestFirst(d.episodes);
        return d;
    }

    std::vector<Video> videos(const std::string& url) override {
        DocPtr doc = page(url);
        std::vector<Video> out;
        std::set<std::string> seen;
        auto handle = [&](const std::string& raw, const std::string& q) {
            std::string u = fixUrl(trim(raw), doc->url());
            if (!startsWith(u, "http") || !seen.insert(u).second) return;
            tryAppend(out, [&]() -> std::vector<Video> {
                if (contains(u, "https://doo") && contains(u, "/e/")) return dood(u, "DoodStream - " + q + " ");
                if (isDood(u)) return dood(u, "DoodStream - " + q + " ");
                if (contains(u, "mp4upload")) return mp4upload(u, "");
                if (contains(u, "ok.ru")) return okru(u, "");
                if (contains(u, "voe.sx") || contains(u, "voe.")) return voe(u, "");
                if (anyIn(u, {"vidbam", "vadbam", "vidbom", "vidbm"})) return vidBom(u);
                if (contains(u, "uqload")) return uqload(u);
                if (contains(u, "streamtape")) return streamtape(u, "");
                if (anyIn(u, {"streamwish", "filelions", "wishfast", "swdyu", "embedwish"})) return streamWish(u, "");
                if (contains(u, "vidhide") || contains(u, "vidhidepro")) return vidHide(u, "");
                if (contains(u, "mixdrop")) return mixDrop(u);
                if (contains(u, "yourupload")) return yourUpload(u);
                if (contains(u, "dailymotion")) return dailymotion(u, "Dailymotion - ");
                return {};
            });
        };
        for (auto& el : doc->select("a.ep-link, [data-src], [data-url], [data-embed], [data-link]")) {
            std::string span = el.selectFirst("span").text();
            std::string q = span == "HD" ? "720p" : span == "FHD" ? "1080p" : span == "SD" ? "480p" : "240p";
            if (el.tag() == "img" || el.tag() == "script") continue;
            for (const char* at : {"data-src", "data-url", "data-embed", "data-link"}) {
                std::string v = el.attr(at);
                if (!v.empty() && !startsWith(v, "http") && !startsWith(v, "//")) {
                    std::string dec = b64(v);
                    if (startsWith(dec, "http")) v = dec;
                }
                if (!v.empty()) handle(v, q);
            }
        }
        for (auto& f : doc->select("iframe")) handle(f.attr("src").empty() ? f.attr("data-src") : f.attr("src"), "");
        finish(out, "1080p");
        return out;
    }

  private:
    /** Gli URL di episodio della lista "recenti" portano alla pagina dell'anime. */
    static std::string toAnimeUrl(const std::string& url) {
        if (!contains(url, "/episode/")) return url;
        return "/anime/" + substringBefore(afterLast(url, "/episode/"), "-episode-");
    }

    Page cards(const html::Document& doc, bool lastSectionOnly) {
        Page p;
        std::set<std::string> seen;
        auto add = [&](const html::Node& el) {
            html::Node a = el.selectFirst("div.anime-title > h4 > a");
            if (!a) a = el.selectFirst("a[href]");
            Anime an{toAnimeUrl(rel(doc, a)), trim(a.text()), el.selectFirst("img").attr("src")};
            if (an.title.empty()) an.title = trim(substringBefore(el.selectFirst("img").attr("alt"), " | "));
            if (!an.url.empty() && seen.insert(an.url).second) p.animes.push_back(an);
        };
        // div.container > div.section:last-child div.anime-card
        if (lastSectionOnly)
            for (auto& sec : doc.select("div.container > div.section"))
                if (isLastChild(sec))
                    for (auto& el : sec.select("div.anime-card")) add(el);
        if (p.animes.empty())
            for (auto& el : doc.select("div.anime-card")) {
                if (hasClass(el.parent().parent(), "related-grid")) continue;
                add(el);
            }
        // ul.pagination > li:last-child:not(.disabled)
        for (auto& li : doc.select("ul.pagination > li"))
            if (isLastChild(li) && !hasClass(li, "disabled")) p.hasNextPage = true;
        if (!p.hasNextPage) p.hasNextPage = doc.selectFirst("a[rel=next]").valid();
        return p;
    }
};

// ---- Anime Blkom (ar.animeblkom)
class AnimeBlkom : public Base {
  public:
    AnimeBlkom() : Base("ar.animeblkom", "أنمي بالكوم", "https://animeblkom.net", "ar", false, false) {}

    Page popular(int page) override { return list("/animes-list/?sort_by=rate&page=" + std::to_string(page)); }
    Page search(const std::string& q, int page) override {
        return list("/search?query=" + http::urlEncode(q) + "&page=" + std::to_string(page));
    }

    Details details(const std::string& url) override {
        DocPtr doc = page(url);
        Details d;
        d.thumbnail = doc->absUrl(doc->selectFirst("div.poster img"), "data-original");
        d.title = doc->selectFirst("div.name span h1").text();
        d.genre = joinText(doc->select("p.genres a"));
        html::Node story = doc->selectFirst("div.story p");
        d.description = story ? story.text() : doc->selectFirst("div.story").text();
        d.author = innermostContaining(doc->select("div"), "الاستديو").selectFirst("span > a").text();
        std::string st = innermostContaining(doc->select("div.info-table div"), "حالة الأنمي").selectFirst("span.info").text();
        d.status = statusFrom(st, "مستمر", "مكتمل");
        auto links = doc->select("ul.episodes-links li a");
        if (links.empty()) {
            d.episodes.push_back({http::pathOf(doc->url()), doc->selectFirst("div.name.col-xs-12 span h1").text(), 1});
            return d;
        }
        for (auto& a : links) {
            Episode e;
            e.url = rel(*doc, a);
            std::string t = nthChild(a, 3, "span").text();
            std::string n = digitsOnly(t);
            e.number = n.empty() ? 1 : toNumber(n, 1);
            e.name = t + " :" + nthChild(a, 1, "span").text();
            d.episodes.push_back(e);
        }
        std::reverse(d.episodes.begin(), d.episodes.end());
        return d;
    }

    std::vector<Video> videos(const std::string& url) override {
        DocPtr doc = page(url);
        std::vector<Video> out;
        for (auto& el : doc->select("span.server a")) {
            std::string u = replaceAll(el.attr("data-src"), "http://", "https://");
            std::string text = el.text();
            tryAppend(out, [&]() -> std::vector<Video> {
                if (contains(u, ".vid4up") || contains(text, "Blkom")) {
                    DocPtr vdoc = getDoc(u, headers());
                    std::vector<Video> v;
                    for (auto& s : vdoc->select("source"))
                        v.push_back(mkVideo(s.attr("src"), "Blkom - " + s.attr("label"), baseUrl()));
                    return v;
                }
                if (contains(u, "ok.ru")) return okru(u, "");
                if (contains(u, "mp4upload")) return mp4upload(u, "");
                return {};
            });
        }
        finish(out, "720p");
        return out;
    }

  protected:
    http::Headers headers() const override { return {{"Referer", baseUrl()}}; }

  private:
    Page list(const std::string& path) {
        DocPtr doc = page(path);
        return listOf(*doc, "div.contents div.poster > a", [&](const html::Node& el) {
            html::Node img = el.selectFirst("img");
            return Anime{rel(*doc, el), removeSuffix(img.attr("alt"), " poster"), doc->absUrl(img, "data-original")};
        }, "ul.pagination li.page-item a[rel=next]");
    }
};

// ---- Animeiat (ar.animeiat): API JSON
class Animeiat : public Base {
  public:
    Animeiat() : Base("ar.animeiat", "Animeiat", "https://api.animeiat.co/v1", "ar") {}

    Page popular(int page) override { return animes("/anime?page=" + std::to_string(page)); }
    Page latest(int page) override {
        json j = parseJson(get("/home/sticky-episodes?page=" + std::to_string(page)));
        Page p;
        std::set<std::string> seen;
        for (auto& e : jarr(j, "data")) {
            Anime a{"/anime/" + substringBefore(jstr(e, "slug"), "-episode-"), jstr(e, "title"), storage(jstr(e, "poster_path"))};
            if (seen.insert(a.url).second) p.animes.push_back(a);
        }
        p.hasNextPage = hasNext(j);
        return p;
    }
    Page search(const std::string& q, int page) override {
        return animes("/anime?q=" + http::urlEncode(q) + "&page=" + std::to_string(page));
    }

    Details details(const std::string& url) override {
        json data = jobj(parseJson(get(url)), "data");
        Details d;
        d.title = jstr(data, "anime_name");
        std::string st = jstr(data, "status");
        d.status = st == "ongoing" ? "In corso" : st == "completed" ? "Completato" : "";
        std::vector<std::string> studios, genres;
        for (auto& s : jarr(data, "studios")) studios.push_back(jstr(s, "name"));
        for (auto& g : jarr(data, "genres")) genres.push_back(jstr(g, "name"));
        d.author = joinStr(studios);
        d.genre = joinStr(genres);
        d.description = jstr(data, "story");
        d.thumbnail = storage(jstr(data, "poster_path"));
        std::string next = full(url) + "/episodes";
        for (int guard = 0; !next.empty() && guard < 100; guard++) {
            json page = parseJson(httpGet(next));
            for (auto& e : jarr(page, "data")) {
                Episode ep;
                ep.name = jstr(e, "title");
                ep.number = toNumber(jstr(e, "number"));
                ep.url = "episode/" + jstr(e, "slug");
                d.episodes.push_back(ep);
            }
            next = jstr(jobj(page, "links"), "next");
        }
        std::reverse(d.episodes.begin(), d.episodes.end());
        return d;
    }

    std::vector<Video> videos(const std::string& url) override {
        std::string body = get("/" + url);
        std::string hash = substringBefore(substringAfter(body, "\"hash\":\""), "\"");
        auto parts = splitStr(b64(hash), "\"");
        if (parts.size() < 2) throw http::Error("Nessun video trovato");
        std::string playerId = parts[parts.size() - 2];
        json j = parseJson(get("/video/" + playerId));
        std::vector<Video> out;
        for (auto& s : jarr(jobj(j, "data"), "sources")) {
            std::string file = jstr(s, "file");
            if (!file.empty()) out.push_back(mkVideo(file, jstr(s, "label") + " " + jstr(s, "quality")));
        }
        finish(out, "1080");
        return out;
    }

    http::Headers imageHeaders() const override { return {}; }

  private:
    static std::string storage(const std::string& p) { return p.empty() ? "" : "https://api.animeiat.co/storage/" + p; }
    static bool hasNext(const json& j) {
        const json& meta = jobj(j, "meta");
        return toNumber(jstr(meta, "current_page"), 0) < toNumber(jstr(meta, "last_page"), 0);
    }
    Page animes(const std::string& path) {
        json j = parseJson(get(path));
        Page p;
        for (auto& a : jarr(j, "data"))
            p.animes.push_back({"/anime/" + jstr(a, "slug"), jstr(a, "anime_name"), storage(jstr(a, "poster_path"))});
        p.hasNextPage = hasNext(j);
        return p;
    }
};

// ---- ArabAnime (ar.arabanime)
class ArabAnime : public Base {
  public:
    ArabAnime() : Base("ar.arabanime", "ArabAnime", "https://www.arabanime.net", "ar") {}

    Page popular(int page) override { return apiList("/api?page=" + std::to_string(page)); }
    Page latest(int) override {
        DocPtr doc = page("/");
        return listOf(*doc, "div.as-episode", [&](const html::Node& el) {
            html::Node a = el.selectFirst("a.as-info");
            std::string href = beforeLast(replaceAll(a.attr("href"), "watch", "show"), "/");
            return Anime{relUrl(http::resolve(doc->url(), href)), a.text(), doc->absUrl(el.selectFirst("img"), "src")};
        });
    }
    Page search(const std::string& q, int) override {
        http::Response r = httpPostRaw(full("/searchq"), formBody({{"searchq", q}}));
        checkStatus(r, full("/searchq"));
        if (contains(lower(r.header("content-type")), "application/json")) return apiParse(r.body);
        html::Document doc(r.body, full("/searchq"));
        return listOf(doc, "div.show", [&](const html::Node& el) {
            return Anime{rel(doc, el.selectFirst("a")), el.selectFirst("h3").text(), doc.absUrl(el.selectFirst("img"), "src")};
        });
    }

    Details details(const std::string& url) override {
        DocPtr doc = page(url);
        json show = parseJson(b64(doc->selectFirst("div#data").text()));
        const json& shows = jarr(show, "show");
        Details d;
        if (!shows.empty()) {
            const json& s = shows[0];
            d.title = jstr(s, "anime_name");
            std::string st = jstr(s, "anime_status");
            d.status = st == "Ongoing" ? "In corso" : st == "Completed" ? "Completato" : "";
            d.genre = jstr(s, "anime_genres");
            d.description = jstr(s, "anime_description");
            d.thumbnail = jstr(s, "anime_cover_image_url");
        }
        for (auto& e : jarr(show, "EPS")) {
            Episode ep;
            ep.name = jstr(e, "episode_name");
            ep.number = toNumber(jstr(e, "episode_number"));
            ep.url = relUrl(jstr(e, "info-src"));
            d.episodes.push_back(ep);
        }
        std::reverse(d.episodes.begin(), d.episodes.end());
        return d;
    }

    std::vector<Video> videos(const std::string& url) override {
        DocPtr doc = page(url);
        json watch = parseJson(b64(doc->selectFirst("div#datawatch").text()));
        const json& info = jarr(watch, "ep_info");
        if (info.empty() || jarr(info[0], "stream_servers").empty()) throw http::Error("Nessun video trovato");
        std::string server = b64(jval(jarr(info[0], "stream_servers")[0]));
        DocPtr wdoc = getDoc(server);
        std::vector<Video> out;
        for (auto& opt : wdoc->select("option")) {
            std::string name = opt.text(), u = b64(opt.attr("data-src"));
            if (!contains(u, baseUrl() + "/embed")) continue;
            tryAppend(out, [&] {
                DocPtr edoc = getDoc(u);
                std::vector<Video> v;
                for (auto& s : edoc->select("source")) {
                    std::string src = s.attr("src");
                    if (src.empty() || contains(src, "static")) continue;
                    std::string q = s.attr("label");
                    if (!contains(q, "p")) q += "p";
                    v.push_back(mkVideo(src, name + ": " + q));
                }
                return v;
            });
        }
        finish(out, "1080");
        return out;
    }

  private:
    Page apiParse(const std::string& body) {
        json j = parseJson(body);
        Page p;
        for (auto& s : jarr(j, "Shows")) {
            json a = json::parse(b64(jval(s)), nullptr, false);
            if (!a.is_object()) continue;
            p.animes.push_back({relUrl(jstr(a, "info_src")), jstr(a, "anime_name"), jstr(a, "anime_cover_image_url")});
        }
        p.hasNextPage = toNumber(jstr(j, "current_page"), 0) < toNumber(jstr(j, "last_page"), 0);
        return p;
    }
    Page apiList(const std::string& path) { return apiParse(get(path)); }
};

// ---- Arab Seed (ar.arabseed)
class ArabSeed : public Base {
  public:
    ArabSeed() : Base("ar.arabseed", "عرب سيد", "https://m.asd.homes", "ar", false, false) {}

    Page popular(int page) override { return list("/movies/?offset=" + std::to_string(page)); }
    Page search(const std::string& q, int page) override {
        return list("/find/?find=" + http::urlEncode(q) + "&offset=" + std::to_string(page));
    }

    Details details(const std::string& url) override {
        DocPtr doc = page(url);
        Details d;
        html::Node img = doc->selectFirst("div.Poster img");
        for (const char* a : {"data-src", "data-lazy-src", "src"}) {
            d.thumbnail = doc->absUrl(img, a);
            if (!d.thumbnail.empty()) break;
        }
        auto crumbs = doc->select("div.BreadCrumbs ol li");
        if (!crumbs.empty()) d.title = crumbs.back().selectFirst("a span").text();
        d.title = replaceAll(replaceAll(d.title, " مترجم", ""), "فيلم ", "");
        std::vector<std::string> genres;
        for (auto& li : allContaining(doc->select("div.MetaTermsInfo > li"), "النوع"))
            for (auto& a : li.children())
                if (a.tag() == "a") genres.push_back(a.text());
        d.genre = joinStr(genres);
        d.description = doc->selectFirst("div.StoryLine p").text();
        d.status = contains(doc->url(), "/selary/") ? "" : "Completato";
        auto eps = doc->select("div.ContainerEpisodesList a");
        if (eps.empty()) {
            d.episodes.push_back({http::pathOf(doc->url()), "مشاهدة", 1});
        } else {
            for (auto& a : eps) {
                Episode e{rel(*doc, a), a.text(), toNumber(a.selectFirst("em").text(), 0)};
                d.episodes.push_back(e);
            }
            newestFirst(d.episodes);
        }
        return d;
    }

    std::vector<Video> videos(const std::string& url) override {
        DocPtr doc = page(url);
        std::string watch = doc->selectFirst("a.watchBTn").attr("href");
        if (watch.empty()) throw http::Error("Nessun video trovato");
        DocPtr wdoc = page(watch);
        std::vector<Video> out;
        for (auto& li : wdoc->select("div.containerServers ul li")) {
            std::string q = li.text(), u = li.attr("data-link");
            tryAppend(out, [&]() -> std::vector<Video> {
                if (contains(u, "reviewtech") || contains(u, "reviewrate")) {
                    DocPtr f = getDoc(u);
                    std::string src = f->absUrl(f->selectFirst("source"), "src");
                    if (src.empty()) return {};
                    return {mkVideo(src, q + "p")};
                }
                if (contains(u, "dood")) return dood(u, "");
                if (contains(u, "fviplions") || contains(u, "wish")) return streamWish(u, "");
                if (contains(u, "voe.sx")) return voe(u, "");
                return {};
            });
        }
        finish(out, "1080");
        return out;
    }

  protected:
    http::Headers headers() const override { return {{"Referer", baseUrl()}}; }

  private:
    Page list(const std::string& path) {
        DocPtr doc = page(path);
        return listOf(*doc, "ul.Blocks-UL div.MovieBlock a", [&](const html::Node& el) {
            return Anime{rel(*doc, el), el.selectFirst("div.BlockName > h4").text(), el.selectFirst("div.Poster img").attr("data-src")};
        }, "ul.page-numbers li a.next");
    }
};

// ---- Asia2TV (ar.asia2tv)
class Asia2TV : public Base {
  public:
    Asia2TV() : Base("ar.asia2tv", "Asia2TV", "https://ww1.asia2tv.pw", "ar", false, false) {}

    Page popular(int page) override { return list("/category/asian-drama/page/" + std::to_string(page) + "/"); }
    Page search(const std::string& q, int page) override {
        return list("/page/" + std::to_string(page) + "/?s=" + http::urlEncode(q));
    }

    Details details(const std::string& url) override {
        DocPtr doc = page(url);
        Details d;
        d.title = html::textOf(doc->select("h1 span.title"));
        d.thumbnail = doc->selectFirst("div.single-thumb-bg > img").attr("src");
        d.description = html::textOf(doc->select("div.getcontent p"));
        std::vector<std::string> g;
        for (auto& a : doc->select("div.box-tags a")) g.push_back(a.text());
        for (auto& li : allContaining(doc->select("li"), "البلد"))
            for (auto& a : li.select("a")) g.push_back(a.text());
        d.genre = joinStr(g);
        for (auto& a : doc->select("div.loop-episode a")) {
            Episode e;
            e.url = rel(*doc, a);
            std::string num = beforeLast(afterLast(a.attr("href"), "-"), "/");
            e.name = num + " : الحلقة";
            e.number = toNumber(num);
            d.episodes.push_back(e);
        }
        std::reverse(d.episodes.begin(), d.episodes.end());
        return d;
    }

    std::vector<Video> videos(const std::string& url) override {
        DocPtr epDoc = page(url);
        std::string link = epDoc->selectFirst("div.loop-episode a.current").attr("href");
        DocPtr doc = link.empty() ? std::move(epDoc) : page(link);
        std::vector<Video> out;
        for (auto& li : doc->select("ul.server-list-menu li")) {
            std::string u = li.attr("data-server");
            tryAppend(out, [&]() -> std::vector<Video> {
                if (contains(u, "dood") || contains(u, "ds2play")) return dood(u, "");
                if (contains(u, "ok.ru") || contains(u, "odnoklassniki.ru")) return okru(u, "");
                if (contains(u, "streamtape")) return streamtape(u, "");
                if (anyIn(u, {"wishfast", "fviplions", "filelions", "streamwish", "dwish"})) return streamWish(u, "");
                if (contains(u, "uqload")) return uqload(u);
                if (anyIn(u, {"vidbam", "vadbam", "vidbom", "vidbm"})) return vidBom(u);
                if (contains(u, "youdbox") || contains(u, "yodbox")) {
                    DocPtr f = getDoc(u);
                    std::string src = f->absUrl(f->selectFirst("source"), "src");
                    if (src.empty()) return {};
                    return {mkVideo(src, "Yodbox: mirror")};
                }
                return {};
            });
        }
        finish(out, "1080");
        return out;
    }

  private:
    Page list(const std::string& path) {
        DocPtr doc = page(path);
        return listOf(*doc, "div.postmovie-photo a[title]", [&](const html::Node& el) {
            html::Node img = el.selectFirst("img");
            std::string thumb = img.attr("data-src").empty() ? img.attr("src") : img.attr("data-src");
            return Anime{rel(*doc, el), el.attr("title"), thumb};
        }, "div.nav-links a.next");
    }
};

// ---- Egy Dead (ar.egydead)
class EgyDead : public Base {
  public:
    EgyDead() : Base("ar.egydead", "Egy Dead", "https://tv10.egydead.live", "ar") {}

    Page popular(int) override {
        DocPtr doc = page("/");
        return listOf(*doc, "div.pin-posts-list li.movieItem", item(*doc));
    }
    Page latest(int page) override {
        DocPtr doc = this->page("/?page=" + std::to_string(page) + "/");
        return listOf(*doc, "section.main-section li.movieItem", item(*doc), "div.pagination ul.page-numbers li a.next");
    }
    Page search(const std::string& q, int page) override {
        DocPtr doc = this->page("/page/" + std::to_string(page) + "/?s=" + http::urlEncode(q));
        Page p = listOf(*doc, "div.catHolder li.movieItem", item(*doc));
        p.hasNextPage = firstContaining(doc->select("div.pagination-two a"), "›").valid();
        return p;
    }

    Details details(const std::string& url) override {
        DocPtr doc = page(url);
        Details d;
        d.thumbnail = doc->selectFirst("div.single-thumbnail img").attr("src");
        d.title = html::textOf(doc->select("div.infoBox div.singleTitle"));
        auto lis = doc->select("div.LeftBox li");
        d.author = joinText(firstContaining(lis, "البلد").select("a"));
        std::vector<std::string> g;
        for (const char* k : {"النوع", "اللغه", "السنه"})
            for (auto& li : allContaining(lis, k))
                for (auto& a : li.select("a")) g.push_back(a.text());
        d.genre = joinStr(g);
        d.description = html::textOf(doc->select("div.infoBox div.extra-content p"));
        d.status = contains(d.title, "كامل") || contains(d.title, "فيلم") ? "Completato" : "In corso";
        addEpisodes(*doc, false, d.episodes, 0);
        newestFirst(d.episodes);
        return d;
    }

    std::vector<Video> videos(const std::string& url) override {
        // il dominio cambia spesso e il redirect trasforma il POST in GET: si risolve prima l'URL finale
        std::string target = full(url);
        try {
            http::Response g = http::request("GET", encodeUrl(target), headers());
            if (!g.finalUrl.empty()) target = g.finalUrl;
        } catch (const std::exception&) {
        }
        http::Response r = httpPostRaw(target, formBody({{"View", "1"}}), withHeaders(headers(), {{"Referer", target}}));
        checkStatus(r, target);
        html::Document doc(r.body, r.finalUrl.empty() ? target : r.finalUrl);
        std::vector<Video> out;
        for (auto& li : doc.select("ul.serversList li")) {
            std::string u = li.attr("data-link");
            tryAppend(out, [&]() -> std::vector<Video> {
                if (isDood(u) || contains(u, "ds2play")) return dood(u, "Dood mirror ");
                if (contains(u, "mdbekjwqa")) return mixDrop(u);
                if (contains(u, "ahvsh")) {
                    DocPtr f = getDoc(u, headers());
                    std::string script = scriptWith(*f, {"sources"});
                    std::string link = fileAfterSources(script);
                    std::string q = substringBefore(substringAfter(substringAfter(substringAfter(script, "'qualityLabels'"), "\":"), "\""), "\"");
                    if (link.empty()) return {};
                    return {mkVideo(link, "StreamHide: " + q)};
                }
                static const std::regex sw("ajmidyad|alhayabambi|atabknh[ks]|https://.*\\.sbs/e/");
                if (regexIn(u, sw)) return streamWish(u, "");
                if (contains(u, "fanakishtuna")) {
                    DocPtr f = getDoc(u, headers());
                    std::string link = fileAfterSources(scriptWith(*f, {"sources"}));
                    if (link.empty()) return {};
                    return {mkVideo(link, "Mirror: High Quality")};
                }
                if (contains(u, "uqload")) {
                    std::string nu = replaceAll(u, "https://uqload.co/", "https://www.uqload.co/");
                    DocPtr f = getDoc(nu, headers());
                    std::string data = scriptWith(*f, {"sources"});
                    std::string link = substringBefore(substringAfter(data, "sources: [\""), "\"]");
                    if (!startsWith(link, "http")) return {};
                    return {mkVideo(link, "Uqload: Mirror", nu)};
                }
                return {};
            });
        }
        finish(out, "1080");
        return out;
    }

  private:
    static std::function<Anime(const html::Node&)> item(const html::Document& doc) {
        return [&doc](const html::Node& el) {
            html::Node a = el.selectFirst("a");
            return Anime{rel(doc, a), el.selectFirst("h1.BottomTitle").text(), el.selectFirst("a img").attr("src")};
        };
    }

    /** sources:\s*\[\{\s*file:\s*["']([^"']+) */
    static std::string fileAfterSources(const std::string& script) {
        size_t p = script.find("sources:");
        if (p == std::string::npos) return "";
        size_t f = script.find("file:", p);
        if (f == std::string::npos || f - p > 64) return "";
        size_t q = script.find_first_of("\"'", f);
        if (q == std::string::npos) return "";
        size_t e = script.find_first_of("\"'", q + 1);
        return e == std::string::npos ? "" : script.substr(q + 1, e - q - 1);
    }

    Episode epFrom(const html::Document& doc, const html::Node& a) {
        Episode e;
        e.url = rel(doc, a);
        e.name = a.text();
        e.number = toNumber(digitsOnly(e.name));
        return e;
    }

    void addEpisodes(const html::Document& doc, bool final, std::vector<Episode>& out, int depth) {
        if (depth > 3) return;
        std::string url = doc.url();
        if (final) {
            std::string season = html::textOf(doc.select("div.infoBox div.singleTitle"));
            std::string seasonTxt = substringBefore(substringAfter(season, "الموسم "), " ");
            for (auto& a : doc.select("div.EpsList li a")) {
                Episode e = epFrom(doc, a);
                if (contains(season, "موسم")) e.name = "الموسم " + seasonTxt + " " + e.name;
                out.push_back(e);
            }
        } else if (contains(url, "assembly")) {
            for (auto& a : doc.select("div.salery-list li.movieItem a")) out.push_back({rel(doc, a), a.attr("title"), -1});
        } else if (contains(url, "serie") || contains(url, "season")) {
            auto seasons = doc.select("div.seasons-list li.movieItem a");
            if (seasons.empty()) {
                for (auto& a : doc.select("div.EpsList li a")) out.push_back(epFrom(doc, a));
            } else {
                for (auto& a : seasons) {
                    try {
                        DocPtr s = getDoc(a.attr("href"));
                        addEpisodes(*s, true, out, depth + 1);
                    } catch (const std::exception&) {
                    }
                }
            }
        } else if (contains(url, "episode")) {
            html::Node b = doc.selectFirst("#breadcrumbs li a[itemprop=url]");
            if (b) {
                DocPtr s = getDoc(b.attr("href"));
                addEpisodes(*s, false, out, depth + 1);
            }
        } else {
            out.push_back({http::pathOf(url), "مشاهدة", 1});
        }
    }
};

// ---- Tuktuk Cinema (ar.tuktukcinema)
class Tuktukcinema : public Base {
  public:
    Tuktukcinema() : Base("ar.tuktukcinema", "توك توك سينما", "https://tuktukhd.com", "ar") {}

    Page popular(int) override { return list("/main/"); }
    Page latest(int page) override { return list("/recent/page/" + std::to_string(page) + "/"); }
    Page search(const std::string& q, int page) override {
        return list("/?s=" + http::urlEncode(q) + "&page=" + std::to_string(page));
    }

    Details details(const std::string& url) override {
        DocPtr doc = page(url);
        Details d;
        d.genre = joinText(doc->select("div.catssection li a"));
        d.title = editTitle(html::textOf(doc->select("h1.post-title")), false);
        d.author = joinText(firstContaining(doc->select("ul.RightTaxContent li"), "دولة").select("a"), " ");
        d.description = trim(html::textOf(doc->select("div.story")));
        d.status = "Completato";
        d.thumbnail = imageUrl(*doc, doc->selectFirst("div.left div.image img"));

        auto seasonsDom = doc->select("section.allseasonss div.Block--Item");
        if (seasonsDom.empty()) {
            d.episodes.push_back({http::pathOf(doc->url()), "مشاهدة", 1});
            return d;
        }
        std::string selected;
        for (auto& s : doc->select("div#mpbreadcrumbs a span"))
            if (contains(s.text(), "الموسم")) {
                selected = s.text();
                break;
            }
        std::reverse(seasonsDom.begin(), seasonsDom.end());
        for (auto& season : seasonsDom) {
            try {
                std::string seasonText = html::textOf(season.select("h3"));
                std::string seasonUrl = doc->absUrl(season.selectFirst("a"), "href");
                if (seasonUrl.empty()) continue;
                DocPtr other;
                const html::Document* sdoc = doc.get();
                if (selected != seasonText) {
                    other = getDoc(seasonUrl, headers());
                    sdoc = other.get();
                }
                std::string seasonNum = seasonsDom.size() == 1 ? "1" : digitsOnly(seasonText);
                if (seasonNum.empty()) seasonNum = "0";
                int index = 0;
                for (auto& ep : sdoc->select("section.allepcont a")) {
                    index++;
                    std::string num = digitsOnly(html::textOf(ep.select("div.epnum")));
                    if (num.empty()) num = std::to_string(index);
                    Episode e;
                    e.url = rel(*sdoc, ep);
                    e.name = seasonText + " : الحلقة " + num;
                    e.number = toNumber(seasonNum + "." + num);
                    d.episodes.push_back(e);
                }
            } catch (const std::exception&) {
            }
        }
        newestFirst(d.episodes);
        return d;
    }

    std::vector<Video> videos(const std::string& url) override {
        DocPtr doc = page(url);
        std::vector<Video> out;
        for (auto& li : doc->select("ul li.server--item")) {
            std::string enc = substringBefore(li.attr("data-link"), "0REL0Y");
            std::reverse(enc.begin(), enc.end());
            std::string u = b64(enc), server = li.text();
            tryAppend(out, [&] { return extract(u, server, "", 0); });
        }
        finish(out, "1080");
        return out;
    }

  protected:
    http::Headers headers() const override { return {{"Referer", baseUrl() + "/"}}; }

  private:
    Page list(const std::string& path) {
        DocPtr doc = page(path);
        return listOf(*doc, "div.Block--Item, div.Small--Box", [&](const html::Node& el) {
            html::Node a = el.selectFirst("a");
            return Anime{rel(*doc, a), editTitle(a.attr("title"), true), imageUrl(*doc, el.selectFirst("img"))};
        }, "div.pagination ul.page-numbers li a.next");
    }

    static std::string imageUrl(const html::Document& doc, const html::Node& img) {
        if (!img) return "";
        std::string u;
        if (img.hasAttr("data-src")) u = doc.absUrl(img, "data-src");
        else if (img.hasAttr("data-lazy-src")) u = doc.absUrl(img, "data-lazy-src");
        else if (img.hasAttr("srcset")) u = substringBefore(doc.absUrl(img, "srcset"), " ");
        else u = doc.absUrl(img, "src");
        return substringBefore(u, "?");
    }

    static std::string editTitle(const std::string& title, bool details) {
        if (title.size() < 1024) {
            static const std::regex movie("(?:فيلم|عرض)\\s(.*\\s\\d+)\\s(\\S+)");
            static const std::regex series("(?:مسلسل|برنامج|انمي)\\s(.+)\\sالحلقة\\s(\\d+)");
            std::smatch m;
            if (std::regex_search(title, m, movie))
                return details ? trim(m[1].str() + " (" + m[2].str() + ")") : trim(m[1].str());
            if (std::regex_search(title, m, series)) {
                std::string name = m[1].str();
                if (details) return trim(name + " (ep:" + m[2].str() + ")");
                if (contains(name, "الموسم")) return trim(substringBefore(name, "الموسم"));
                return trim(name);
            }
        }
        return trim(title);
    }

    std::vector<Video> extract(const std::string& u, const std::string& server, const std::string& quality, int depth) {
        if (contains(u, "iframe") && depth == 0) {
            std::vector<Video> out;
            for (auto& p : megamax(u, headers()))
                tryAppend(out, [&] { return extract(p.url, p.name, p.quality, depth + 1); });
            return out;
        }
        std::string qp = quality.empty() ? "" : quality + " ";
        if (contains(server, "mixdrop")) return mixDrop(u, qp, "Ar");
        if (contains(server, "dood")) return dood(u, qp);
        if (contains(server, "lulustream")) return streamWish(u, "Lulustream ");
        if (contains(server, "krakenfiles")) {
            DocPtr f = getDoc(u, headers());
            std::vector<Video> v;
            for (auto& s : f->select("source"))
                v.push_back(mkVideo(s.attr("src"), "Kraken" + (quality.empty() ? "" : ": " + quality)));
            return v;
        }
        if (contains(server, "earnvids")) return vidLand(u);
        if (contains(server, "Vidbom") || contains(server, "Vidshare") || contains(server, "Govid"))
            return vidBom(u, headers());
        return {};
    }
};

// =============================================================================================== TURCO

// ---- Türk Anime TV (tr.turkanime)
class TurkAnime : public Base {
  public:
    TurkAnime() : Base("tr.turkanime", "Türk Anime TV", "https://www.turkanime.tv", "tr") {}

    Page popular(int page) override { return list(page, "/ajax/rankagore?sayfa="); }
    Page latest(int page) override { return list(page, "/ajax/yenieklenenseriler?sayfa="); }
    Page search(const std::string& q, int page) override {
        std::string url = full("/arama?sayfa=" + std::to_string(page));
        html::Document doc(httpPost(url, formBody({{"arama", q}})), url);
        std::string loc;
        for (auto& s : doc.select("div.panel-body > script"))
            if (contains(s.data(), "window.location")) {
                loc = substringBefore(substringAfter(substringAfter(s.data(), "window.location"), "\""), "\"");
                break;
            }
        if (!loc.empty()) {
            std::string slug = startsWith(loc, "/") ? loc : "/" + loc;
            Page p;
            p.animes.push_back({slug, substringAfter(slug, "anime/"), ""});
            return p;
        }
        Page p = listOf(doc, "div.panel-visible", item(doc));
        p.hasNextPage = !p.animes.empty();
        return p;
    }

    Details details(const std::string& url) override {
        DocPtr doc = page(url);
        Details d;
        html::Node img = doc->selectFirst("div.imaj > img.media-object");
        if (img) d.thumbnail = fixUrl(img.attr("data-src"));
        auto rows = doc->select("div#animedetay > table tr");
        auto lastTdLinks = [&](const char* label) {
            std::vector<std::string> v;
            html::Node tr = firstContaining(rows, label);
            auto tds = tr.select("td");
            if (!tds.empty())
                for (auto& a : tds.back().select("a")) v.push_back(a.text());
            return v;
        };
        auto studio = lastTdLinks("Stüdyo");
        if (!studio.empty()) d.author = studio.front();
        d.genre = joinStr(lastTdLinks("Anime Türü"));
        d.title = html::textOf(doc->select("div#detayPaylas div.panel-title"));
        d.description = doc->selectFirst("div#animedetay p.ozet").text();

        std::string animeId = http::queryParam(full(url), "animeId");
        if (animeId.empty()) animeId = doc->selectFirst("a[data-unique-id]").attr("data-unique-id");
        DocPtr eps = page("/ajax/bolumler?animeId=" + animeId, xml());
        for (auto& li : eps->select("ul.menum li")) {
            html::Node a;
            for (auto& x : li.select("a"))
                if (x.selectFirst("span.bolumAdi")) {
                    a = x;
                    break;
                }
            if (!a) continue;
            Episode e;
            e.name = a.attr("title");
            std::string sub = substringBefore(e.name, ". Bölüm");
            size_t i = sub.size();
            while (i > 0 && std::isdigit((unsigned char)sub[i - 1])) i--;
            e.number = toNumber(sub.substr(i), 1);
            e.url = rel(*eps, a);
            d.episodes.push_back(e);
        }
        std::reverse(d.episodes.begin(), d.episodes.end());
        return d;
    }

    std::vector<Video> videos(const std::string& url) override {
        DocPtr doc = page(url);
        std::vector<std::pair<DocPtr, std::string>> subs;
        auto fansubbers = doc->select("div#videodetay div.pull-right button");
        if (fansubbers.size() == 1) {
            std::string name = fansubbers[0].text();
            subs.push_back({std::move(doc), name});
        } else {
            for (auto& f : fansubbers) {
                try {
                    subs.push_back({page(onClick(f.attr("onclick")), xml()), f.text()});
                } catch (const std::exception&) {
                }
            }
        }
        // l'estensione abilita di default solo GDRIVE e VOE; se non bastano si provano gli altri hoster supportati
        std::set<std::string> first = {"GDRIVE", "VOE"}, rest;
        for (auto* h : SUPPORTED)
            if (!first.count(h)) rest.insert(h);
        std::vector<Video> out;
        for (auto& s : subs) append(out, fromHosters(*s.first, s.second, first));
        if (out.empty())
            for (auto& s : subs) append(out, fromHosters(*s.first, s.second, rest));
        if (out.empty()) throw http::Error("Nessun video trovato");
        std::stable_sort(out.begin(), out.end(), [](const Video& a, const Video& b) {
            int pa = contains(a.title, "1080"), pb = contains(b.title, "1080");
            if (pa != pb) return pa > pb;
            return qualityOf(a.title) > qualityOf(b.title);
        });
        return out;
    }

  private:
    static constexpr const char* DEFAULT_KEY =
        "710^8A@3@>T2}#zN5xK?kR7KNKb@-A!LzYL5~M1qU0UfdWsZoBm4UUat%}ueUv6E--*hDPPbH7K2bp9^3o41hw,khL:}Kx8080@M";
    static constexpr const char* SUPPORTED[] = {"DOODSTREAM", "EMBEDGRAM", "FILEMOON", "GDRIVE", "MAIL",
                                                "MP4UPLOAD", "MVIDOO", "ODNOKLASSNIKI", "SENDVID", "SIBNET",
                                                "UQLOAD", "VK", "VOE", "VTUBE", "VUDEA", "WOLFSTREAM"};

    static http::Headers xml() { return {{"X-Requested-With", "XMLHttpRequest"}}; }
    static bool supported(const std::string& n) {
        for (auto* h : SUPPORTED)
            if (n == h) return true;
        return false;
    }
    std::string onClick(const std::string& s) const {
        return baseUrl() + "/" + substringBefore(substringAfter(s, "IndexIcerik('"), "'");
    }

    static std::function<Anime(const html::Node&)> item(const html::Document& doc) {
        return [&doc](const html::Node& el) {
            html::Node t = el.selectFirst("div.panel-title > a");
            std::string id = el.selectFirst("a.reactions").attr("data-unique-id");
            std::string path = rel(doc, t);
            if (!id.empty()) path += (contains(path, "?") ? "&" : "?") + std::string("animeId=") + id;
            return Anime{path, substringBefore(t.attr("title"), " izle"), doc.absUrl(el.selectFirst("img.media-object"), "data-src")};
        };
    }

    Page list(int page, const std::string& path) {
        DocPtr doc = this->page(path + std::to_string(page), xml());
        return listOf(*doc, "div.panel-visible", item(*doc), "button.btn-default[data-loading-text*=Sonraki]");
    }

    std::vector<Video> fromHosters(const html::Document& doc, const std::string& subber, const std::set<std::string>& sel) {
        std::vector<Video> out;
        std::string selected = html::textOf(doc.select("div#videodetay div.btn-group:not(.pull-right) > button.btn-danger"));
        if (supported(selected) && sel.count(selected)) {
            std::string src = doc.selectFirst("iframe").attr("src");
            if (!src.empty()) tryAppend(out, [&] { return fromSource(src, selected, subber); });
        }
        for (auto& b : doc.select("div#videodetay div.btn-group:not(.pull-right) > button.btn-default[onclick*=videosec]")) {
            std::string name = b.text();
            if (!supported(name) || !sel.count(name)) continue;
            tryAppend(out, [&]() -> std::vector<Video> {
                DocPtr vdoc = page(onClick(b.attr("onclick")), xml());
                std::string src = vdoc->selectFirst("iframe").attr("src");
                if (src.empty()) return {};
                return fromSource(fixUrl(src), name, subber);
            });
        }
        return out;
    }

    static std::string decrypt(const std::string& ct, const std::string& salt) {
        std::string kv = crypto::evpBytesToKey(DEFAULT_KEY, crypto::fromHex(salt), 32, 16);
        std::string dec = crypto::aesCbcDecrypt(b64(ct), kv.substr(0, 32), kv.substr(32, 16));
        json j = json::parse(dec, nullptr, false);
        return j.is_string() ? j.get<std::string>() : "";
    }

    std::vector<Video> fromSource(const std::string& src, const std::string& hoster, const std::string& subber) {
        std::string enc = substringBefore(substringAfter(src, "/embed/#/url/"), "?status");
        json params = parseJson(b64(enc));
        // NB: la chiave non viene aggiornata (l'estensione la ricava deoffuscando un JS): si usa quella predefinita
        std::string dec = decrypt(jstr(params, "ct"), jstr(params, "s"));
        if (dec.empty()) return {};
        std::string link = "https:" + dec;
        std::string p = subber + ": ";
        if (hoster == "DOODSTREAM") return dood(link, p);
        if (hoster == "EMBEDGRAM") return embedgram(link, p);
        if (hoster == "FILEMOON") return moon(link, baseUrl(), p);
        if (hoster == "GDRIVE") {
            // Regex("[\w-]{28,}")
            size_t i = 0;
            while (i < link.size()) {
                size_t s = i;
                while (i < link.size() && (std::isalnum((unsigned char)link[i]) || link[i] == '_' || link[i] == '-')) i++;
                if (i - s >= 28) return googleDrive(link.substr(s, i - s), p + "Gdrive");
                if (i == s) i++;
            }
            return {};
        }
        if (hoster == "MAIL") return mailRu(link, p);
        if (hoster == "MP4UPLOAD") return mp4upload(link, p);
        if (hoster == "MVIDOO") return mvidoo(link, p);
        if (hoster == "ODNOKLASSNIKI") return okru(link, p);
        if (hoster == "SENDVID") return sendvid(link, p);
        if (hoster == "SIBNET") return sibnet(link, p);
        if (hoster == "UQLOAD") return uqload(link, subber + ":");
        if (hoster == "VK") return vk("https://vk.com" + substringAfter(link, "vk.com"), p);
        if (hoster == "VOE") return voe(link, "(" + subber + ") ");
        if (hoster == "VTUBE") return vtube(link, baseUrl(), p);
        if (hoster == "VUDEA") return vudeo(link, p);
        if (hoster == "WOLFSTREAM") return wolfstream(link, p);
        return {};
    }
};
constexpr const char* TurkAnime::SUPPORTED[];

// ---- Anizm (tr.anizm)
class Anizm : public Base {
  public:
    Anizm() : Base("tr.anizm", "Anizm", "https://anizm.net", "tr") {}

    Page popular(int) override {
        DocPtr doc = page("/");
        return listOf(*doc, "div.popularAnimeCarousel a.slideAnimeLink", item(*doc));
    }
    Page latest(int page) override {
        DocPtr doc = this->page("/anime-izle?sayfa=" + std::to_string(page));
        return listOf(*doc, "div#episodesMiddle div.posterBlock > a", item(*doc),
                      "div.nextBeforeButtons > div.ui > a.right:not(.disabled)");
    }
    Page search(const std::string& q, int page) override {
        const json& all = searchList();
        std::string lq = lower(q);
        std::vector<const json*> res;
        for (auto& a : all) {
            bool ok = lq.empty();
            for (const char* k : {"info_othernames", "info_japanese", "info_title"})
                if (!ok && contains(lower(jstr(a, k)), lq)) ok = true;
            if (ok) res.push_back(&a);
        }
        std::stable_sort(res.begin(), res.end(), [](const json* a, const json* b) {
            return lower(jstr(*a, "info_title")) < lower(jstr(*b, "info_title"));
        });
        Page p;
        size_t start = (size_t)std::max(0, page - 1) * 30;
        for (size_t i = start; i < res.size() && i < start + 30; i++)
            p.animes.push_back({"/" + jstr(*res[i], "info_slug"), jstr(*res[i], "info_title"),
                                baseUrl() + "/storage/pcovers/" + jstr(*res[i], "info_poster")});
        p.hasNextPage = res.size() > start + 30;
        return p;
    }

    Details details(const std::string& url) override {
        DocPtr doc = page(url);
        Details d;
        d.title = doc->selectFirst("h2.anizm_pageTitle").text();
        d.thumbnail = doc->absUrl(doc->selectFirst("div.infoPosterImg > img"), "src");
        html::Node infos = doc->selectFirst("div.anizm_boxContent");
        d.genre = joinText(infos.select("span.dataValue > span.tag > span.label"));
        for (auto& t : infos.select("span.dataTitle"))
            if (contains(t.text(), "Stüdyo")) {
                html::Node n = nextElement(t);
                if (n.tag() == "span") d.author = n.text();
                break;
            }
        std::string desc = infos.selectFirst("div.infoDesc").text();
        for (auto& row : infos.select("li.dataRow")) {
            if (row.selectFirst("span.ui.tag") || row.selectFirst("div.star")) continue;
            for (auto& s : row.children()) {
                if (s.tag() != "span") continue;
                if (hasClass(s, "dataTitle")) desc += "\n" + s.text() + ": ";
                else desc += s.text();
            }
        }
        d.description = trim(desc);
        for (auto& a : doc->select("div.episodeListTabContent div > a")) {
            Episode e;
            e.url = rel(*doc, a);
            e.name = a.text();
            std::string n = digitsOnly(e.name);
            e.number = n.empty() ? 1 : toNumber(n, 1);
            d.episodes.push_back(e);
        }
        std::reverse(d.episodes.begin(), d.episodes.end());
        return d;
    }

    std::vector<Video> videos(const std::string& url) override {
        DocPtr doc = page(url);
        std::vector<std::pair<std::string, std::string>> players;
        for (auto& a : doc->select("div#fansec > a")) {
            std::string fansub = trim(substringBefore(substringBefore(substringBefore(a.text(), "- BD"), "Fansub"), "Bağımsız"));
            try {
                json j = parseJson(get(a.attr("translator")));
                html::Document pd(jstr(j, "data"), full(url));
                for (auto& b : pd.select("a.videoPlayerButtons"))
                    players.push_back({fansub, replaceAll(b.attr("video"), "/video/", "/player/")});
            } catch (const std::exception&) {
            }
        }
        if (players.empty()) throw http::Error("Nessun fansub disponibile");
        std::vector<Video> out;
        for (auto& pl : players) {
            tryAppend(out, [&] { return prefixed(fromPlayer(pl.second), "[" + pl.first + "] "); });
        }
        if (out.empty()) throw http::Error("Nessun video trovato");
        std::stable_sort(out.begin(), out.end(), [](const Video& a, const Video& b) {
            int pa = contains(a.title, "720p"), pb = contains(b.title, "720p");
            if (pa != pb) return pa > pb;
            return qualityOf(a.title) > qualityOf(b.title);
        });
        return out;
    }

  protected:
    http::Headers headers() const override { return {{"Origin", baseUrl()}, {"Referer", baseUrl() + "/"}}; }

  private:
    std::mutex listMutex;
    json cache;

    const json& searchList() {
        std::lock_guard<std::mutex> lock(listMutex);
        if (!cache.is_array()) {
            json j = parseJson(get("/getAnimeListForSearch"));
            if (!j.is_array()) throw http::Error("Elenco anime non disponibile");
            cache = std::move(j);
        }
        return cache;
    }

    static std::function<Anime(const html::Node&)> item(const html::Document& doc) {
        return [&doc](const html::Node& el) {
            std::string href = beforeLast(substringBefore(el.attr("href"), "-bolum-izle"), "-");
            return Anime{relUrl(http::resolve(doc.url(), href)), el.selectFirst(".title").text(), el.selectFirst("img").attr("src")};
        };
    }

    std::vector<Video> fromPlayer(const std::string& first) {
        http::Response r = http::request("GET", full(first), headers(), "", 30, false);
        std::string u = r.header("location");
        if (u.empty()) return {};
        u = fixUrl(u, full(first));
        if (contains(u, "filemoon.sx")) return moon(u, baseUrl(), "");
        if (contains(u, "sendvid.com")) return sendvid(u);
        if (contains(u, "video.sibnet")) return sibnet(u);
        if (contains(u, "mp4upload")) return mp4upload(u, "");
        if (contains(u, "ok.ru") || contains(u, "odnoklassniki.ru")) return okru(u, "");
        if (contains(u, "yourupload")) return yourUpload(u);
        if (contains(u, "streamtape")) return streamtape(u, "");
        if (contains(u, "dood")) return dood(u, "");
        if (contains(u, "uqload")) return uqload(u);
        if (contains(u, "voe.sx")) return voe(u, "");
        if (contains(u, "anizmplayer.com")) return aincrad(u);
        return {};
    }
};

// ---- Animeler (tr.animeler): API WordPress "kiranime"
class Animeler : public Base {
  public:
    Animeler() : Base("tr.animeler", "Animeler", "https://animeler.pw", "tr") {}

    Page popular(int page) override { return orderBy("total_kiranime_views", page); }
    Page latest(int page) override { return orderBy("kiranime_anime_updated", page); }
    Page search(const std::string& q, int page) override {
        json body = {{"single",
                      {{"paged", page},
                       {"meta_key", "total_kiranime_views"},
                       {"order", "desc"},
                       {"orderBy", "meta_value_num"},
                       {"season", nullptr},
                       {"year", nullptr}}},
                     {"keyword", q},
                     {"query", q},
                     {"tax", json::array()}};
        return request(body.dump(), page);
    }

    Details details(const std::string& url) override {
        std::string body = get(url);
        json a = parseJson(substringBefore(substringAfter(body, "const anime = "), "};") + "}");
        Details d;
        const json& post = jobj(a, "post");
        const json& meta = jobj(a, "meta");
        const json& tax = jobj(a, "taxonomies");
        auto names = [&](const char* k) {
            std::vector<std::string> v;
            for (auto& i : jarr(tax, k)) v.push_back(jstr(i, "name"));
            return joinStr(v);
        };
        d.title = jstr(post, "post_title");
        d.thumbnail = jstr(a, "image");
        if (d.thumbnail.empty()) d.thumbnail = jstr(jobj(a, "images"), "featured_url");
        d.author = joinStr({names("studio"), names("producer")});
        d.genre = names("genre");
        d.status = contains(jstr(meta, "aired"), " to ") ? "Completato" : "";
        std::string desc = jstr(post, "post_content");
        if (!desc.empty()) desc += "\n";
        auto add = [&](const char* label, const char* k) {
            std::string v = trim(jstr(meta, k));
            if (!v.empty()) desc += std::string("\n") + label + v;
        };
        add("Score: ", "score");
        add("Native: ", "native");
        add("Diğer İsimleri: ", "synonyms");
        add("Rate: ", "rate");
        add("Premiered: ", "premiered");
        add("Yayınlandı: ", "aired");
        add("Süre: ", "duration");
        d.description = trim(desc);
        for (auto& e : jarr(a, "episodes")) {
            Episode ep;
            ep.url = relUrl(jstr(e, "url"));
            std::string n = jstr(jobj(e, "meta"), "number");
            ep.name = "Bölüm " + n;
            ep.number = toNumber(n);
            d.episodes.push_back(ep);
        }
        std::stable_sort(d.episodes.begin(), d.episodes.end(),
                         [](const Episode& x, const Episode& y) { return x.number > y.number; });
        return d;
    }

    std::vector<Video> videos(const std::string& url) override {
        DocPtr doc = page(url);
        html::Node iframe = doc->selectFirst("div.episode-player-box > iframe");
        std::string iframeUrl = iframe ? (trim(iframe.attr("data-src")).empty() ? iframe.attr("src") : iframe.attr("data-src")) : "";
        if (iframeUrl.empty()) {
            std::string s = scriptWith(*doc, {"embedUrl"});
            if (!s.empty()) iframeUrl = substringBefore(substringAfter(s, "\"embedUrl\": \""), "\"");
        }
        if (iframeUrl.empty()) throw http::Error("Nessun video disponibile");
        iframeUrl = fixUrl(iframeUrl, full(url));
        std::string hash = substringAfter(iframeUrl, "/video/");
        std::string action = iframeUrl + "?do=getVideo";
        http::Headers h = {{"Origin", "https://" + http::hostOf(iframeUrl)}, {"X-Requested-With", "XMLHttpRequest"}};
        json players = parseJson(httpPost(action, formBody({{"hash", hash}, {"r", baseUrl() + "/"}, {"s", ""}}), h));
        static const char* SUPPORTED[] = {"doodstream.com", "G.Drive", "Moon", "ok.ru", "S.Tape",
                                          "Sibnet", "Streamlare", "UQload", "Voe", "vudeo"};
        std::vector<Video> out;
        for (auto& src : jobj(players, "sourceList").items()) {
            std::string name = lower(jval(src.value()));
            bool ok = false;
            for (auto* s : SUPPORTED)
                if (contains(lower(s), name)) ok = true;
            if (!ok) continue;
            std::string key = src.key();
            tryAppend(out, [&]() -> std::vector<Video> {
                json v = parseJson(httpPost(action, formBody({{"hash", hash}, {"r", baseUrl() + "/"}, {"s", key}}), h));
                std::string u = jstr(v, "videoSrc");
                if (contains(u, "dood")) return dood(u, "");
                if (contains(u, "drive.google")) return {};
                if (contains(u, "filemoon.")) return moon(u, baseUrl(), "");
                if (contains(u, "ok.ru") || contains(u, "odnoklassniki.ru")) return okru(u, "");
                if (contains(u, "streamtape")) return streamtape(u, "");
                if (contains(u, "sibnet")) return sibnet(u);
                if (contains(u, "streamlare")) return streamlare(u);
                if (contains(u, "uqload")) return uqload(u);
                if (contains(u, "voe.")) return voe(u, "");
                if (contains(u, "vudeo.")) return vudeo(u);
                return {};
            });
        }
        finish(out, "720p");
        return out;
    }

  private:
    Page orderBy(const std::string& order, int page) {
        std::string body = "{\"keyword\":\"\",\"query\":\"\",\"single\":{\"paged\":" + std::to_string(page) +
                           ",\"orderby\":\"meta_value_num\",\"meta_key\":\"" + order + "\",\"order\":\"desc\"},\"tax\":[]}";
        return request(body, page);
    }

    Page request(const std::string& body, int page) {
        json j = parseJson(post("/wp-json/kiranime/v1/anime/advancedsearch?_locale=user&page=" + std::to_string(page),
                                body, {}, "application/json"));
        html::Document doc(jstr(j, "data"), baseUrl() + "/");
        Page p;
        std::set<std::string> seen;
        for (auto& el : doc.select("div.w-full")) {
            if (!el.selectFirst("div.kira-anime")) continue;
            html::Node a = el.selectFirst("h3 > a");
            if (!a) continue;
            Anime an{rel(doc, a), a.text(), el.selectFirst("img").attr("src")};
            if (!an.url.empty() && seen.insert(an.url).second) p.animes.push_back(an);
        }
        p.hasNextPage = page < (int)toNumber(jstr(j, "pages"), 0);
        return p;
    }
};

// ---- TR Anime Izle (tr.tranimeizle): con risolutore del captcha "scegli l'immagine diversa"
class TRAnimeIzle : public Base {
  public:
    TRAnimeIzle() : Base("tr.tranimeizle", "TR Anime Izle", "https://www.tranimeizle.io", "tr") {}

    Page popular(int page) override { return list("/listeler/populer/sayfa-" + std::to_string(page), false); }
    Page latest(int page) override { return list("/listeler/yenibolum/sayfa-" + std::to_string(page), true); }
    Page search(const std::string& q, int page) override {
        return list("/arama/" + http::urlEncode(q) + "?page=" + std::to_string(page), false);
    }

    Details details(const std::string& url) override {
        DocPtr doc = fetch(url);
        Details d;
        d.title = clearName(doc->selectFirst("div.playlist-title h1").text());
        d.thumbnail = doc->selectFirst("div.poster .social-icon img").attr("src");
        html::Node infos = doc->selectFirst("div.col-md-6 > div.row");
        d.genre = joinText(infos.select("div > a.genre"));
        for (auto& dd : infos.select("dd"))
            if (contains(dd.text(), "Fansublar")) {
                html::Node dt = nextElement(dd);
                if (dt.tag() == "dt") d.author = joinText(dt.select("a"));
                break;
            }
        std::string desc = doc->selectFirst("div.p-10 > p").text();
        int dtCount = 0;
        for (auto& el : infos.select("*")) {
            std::string tag = el.tag();
            if (tag != "dd" && tag != "dt") continue;
            std::string text = el.text();
            if (tag == "dd" && (contains(text, "Puanlama") || contains(text, "Anime Türü"))) continue;
            if (tag == "dt" && (el.selectFirst("i.fa-star") || el.selectFirst("a.genre"))) continue;
            if (tag == "dd") {
                desc += "\n" + text + ": ";
                dtCount = 0;
            } else {
                desc += (dtCount == 0 ? "" : ", ") + text;
                dtCount++;
            }
        }
        d.description = trim(desc);
        for (auto& a : doc->select("div.animeDetail-items > ol a")) {
            if (!a.selectFirst("div.episode-li")) continue;
            Episode e;
            e.url = rel(*doc, a);
            std::string t = a.selectFirst(".etitle > span").text();
            std::string head = beforeOr(t, ". Bölüm", "");
            std::string n = contains(head, " ") ? afterLast(head, " ") : "";
            int num = (int)toNumber(n, 1);
            e.name = "Bölüm " + std::to_string(num);
            e.number = num;
            d.episodes.push_back(e);
        }
        std::reverse(d.episodes.begin(), d.episodes.end());
        return d;
    }

    std::vector<Video> videos(const std::string& url) override {
        DocPtr doc = fetch(url);
        std::string episodeId = doc->selectFirst("input#EpisodeId").attr("value");
        std::vector<Video> out;
        for (auto& fs : doc->select("div.fansubSelector")) {
            std::string fid = fs.attr("data-fid"), fname = fs.text();
            try {
                std::string body = "{\"EpisodeId\":" + episodeId + ",\"FansubId\":" + fid + "}";
                html::Document src(post("/api/fansubSources", body, {}, "application/json"), full(url));
                for (auto& li : src.select("li.sourceBtn")) {
                    std::string id = li.attr("data-id");
                    tryAppend(out, [&] { return prefixed(fromId(id), "[" + fname + "] "); });
                }
            } catch (const std::exception&) {
            }
        }
        finish(out, "720p");
        return out;
    }

  protected:
    http::Headers headers() const override { return {{"Referer", baseUrl() + "/"}, {"Origin", baseUrl()}}; }

  private:
    std::mutex captchaMutex;

    static std::string clearName(const std::string& s) { return removeSuffix(removeSuffix(s, " İzle"), " Bölüm"); }

    /** GET con risoluzione automatica del captcha (ShittyCaptchaInterceptor). */
    DocPtr fetch(const std::string& path) {
        std::string url = full(path);
        http::Response r = http::request("GET", encodeUrl(url), headers());
        if (contains(r.finalUrl, "/api/CaptchaChallenge")) {
            std::lock_guard<std::mutex> lock(captchaMutex);
            std::string current = r.finalUrl;
            http::Headers ch = withHeaders(headers(), {{"Referer", current}, {"X-Requested-With", "XMLHttpRequest"}});
            json ids = parseJson(httpPost(baseUrl() + "/api/Captcha/", formBody({{"cID", "0"}, {"rT", "1"}, {"tM", "light"}}), ch));
            std::vector<std::pair<std::string, std::string>> hashes;
            std::map<std::string, int> count;
            if (ids.is_array())
                for (auto& id : ids) {
                    std::string sid = jval(id);
                    http::Response img = http::request("GET", baseUrl() + "/api/Captcha/?cid=0&hash=" + sid, headers());
                    std::string h = crypto::toHex(crypto::md5(img.body));
                    hashes.push_back({sid, h});
                    count[h]++;
                }
            std::string correct;
            int best = 1 << 30;
            for (auto& h : hashes)
                if (count[h.second] < best) {
                    best = count[h.second];
                    correct = h.first;
                }
            if (correct.empty()) throw http::Error("Errore nel superare il captcha");
            httpPostRaw(baseUrl() + "/api/Captcha/", formBody({{"cID", "0"}, {"rT", "2"}, {"pC", correct}}), ch);
            r = http::request("GET", current, headers());
            if (contains(r.finalUrl, "/api/CaptchaChallenge") || r.status >= 400)
                r = http::request("GET", encodeUrl(url), headers());
        }
        checkStatus(r, url);
        return std::make_unique<html::Document>(r.body, r.finalUrl.empty() ? url : r.finalUrl);
    }

    Page list(const std::string& path, bool latestEpisodes) {
        DocPtr doc = fetch(path);
        Page p = listOf(*doc, "div.post-body div.flx-block", [&](const html::Node& el) {
            Anime a;
            a.url = relUrl(http::resolve(doc->url(), el.attr("data-href")));
            if (latestEpisodes) a.url = beforeLast(substringBefore("/anime" + a.url, "-bolum"), "-") + "-izle";
            a.thumbnail = el.selectFirst("img").attr("src");
            a.title = clearName(el.selectFirst("div.bar > h4").text());
            return a;
        });
        // ul.pagination > li:has(.ti-angle-right):not(.disabled)
        for (auto& li : doc->select("ul.pagination > li"))
            if (li.selectFirst(".ti-angle-right") && !hasClass(li, "disabled")) p.hasNextPage = true;
        return p;
    }

    std::vector<Video> fromId(const std::string& id) {
        std::string body = post("/api/sourcePlayer/" + id, "");
        std::string u = trim(replaceAll(
            substringBefore(substringAfter(substringAfter(substringAfter(body, "src="), "\""), "/embed2/?id="), "\""), "\\", ""));
        if (!startsWith(u, "https")) u = "https:" + u;
        if (contains(u, "filemoon.sx")) return moon(u, baseUrl(), "");
        if (contains(u, "mixdrop")) return mixDrop(u);
        if (contains(u, "mp4upload")) return mp4upload(u, "");
        if (contains(u, "ok.ru") || contains(u, "odnoklassniki.ru")) return okru(u, "");
        if (contains(u, "sendvid.com")) return sendvid(u);
        if (contains(u, "video.sibnet")) return sibnet(u);
        if (contains(u, "streamlare.com")) return streamlare(u);
        if (contains(u, "voe.sx")) return voe(u, "");
        if (contains(u, "//vudeo.")) return vudeo(u);
        if (contains(u, "yourupload.com")) return yourUpload(u);
        return {};
    }
};

// ---- HentaiZM (tr.hentaizm, 18+)
class HentaiZM : public Base {
  public:
    HentaiZM() : Base("tr.hentaizm", "HentaiZM", "https://www.hentaizm6.online", "tr", true) {}

    Page popular(int page) override {
        login();
        DocPtr doc = this->page("/en-cok-izlenenler/page/" + std::to_string(page));
        Page p = listOf(*doc, "div.moviefilm", item(*doc));
        p.hasNextPage = nextAfterCurrent(*doc);
        return p;
    }
    Page latest(int page) override {
        login();
        DocPtr doc = this->page("/yeni-eklenenler?c=" + std::to_string(page - 1));
        Page p = listOf(*doc, "div.moviefilm", item(*doc));
        p.hasNextPage = firstContaining(doc->select("a[rel=next]"), "Sonraki Sayfa").valid();
        return p;
    }
    Page search(const std::string& q, int page) override {
        login();
        DocPtr doc = this->page("/page/" + std::to_string(page) + "/?s=" + http::urlEncode(q));
        Page p = listOf(*doc, "div.moviefilm", item(*doc));
        p.hasNextPage = nextAfterCurrent(*doc);
        return p;
    }

    Details details(const std::string& url) override {
        login();
        DocPtr doc = page(url);
        Details d;
        html::Node content = doc->selectFirst("div.filmcontent");
        d.title = content.selectFirst("h1").text();
        d.thumbnail = doc->absUrl(content.selectFirst("img"), "src");
        auto rows = content.select("tr");
        std::vector<std::string> g;
        for (auto& tr : allContaining(rows, "Hentai Türü"))
            for (auto& a : tr.select("td > a")) g.push_back(a.text());
        d.genre = joinStr(g);
        for (auto& tr : rows)
            if (contains(tr.text(), "Özet")) {
                html::Node n = nextElement(tr);
                if (n.tag() == "tr") d.description = n.selectFirst("td").text();
                break;
            }
        for (auto& a : doc->select("div#Bolumler li > a")) {
            std::string t = a.text();
            std::string num = afterLast(beforeOr(t, ". Bölüm", ""), " ");
            if (trim(num).empty()) num = "1";
            Episode e;
            e.url = rel(*doc, a);
            e.number = toNumber(num, 1);
            e.name = num + ". Bölüm";
            d.episodes.push_back(e);
        }
        newestFirst(d.episodes);
        return d;
    }

    std::vector<Video> videos(const std::string& url) override {
        login();
        DocPtr doc = page(url);
        html::Node item = firstContaining(doc->select("div.alternatif a"), "Videa");
        if (!item) throw http::Error("Nessun video Videa trovato");
        std::string path = substringBefore(substringAfter(item.attr("onclick"), "../../"), "'");
        DocPtr frame = page("/" + path);
        std::string videaUrl = frame->absUrl(frame->selectFirst("iframe"), "src");
        std::vector<Video> out = videa(videaUrl);
        finish(out, "720p");
        return out;
    }

  protected:
    http::Headers headers() const override { return {{"Origin", baseUrl()}, {"Referer", baseUrl() + "/"}}; }

  private:
    std::mutex loginMutex;
    bool loggedIn = false;

    /** Accesso con l'account dimostrativo pubblico del sito (come fa l'estensione). */
    void login() {
        std::lock_guard<std::mutex> lock(loginMutex);
        if (loggedIn) return;
        loggedIn = true;
        try {
            httpPostRaw(full("/giris"), formBody({{"user", "demo"}, {"pass", "demo"}, {"redirect_to", baseUrl()}}),
                        withHeaders(headers(), {{"X-Requested-With", "XMLHttpRequest"}}));
        } catch (const std::exception&) {
        }
    }

    static bool nextAfterCurrent(const html::Document& doc) {
        for (auto& s : doc.select("span.current")) {
            html::Node n = nextElement(s);
            if (n.tag() == "a") return true;
        }
        return false;
    }

    static std::function<Anime(const html::Node&)> item(const html::Document& doc) {
        return [&doc](const html::Node& el) {
            std::string t = beforeLast(substringBefore(el.selectFirst("div.movief > a").text(), ". Bölüm"), " ");
            std::string img = doc.absUrl(el.selectFirst("img"), "src");
            std::string slug = substringBefore(afterLast(img, "/"), ".");
            return Anime{"/hentai-detay/" + slug, t, img};
        };
    }
};

// ---- HDFilmCehennemi (tr.hdfilmcehennemi, 18+ secondo build.gradle)
class HDFilmCehennemi : public Base {
  public:
    HDFilmCehennemi() : Base("tr.hdfilmcehennemi", "HDFilmCehennemi", "https://www.hdfilmcehennemi.nl", "tr", true) {}

    Page popular(int page) override { return load("/load/page/" + std::to_string(page) + "/mostLiked/"); }
    Page latest(int page) override { return load("/load/page/" + std::to_string(page) + "/home/"); }
    Page search(const std::string& q, int) override {
        json j = parseJson(post("/search", formBody({{"query", q}}), api()));
        Page p;
        std::set<std::string> seen;
        for (auto& h : jarr(j, "results")) {
            html::Document doc(jval(h), baseUrl() + "/");
            html::Node a = doc.selectFirst("a[href]");
            if (!a) continue;
            Anime an = poster(doc, a);
            if (seen.insert(an.url).second) p.animes.push_back(an);
        }
        return p;
    }

    Details details(const std::string& url) override {
        DocPtr doc = page(url);
        Details d;
        d.status = contains(doc->url(), "/dizi/") ? "" : "Completato";
        d.title = substringBefore(substringBefore(ownText(doc->selectFirst(".section-title")), " Filminin Bilgileri"), " izle");
        html::Node div = doc->selectFirst("div.section-content div.post-info");
        d.thumbnail = doc->absUrl(div.selectFirst("img"), "data-src");
        d.genre = joinText(div.select("div.post-info-genres > a"));
        d.author = joinText(div.select("div.post-info-cast > a"));
        d.description = div.selectFirst("div.post-info-content > p").text();
        if (!contains(url, "/dizi/")) {
            d.episodes.push_back({url, "Movie", 1});
            return d;
        }
        for (auto& a : doc->select("div.seasons-tabs-wrapper > div.seasons-tab-content > a")) {
            Episode e;
            e.url = rel(*doc, a);
            e.name = a.selectFirst("h3, h4").text();
            // Regex("(\d+)\.") -> numeri di stagione ed episodio
            std::vector<std::string> nums;
            for (size_t i = 0; i < e.name.size(); i++) {
                if (!std::isdigit((unsigned char)e.name[i])) continue;
                size_t s = i;
                while (i < e.name.size() && std::isdigit((unsigned char)e.name[i])) i++;
                if (i < e.name.size() && e.name[i] == '.') nums.push_back(e.name.substr(s, i - s));
            }
            e.number = 1;
            if (nums.size() >= 2) {
                std::string ep = nums[1];
                while (ep.size() < 3) ep = "0" + ep;
                e.number = toNumber(nums[0] + "." + ep, 1);
            }
            d.episodes.push_back(e);
        }
        std::stable_sort(d.episodes.begin(), d.episodes.end(),
                         [](const Episode& a, const Episode& b) { return a.number > b.number; });
        return d;
    }

    std::vector<Video> videos(const std::string& url) override {
        DocPtr doc = page(url);
        std::vector<Video> out;
        for (auto& tab : doc->select("div.alternative-tab div.alternative-links[data-lang]")) {
            for (auto& btn : tab.select("button.alternative-link[data-video]")) {
                std::string name = "[" + tab.attr("data-lang") + "] " + btn.text();
                std::string id = btn.attr("data-video");
                tryAppend(out, [&]() -> std::vector<Video> {
                    std::string body = get("/video/" + id + "/", api());
                    std::string u = substringBefore(substringAfter(body, "src="), " ");
                    size_t s = 0, e = u.size();
                    while (s < e && std::strchr("\\\"' ", u[s])) s++;
                    while (e > s && std::strchr("\\\"' ", u[e - 1])) e--;
                    u = replaceAll(u.substr(s, e - s), "\\/", "/");
                    if (contains(u, "/rplayer")) return rapidrame(u, name, headers());
                    if (contains(name, "close") || contains(u, "rapidrame")) return closeload(u, name, headers());
                    if (contains(u, "trstx.org")) return xbet(u, headers());
                    return {};
                });
            }
        }
        finish(out, "720p");
        return out;
    }

  protected:
    http::Headers headers() const override { return {{"Referer", baseUrl() + "/"}, {"Origin", baseUrl()}}; }

  private:
    http::Headers api() const { return {{"X-Requested-With", "fetch"}}; }

    static Anime poster(const html::Document& doc, const html::Node& a) {
        html::Node img = a.selectFirst("img");
        std::string thumb = doc.absUrl(img, "data-src");
        if (trim(thumb).empty()) thumb = doc.absUrl(img, "src");
        return Anime{rel(doc, a), a.selectFirst("strong.poster-title, h4.title").text(), thumb};
    }

    Page load(const std::string& path) {
        json j = parseJson(get(path, api()));
        html::Document doc(jstr(j, "html"), baseUrl() + "/");
        Page p = listOf(doc, "a.poster", [&](const html::Node& a) { return poster(doc, a); });
        p.hasNextPage = p.animes.size() >= 28;
        return p;
    }
};

// =============================================================================================== RUSSO

// ---- Animevost (ru.animevost, ru.animevost.mirror)
class Animevost : public Base {
  public:
    Animevost(const char* id, const char* name, const char* url) : Base(id, name, url, "ru") {}

    Page popular(int page) override { return request(page, "rating"); }
    Page latest(int page) override { return request(page, "date"); }
    Page search(const std::string& q, int page) override {
        int searchStart = page <= 1 ? 0 : page;
        int resultFrom = (page - 1) * 10 + 1;
        std::string url = baseUrl() + "/index.php?do=search";
        std::string body = httpPost(url, formBody({{"do", "search"},
                                                   {"subaction", "search"},
                                                   {"search_start", std::to_string(searchStart)},
                                                   {"full_search", "0"},
                                                   {"result_from", std::to_string(resultFrom)},
                                                   {"story", q}}));
        return parseList(html::Document(body, url));
    }

    Details details(const std::string& url) override {
        DocPtr doc = page(url);
        Details d;
        html::Node img = doc->selectFirst("img[src*='/uploads/']");
        std::string src = img.attr("src").empty() ? img.attr("data-src") : img.attr("src");
        if (!src.empty()) d.thumbnail = fixUrl(src, baseUrl());
        d.title = doc->selectFirst("h1, .title, .shortstoryHead h1").text();
        if (d.title.empty()) d.title = doc->selectFirst("title").text();
        html::Node block = doc->selectFirst(".shortstoryContent, .full_story, .fullstory, .shortstory, #dle-content");
        std::string text = block.text();
        // i commenti degli utenti non devono finire nella descrizione
        for (auto& c : block.select("#comments, .comments, .comment_list, .comment_block, .zcomment, #zcomment")) {
            std::string ct = c.text();
            if (!ct.empty()) text = replaceAll(text, ct, "");
        }
        auto field = [&](const std::string& label) {
            auto p = text.find(label);
            if (p == std::string::npos) return std::string();
            size_t s = p + label.size(), e = std::string::npos;
            for (const char* stop : {"Тип:", "Жанр:", "Год выхода:", "Количество серий:", "Режиссёр:", "Описание:"}) {
                auto q = text.find(stop, s);
                if (q != std::string::npos && q < e) e = q;
            }
            return trim(text.substr(s, e == std::string::npos ? std::string::npos : e - s));
        };
        std::string year = field("Год выхода:"), type = field("Тип:");
        d.genre = field("Жанр:");
        std::string desc;
        if (!year.empty()) desc += "Год: " + year + "\n";
        if (!type.empty()) desc += "Тип: " + type + "\n";
        if (!desc.empty()) desc += "\n";
        std::string body = contains(text, "Описание:") ? trim(substringAfter(text, "Описание:")) : "";
        if (body.empty()) body = trim(substringBefore(text, "Год выхода:"));
        d.description = trim(desc + replaceAll(body, "<br />", ""));

        // episodi: var data = {"1 серия":"123", ...};
        std::string script = scriptWith(*doc, {"var data = {"});
        std::string data = substringBefore(substringAfter(script, "var data = {"), "};");
        data = trim(data);
        while (!data.empty() && (data.back() == ',' || std::isspace((unsigned char)data.back()))) data.pop_back();
        std::vector<std::pair<std::string, std::string>> pairs;
        ojson j = ojson::parse("{" + data + "}", nullptr, false);
        if (j.is_object()) {
            for (auto& it : j.items()) pairs.push_back({it.key(), it.value().is_string() ? it.value().get<std::string>() : ""});
        } else {
            // JSON non rigoroso: coppie "chiave":"valore"
            size_t pos = 0;
            while (true) {
                size_t k1 = data.find('"', pos);
                if (k1 == std::string::npos) break;
                size_t k2 = data.find('"', k1 + 1);
                size_t v1 = k2 == std::string::npos ? k2 : data.find('"', k2 + 1);
                size_t v2 = v1 == std::string::npos ? v1 : data.find('"', v1 + 1);
                if (v2 == std::string::npos) break;
                pairs.push_back({data.substr(k1 + 1, k2 - k1 - 1), data.substr(v1 + 1, v2 - v1 - 1)});
                pos = v2 + 1;
            }
        }
        int index = 0;
        for (auto& p : pairs) {
            index++;
            if (p.first.empty() || p.second.empty()) continue;
            d.episodes.push_back({"/frame5.php?play=" + p.second + "&old=1", p.first, (double)index});
        }
        std::reverse(d.episodes.begin(), d.episodes.end());
        return d;
    }

    std::vector<Video> videos(const std::string& url) override {
        std::string body = get(url);
        // "file":"[720p]https://... or https://...,[480p]..." (il piu' lungo con http)
        std::string fileData;
        size_t pos = 0;
        while ((pos = body.find("\"file\"", pos)) != std::string::npos) {
            size_t p = pos + 6;
            pos = p;
            while (p < body.size() && std::isspace((unsigned char)body[p])) p++;
            if (p >= body.size() || body[p] != ':') continue;
            p++;
            while (p < body.size() && std::isspace((unsigned char)body[p])) p++;
            if (p >= body.size() || body[p] != '"') continue;
            size_t e = body.find('"', p + 1);
            if (e == std::string::npos) break;
            std::string v = body.substr(p + 1, e - p - 1);
            if (contains(v, "http") && v.size() > fileData.size()) fileData = v;
        }
        std::vector<Video> out;
        size_t p = 0;
        while ((p = fileData.find('[', p)) != std::string::npos) {
            size_t rb = fileData.find(']', p);
            if (rb == std::string::npos) break;
            std::string quality = fileData.substr(p + 1, rb - p - 1);
            size_t next = fileData.find(",[", rb);
            std::string urls = fileData.substr(rb + 1, next == std::string::npos ? std::string::npos : next - rb - 1);
            std::vector<std::string> list;
            for (auto& u : splitStr(urls, " or "))
                if (startsWith(trim(u), "http")) list.push_back(trim(u));
            for (size_t i = 0; i < list.size(); i++)
                out.push_back(mkVideo(list[i], list.size() > 1 ? quality + " - Mirror " + std::to_string(i + 1) : quality));
            if (next == std::string::npos) break;
            p = next + 1;
        }
        finish(out, "720");
        return out;
    }

  private:
    Page request(int page, const std::string& sortBy) {
        std::string url = baseUrl() + "/page/" + std::to_string(page);
        std::string body = httpPost(url, formBody({{"dlenewssortby", sortBy},
                                                   {"dledirection", "desc"},
                                                   {"set_new_sort", "dle_sort_main"},
                                                   {"set_direction_sort", "dle_direction_main"}}));
        return parseList(html::Document(body, url));
    }

    static std::string thumbOf(const html::Node& container) {
        html::Node img = container.selectFirst("img");
        std::string src = img.attr("src").empty() ? img.attr("data-src") : img.attr("src");
        if (!src.empty()) return src;
        std::string style = container.attr("style");
        auto p = style.find("background-image");
        if (p == std::string::npos) return "";
        std::string u = substringBefore(substringAfter(style.substr(p), "url("), ")");
        u = trim(u);
        if (!u.empty() && (u.front() == '"' || u.front() == '\'')) u.erase(0, 1);
        if (!u.empty() && (u.back() == '"' || u.back() == '\'')) u.pop_back();
        return u;
    }

    Page parseList(const html::Document& doc) {
        Page p;
        if (doc.selectFirst(".searchnoresult, .search_noresult")) return p;
        for (auto& n : doc.select("div, p")) {
            std::string own = ownText(n);
            if (contains(own, "Ничего не найдено") || contains(own, "По вашему запросу ничего не найдено") ||
                contains(own, "Извините, по вашему запросу"))
                return p;
        }
        auto containers = doc.select("div.shortstory, div.searchnews, div.searchitem, div.post, article");
        if (containers.empty()) {
            std::set<GumboInternalNode*> seen;
            for (auto& a : doc.select("a[href*='/tip/']")) {
                html::Node parent = a.parent();
                if (parent && parent.selectFirst("img") && seen.insert(parent.raw()).second) containers.push_back(parent);
            }
        }
        static const std::set<std::string> deny = {"tv", "tvspeshl", "tv-speshl", "special", "speshl", "ova", "ona",
                                                   "film", "films", "movie", "dunhua", "korotkometrazhniy",
                                                   "korotkometrazhnyy", "polnometrazhniy", "polnometrazhnyy"};
        std::set<std::string> seenUrls;
        for (auto& c : containers) {
            html::Node link = c.selectFirst("a[href*='/tip/']");
            if (!link) continue;
            std::string href = doc.absUrl(link, "href");
            if (href.empty()) href = link.attr("href");
            if (href.empty() || !seenUrls.insert(href).second) continue;
            std::string trimmed = href;
            while (!trimmed.empty() && trimmed.back() == '/') trimmed.pop_back();
            std::string slug = lower(afterLast(trimmed, "/"));
            bool letters = !slug.empty();
            for (char ch : slug)
                if (!std::isalpha((unsigned char)ch)) letters = false;
            if ((slug.size() <= 6 && letters) || deny.count(slug)) continue;
            Anime a;
            a.url = relUrl(href);
            html::Node imgIn = link.selectFirst("img");
            a.title = link.attr("title");
            if (a.title.empty()) a.title = imgIn.attr("alt");
            if (a.title.empty()) a.title = c.selectFirst("h1, h2, h3, h4, .shortstoryHead a, .shortstoryHead").text();
            if (a.title.empty()) a.title = link.text();
            if (a.title.empty()) a.title = "No title";
            html::Node poster = c.selectFirst("img[src*='/uploads/'], img[data-src*='/uploads/']");
            std::string src = contains(poster.attr("src"), "/uploads/") ? poster.attr("src") : poster.attr("data-src");
            if (src.empty()) src = thumbOf(c);
            if (!src.empty()) a.thumbnail = fixUrl(src, baseUrl());
            p.animes.push_back(a);
        }
        // "span.nav_ext + a, td.block_4 span:not(.nav_ext) + a"
        for (auto& s : doc.select("span.nav_ext, td.block_4 span")) {
            html::Node n = nextElement(s);
            if (n.tag() == "a") {
                p.hasNextPage = true;
                break;
            }
        }
        return p;
    }
};

// ---- YummyAnime (ru.yummyanime): API api.yani.tv
class YummyAnime : public Base {
  public:
    YummyAnime() : Base("ru.yummyanime", "YummyAnime", "https://ru.yummyani.me", "ru") {}

    Page popular(int page) override {
        json j = api("/anime/catalog?limit=20&offset=" + std::to_string((page - 1) * 20));
        Page p;
        for (auto& a : jarr(jobj(j, "response"), "data")) p.animes.push_back(toAnime(a));
        p.hasNextPage = p.animes.size() == 20;
        return p;
    }
    Page latest(int) override { return plainList(api("/anime/schedule")); }
    Page search(const std::string& q, int) override { return plainList(api("/search?q=" + http::urlEncode(q))); }

    Details details(const std::string& url) override {
        std::string slug = afterLast(url, "/");
        json data = jobj(api("/anime/" + slug + "?need_videos=true"), "response");
        Details d;
        d.title = jstr(data, "title");
        d.description = jstr(data, "description");
        std::vector<std::string> g, s;
        for (auto& x : jarr(data, "genres")) g.push_back(jstr(x, "title"));
        for (auto& x : jarr(data, "studios")) s.push_back(jstr(x, "title"));
        d.genre = joinStr(g);
        d.author = joinStr(s);
        std::string st = jval(jobj(data, "anime_status").value("value", json()));
        d.status = st == "0" ? "Completato" : st == "1" ? "In corso" : "";
        const json& poster = jobj(data, "poster");
        d.thumbnail = fixUrl(jstr(poster, "huge").empty() ? jstr(poster, "big") : jstr(poster, "huge"));
        std::vector<std::string> nums;
        for (auto& v : jarr(data, "videos")) {
            std::string n = jval(v.value("number", json()));
            if (n.empty()) n = "1";
            if (std::find(nums.begin(), nums.end(), n) == nums.end()) nums.push_back(n);
        }
        for (auto& n : nums) d.episodes.push_back({slug + "|" + n, "Серия " + n, toNumber(n, 1)});
        std::stable_sort(d.episodes.begin(), d.episodes.end(), [](const Episode& a, const Episode& b) { return a.number > b.number; });
        bool movie = contains(jstr(jobj(data, "type"), "alias"), "movie");
        if (movie && d.episodes.size() == 1) d.episodes[0].name = "Фильм";
        return d;
    }

    std::vector<Video> videos(const std::string& url) override {
        std::string slug = substringBefore(url, "|"), num = substringAfter(url, "|");
        if (num == url) num = "1";
        json data = jobj(api("/anime/" + slug + "?need_videos=true&episode=" + num), "response");
        std::vector<Video> out;
        for (auto& v : jarr(data, "videos")) {
            if (jval(v.value("number", json())) != num) continue;
            const json& vd = jobj(v, "data");
            std::string dubbing = jstr(vd, "dubbing");
            if (dubbing.empty()) dubbing = "Unknown";
            std::string player = jstr(vd, "player");
            std::string iframe = fixUrl(jstr(v, "iframe_url"));
            if (iframe.empty()) continue;
            tryAppend(out, [&]() -> std::vector<Video> {
                if (containsCI(player, "kodik"))
                    return kodik(iframe, baseUrl() + "/", baseUrl(), dubbing, {{"X-Application", TOKEN}});
                return fallback(iframe, dubbing);
            });
        }
        if (out.empty()) throw http::Error("Nessun video trovato");
        // voci prima dei sottotitoli, poi la qualita' piu' vicina a 720p
        std::stable_sort(out.begin(), out.end(), [](const Video& a, const Video& b) {
            auto sub = [](const Video& v) { return containsCI(v.title, "Субтитры") || containsCI(v.title, "Subtitle") ? 1 : 0; };
            if (sub(a) != sub(b)) return sub(a) < sub(b);
            int qa = a.quality ? a.quality : qualityOf(a.title), qb = b.quality ? b.quality : qualityOf(b.title);
            return std::abs(qa - 720) < std::abs(qb - 720);
        });
        return out;
    }

    http::Headers imageHeaders() const override { return {}; }

  private:
    static constexpr const char* API = "https://api.yani.tv";
    static constexpr const char* TOKEN = "o0nap18m_7a0od86";

    static json api(const std::string& path) {
        return parseJson(httpGet(API + path, {{"Accept", "application/json"}, {"X-Application", TOKEN}}));
    }
    static Anime toAnime(const json& a) {
        return Anime{jstr(a, "anime_url"), jstr(a, "title"), fixUrl(jstr(jobj(a, "poster"), "big"))};
    }
    static Page plainList(const json& j) {
        Page p;
        for (auto& a : jarr(j, "response")) p.animes.push_back(toAnime(a));
        return p;
    }

    std::vector<Video> fallback(const std::string& iframe, const std::string& dubbing) {
        std::string body = httpGet(iframe, {{"Accept", "application/json"}, {"X-Application", TOKEN}});
        if (contains(iframe, "sibnet.ru") || contains(body, "player.src")) {
            auto v = sibnet(iframe, dubbing + " (Sibnet) ");
            if (!v.empty()) return v;
        }
        std::string origin = http::originOf(iframe);
        http::Headers vh = {{"Origin", origin}, {"X-Application", TOKEN}};
        std::string mpd = findUrlWith(body, ".mpd");
        if (!mpd.empty()) return {mkVideo(mpd, dubbing + " (DASH)", iframe, vh)};
        std::string m3u8 = findUrlWith(body, ".m3u8");
        if (m3u8.empty()) return {};
        return {mkVideo(m3u8, dubbing + " (Unknown)", iframe, vh)};
    }
};

// ---- Animelib (ru.animelib, 18+ secondo build.gradle): API hapi.hentaicdn.org
class Animelib : public Base {
  public:
    Animelib() : Base("ru.animelib", "Animelib", "https://animelib.org", "ru", true) {}

    Page popular(int page) override { return list("/anime?page=" + std::to_string(page) + "&site_id%5B%5D=5&links%5B%5D="); }
    Page latest(int page) override {
        return list("/anime?page=" + std::to_string(page) + "&site_id%5B%5D=5&links%5B%5D=&sort_by=last_episode_at");
    }
    Page search(const std::string& q, int page) override {
        return list("/anime?page=" + std::to_string(page) + "&site_id%5B%5D=5&links%5B%5D=&sort_by=rating_score&q=" +
                    http::urlEncode(q));
    }

    Details details(const std::string& url) override {
        std::string slug = trimSlash(url);
        json data = jobj(api("/anime/" + slug +
                             "?fields%5B%5D=genres&fields%5B%5D=summary&fields%5B%5D=authors&fields%5B%5D=publisher"
                             "&fields%5B%5D=otherNames&fields%5B%5D=anime_status_id"),
                         "data");
        Anime a = toAnime(data);
        Details d;
        d.title = a.title;
        d.thumbnail = a.thumbnail;
        d.description = summary(data.value("summary", json()));
        int st = (int)toNumber(jstr(jobj(data, "status"), "id"), 0);
        d.status = st == 1 ? "In corso" : st == 2 ? "Completato" : st == 4 ? "In pausa" : st == 5 ? "Cancellato" : "";
        std::vector<std::string> pub, auth, gen;
        for (auto& x : jarr(data, "publisher")) pub.push_back(jstr(x, "name"));
        for (auto& x : jarr(data, "authors")) auth.push_back(jstr(x, "name"));
        for (auto& x : jarr(data, "genres")) gen.push_back(jstr(x, "name"));
        d.author = joinStr({joinStr(pub), joinStr(auth)});
        d.genre = joinStr(gen);
        json eps = api("/episodes?anime_id=" + http::urlEncode(slug));
        for (auto& e : jarr(eps, "data")) {
            Episode ep;
            ep.url = "api/episodes/" + jstr(e, "id");
            ep.name = trim("Сезон " + jstr(e, "season") + " Серия " + jstr(e, "number") + " " + jstr(e, "name"));
            ep.number = toNumber(jstr(e, "number"), 0);
            d.episodes.push_back(ep);
        }
        std::reverse(d.episodes.begin(), d.episodes.end());
        return d;
    }

    std::vector<Video> videos(const std::string& url) override {
        std::string path = startsWith(url, "http") ? http::pathOf(url) : url;
        if (!startsWith(path, "/")) path = "/" + path;
        json ep = jobj(parseJson(httpGet(API_SITE + path, apiHeaders())), "data");
        const json& players = jarr(ep, "players");
        std::string server;
        try {
            json c = api("/constants?fields%5B%5D=videoServers");
            const json& servers = jarr(jobj(c, "data"), "videoServers");
            for (auto& s : servers)
                if (jstr(s, "label") == "Основной") server = jstr(s, "url");
            if (server.empty() && !servers.empty()) server = jstr(servers[0], "url");
        } catch (const std::exception&) {
        }
        auto best = [](const json& p) {
            std::string pl = lower(jstr(p, "player"));
            if (pl == "kodik") return 720;
            int m = 0;
            if (pl == "animelib")
                for (auto& q : jarr(jobj(p, "video"), "quality")) m = std::max(m, (int)toNumber(jstr(q, "quality"), 0));
            return m;
        };
        std::vector<Video> out;
        for (auto& p : players) {
            std::string team = jstr(jobj(p, "team"), "name");
            // solo la qualita' migliore per ogni studio; si escludono i sottotitoli (predefiniti dell'estensione)
            bool better = false;
            for (auto& o : players)
                if (jstr(jobj(o, "team"), "name") == team && best(o) > best(p)) better = true;
            if (better) continue;
            if ((int)toNumber(jstr(jobj(p, "translation_type"), "id"), 0) == 1) continue;
            std::string pl = lower(jstr(p, "player"));
            tryAppend(out, [&]() -> std::vector<Video> {
                if (pl == "kodik") {
                    std::string src = jstr(p, "src");
                    if (src.empty()) return {};
                    return kodik(src, baseUrl() + "/ru/", baseUrl(), team, {}, true);
                }
                if (pl != "animelib") return {};
                std::vector<Video::Track> subs;
                for (auto& s : jarr(p, "subtitles"))
                    subs.push_back({jstr(s, "src"), team + " (" + jstr(s, "format") + ")"});
                int maxQ = best(p);
                std::vector<Video> v;
                for (auto& q : jarr(jobj(p, "video"), "quality")) {
                    int qq = (int)toNumber(jstr(q, "quality"), 0);
                    if (qq != maxQ) continue;
                    std::string href = trim(jstr(q, "href"));
                    std::string u = server.empty() ? fixUrl(href) : fixUrl(href, server.back() == '/' ? server : server + "/");
                    if (!startsWith(href, "http") && !startsWith(href, "//") && !server.empty())
                        u = (server.back() == '/' ? server.substr(0, server.size() - 1) : server) + (startsWith(href, "/") ? "" : "/") + href;
                    Video vid = mkVideo(u, team + " (" + std::to_string(qq) + "p Animelib)", baseUrl() + "/");
                    vid.quality = qq;
                    vid.subtitles = subs;
                    v.push_back(vid);
                }
                return v;
            });
        }
        if (out.empty()) throw http::Error("Nessun video trovato");
        return out;
    }

    http::Headers imageHeaders() const override { return {{"Referer", baseUrl() + "/"}, {"Origin", baseUrl()}}; }

  private:
    static constexpr const char* API_SITE = "https://hapi.hentaicdn.org";

    http::Headers apiHeaders() const {
        return {{"Referer", baseUrl() + "/"},
                {"Origin", baseUrl()},
                {"Accept", "application/json, text/plain, */*"},
                {"X-Requested-With", "XMLHttpRequest"},
                {"User-Agent", "Mozilla/5.0 (Android)"}};
    }
    json api(const std::string& path) const { return parseJson(httpGet(std::string(API_SITE) + "/api" + path, apiHeaders())); }

    static std::string trimSlash(std::string s) {
        while (!s.empty() && s.front() == '/') s.erase(0, 1);
        return s;
    }

    static std::string summary(const json& el) {
        std::string out;
        std::function<void(const json&)> rec = [&](const json& e) {
            if (e.is_string()) out += e.get<std::string>();
            else if (e.is_primitive() && !e.is_null()) out += jval(e);
            else if (e.is_object()) {
                auto t = e.find("text");
                if (t != e.end() && t->is_string()) out += t->get<std::string>();
                auto c = e.find("content");
                if (c != e.end()) rec(*c);
            } else if (e.is_array()) {
                for (auto& x : e) {
                    rec(x);
                    out += " ";
                }
            }
        };
        rec(el);
        std::string norm;
        bool sp = false;
        for (char c : out) {
            if (std::isspace((unsigned char)c)) {
                sp = true;
                continue;
            }
            if (sp && !norm.empty()) norm += ' ';
            sp = false;
            norm += c;
        }
        return norm;
    }

    static Anime toAnime(const json& a) {
        Anime an;
        an.url = jstr(a, "slug_url");
        an.title = jstr(a, "rus_name");
        if (an.title.empty()) an.title = jstr(a, "eng_name");
        if (an.title.empty()) {
            json on = a.value("otherNames", json());
            if (on.is_array() && !on.empty()) an.title = jval(on[0]);
            else if (on.is_string()) an.title = on.get<std::string>();
        }
        if (an.title.empty()) an.title = an.url;
        std::string cover = trim(jstr(jobj(a, "cover"), "default"));
        if (!cover.empty()) an.thumbnail = fixUrl(cover, "https://cover.hentaicdn.org");
        return an;
    }

    Page list(const std::string& path) {
        json j = api(path);
        Page p;
        for (auto& a : jarr(j, "data")) p.animes.push_back(toAnime(a));
        p.hasNextPage = !jstr(jobj(j, "links"), "next").empty();
        return p;
    }
};

// =============================================================================================== POLACCO

// ---- Docchi (pl.docchi, 18+ secondo build.gradle): API api.docchi.pl
class Docchi : public Base {
  public:
    Docchi() : Base("pl.docchi", "Docchi", "https://docchi.pl", "pl", true) {}

    Page popular(int page) override { return list("/v1/series/list?limit=20&before=" + std::to_string((page - 1) * 20)); }
    Page latest(int page) override {
        return list("/v1/series/list?limit=20&before=" + std::to_string((page - 1) * 20) + "&sort=DESC");
    }
    Page search(const std::string& q, int) override {
        Page p = list("/v1/series/related/" + http::urlEncode(q));
        p.hasNextPage = false;
        return p;
    }

    Details details(const std::string& url) override {
        std::string slug = afterLast(url, "/");
        json a = parseJson(httpGet(API + "/v1/series/find/" + slug));
        Details d;
        d.title = jstr(a, "title");
        d.description = jstr(a, "description");
        d.thumbnail = jstr(a, "cover");
        std::vector<std::string> g;
        for (auto& x : jarr(a, "genres")) g.push_back(jval(x));
        d.genre = joinStr(g);
        try {
            json mal = jobj(parseJson(httpGet("https://api.jikan.moe/v4/anime/" + jstr(a, "mal_id"))), "data");
            const json& studios = jarr(mal, "studios");
            if (!studios.empty()) d.author = jstr(studios[0], "name");
            std::string st = lower(jstr(mal, "status"));
            d.status = contains(st, "currently airing") ? "In corso" : contains(st, "finished airing") ? "Completato" : "";
        } catch (const std::exception&) {
        }
        json eps = parseJson(httpGet(API + "/v1/episodes/count/" + slug));
        if (eps.is_array())
            for (auto& e : eps) {
                double n = toNumber(jstr(e, "anime_episode_number"), 0);
                d.episodes.push_back({"/production/as/" + jstr(e, "anime_id") + "/" + kotlinFloatStr(n),
                                      std::to_string((long long)n) + " Odcinek", n});
            }
        std::reverse(d.episodes.begin(), d.episodes.end());
        return d;
    }

    std::vector<Video> videos(const std::string& url) override {
        std::string num = afterLast(url, "/");
        std::string id = afterLast(beforeLast(url, "/"), "/");
        json list = parseJson(httpGet(API + "/v1/episodes/find/" + id + "/" + num));
        std::vector<Video> out;
        if (list.is_array())
            for (auto& p : list) {
                std::string sub = jstr(p, "translator_title");
                std::transform(sub.begin(), sub.end(), sub.begin(), [](unsigned char c) { return (char)std::toupper(c); });
                std::string prefix = (jstr(p, "isInverted") == "true" ? "[Odwrócone Kolory] " : "") + sub + " - ";
                std::string host = lower(jstr(p, "player_hosting")), u = jstr(p, "player");
                tryAppend(out, [&]() -> std::vector<Video> {
                    if (contains(host, "filemoon")) return moon(u, baseUrl(), prefix + "Filemoon - ");
                    if (contains(u, "vk.com")) return vk(u, prefix);
                    if (contains(u, "mp4upload")) return mp4upload(u, prefix);
                    if (contains(u, "cda.pl")) return cda(u, prefix);
                    if (contains(u, "dailymotion")) return dailymotion(u, prefix + "Dailymotion -");
                    if (contains(u, "sibnet.ru")) return sibnet(u, prefix);
                    if (contains(u, "dood")) return dood(u, prefix + "Dood ");
                    if (contains(u, "lulu")) return lulu(u, prefix);
                    if (contains(u, "drive.google.com")) return googleDrive(substringBefore(substringAfter(u, "/d/"), "/"), prefix + "Gdrive -");
                    if (contains(u, "strmup.to")) return streamup(u, prefix);
                    return {};  // lycoris.cafe non supportato
                });
            }
        if (out.empty()) throw http::Error("Nessun video trovato");
        std::stable_sort(out.begin(), out.end(), [](const Video& a, const Video& b) {
            auto key = [](const Video& v) {
                return std::make_tuple(containsCI(v.title, "AI") ? 0 : 1, contains(v.title, "1080") ? 1 : 0,
                                       containsCI(v.title, "cda.pl") ? 1 : 0);
            };
            return key(a) > key(b);
        });
        return out;
    }

    http::Headers imageHeaders() const override { return {}; }

  private:
    const std::string API = "https://api.docchi.pl";

    Page list(const std::string& path) {
        json arr = parseJson(httpGet(API + path));
        Page p;
        if (!arr.is_array()) return p;
        for (auto& a : arr) {
            bool adult = a.value("adult_content", false);
            p.animes.push_back({std::string(adult ? "/hentai/" : "/production/as/") + jstr(a, "slug"), jstr(a, "title"), jstr(a, "cover")});
        }
        p.hasNextPage = !arr.empty();
        return p;
    }
};

// ---- OgladajAnime (pl.ogladajanime, 18+ secondo build.gradle)
class OgladajAnime : public Base {
  public:
    OgladajAnime() : Base("pl.ogladajanime", "OgladajAnime", "https://ogladajanime.pl", "pl", true) {}

    Page popular(int page) override { return searchReq({{"page", std::to_string(page)}, {"search_type", "page"}}); }
    Page latest(int page) override { return searchReq({{"page", std::to_string(page)}, {"search_type", "new"}}); }
    Page search(const std::string& q, int page) override {
        return searchReq({{"page", std::to_string(page)}, {"search_type", "name"}, {"search", q}});
    }

    Details details(const std::string& url) override {
        DocPtr doc = page(url);
        Details d;
        d.title = doc->selectFirst("meta[property=og:title]").attr("content");
        if (d.title.empty()) d.title = doc->selectFirst("h1").text();
        d.thumbnail = doc->selectFirst("meta[property=og:image]").attr("content");
        std::string st = lower(html::textOf(allContaining(doc->select("div.col-12 > p.m-0"), "Status")));
        d.status = contains(st, "emitowane") ? "In corso" : contains(st, "zakończone") ? "Completato"
                   : (contains(st, "zapowiedź") || contains(st, "deklaracja")) ? "In pausa" : "";
        d.description = doc->selectFirst("p#animeDesc").text();
        d.genre = joinText(doc->select("div.row > div.col-12 > span.badge[href^=\"/search/name/\"]"));
        std::vector<std::string> studios;
        for (auto& col : allContaining(doc->select("div.row > div.col-12"), "Studio:"))
            for (auto& b : col.children())
                if (b.tag() == "span" && hasClass(b, "badge") && b.attr("href") == "#") studios.push_back(b.text());
        d.author = joinStr(studios);
        for (auto& li : doc->select("ul#ep_list > li")) {
            html::Node img;
            for (auto& div : li.children())
                if (div.tag() == "div")
                    for (auto& im : div.children())
                        if (im.tag() == "img" && !img) img = im;
            if (!img) continue;
            double n = toNumber(li.attr("value"), 0);
            std::string text = html::textOf(li.select("div > div > p"));
            std::string lang = img.attr("alt");
            std::transform(lang.begin(), lang.end(), lang.begin(), [](unsigned char c) { return (char)std::toupper(c); });
            std::string num = std::to_string((long long)n);
            std::string name = num + (lang == "PL" ? "" : " [" + lang + "]") + " " + (text.empty() ? "Odcinek" : text);
            d.episodes.push_back({li.attr("ep_id"), name, n});
        }
        std::reverse(d.episodes.begin(), d.episodes.end());
        return d;
    }

    std::vector<Video> videos(const std::string& url) override {
        json players = parseJson(httpGet(baseUrl() + ":8443/Player/" + url, apiHeaders()));
        std::vector<Video> out;
        if (players.is_array())
            for (auto& p : players) {
                std::string main = jstr(p, "mainUrl");
                std::string host = substringBefore(substringAfter(main, "://"), "/");
                if (startsWith(host, "www.")) host = host.substr(4);
                if (host.empty()) host = "unknown-host";
                std::string title = (jstr(p, "extra") == "inv" ? "[Odwrócone Kolory] " : "") + host + " - " + jstr(p, "res") + "p";
                std::string src = jstr(p, "src");
                if (!src.empty()) out.push_back(mkVideo(src, title));
            }
        finish(out, "1080");
        return out;
    }

  protected:
    http::Headers headers() const override { return apiHeaders(); }

  private:
    http::Headers apiHeaders() const {
        return {{"Accept", "application/json, text/plain, */*"},
                {"Referer", baseUrl() + "/"},
                {"Origin", baseUrl()},
                {"Accept-Language", "pl,en-US;q=0.7,en;q=0.3"}};
    }

    Page searchReq(const Form& f) {
        json j = parseJson(post("/manager.php?action=get_search", formBody(f)));
        std::string html = jstr(j, "data");
        html.erase(std::remove_if(html.begin(), html.end(), [](char c) { return c == '\t' || c == '\n' || c == '\r'; }), html.end());
        html::Document doc(html, baseUrl() + "/");
        Page p;
        int counter = 0;
        for (auto& el : doc.select("div.anime-item div.card.bg-white")) {
            counter++;
            Anime a{rel(doc, el.selectFirst("a")), el.selectFirst("h5.card-title > a").text(),
                    el.selectFirst("img").attr("data-srcset")};
            if (!a.url.empty()) p.animes.push_back(a);
        }
        p.hasNextPage = counter >= 25;
        return p;
    }
};

}  // namespace

std::vector<std::shared_ptr<Source>> makeArTrRuPlSources() {
    std::vector<std::shared_ptr<Source>> out;
    // arabo (anime)
    out.push_back(std::make_shared<AnimeListTheme>(AnimeListTheme::ANIME4UP, "ar.anime4up", "Anime4Up", "https://w1.anime4up.rest"));
    out.push_back(std::make_shared<AnimeListTheme>(AnimeListTheme::WITANIME, "ar.witanime", "WIT ANIME", "https://witanime.site"));
    out.push_back(std::make_shared<Animerco>());
    out.push_back(std::make_shared<AnimeLek>());
    out.push_back(std::make_shared<Okanime>());
    out.push_back(std::make_shared<AnimeBlkom>());
    out.push_back(std::make_shared<Animeiat>());
    out.push_back(std::make_shared<ArabAnime>());
    // arabo (film e serie)
    out.push_back(std::make_shared<ArabSeed>());
    out.push_back(std::make_shared<Asia2TV>());
    out.push_back(std::make_shared<EgyDead>());
    out.push_back(std::make_shared<Tuktukcinema>());
    // turco
    out.push_back(std::make_shared<TurkAnime>());
    out.push_back(std::make_shared<Anizm>());
    out.push_back(std::make_shared<Animeler>());
    out.push_back(std::make_shared<TRAnimeIzle>());
    out.push_back(std::make_shared<HentaiZM>());
    out.push_back(std::make_shared<HDFilmCehennemi>());
    // russo
    out.push_back(std::make_shared<Animevost>("ru.animevost", "Animevost", "https://animevost.org"));
    out.push_back(std::make_shared<Animevost>("ru.animevost.mirror", "Animevost Mirror", "https://v13.vost.pw"));
    out.push_back(std::make_shared<YummyAnime>());
    out.push_back(std::make_shared<Animelib>());
    // polacco
    out.push_back(std::make_shared<Docchi>());
    out.push_back(std::make_shared<OgladajAnime>());
    return out;
}

}  // namespace src
