// Porting in C++ delle estensioni Aniyomi (yuzono) per indonesiano, cinese, coreano, serbo, ucraino, hindi e
// alcune "all": OtakuDesu, Samehadaku, Kuramanime, Kuronime, Oploverz, NeoNime, NimeGami, Anime1.me, Xfani,
// Cycity, Iyf, Nivod, Xiaoxintv, Hanime1 (18+), Aniweek, AnimeSrbija, UAKino, UFDub, YoMovies (18+),
// AnimeWorld India, AniZone, AnimeOnsen, StreamingUnity (StreamingCommunity).
// Gli hoster non presenti in extractors.hpp (YourUpload, Blogger, Streamlare, Linkbox, HxFile, Animeku,
// MixDrop, Speedostream, Minoplres, Movembed, MyStream, VixCloud) sono implementati qui sotto.

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
#include <set>

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
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/132.0.0.0 Safari/537.36";
const char* MOBILE_UA =
    "Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Mobile Safari/537.36";

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

/** substringAfter che restituisce "" se il delimitatore manca. */
std::string afterOr(const std::string& s, const std::string& d) {
    auto p = s.find(d);
    return p == std::string::npos ? "" : s.substr(p + d.size());
}

std::string removePrefix(const std::string& s, const std::string& p) { return startsWith(s, p) ? s.substr(p.size()) : s; }
std::string removeSuffix(const std::string& s, const std::string& p) {
    return endsWith(s, p) ? s.substr(0, s.size() - p.size()) : s;
}

std::string trimChars(const std::string& s, const std::string& chars) {
    size_t a = 0, b = s.size();
    while (a < b && chars.find(s[a]) != std::string::npos) a++;
    while (b > a && chars.find(s[b - 1]) != std::string::npos) b--;
    return s.substr(a, b - a);
}

std::string lowerAscii(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

/** Prima sequenza numerica (anche decimale) nel testo, def se assente. */
double firstNumber(const std::string& s, double def = -1) {
    for (size_t i = 0; i < s.size(); i++) {
        if (!std::isdigit((unsigned char)s[i])) continue;
        size_t e = i;
        while (e < s.size() && (std::isdigit((unsigned char)s[e]) || s[e] == '.')) e++;
        std::string n = s.substr(i, e - i);
        while (!n.empty() && n.back() == '.') n.pop_back();
        return std::atof(n.c_str());
    }
    return def;
}

/** Come toFloatOrNull() di Kotlin: l'intera stringa deve essere un numero. */
double toNumber(const std::string& s, double def) {
    std::string t = trim(s);
    if (t.empty()) return def;
    char* end = nullptr;
    double v = std::strtod(t.c_str(), &end);
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

/** Corpo application/x-www-form-urlencoded. */
std::string formBody(const std::vector<std::pair<std::string, std::string>>& kv) {
    std::string out;
    for (auto& p : kv) {
        if (!out.empty()) out += "&";
        out += http::urlEncode(p.first) + "=" + http::urlEncode(p.second);
    }
    return out;
}

/** Testo di uno script contenente tutte le stringhe indicate (script:containsData(a):containsData(b)). */
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

/** Concatenazione dei testi di tutti gli script (come select("script").html()). */
std::string allScripts(const html::Document& doc) {
    std::string out;
    for (auto& s : doc.select("script")) {
        out += s.data();
        out += "\n";
    }
    return out;
}

/** Posizione (1-based) di un elemento tra i fratelli (per :nth-child). */
int nthChild(const html::Node& n) {
    html::Node p = n.parent();
    if (!p) return 0;
    int i = 0;
    for (auto& c : p.children()) {
        i++;
        if (c.raw() == n.raw()) return i;
    }
    return 0;
}

/** Figlio n-esimo (1-based) di un elemento, oppure nodo vuoto. */
html::Node childAt(const html::Node& n, size_t index) {
    auto ch = n.children();
    return index >= 1 && index <= ch.size() ? ch[index - 1] : html::Node();
}

/** Primo elemento che soddisfa "sel" e contiene il testo (come sel:contains(text)). */
html::Node firstContaining(const std::vector<html::Node>& nodes, const std::string& text) {
    for (auto& n : nodes)
        if (containsCI(n.text(), text)) return n;
    return html::Node();
}

std::string headerValue(const http::Headers& h, const std::string& name) {
    for (auto& kv : h)
        if (lower(kv.first) == lower(name)) return kv.second;
    return "";
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

/** Tutti i cookie "nome=valore" dei Set-Cookie di una risposta, uniti da "; ". */
std::string cookiesOf(const http::Response& r, const std::string& onlyName = "") {
    std::string out;
    auto range = r.headers.equal_range("set-cookie");
    for (auto it = range.first; it != range.second; ++it) {
        std::string c = trim(substringBefore(it->second, ";"));
        if (c.empty()) continue;
        if (!onlyName.empty() && lower(substringBefore(c, "=")) != lower(onlyName)) continue;
        if (!out.empty()) out += "; ";
        out += c;
    }
    return out;
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
        else if (k == "cookie") v.cookie = kv.second;
        else v.headers.push_back(kv);
    }
    return v;
}

void append(std::vector<Video>& out, std::vector<Video> v) { out.insert(out.end(), v.begin(), v.end()); }

/** Ordina: prima i titoli che contengono la qualita' preferita, poi qualita' decrescente (stabile). */
void sortByQuality(std::vector<Video>& v, const std::string& pref) {
    auto key = [&](const Video& x) {
        int p = !pref.empty() && contains(x.title, pref) ? 1 : 0;
        int h = x.quality ? x.quality : qualityOf(x.title);
        return std::make_pair(p, h);
    };
    std::stable_sort(v.begin(), v.end(), [&](const Video& a, const Video& b) { return key(a) > key(b); });
}

/** Solo "preferita per prima" (sortedByDescending { contains(pref) } di Kotlin). */
void preferFirst(std::vector<Video>& v, const std::string& pref) {
    std::stable_partition(v.begin(), v.end(), [&](const Video& x) { return contains(x.title, pref); });
}

/** Blocco JSON bilanciato ({...} o [...]) che inizia al primo '{' o '[' dopo "marker" (rispetta le stringhe). */
std::string balancedAfter(const std::string& text, const std::string& marker, size_t from = 0) {
    size_t p = marker.empty() ? from : text.find(marker, from);
    if (p == std::string::npos) return "";
    p += marker.size();
    while (p < text.size() && text[p] != '{' && text[p] != '[') {
        if (!std::isspace((unsigned char)text[p]) && text[p] != '=' && text[p] != ':' && text[p] != '(') return "";
        p++;
    }
    if (p >= text.size()) return "";
    char open = text[p], close = open == '{' ? '}' : ']';
    int depth = 0;
    bool inStr = false;
    char q = 0;
    for (size_t i = p; i < text.size(); i++) {
        char c = text[i];
        if (inStr) {
            if (c == '\\') i++;
            else if (c == q) inStr = false;
            continue;
        }
        if (c == '"' || c == '\'') {
            inStr = true;
            q = c;
        } else if (c == open) {
            depth++;
        } else if (c == close) {
            if (--depth == 0) return text.substr(p, i - p + 1);
        }
    }
    return "";
}

/** Valore stringa di un campo JSON (anche numerico), "" se assente o di altro tipo. */
std::string js(const json& j, const char* key) {
    if (!j.is_object() || !j.contains(key)) return "";
    const json& v = j[key];
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number()) return numStr(v.get<double>());
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    return "";
}

const json& jget(const json& j, const char* key) {
    static const json empty;
    if (!j.is_object() || !j.contains(key)) return empty;
    return j[key];
}

json parseJson(const std::string& s) { return json::parse(s, nullptr, false); }

/** Primo valore dell'attributo src="..." / src='...' in un frammento HTML breve (srcRegex). */
std::string srcAttr(const std::string& html) {
    size_t p = 0;
    while ((p = html.find("src", p)) != std::string::npos) {
        size_t i = p + 3;
        while (i < html.size() && std::isspace((unsigned char)html[i])) i++;
        if (i < html.size() && html[i] == '=') {
            i++;
            while (i < html.size() && std::isspace((unsigned char)html[i])) i++;
            if (i < html.size() && (html[i] == '"' || html[i] == '\'')) {
                char q = html[i];
                auto e = html.find_first_of("\"'", i + 1);
                (void)q;
                if (e != std::string::npos && e > i + 1) return html.substr(i + 1, e - i - 1);
            }
        }
        p += 3;
    }
    return "";
}

/** Legge l'oggetto var player_aaaa={...} delle pagine MacCMS e ne restituisce l'url (decodificato se serve). */
std::string macCmsPlayerUrl(const html::Document& doc) {
    std::string script = scriptWith(doc, {"player_aaaa"});
    if (script.empty()) return "";
    json info = parseJson(balancedAfter(script, "player_aaaa="));
    if (!info.is_object()) return "";
    std::string url = js(info, "url");
    std::string enc = js(info, "encrypt");
    if (enc == "1") url = urlDecode(url);
    else if (enc == "2") url = urlDecode(base64Decode(url));
    return url;
}

// =============================================================================================== hoster

std::vector<Video> yourUpload(const std::string& url, const std::string& title) {
    http::Headers h = {{"Referer", "https://www.yourupload.com/"}};
    html::Document doc(http::getText(url, h), url);
    std::string data = scriptWith(doc, {"jwplayerOptions"});
    if (data.empty()) return {};
    std::string file = substringBefore(substringAfter(data, "file: '"), "',");
    if (!startsWith(file, "http")) return {};
    return {simpleVideo(file, title, "https://www.yourupload.com/")};
}

// ---- Blogger (lib/bloggerextractor)
std::string bloggerQuality(const std::string& format) {
    if (format == "7") return "240p";
    if (format == "18") return "360p";
    if (format == "22") return "720p";
    if (format == "37") return "1080p";
    return "Unknown";
}

std::vector<Video> blogger(const std::string& url, const std::string& suffix) {
    http::Headers h = {{"User-Agent", http::DEFAULT_UA}};
    std::string body = http::getText(url, h);
    std::vector<Video> out;
    if (!contains(body, "errorContainer")) {
        std::string streams = substringBefore(afterOr(body, "\"streams\":["), "]");
        for (auto& it : split(streams, "},")) {
            std::string videoUrl = substringBefore(afterOr(it, "\"play_url\":\""), "\"");
            if (trim(videoUrl).empty()) continue;
            videoUrl = replaceAll(replaceAll(videoUrl, "\\u0026", "&"), "\\/", "/");
            std::string format = trim(substringBefore(substringAfter(it, "\"format_id\":"), "}"));
            std::string title = trim("Blogger - " + bloggerQuality(format) + " " + suffix);
            out.push_back(simpleVideo(videoUrl, title));
        }
    }
    if (!out.empty()) return out;

    // chiamata RPC di Blogger (batchexecute)
    std::string token = http::queryParam(url, "token");
    if (trim(token).empty()) return {};
    std::string fsid = substringBefore(substringAfter(body, "FdrFJe\":\""), "\"");
    std::string bl = substringBefore(substringAfter(body, "cfb2h\":\""), "\"");
    std::string reqId = std::to_string((long long)(std::time(nullptr) % 86400));
    std::string rpcUrl = "https://www.blogger.com/_/BloggerVideoPlayerUi/data/batchexecute?rpcids=WcwnYd&source-path=" +
                         http::urlEncode("/video.g") + "&f.sid=" + http::urlEncode(fsid) + "&bl=" + http::urlEncode(bl) +
                         "&hl=en-US&_reqid=" + reqId + "&rt=c";
    std::string rpcBody = "f.req=%5B%5B%5B%22WcwnYd%22%2C%22%5B%5C%22" + token +
                          "%5C%22%2C%5C%22%5C%22%2C0%5D%22%2Cnull%2C%22generic%22%5D%5D%5D&";
    http::Headers rh = {{"Accept", "*/*"},
                        {"Content-Type", "application/x-www-form-urlencoded;charset=UTF-8"},
                        {"User-Agent", http::DEFAULT_UA},
                        {"X-Same-Domain", "1"},
                        {"Referer", "https://www.blogger.com/"}};
    http::Response r = http::request("POST", rpcUrl, rh, rpcBody);
    std::string rpc = r.body;
    if (!contains(rpc, "https://")) return {};
    std::string inner = "\\\"" + substringBefore(afterOr(rpc, "[[\\\""), "]]]") + "]";
    for (auto& it : split(inner, "],[")) {
        std::string raw = substringBefore(afterOr(it, "\\\""), "\\\"");
        if (trim(raw).empty()) continue;
        // doppio escape JSON: \\u003d -> = -> =
        std::string once;
        json a = parseJson("\"" + raw + "\"");
        if (!a.is_string()) continue;
        once = a.get<std::string>();
        json b = parseJson("\"" + once + "\"");
        std::string videoUrl = b.is_string() ? b.get<std::string>() : once;
        if (!startsWith(videoUrl, "http")) continue;
        std::string format = substringBefore(substringAfter(it, "["), "]");
        out.push_back(simpleVideo(videoUrl, trim("Blogger - " + bloggerQuality(format) + " " + suffix)));
    }
    return out;
}

std::vector<Video> streamlare(const std::string& url, const std::string& prefix, const std::string& suffix) {
    std::string id = substringAfterLast(url, "/");
    http::Response r = http::request("POST", "https://slwatch.co/api/video/stream/get", {{"Content-Type", "application/json"}},
                                     "{\"id\":\"" + id + "\"}");
    std::string playlist = r.body;
    std::string type = substringBefore(substringAfter(playlist, "\"type\":\""), "\"");
    std::string pre = trim(prefix).empty() ? "" : trim(prefix) + " ";
    std::string suf = trim(suffix).empty() ? "" : " " + trim(suffix);
    if (type == "hls") {
        std::string master = replaceAll(substringBefore(substringAfter(playlist, "\"file\":\""), "\""), "\\/", "/");
        auto v = hlsVideos(master, "", pre + "Streamlare:");
        for (auto& x : v) x.title += suf;
        return v;
    }
    std::vector<Video> out;
    const std::string sep = "\"label\":\"";
    for (auto& it : split(substringAfter(playlist, sep), sep)) {
        std::string quality = substringBefore(it, "\",");
        std::string api = replaceAll(substringBefore(substringAfter(it, "\"file\":\""), "\","), "\\", "");
        if (!startsWith(api, "http")) continue;
        try {
            http::Response pr = http::request("POST", api, {}, "", 20);
            std::string videoUrl = pr.finalUrl.empty() ? api : pr.finalUrl;
            out.push_back(simpleVideo(videoUrl, pre + "Streamlare:" + quality + suf));
        } catch (const std::exception&) {
        }
    }
    return out;
}

std::vector<Video> linkbox(const std::string& url, const std::string& name) {
    std::string id = contains(url, "/file/") ? substringAfter(url, "/file/") : substringAfter(url, "?id=");
    json j = parseJson(http::getText("https://www.linkbox.to/api/open/get_url?itemId=" + http::urlEncode(id)));
    std::vector<Video> out;
    for (auto& it : jget(jget(j, "data"), "rList")) {
        std::string u = js(it, "url");
        if (u.empty()) continue;
        out.push_back(simpleVideo(u, js(it, "resolution") + " - " + name));
    }
    return out;
}

std::vector<Video> hxfile(const std::string& url, const std::string& title) {
    std::string embed = contains(url, "embed-") ? url : replaceAll(url, ".co/", ".co/embed-") + ".html";
    html::Document doc(http::getText(embed), embed);
    std::string packed = scriptAll(doc, {"eval", "p,a,c,k,e,d"});
    if (packed.empty()) return {};
    std::string un = unpacker::unpackAndCombine(packed);
    std::string file = substringBefore(afterOr(afterOr(un, "sources:["), "file\":\""), "\"");
    if (trim(file).empty()) {
        // variante HxFileExtractor di Kuronime
        file = substringBefore(afterOr(afterOr(un, "\"type\":\"video"), "\"file\":\""), "\"");
    }
    if (trim(file).empty()) return {};
    return {simpleVideo(file, title, "https://hxfile.co/")};
}

/**
 * Decodifica l'offuscamento "hunter": eval(function(h,u,n,t,e,r){...}("...",u,"alfabeto",t,e,r)).
 * Restituisce i byte prodotti (gia' UTF-8, come decodeURIComponent(escape(r))).
 */
std::string hunterDecode(const std::string& script) {
    auto p = script.find("}(\"");
    if (p == std::string::npos) return "";
    p += 3;
    auto e = script.find('"', p);
    if (e == std::string::npos) return "";
    std::string h = script.substr(p, e - p);
    // argomenti successivi: ,u,"n",t,e,r)
    std::string rest = script.substr(e + 1, 400);
    std::vector<std::string> args;
    std::string cur;
    bool inStr = false;
    for (char c : rest) {
        if (c == '"') {
            inStr = !inStr;
            continue;
        }
        if (!inStr && (c == ',' || c == ')')) {
            if (!trim(cur).empty()) args.push_back(trim(cur));
            cur.clear();
            if (c == ')') break;
            continue;
        }
        cur += c;
    }
    if (args.size() < 4) return "";
    std::string n = args[1];
    int t = std::atoi(args[2].c_str());
    int base = std::atoi(args[3].c_str());
    if (base <= 1 || base >= (int)n.size()) return "";
    char delim = n[base];
    std::string out;
    size_t i = 0;
    while (i < h.size()) {
        std::string s;
        while (i < h.size() && h[i] != delim) s += h[i++];
        i++;  // salta il separatore
        long long val = 0;
        for (char c : s) {
            auto idx = n.find(c);
            if (idx == std::string::npos) continue;
            val = val * base + (long long)idx;
        }
        out += (char)(val - t);
    }
    return out;
}

std::vector<Video> animeku(const std::string& url, const std::string& name) {
    html::Document doc(http::getText(url), url);
    std::string script = scriptWith(doc, {"decodeURIComponent"});
    if (script.empty()) return {};
    std::string decoded = hunterDecode(substringBefore(trim(script), "\n"));
    json srcs = parseJson(substringBefore(afterOr(decoded, "var srcs = "), ";"));
    std::vector<Video> out;
    for (auto& s : srcs) {
        std::string f = js(s, "file");
        if (!f.empty()) out.push_back(simpleVideo(f, js(s, "label") + " - " + name));
    }
    return out;
}

std::vector<Video> mixDrop(const std::string& url, const std::string& prefix, const std::vector<Video::Track>& subs = {}) {
    const std::string referer = "https://mixdrop.co/";
    const char* ua = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/129.0.0.0 Safari/537.36";
    html::Document doc(http::getText(url, {{"Referer", referer}, {"User-Agent", ua}}), url);
    std::string packed = scriptAll(doc, {"eval", "MDCore"});
    if (packed.empty()) return {};
    std::string un = unpacker::unpackAndCombine(packed);
    if (un.empty()) return {};
    std::string videoUrl = "https:" + substringBefore(substringAfter(un, "Core.wurl=\""), "\"");
    Video v = simpleVideo(videoUrl, prefix + "MixDrop", referer);
    v.userAgent = ua;
    std::string sub = substringBefore(afterOr(un, "Core.remotesub=\""), "\"");
    if (!trim(sub).empty()) v.subtitles.push_back({urlDecode(sub), "sub"});
    for (auto& s : subs) v.subtitles.push_back(s);
    return {v};
}

}  // namespace

namespace {

// =============================================================================================== base

struct Info {
    const char* id;
    const char* name;
    const char* url;
    const char* lang;
    bool nsfw;
    bool latest;
};

class AsiaSource : public Source {
  public:
    explicit AsiaSource(Info i) : info(i) {}
    std::string id() const override { return info.id; }
    std::string name() const override { return info.name; }
    std::string defaultBaseUrl() const override { return info.url; }
    std::string lang() const override { return info.lang; }
    bool nsfw() const override { return info.nsfw; }
    bool supportsLatest() const override { return info.latest; }

    Page latest(int page) override {
        if (!info.latest) return popular(page);
        return latestPage(page);
    }

  protected:
    Info info;

    virtual Page latestPage(int page) { return popular(page); }
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
        return http::request(method, url, mergeHeaders(baseHeaders(), extra), body, 30, follow);
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

    std::string post(const std::string& url, const std::string& body, const http::Headers& extra = {}) const {
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

    DocPtr postDoc(const std::string& url, const std::string& body, const http::Headers& extra = {}) const {
        return std::make_unique<html::Document>(post(url, body, extra), url);
    }

    /** Elenco generico: per ogni elemento "sel" una lambda riempie l'Anime (scartato se url o titolo vuoti). */
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

    static std::string statusOf(const std::string& s) {
        std::string t = lower(trim(s));
        if (t.empty()) return "";
        if (contains(t, "complet") || contains(t, "selesai") || contains(t, "finished") || contains(t, "ended") ||
            contains(t, "završeno") || contains(t, "完结") || contains(t, "(end)"))
            return "Completato";
        if (contains(t, "ongoing") || contains(t, "sedang") || contains(t, "airing") || contains(t, "emituje") ||
            contains(t, "连载") || contains(t, "連載") || contains(t, "on-going"))
            return "In corso";
        return "";
    }

    static std::vector<Video> ensure(std::vector<Video> v) {
        if (v.empty()) throw http::Error("Nessun video trovato");
        return v;
    }

    static void sortEpisodesDesc(std::vector<Episode>& eps) {
        std::stable_sort(eps.begin(), eps.end(), [](const Episode& a, const Episode& b) { return a.number > b.number; });
    }
};

}  // namespace

// ####################################################################################################
// ############################################ INDONESIANO ###########################################
// ####################################################################################################

namespace {

// ---------------------------------------------------------------------------------------- OtakuDesu
class OtakuDesu : public AsiaSource {
  public:
    OtakuDesu() : AsiaSource({"id.otakudesu", "OtakuDesu", "https://otakudesu.blog", "id", false, true}) {}

    Page popular(int page) override { return listing(baseUrl() + "/complete-anime/page/" + std::to_string(page)); }
    Page latestPage(int page) override { return listing(baseUrl() + "/ongoing-anime/page/" + std::to_string(page)); }

    Page search(const std::string& q, int page) override {
        if (trim(q).empty()) return popular(page);
        auto d = doc(baseUrl() + "/?s=" + http::urlEncode(q) + "&post_type=anime");
        if (!d->selectFirst(".col-anime")) {
            return list(*d, "#venkonten > div > div.venser > div > div > ul > li", [&](const html::Node& el, Anime& a) {
                html::Node link = el.selectFirst("h2 > a");
                a.url = link.attr("href");
                a.title = replaceAll(link.text(), " Subtitle Indonesia", "");
                a.thumbnail = el.selectFirst("img").attr("src");
            }, "a.next.page-numbers");
        }
        return list(*d, ".col-anime", [&](const html::Node& el, Anime& a) {
            html::Node link = el.selectFirst(".col-anime-title > a");
            a.url = link.attr("href");
            a.title = link.text();
            a.thumbnail = el.selectFirst(".col-anime-cover > img").attr("src");
        }, "a.next.page-numbers");
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details det;
        html::Node info = d->selectFirst("div.infozingle");
        auto getInfo = [&](const std::string& key, bool cut) -> std::string {
            for (auto& span : info.select("p > span")) {
                if (!contains(span.selectFirst("b").text(), key)) continue;
                std::string t = span.text();
                return trim(cut ? substringAfter(t, ":") : t);
            }
            return "";
        };
        det.title = getInfo("Judul", true);
        det.genre = getInfo("Genre", true);
        std::string st = getInfo("Status", true);
        det.status = st == "Ongoing" ? "In corso" : st == "Completed" ? "Completato" : "";
        det.author = getInfo("Studio", true);
        det.thumbnail = d->selectFirst("div.fotoanime img").attr("src");
        std::string desc;
        for (const char* k : {"Japanese", "Skor", "Total Episode"}) {
            std::string v = getInfo(k, false);
            if (!v.empty()) desc += v + "\n";
        }
        desc += "\n\nSynopsis:\n";
        for (auto& p : d->select("div.sinopc > p")) desc += p.text() + "\n\n";
        det.description = trim(desc);

        // "#venkonten > div.venser > div:nth-child(8) > ul > li"
        std::vector<html::Node> items;
        html::Node venser = d->selectFirst("#venkonten > div.venser");
        html::Node eighth = childAt(venser, 8);
        if (eighth && eighth.tag() == "div") {
            for (auto& ul : eighth.children())
                if (ul.tag() == "ul")
                    for (auto& li : ul.children())
                        if (li.tag() == "li") items.push_back(li);
        }
        if (items.empty()) {
            for (auto& li : d->select("div.episodelist ul > li"))
                if (contains(li.selectFirst("span > a").attr("href"), "/episode/")) items.push_back(li);
        }
        for (auto& li : items) {
            html::Node link = li.selectFirst("span > a");
            if (!link) continue;
            std::string text = link.text();
            Episode e;
            e.url = rel(link.attr("href"));
            e.number = toNumber(substringBefore(substringAfter(text, "Episode "), " "), 1);
            // replace(".+?(?=Episode)|\\sSubtitle.+", "")
            std::string name = text;
            auto ep = name.find("Episode");
            if (ep != std::string::npos && ep > 0) name = name.substr(ep);
            auto sp = name.find(" Subtitle");
            if (sp != std::string::npos) name = name.substr(0, sp);
            e.name = trim(name);
            det.episodes.push_back(e);
        }
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        auto d = doc(abs(episodeUrl));
        std::string script = scriptWith(*d, {"{action:"});
        if (script.empty()) throw http::Error("Script dei server non trovato");
        std::string nonceAction = substringBefore(substringAfter(script, "{action:\""), "\"");
        std::string action = substringBefore(substringAfter(script, "action:\""), "\"");
        const std::string ajax = baseUrl() + "/wp-admin/admin-ajax.php";
        std::string nonce = substringBefore(substringAfter(post(ajax, formBody({{"action", nonceAction}})), ":\""), "\"");

        std::vector<Video> out;
        for (auto& a : d->select("div.mirrorstream ul li > a")) {
            try {
                std::string decoded = base64Decode(a.attr("data-content"));
                if (decoded.size() < 2) continue;
                decoded = decoded.substr(1, decoded.size() - 2);
                auto parts = split(decoded, ",");
                if (parts.size() < 3) continue;
                auto val = [](const std::string& s) { return replaceAll(substringAfter(s, ":"), "\"", ""); };
                std::string id = val(parts[0]), mirror = val(parts[1]), quality = val(parts[2]);
                std::string resp = post(ajax, formBody({{"id", id}, {"i", mirror}, {"q", quality}, {"nonce", nonce}, {"action", action}}));
                std::string frag = base64Decode(substringBefore(substringAfter(resp, ":\""), "\""));
                html::Document fd(frag);
                std::string link = fd.selectFirst("iframe").attr("src");
                if (link.empty()) continue;
                append(out, embed(quality, link));
            } catch (const std::exception&) {
            }
        }
        preferFirst(out, "1080p");
        return ensure(out);
    }

  private:
    std::vector<Video> embed(const std::string& quality, const std::string& link) {
        if (contains(link, "filelions")) return streamWish(link, "FileLions - ");
        if (contains(link, "yourupload")) {
            std::string id = substringBefore(substringAfter(link, "id="), "&");
            return yourUpload("https://yourupload.com/embed/" + id, "YourUpload - " + quality);
        }
        if (contains(link, "desustream")) {
            html::Document d(fetch(link), link);
            std::string script = scriptWith(d, {"sources"});
            std::string u = substringBefore(substringAfter(substringAfter(script, "sources:[{"), "file':'"), "'");
            if (!startsWith(u, "http")) return {};
            return {simpleVideo(u, "DesuStream - " + quality)};
        }
        if (contains(link, "mp4upload")) {
            html::Document d(fetch(link), link);
            std::string script = scriptWith(d, {"player.src"});
            std::string u = substringBefore(substringAfter(script, "src: \""), "\"");
            if (!startsWith(u, "http")) return mp4upload(link, "");
            return {simpleVideo(u, "Mp4upload - " + quality, "https://www.mp4upload.com/")};
        }
        if (contains(link, "vidhide")) return vidHide(link, "VidHide - ");
        return {};
    }

    Page listing(const std::string& url) {
        auto d = doc(url);
        return list(*d, "div.detpost div.thumb > a", [&](const html::Node& el, Anime& a) {
            a.url = el.attr("href");
            a.thumbnail = el.selectFirst("img").attr("src");
            a.title = el.selectFirst("h2").text();
        }, "a.next.page-numbers");
    }
};

// ------------------------------------------------------------------- Samehadaku / Oploverz (tema "animposx")
class AnimposxSource : public AsiaSource {
  public:
    struct Cfg {
        Info info;
        std::string listPath;  // "daftar-anime-2" o "anime-list"
        bool samehadaku;
    };
    explicit AnimposxSource(Cfg c) : AsiaSource(c.info), cfg(std::move(c)) {}

    Page popular(int page) override { return listing(pageUrl(page) + "?order=popular", "div.relat > article"); }
    Page latestPage(int page) override {
        return listing(pageUrl(page) + (cfg.samehadaku ? "?order=update" : "?order=latest"), "div.relat > article");
    }

    Page search(const std::string& q, int page) override {
        std::string url;
        if (cfg.samehadaku) {
            url = baseUrl() + "/" + cfg.listPath + "/" + (page > 1 ? "page/" + std::to_string(page) + "/" : "") +
                  "?title=" + http::urlEncode(q);
        } else {
            url = pageUrl(page) + "?title=" + http::urlEncode(q);
        }
        auto d = doc(url);
        if (cfg.samehadaku && d->selectFirst("main.site-main.relat > article")) return parse(*d, "main.site-main.relat > article");
        return parse(*d, "div.relat > article");
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details det;
        html::Node detail = d->selectFirst("div.infox > div.spe");
        auto getInfo = [&](const std::string& key) -> std::string {
            for (auto& span : detail.select("span")) {
                bool has = false;
                for (auto& b : span.select("b"))
                    if (contains(b.text(), key)) has = true;
                if (has) return trim(substringAfter(span.text(), " "));
            }
            return "";
        };
        det.author = getInfo("Studio");
        if (cfg.samehadaku) {
            det.status = statusOf(getInfo("Status"));
            std::string title;
            if (auto h3 = d->selectFirst("h3.anim-detail")) {
                auto parts = split(h3.text(), "Detail Anime");
                if (parts.size() > 1) title = parts[1];
            }
            if (trim(title).empty()) {
                if (auto h2 = d->selectFirst("h2.entry-title[itemprop=partOfSeries]")) {
                    title = h2.text();
                    if (startsWith(title, "Sinopsis Anime") && endsWith(title, "Indo"))
                        title = title.substr(14, title.size() - 14 - 4);
                } else if (auto h1 = d->selectFirst("h1.entry-title")) {
                    title = removeSuffix(h1.text(), "Sub Indo");
                }
            }
            det.title = trim(title);
            det.thumbnail = d->selectFirst("div.infoanime.widget_senction > div.thumb > img").attr("src");
            if (det.thumbnail.empty())
                det.thumbnail = d->selectFirst("div.episodeinf > div.infoanime > div.areainfo > div.thumb > img").attr("src");
            html::Node p = d->selectFirst("div.entry-content.entry-content-single > p");
            det.description = p ? p.text() : d->selectFirst("div.desc > div.entry-content.entry-content-single").text();
            det.genre = joinText(d->select("div.genre-info a"));
            if (det.genre.empty()) {
                for (auto& span : detail.select("span")) {
                    if (!contains(span.selectFirst("b").text(), "Genre")) continue;
                    det.genre = joinText(span.select("a"));
                    if (det.genre.empty()) det.genre = trim(substringAfter(span.text(), ":"));
                    break;
                }
            }
        } else {
            html::Node second = childAt(d->selectFirst("div.alternati"), 2);
            det.status = statusOf(second.tag() == "span" ? second.text() : "");
            det.title = d->selectFirst("div.title > h1.entry-title").text();
            det.thumbnail = d->selectFirst("div.infoanime.widget_senction > div.thumb > img").attr("src");
            std::vector<std::string> paras;
            for (auto& p : d->select("div.entry-content.entry-content-single > p")) paras.push_back(p.text());
            det.description = joinStr(paras, "\n\n");
        }
        std::string sel = cfg.samehadaku ? "div.lstepsiode > ul > li" : "div.lstepsiode.listeps > ul.scrolling > li";
        for (auto& li : d->select(sel)) {
            html::Node ep = li.selectFirst("span.eps > a");
            html::Node nm = li.selectFirst("span.lchx > a");
            if (!ep || !nm) continue;
            Episode e;
            e.url = rel(ep.attr("href"));
            e.number = toNumber(ep.text(), 1);
            e.name = nm.text();
            det.episodes.push_back(e);
        }
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        http::Response r = req("GET", abs(episodeUrl));
        check(r);
        std::string pageUrl = r.finalUrl.empty() ? abs(episodeUrl) : r.finalUrl;
        html::Document d(r.body, pageUrl);
        std::string origin = http::originOf(pageUrl);
        std::vector<Video> out;
        std::string sel = cfg.samehadaku ? "#server > ul > li > div" : "#server > ul > li > div.east_player_option";
        for (auto& el : d.select(sel)) {
            try {
                std::string body = formBody({{"action", "player_ajax"},
                                             {"post", el.attr("data-post")},
                                             {"nume", el.attr("data-nume")},
                                             {"type", el.attr("data-type")}});
                http::Headers h;
                if (cfg.samehadaku) h.push_back({"User-Agent", MOBILE_UA});
                std::string resp = post(origin + "/wp-admin/admin-ajax.php", body, h);
                std::string link;
                if (cfg.samehadaku) {
                    link = srcAttr(resp.substr(0, 4096));
                } else {
                    html::Document fd(resp);
                    link = fd.selectFirst(".playeriframe").attr("src");
                }
                if (trim(link).empty()) continue;
                link = fixUrl(link);
                std::string server = el.selectFirst("span").text();
                append(out, cfg.samehadaku ? samehadakuEmbed(server, link) : oploverzEmbed(link));
            } catch (const std::exception&) {
            }
        }
        preferFirst(out, cfg.samehadaku ? "720p" : "720p");
        return ensure(out);
    }

  private:
    Cfg cfg;

    std::string pageUrl(int page) const { return baseUrl() + "/" + cfg.listPath + "/page/" + std::to_string(page) + "/"; }

    Page listing(const std::string& url, const std::string& sel) {
        auto d = doc(url);
        return parse(*d, sel);
    }

    Page parse(const html::Document& d, const std::string& sel) {
        Page p = list(d, sel, [&](const html::Node& el, Anime& a) {
            a.url = el.selectFirst(cfg.samehadaku ? "div > a" : "div.animposx > a").attr("href");
            a.title = el.selectFirst("div.title > h2").text();
            a.thumbnail = el.selectFirst("div.content-thumb > img").attr("src");
        });
        html::Node pag = d.selectFirst("div.pagination");
        if (pag) {
            html::Node first = childAt(pag, 1);
            std::string total = first.tag() == "span" ? substringAfterLast(trim(first.text()), " ") : "";
            std::string current = pag.selectFirst("span.page-numbers.current").text();
            int t = std::atoi(total.c_str()), c = std::atoi(current.c_str());
            p.hasNextPage = t > 0 && c > 0 && c < t;
        }
        return p;
    }

    std::vector<Video> samehadakuEmbed(const std::string& server, const std::string& link) {
        http::Headers vh = {{"User-Agent", MOBILE_UA}, {"Referer", link}};
        if (contains(link, "mega.nz")) return {};
        if (contains(link, ".mp4") || contains(link, ".webm") || contains(link, ".m3u8"))
            return {simpleVideo(link, server, link, {{"User-Agent", MOBILE_UA}})};
        if (contains(link, "filedon") || contains(link, "uservideo") || contains(link, "userdrive") || contains(link, "samevideo")) {
            html::Document d(http::getText(link, vh), link);
            json page = parseJson(d.selectFirst("div#app").attr("data-page"));
            std::string u = js(jget(page, "props"), "url");
            if (u.empty()) return {};
            return {simpleVideo(u, server, u, {{"User-Agent", MOBILE_UA}})};
        }
        if (contains(link, "krakenfiles")) {
            html::Document d(http::getText(link, vh), link);
            std::string u = d.selectFirst("source").attr("src");
            if (u.empty()) return {};
            u = replaceAll(fixUrl(u), "&amp;", "&");
            return {simpleVideo(u, server, u, {{"User-Agent", MOBILE_UA}})};
        }
        if (contains(link, "blogger")) return blogger(link, server);
        html::Document d(http::getText(link, vh), link);
        std::string u = d.selectFirst("video source").attr("src");
        if (u.empty()) return {};
        u = fixUrl(u, link);
        return {simpleVideo(u, server, u, {{"User-Agent", MOBILE_UA}})};
    }

    std::vector<Video> oploverzEmbed(const std::string& link) {
        if (!contains(link, "blogger")) return {};
        auto v = blogger(link, "");
        for (auto& x : v) {
            if (contains(x.title, "360p")) x.title = "Google - 360p";
            else if (contains(x.title, "720p")) x.title = "Google - 720p";
        }
        return v;
    }
};

// ---------------------------------------------------------------------------------------- Kuramanime
class Kuramanime : public AsiaSource {
  public:
    Kuramanime() : AsiaSource({"id.kuramanime", "Kuramanime", "https://v8.kuramanime.tel", "id", false, true}) {}

    Page popular(int page) override { return listing(baseUrl() + "/anime?page=" + std::to_string(page)); }
    Page latestPage(int page) override { return listing(baseUrl() + "/anime?order_by=updated&page=" + std::to_string(page)); }
    Page search(const std::string& q, int page) override {
        return listing(baseUrl() + "/anime?search=" + http::urlEncode(q) + "&page=" + std::to_string(page));
    }

    Details details(const std::string& url) override {
        http::Response r = req("GET", abs(url));
        check(r);
        std::string location = r.finalUrl.empty() ? abs(url) : r.finalUrl;
        html::Document d(r.body, location);
        Details det;
        det.thumbnail = d.selectFirst("div.anime__details__pic").attr("data-setbg");
        html::Node details = d.selectFirst("div.anime__details__text");
        det.title = replaceAll(details.selectFirst("div > h3").text(), "Judul: ", "");
        html::Node infos = details.selectFirst("div.anime__details__widget");
        std::vector<std::string> studios, genres;
        std::string status;
        for (auto& li : infos.select("li")) {
            std::string t = li.text();
            std::vector<std::string> links;
            for (auto& a : li.children())
                if (a.tag() == "a") links.push_back(trimChars(a.text(), ", "));
            if (contains(t, "Studio:")) studios.insert(studios.end(), links.begin(), links.end());
            if (contains(t, "Status:") && !links.empty()) status = links[0];
            if (contains(t, "Genre:") || contains(t, "Tema:") || contains(t, "Demografis:"))
                genres.insert(genres.end(), links.begin(), links.end());
        }
        det.author = joinStr(studios);
        det.genre = joinStr(genres);
        det.status = status == "Sedang Tayang" ? "In corso" : status == "Selesai Tayang" ? "Completato" : "";
        std::string desc = details.selectFirst("p#synopsisField").text();
        std::string alt = details.selectFirst("div.anime__details__title > span").text();
        if (!alt.empty()) desc += "\n\nAlternative names: " + alt + "\n";
        for (auto& li : infos.select("ul > li")) desc += "\n" + li.text();
        det.description = trim(desc);

        std::string content = d.selectFirst("a#episodeLists").attr("data-content");
        if (content.empty()) return det;
        html::Document nd(content, location);
        auto limits = nd.select("a.btn-secondary");
        if (limits.empty()) {
            for (auto& a : nd.select("a")) {
                if (contains(a.attr("href"), "batch")) continue;
                Episode e;
                e.url = rel(nd.absUrl(a, "href"));
                e.name = a.text();
                e.number = toNumber(digitsOnly(e.name), 1);
                det.episodes.push_back(e);
            }
            std::reverse(det.episodes.begin(), det.episodes.end());
        } else if (limits.size() >= 2) {
            int start = std::atoi(digitsOnly(limits[0].text()).c_str());
            int end = std::atoi(digitsOnly(limits[1].text()).c_str());
            std::string base = rel(substringBefore(location, "?"));
            for (int n = end; n >= start && n > 0; n--) {
                Episode e;
                e.name = "Ep " + std::to_string(n);
                e.number = n;
                e.url = base + "/episode/" + std::to_string(n);
                det.episodes.push_back(e);
            }
        }
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::string epUrl = abs(episodeUrl);
        auto d = doc(epUrl);
        std::string kps = d->selectFirst("[data-kps]").attr("data-kps");
        if (kps.empty()) throw http::Error("Dati del lettore non trovati");
        std::map<std::string, std::string> env = scriptData(kps);
        std::string csrf = d->selectFirst("meta[name=csrf-token]").attr("content");
        std::string authPath = env["MIX_PREFIX_AUTH_ROUTE_PARAM"] + env["MIX_AUTH_ROUTE_PARAM"];
        std::string tokenId = env["MIX_AUTH_KEY"] + ":" + env["MIX_AUTH_TOKEN"];
        std::string tokenParam = env["MIX_PAGE_TOKEN_KEY"], serverParam = env["MIX_STREAM_SERVER_KEY"];
        static const std::set<std::string> supported = {"kuramadrive", "kuramadrive-v2", "filelions", "filemoon", "mega",
                                                        "streamwish", "streamtape", "vidguard"};
        http::Headers h = {{"Referer", epUrl}, {"X-Requested-With", "XMLHttpRequest"}};
        std::vector<Video> out;
        for (auto& opt : d->select("select#changeServer > option")) {
            std::string server = opt.attr("value");
            std::string serverName = substringBefore(opt.text(), " (");
            if (!supported.count(server)) continue;
            try {
                http::Headers th = h;
                th.push_back({"X-CSRF-TOKEN", csrf});
                th.push_back({"X-Fuck-ID", tokenId});
                th.push_back({"X-Request-ID", randomAlnum(8)});
                th.push_back({"X-Request-Index", "0"});
                std::string hash = trimChars(trim(fetch(baseUrl() + "/" + authPath, th)), "\"");
                std::string newUrl = epUrl + (contains(epUrl, "?") ? "&" : "?") + http::urlEncode(tokenParam) + "=" +
                                     http::urlEncode(hash) + "&" + http::urlEncode(serverParam) + "=" + http::urlEncode(server);
                html::Document pd(fetch(newUrl, h), newUrl);
                std::string iframe = pd.selectFirst("div.video-content iframe").attr("src");
                if ((server == "filelions" || server == "streamwish") && !iframe.empty()) {
                    append(out, streamWish(iframe, "StreamWish - "));
                } else if (server == "filemoon" && !iframe.empty()) {
                    append(out, moon(iframe, baseUrl(), "Filemoon - "));
                } else if (server == "streamtape" && !iframe.empty()) {
                    append(out, streamtape(iframe, ""));
                } else if (server == "vidguard") {
                    // VidGuard richiede l'esecuzione di JavaScript (Rhino): non supportato
                } else {
                    for (auto& s : pd.select("video#player > source")) {
                        std::string src = s.attr("src");
                        if (src.empty()) continue;
                        out.push_back(simpleVideo(src, s.attr("size") + "p - " + serverName, baseUrl() + "/"));
                    }
                }
            } catch (const std::exception&) {
            }
        }
        preferFirst(out, "1080p");
        return ensure(out);
    }

  private:
    Page listing(const std::string& url) {
        auto d = doc(url);
        Page p = list(*d, "div.filter__gallery > a", [&](const html::Node& el, Anime& a) {
            a.url = el.attr("href");
            a.thumbnail = el.selectFirst("div.set-bg").attr("data-setbg");
            a.title = el.selectFirst("div > h5").text();
        });
        // "div.product__pagination > a:last-child:not([aria-disabled='true'])"
        auto ch = d->selectFirst("div.product__pagination").children();
        p.hasNextPage = !ch.empty() && ch.back().tag() == "a" && ch.back().attr("aria-disabled") != "true";
        return p;
    }

    /** window.process = { ... env: { KEY: 'valore', ... } ... } nello script /assets/js/<nome>.js */
    std::map<std::string, std::string> scriptData(const std::string& name) {
        std::map<std::string, std::string> env;
        std::string js = fetch(baseUrl() + "/assets/js/" + name + ".js");
        auto p = js.find("window.process");
        if (p == std::string::npos) return env;
        p = js.find("env:", p);
        if (p == std::string::npos) return env;
        p = js.find('{', p);
        auto e = js.find('}', p);
        if (p == std::string::npos || e == std::string::npos) return env;
        std::string content = js.substr(p + 1, e - p - 1);
        // (\w+):\s*['"]([^'"]+)['"]
        size_t i = 0;
        while (i < content.size()) {
            auto colon = content.find(':', i);
            if (colon == std::string::npos) break;
            size_t ks = colon;
            while (ks > 0 && (std::isalnum((unsigned char)content[ks - 1]) || content[ks - 1] == '_')) ks--;
            std::string key = content.substr(ks, colon - ks);
            size_t v = colon + 1;
            while (v < content.size() && std::isspace((unsigned char)content[v])) v++;
            if (v < content.size() && (content[v] == '\'' || content[v] == '"')) {
                auto ve = content.find_first_of("'\"", v + 1);
                if (ve == std::string::npos) break;
                if (!key.empty()) env[key] = content.substr(v + 1, ve - v - 1);
                i = ve + 1;
            } else {
                i = colon + 1;
            }
        }
        return env;
    }
};

// ---------------------------------------------------------------------------------------- Kuronime
class Kuronime : public AsiaSource {
  public:
    Kuronime() : AsiaSource({"id.kuronime", "Kuronime", "https://tv1.kuronime.vip", "id", false, true}) {}

    Page popular(int page) override { return listing(baseUrl() + "/anime/page/" + std::to_string(page), "div.pagination > a.next"); }
    Page latestPage(int page) override {
        return listing(baseUrl() + "/anime/?page=" + std::to_string(page) + "&status=ongoing&sub=&order=update", "div.pagination > a.next");
    }
    Page search(const std::string& q, int page) override {
        return listing(baseUrl() + "/page/" + std::to_string(page) + "/?s=" + http::urlEncode(q), "a.next.page-numbers");
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details det;
        auto lis = d->select("div.infodetail ul > li");
        auto li = [&](size_t i) { return i < lis.size() ? lis[i].text() : std::string(); };
        det.title = replaceAll(li(0), "Judul: ", "");
        det.genre = li(1);
        det.status = statusOf(replaceAll(li(2), "Status: ", ""));
        det.author = replaceAll(li(3), "Studio: ", "");
        det.description = "Synopsis: \n" + html::textOf(d->select("div.main-info > div.con > div.r > div > span > p"));
        det.thumbnail = d->selectFirst("div.con > div.l img").attr("src");
        for (auto& el : d->select("div.bixbox.bxcl > ul > li")) {
            html::Node a = el.selectFirst("a");
            if (!a) continue;
            Episode e;
            e.name = html::textOf(el.select("span.lchx"));
            std::string num = digitsOnly(e.name);
            e.number = num.empty() ? 1 : toNumber(num, 1);
            e.url = rel(a.attr("href"));
            det.episodes.push_back(e);
        }
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        auto d = doc(abs(episodeUrl));
        std::vector<Video> out;
        for (auto& opt : d->select("select.mirror > option[value]")) {
            try {
                std::string decoded;
                if (opt.attr("value").empty()) {
                    decoded = d->selectFirst("iframe").attr("data-src");
                } else {
                    html::Document fd(base64Decode(opt.attr("value")));
                    for (auto& f : fd.select("iframe[data-src]"))
                        if (!f.attr("data-src").empty()) {
                            decoded = f.attr("data-src");
                            break;
                        }
                }
                if (decoded.empty()) continue;
                decoded = fixUrl(decoded);
                std::string name = opt.text();
                if (contains(decoded, "animeku.org")) append(out, animeku(decoded, name));
                else if (contains(decoded, "mp4upload.com")) {
                    auto v = mp4upload(decoded, "");
                    for (auto& x : v) x.title += " - " + name;
                    append(out, v);
                } else if (contains(decoded, "yourupload.com"))
                    append(out, yourUpload(decoded, "Original - " + name));
                else if (contains(decoded, "streamlare.com"))
                    append(out, streamlare(decoded, "", "- " + name));
                else if (contains(decoded, "linkbox.to"))
                    append(out, linkbox(decoded, name));
            } catch (const std::exception&) {
            }
        }
        preferFirst(out, "1080");
        return ensure(out);
    }

  private:
    Page listing(const std::string& url, const std::string& next) {
        auto d = doc(url);
        return list(*d, "div.listupd > article", [&](const html::Node& el, Anime& a) {
            a.url = el.selectFirst("div > a").attr("href");
            html::Node img = el.selectFirst("div > a > div.limit > img");
            std::string src = img.attr("src");
            a.thumbnail = startsWith(src, "https:") ? src : img.attr("data-src");
            a.title = html::textOf(el.select("div > a > div.tt > h4"));
        }, next);
    }
};

// ---------------------------------------------------------------------------------------- NeoNime
class NeoNime : public AsiaSource {
  public:
    NeoNime() : AsiaSource({"id.neonime", "NeoNime", "https://neonime.ink", "id", false, true}) {}

    Page popular(int page) override {
        auto d = doc(baseUrl() + "/tvshows/page/" + std::to_string(page));
        return list(*d, "div.items > div.item", [&](const html::Node& el, Anime& a) {
            a.url = el.selectFirst("a").attr("href");
            a.thumbnail = el.selectFirst("a > div.image > img").attr("data-src");
            a.title = html::textOf(el.select("div.fixyear > div > h2"));
        }, "div.respo_pag > div.pag_b > a");
    }

    // L'originale apre la pagina di ogni episodio per trovare la serie: qui si usa la pagina dell'episodio come
    // indirizzo e la serie viene risolta in details().
    Page latestPage(int page) override {
        auto d = doc(baseUrl() + "/episode/page/" + std::to_string(page));
        return list(*d, "table > tbody > tr", [&](const html::Node& el, Anime& a) {
            html::Node link = el.selectFirst("td.bb > a");
            a.url = link.attr("href");
            std::string t = link.text();
            auto p = t.find(" Episode");
            a.title = p == std::string::npos ? t : t.substr(0, p);
            html::Node img = el.selectFirst("img");
            a.thumbnail = img.hasAttr("data-src") ? img.attr("data-src") : img.attr("src");
        }, "div.respo_pag > div.pag_b > a");
    }

    Page search(const std::string& q, int) override {
        auto d = doc(baseUrl() + "/list-anime/");
        Page p;
        std::string ql = lower(trim(q));
        std::set<std::string> seen;
        for (auto& a : d->select("div.letter-section > ul > li > a")) {
            std::string href = a.attr("href");
            if (!contains(href, "/tvshows")) continue;
            if (!ql.empty() && !contains(lower(a.text()), ql)) continue;
            Anime an;
            an.url = rel(href);
            an.title = a.text();
            if (seen.insert(an.url).second) p.animes.push_back(an);
        }
        return p;
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        if (contains(url, "/episode/")) {
            std::string series = d->selectFirst("#fixar > div.imagen > a").attr("href");
            if (!series.empty()) d = doc(abs(series));
        }
        Details det;
        html::Node info = d->selectFirst("#info");
        det.title = html::textOf(childAt(info, 2).select("span"));
        det.thumbnail = d->selectFirst("div.imagen > img").attr("data-src");
        det.status = statusOf(html::textOf(childAt(info, 13).select("span")));
        std::vector<std::string> genres;
        for (auto& a : childAt(info, 3).select("span > a")) genres.push_back(a.text());
        det.genre = joinStr(genres);
        det.description = html::textOf(d->select("#info > div.contenidotv"));
        for (auto& li : d->select("ul.episodios > li")) {
            html::Node a = li.selectFirst("div.episodiotitle > a");
            if (!a) continue;
            Episode e;
            e.url = rel(a.attr("href"));
            e.name = a.text();
            std::string n = digitsOnly(e.name);
            e.number = n.empty() ? 1 : toNumber(n, 1);
            det.episodes.push_back(e);
        }
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        auto d = doc(abs(episodeUrl));
        std::vector<Video> out;
        for (auto& el : d->select("div.player2 > div.embed2 > div")) {
            html::Node iframe = el.selectFirst("iframe");
            if (!iframe) continue;
            std::string link = iframe.attr("data-src");
            if (!startsWith(link, "http")) link = "https:" + link;
            std::string name = el.text();
            try {
                if (contains(link, "linkbox.to")) append(out, linkbox(link, name));
                else if (contains(link, "ok.ru")) append(out, okru(link, ""));
                else if (contains(link, "blogger.com")) append(out, blogger(link, name));
                else if (contains(link, "yourupload.com")) append(out, yourUpload(link, "Original - " + name));
                // gdriveplayer (neonime.fun) richiede due livelli di unpack + AES: sito non piu' attivo, non portato
            } catch (const std::exception&) {
            }
        }
        preferFirst(out, "1080");
        return ensure(out);
    }
};

// ---------------------------------------------------------------------------------------- NimeGami
class NimeGami : public AsiaSource {
  public:
    NimeGami() : AsiaSource({"id.nimegami", "NimeGami", "https://nimegami.id", "id", false, true}) {}

    Page popular(int) override {
        auto d = doc(baseUrl());
        return list(*d, "div.wrapper-2-a > article > a", [&](const html::Node& el, Anime& a) {
            a.url = el.attr("href");
            a.thumbnail = el.selectFirst("img").attr("data-lazy-src");
            a.title = el.selectFirst("div.title-post2").text();
        });
    }

    Page latestPage(int page) override { return listing(baseUrl() + "/page/" + std::to_string(page), "div.post article"); }

    Page search(const std::string& q, int page) override {
        return listing(baseUrl() + "/page/" + std::to_string(page) + "/?s=" + http::urlEncode(q) + "&post_type=post",
                       "div.archive > div > article");
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details det;
        det.thumbnail = d->selectFirst("div.coverthumbnail img").attr("src");
        html::Node infos = d->selectFirst("div.info2 > table > tbody");
        if (!infos) infos = d->selectFirst("div.info2 > table");
        auto getInfo = [&](const std::string& key) -> std::string {
            for (auto& tr : infos.select("tr"))
                if (contains(tr.selectFirst("td.tablex").text(), key)) return substringAfter(tr.text(), ": ");
            return "";
        };
        det.title = getInfo("Judul:");
        if (det.title.empty()) det.title = d->selectFirst("h2[itemprop=name]").text();
        det.genre = getInfo("Kategori");
        det.author = getInfo("Studio");
        std::string h1 = d->selectFirst("h1.title").text();
        det.status = contains(h1, "(On-Going)") ? "In corso"
                     : (contains(h1, "(End)") || contains(h1, "(Movie)")) ? "Completato"
                                                                          : "";
        std::string desc;
        for (auto& p : d->select("div#Sinopsis p")) desc += p.text() + "\n";
        for (auto& tr : infos.select("tr")) {
            std::string t = tr.text();
            if (t == "Judul:" || t == "Kategori" || t == "Studio") continue;
            desc += "\n" + t;
        }
        det.description = trim(desc);
        for (auto& li : d->select("div.list_eps_stream > li.select-eps")) {
            std::string num = substringAfterLast(li.attr("id"), "_");
            Episode e;
            e.number = toNumber(num, 1);
            e.name = "Episode " + num;
            e.url = numStr(e.number) + "|" + li.attr("data");  // numero necessario per bunga.nimegami
            det.episodes.push_back(e);
        }
        std::reverse(det.episodes.begin(), det.episodes.end());
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        int index = std::atoi(substringBefore(episodeUrl, "|").c_str()) - 1;
        json qualities = parseJson(base64Decode(substringAfter(episodeUrl, "|")));
        std::vector<Video> out;
        for (auto& q : qualities) {
            std::string quality = js(q, "format");
            for (auto& u : jget(q, "url")) {
                if (!u.is_string()) continue;
                try {
                    append(out, extract(u.get<std::string>(), quality, index));
                } catch (const std::exception&) {
                }
            }
        }
        return ensure(out);
    }

  private:
    Page listing(const std::string& url, const std::string& sel) {
        auto d = doc(url);
        Page p = list(*d, sel, [&](const html::Node& el, Anime& a) {
            html::Node link = el.selectFirst("h2 > a");
            a.url = link.attr("href");
            a.title = link.text();
            a.thumbnail = substringBefore(el.selectFirst("img").attr("srcset"), " ");
        });
        for (auto& a : d->select("ul.pagination > li > a"))
            if (contains(a.text(), "Next")) p.hasNextPage = true;
        return p;
    }

    std::vector<Video> extract(const std::string& url, const std::string& quality, int index) {
        (void)index;
        if (contains(url, "video.nimegami.id")) {
            std::string real = base64Decode(substringBefore(substringAfter(url, "url="), "&"));
            return extract(real, quality, index);
        }
        if (contains(url, "berkasdrive") || contains(url, "drive.nimegami")) {
            html::Document d(fetch(url), url);
            std::string src = d.selectFirst("source[src]").attr("src");
            if (src.empty()) return {};
            return {simpleVideo(src, "Berkasdrive - " + quality, baseUrl() + "/")};
        }
        if (contains(url, "hxfile.co")) return hxfile(url, "HXFile - " + quality);
        // bunga.nimegami (uservideo): lo script del lettore e' offuscato con obfuscator.io (Synchrony): non supportato
        return {};
    }
};

}  // namespace

// ####################################################################################################
// ############################################### CINESE #############################################
// ####################################################################################################

namespace {

// ---------------------------------------------------------------------------------------- Anime1.me
class Anime1 : public AsiaSource {
  public:
    Anime1() : AsiaSource({"zh.anime1", "Anime1.me", "https://anime1.me", "zh", false, true}) {}

    static constexpr const char* FIX_COVER = "https://sta.anicdn.com/playerImg/8.jpg";

    Page popular(int page) override { return latestPage(page); }

    Page latestPage(int page) override {
        json items;
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (!data.is_array()) {
                std::string body = fetch("https://d1zquzjgwo9yb.cloudfront.net/?_=" + std::to_string((long long)std::time(nullptr)) + "000");
                data = parseJson(body);
                if (!data.is_array()) throw http::Error("Elenco di Anime1 non valido");
            }
            items = data;
        }
        const size_t PAGE = 20;
        size_t from = (size_t)std::max(0, page - 1) * PAGE;
        size_t to = std::min(items.size(), from + PAGE);
        Page p;
        for (size_t i = from; i < to; i++) {
            const json& arr = items[i];
            if (!arr.is_array() || arr.size() < 2) continue;
            auto content = [&](size_t k) -> std::string {
                if (k >= arr.size()) return "";
                if (arr[k].is_string()) return arr[k].get<std::string>();
                if (arr[k].is_number_integer()) return std::to_string(arr[k].get<long long>());
                return "";
            };
            Anime a;
            std::string id = content(0);
            a.url = "?cat=" + id;
            a.title = content(1);
            if (id == "0" || contains(a.title, "</a>")) {
                html::Document d(a.title, baseUrl());
                html::Node link = d.selectFirst("a");
                if (link) a.url = link.attr("href");
                a.title = d.root().text();
            }
            a.url = rel(a.url);
            a.title = trim(a.title);
            a.thumbnail = FIX_COVER;
            if (!a.title.empty()) p.animes.push_back(a);
        }
        p.hasNextPage = to - from == PAGE && to < items.size();
        return p;
    }

    Page search(const std::string& q, int page) override {
        std::string url = baseUrl() + (page > 1 ? "/page/" + std::to_string(page) : std::string("")) + "/?s=" + http::urlEncode(q);
        auto d = doc(url);
        Page p = list(*d, "article.post .entry-title a", [&](const html::Node& el, Anime& a) {
            a.url = el.attr("href");
            a.title = ownText(el);
            if (trim(a.title).empty()) a.title = el.text();
            a.thumbnail = FIX_COVER;
        });
        p.hasNextPage = d->selectFirst(".nav-previous").valid();
        return p;
    }

    Details details(const std::string& url) override {
        std::string pageUrl = abs(url);
        auto d = doc(pageUrl);
        Details det;
        det.thumbnail = FIX_COVER;
        det.title = d->selectFirst("h1.page-title").text();
        if (det.title.empty()) det.title = d->selectFirst(".entry-title").text();
        int guard = 0;
        std::string current = pageUrl;
        while (d && guard++ < 40) {
            for (auto& art : d->select("article.post")) {
                Episode e;
                e.name = html::textOf(art.select(".entry-title"));
                std::string href = art.selectFirst(".entry-title a").attr("href");
                e.url = rel(href.empty() ? current : href);
                // "[12]" -> 12
                std::string inside = substringBefore(substringAfterLast(e.name, "["), "]");
                e.number = toNumber(inside, -1);
                det.episodes.push_back(e);
            }
            std::string prev = d->selectFirst(".nav-previous a").attr("href");
            if (trim(prev).empty()) break;
            current = prev;
            try {
                d = doc(prev);
            } catch (const std::exception&) {
                break;
            }
        }
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        auto d = doc(abs(episodeUrl));
        std::string apireq = d->selectFirst("video").attr("data-apireq");
        if (apireq.empty()) throw http::Error("Video non trovato");
        http::Response r = http::request("POST", "https://v.anime1.me/api",
                                         {{"Content-Type", "application/x-www-form-urlencoded"}, {"Referer", baseUrl() + "/"}},
                                         "d=" + apireq);
        check(r);
        std::string cookie = cookiesOf(r);
        json j = parseJson(r.body);
        std::vector<Video> out;
        for (auto& s : jget(j, "s")) {
            std::string src = js(s, "src");
            if (src.empty()) continue;
            std::string videoUrl = startsWith(src, "//") ? "https:" + src : src;
            Video v = simpleVideo(videoUrl, "Anime1 - " + js(s, "type"), baseUrl() + "/");
            v.cookie = cookie;
            out.push_back(v);
        }
        return ensure(out);
    }

  private:
    std::mutex mtx;
    json data;
};

// ---------------------------------------------------------------------------------------- Xfani (稀饭动漫)
class Xfani : public AsiaSource {
  public:
    Xfani() : AsiaSource({"zh.xfani", "稀饭动漫", "https://anime.xifanacg.com", "zh", false, true}) {}

    Page popular(int page) override { return vodList(page, "hits"); }
    Page latestPage(int page) override { return vodList(page, ""); }

    Page search(const std::string& q, int page) override {
        if (trim(q).empty()) return popular(page);
        json j = parseJson(fetch(baseUrl() + "/index.php/ajax/suggest?mid=1&wd=" + http::urlEncode(q) + "&page=" +
                                 std::to_string(page) + "&limit=40"));
        Page p;
        for (auto& it : jget(j, "list")) {
            Anime a;
            a.url = "/bangumi/" + js(it, "id") + ".html";
            a.title = js(it, "name");
            a.thumbnail = abs(js(it, "pic"));
            if (!a.title.empty()) p.animes.push_back(a);
        }
        p.hasNextPage = std::atoi(js(j, "page").c_str()) < std::atoi(js(j, "pagecount").c_str());
        return p;
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details det;
        det.description = html::textOf(d->select("#height_limit.text"));
        det.title = html::textOf(d->select(".slide-info-title"));
        html::Node img = d->selectFirst(".detail-pic img");
        det.thumbnail = img.hasAttr("data-src") ? img.attr("data-src") : img.attr("src");
        auto info = [&](const std::string& key) {
            std::string t = firstContaining(d->select(".slide-info"), key).text();
            return removeSuffix(trim(removePrefix(t, key)), ",");
        };
        det.author = info("导演 :");
        det.genre = replaceAll(info("类型 :"), ",", ", ");
        auto lists = d->select("ul.anthology-list-play.size");
        if (lists.empty()) return det;
        for (auto& a : lists[0].select("li > a")) {
            Episode e;
            e.name = a.text();
            e.url = a.attr("href");
            e.number = firstNumber(e.name);
            det.episodes.push_back(e);
        }
        std::reverse(det.episodes.begin(), det.episodes.end());
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::string pageUrl = abs(episodeUrl);
        auto d = doc(pageUrl);
        std::string currentPath = substringBefore(http::pathOf(pageUrl), "?");
        std::string lastSeg = substringAfterLast(currentPath, "/");
        std::vector<std::vector<html::Node>> all;
        for (auto& box : d->select(".player-anthology .anthology-list .anthology-list-box"))
            all.push_back(box.select(".anthology-list-play li a"));
        std::string currentName;
        for (auto& els : all) {
            for (auto& a : els)
                if (a.attr("href") == currentPath) {
                    currentName = a.selectFirst("span").text();
                    break;
                }
            if (!currentName.empty()) break;
        }
        int target = currentName.empty() ? -1 : (int)firstNumber(currentName, -1);
        std::vector<std::string> names;
        for (auto& a : d->select(".anthology-tab .swiper-wrapper a")) names.push_back(trim(ownText(a)));

        std::vector<Video> out;
        for (size_t i = 0; i < all.size() && i < names.size(); i++) {
            html::Node found;
            for (auto& a : all[i])
                if (!currentName.empty() && a.selectFirst("span").text() == currentName) { found = a; break; }
            if (!found)
                for (auto& a : all[i])
                    if (target >= 0 && (int)firstNumber(a.selectFirst("span").text(), -2) == target) { found = a; break; }
            if (!found)
                for (auto& a : all[i])
                    if (!lastSeg.empty() && endsWith(a.attr("href"), lastSeg)) { found = a; break; }
            if (!found) continue;
            std::string href = found.attr("href");
            std::string title = names[i] + "-" + found.selectFirst("span").text();
            try {
                std::string videoUrl;
                if (endsWith(href, currentPath)) {
                    videoUrl = macCmsPlayerUrl(*d);
                } else {
                    auto od = doc(abs(href));
                    videoUrl = macCmsPlayerUrl(*od);
                }
                if (!startsWith(videoUrl, "http")) continue;
                Video v = simpleVideo(videoUrl, title, baseUrl() + "/");
                if (endsWith(href, currentPath)) out.insert(out.begin(), v);
                else out.push_back(v);
            } catch (const std::exception&) {
            }
        }
        if (out.empty()) {
            std::string u = macCmsPlayerUrl(*d);
            if (startsWith(u, "http")) out.push_back(simpleVideo(u, "稀饭动漫", baseUrl() + "/"));
        }
        return ensure(out);
    }

  private:
    Page vodList(int page, const std::string& by) {
        long long t = (long long)std::time(nullptr);
        std::string key = crypto::toHex(crypto::md5("DS" + std::to_string(t) + "DCC147D11943AF75"));
        std::vector<std::pair<std::string, std::string>> parts = {{"page", std::to_string(page)}, {"time", std::to_string(t)}, {"key", key}};
        if (!by.empty()) parts.push_back({"by", by});
        parts.push_back({"type", "1"});
        const std::string boundary = "----AnikkuNXBoundary7MA4YWxkTrZu0gW";
        std::string body;
        for (auto& p : parts)
            body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + p.first + "\"\r\n\r\n" + p.second + "\r\n";
        body += "--" + boundary + "--\r\n";
        http::Response r = req("POST", baseUrl() + "/index.php/api/vod", {{"Content-Type", "multipart/form-data; boundary=" + boundary}}, body);
        check(r);
        json j = parseJson(r.body);
        Page p;
        for (auto& it : jget(j, "list")) {
            Anime a;
            a.url = "/bangumi/" + js(it, "vod_id") + ".html";
            a.title = js(it, "vod_name");
            std::string thumb = js(it, "vod_pic_thumb");
            a.thumbnail = abs(thumb.empty() ? js(it, "vod_pic") : thumb);
            if (!a.title.empty()) p.animes.push_back(a);
        }
        int pg = std::atoi(js(j, "page").c_str()), limit = std::atoi(js(j, "limit").c_str()), total = std::atoi(js(j, "total").c_str());
        p.hasNextPage = !p.animes.empty() && pg * limit < total;
        return p;
    }
};

// ---------------------------------------------------------------------------------------- Cycity (次元城动漫)
class Cycity : public AsiaSource {
  public:
    Cycity() : AsiaSource({"zh.cycity", "次元城动漫", "https://www.cycani.org", "zh", false, true}) {}

    Page popular(int page) override { return vodList("hits", page); }
    Page latestPage(int page) override { return vodList("time", page); }

    Page search(const std::string& q, int page) override {
        if (trim(q).empty()) return popular(page);
        auto d = doc(baseUrl() + "/search/wd/" + http::urlEncode(q) + "/page/" + std::to_string(page) + ".html");
        if (d->selectFirst(".ft6")) throw http::Error("Il sito richiede un codice di verifica (captcha)");
        Page p;
        for (auto& item : d->select(".search-list")) {
            Anime a;
            a.url = rel(item.selectFirst(".detail-info > a").attr("href"));
            html::Node img = item.selectFirst(".detail-pic img[data-src]");
            a.title = img.attr("alt");
            a.thumbnail = img.attr("data-src");
            if (!a.url.empty() && !a.title.empty()) p.animes.push_back(a);
        }
        p.hasNextPage = hasNext(*d);
        return p;
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details det;
        auto infos = d->select(".slide-info");
        std::string remark = d->selectFirst(".slide-info-remarks").text();
        det.title = d->selectFirst(".slide-info-title").text();
        if (infos.size() > 1) det.author = infos[1].selectFirst("a").text();
        if (infos.size() > 3) det.genre = joinText(infos[3].select("a"));
        det.description = d->selectFirst("#height_limit.text").text();
        html::Node img = d->selectFirst(".detail-pic img");
        det.thumbnail = img.hasAttr("data-src") ? img.attr("data-src") : img.attr("src");
        det.status = contains(remark, "|") ? "In corso" : remark == "已完结" ? "Completato" : "";
        std::vector<std::string> hosts;
        for (auto& a : d->select(".anthology-tab a")) {
            std::string span = a.selectFirst("span").text();
            std::string t = a.text();
            hosts.push_back(trim(span.empty() ? t : substringBefore(t, span)));
        }
        auto lists = d->select(".anthology-list-play");
        for (size_t i = 0; i < lists.size(); i++) {
            for (auto& a : lists[i].select("a")) {
                Episode e;
                e.url = rel(d->absUrl(a, "href"));
                std::string host = i < hosts.size() ? hosts[i] : "";
                e.name = (lists.size() > 1 && !host.empty() ? "[" + host + "] " : "") + a.text();
                e.number = firstNumber(a.text());
                det.episodes.push_back(e);
            }
        }
        std::reverse(det.episodes.begin(), det.episodes.end());
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::string page = fetch(abs(episodeUrl));
        // \bplayer_aaaa[^<>]*"url": ?"(.*?)"
        auto p = page.find("player_aaaa");
        if (p == std::string::npos) throw http::Error("Lettore non trovato");
        p = page.find("\"url\":", p);
        if (p == std::string::npos) throw http::Error("Lettore non trovato");
        p += 6;
        while (p < page.size() && page[p] == ' ') p++;
        if (p >= page.size() || page[p] != '"') throw http::Error("Lettore non trovato");
        std::string origin = page.substr(p + 1, page.find('"', p + 1) - p - 1);
        std::string videoId = urlDecode(base64Decode(origin));

        std::string body = fetch("https://player.cycanime.com/?url=" + videoId, {{"Referer", baseUrl() + "/"}});
        // now_(\w+) (due occorrenze)
        std::vector<std::string> keys;
        size_t k = 0;
        while ((k = body.find("now_", k)) != std::string::npos) {
            size_t s = k + 4, e = s;
            while (e < body.size() && (std::isalnum((unsigned char)body[e]) || body[e] == '_')) e++;
            if (e > s) keys.push_back(body.substr(s, e - s));
            k = e;
        }
        if (keys.size() != 2) throw http::Error("视频URL解析失败！");
        // "url": "([^:]+?)"
        std::string enc;
        size_t u = 0;
        while ((u = body.find("\"url\": \"", u)) != std::string::npos) {
            u += 8;
            auto e = body.find('"', u);
            if (e == std::string::npos) break;
            std::string cand = body.substr(u, e - u);
            if (!cand.empty() && cand.find(':') == std::string::npos) {
                enc = cand;
                break;
            }
        }
        if (enc.empty()) throw http::Error("视频URL解析失败！");
        std::string k1 = keys[0], k2 = keys[1];
        std::string prefix(k2.size(), ' ');
        for (size_t i = 0; i < k1.size() && i < k2.size(); i++) {
            int idx = k1[i] - '0';
            if (idx >= 0 && idx < (int)prefix.size()) prefix[idx] = k2[i];
        }
        std::string a = crypto::toHex(crypto::md5(prefix + "YLwJVbXw77pk2eOrAnFdBo2c3mWkLtodMni2wk81GCnP94ZltW"));
        std::string videoUrl = crypto::aesCbcDecrypt(base64Decode(enc), a.substr(16), a.substr(0, 16));
        if (!startsWith(videoUrl, "http")) throw http::Error("视频URL解析失败！");
        return {simpleVideo(videoUrl, "默认", baseUrl() + "/")};
    }

  private:
    static bool hasNext(const html::Document& d) {
        html::Node tip = d.selectFirst(".page-tip");
        if (!tip) return false;
        auto pages = split(substringBefore(substringAfter(tip.text(), "当前"), "页"), "/");
        return pages.size() >= 2 && trim(pages[0]) != trim(pages[1]);
    }

    Page vodList(const std::string& by, int page) {
        std::string url = baseUrl() + "/show/20/by/" + by + "/page/" + std::to_string(page) + ".html";
        http::Response r = req("POST", url, {{"Content-Type", "application/x-www-form-urlencoded"}}, "");
        check(r);
        html::Document d(r.body, url);
        Page p;
        for (auto& it : d.select(".public-list-box")) {
            html::Node item = it.selectFirst(".public-list-button a");
            if (!item) continue;
            Anime a;
            a.thumbnail = it.selectFirst("img").attr("data-src");
            a.title = item.text();
            a.url = rel(d.absUrl(item, "href"));
            if (!a.title.empty()) p.animes.push_back(a);
        }
        p.hasNextPage = hasNext(d);
        return p;
    }
};

// ---------------------------------------------------------------------------------------- Iyf (爱壹帆)
class Iyf : public AsiaSource {
  public:
    Iyf() : AsiaSource({"zh.iyf", "爱壹帆", "https://www.iyf.tv", "zh", false, true}) {}

    Page popular(int page) override { return filterSearch(page, "2"); }
    Page latestPage(int page) override { return filterSearch(page, "1"); }

    Page search(const std::string& q, int page) override {
        if (q.empty()) return popular(page);
        Params params = {{"tags", q}, {"orderby", "4"}, {"page", std::to_string(page)}, {"size", "32"}, {"desc", "1"}, {"isserial", "-1"}};
        std::string url = "https://rankv21.iyf.tv/v3/list/briefsearch?" + encode(params);
        std::string body = "tags=" + http::urlEncode(q) + "&vv=" + sign(params) + "&pub=" + config().first;
        return parseResult(post(url, body, headers()));
    }

    Details details(const std::string& url) override {
        std::string vid = videoId(url);
        Params params = {{"cinema", "1"}, {"device", "1"}, {"player", "CkPlayer"}, {"tech", "HLS"}, {"country", "HU"},
                         {"lang", "cns"}, {"v", "1"}, {"id", vid}, {"region", "SG"}};
        json j = api("https://m10.iyf.tv/v3/video/detail", params);
        const json& info = jget(jget(j, "data"), "info");
        if (!info.is_array() || info.empty()) throw http::Error("Dettagli non disponibili");
        const json& dt = info[0];
        Details det;
        det.title = js(dt, "title");
        det.thumbnail = js(dt, "imgPath");
        std::vector<std::string> dirs, stars;
        for (auto& x : jget(dt, "directors"))
            if (x.is_string()) dirs.push_back(x.get<std::string>());
        det.author = joinStr(dirs);
        det.genre = joinStr(split(js(dt, "keyWord"), ","));
        det.status = js(dt, "serialCount") == "0" ? "Completato" : "In corso";
        det.description = "添加：" + js(dt, "add_date") + "\n更新：" + js(dt, "updateweekly") + "\n简介：" + js(dt, "contxt");

        std::string cid = url.find('#') != std::string::npos ? substringAfter(url, "#") : "";
        Params pl = {{"cinema", "1"}, {"vid", vid}, {"lsk", "1"}, {"taxis", "0"}, {"cid", cid}};
        json pj = api("https://m10.iyf.tv/v3/video/languagesplaylist", pl);
        const json& pinfo = jget(jget(pj, "data"), "info");
        if (pinfo.is_array() && !pinfo.empty()) {
            int n = 0;
            for (auto& it : jget(pinfo[0], "playList")) {
                Episode e;
                e.url = "/" + js(it, "key");
                e.name = js(it, "name");
                e.number = ++n;
                det.episodes.push_back(e);
            }
        }
        std::reverse(det.episodes.begin(), det.episodes.end());
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::string vid = videoId(episodeUrl);
        Params params = {{"cinema", "1"}, {"id", vid}, {"a", "0"}, {"lang", "none"}, {"usersign", "1"},
                         {"region", "SG"}, {"device", "1"}, {"isMasterSupport", "1"}};
        json j = api("https://m10.iyf.tv/v3/video/play", params);
        const json& info = jget(jget(j, "data"), "info");
        std::vector<Video> out;
        if (info.is_array() && !info.empty()) {
            for (auto& c : jget(info[0], "clarity")) {
                std::string rtmp = js(jget(c, "path"), "rtmp");
                if (rtmp.empty()) continue;
                // removeAllQueryParameters("us")
                std::string base = substringBefore(rtmp, "?");
                std::string query = rtmp.find('?') != std::string::npos ? substringAfter(rtmp, "?") : "";
                std::string kept;
                for (auto& kv : split(query, "&")) {
                    if (kv.empty() || substringBefore(kv, "=") == "us") continue;
                    kept += (kept.empty() ? "" : "&") + kv;
                }
                std::string u = base + (kept.empty() ? "" : "?" + kept);
                out.push_back(simpleVideo(u, js(c, "title"), baseUrl() + "/", {{"User-Agent", DESKTOP_UA}}));
            }
        }
        sortByQuality(out, "");
        return ensure(out);
    }

  protected:
    http::Headers baseHeaders() const override { return {{"Referer", baseUrl() + "/"}, {"User-Agent", DESKTOP_UA}}; }

  private:
    using Params = std::vector<std::pair<std::string, std::string>>;
    std::mutex mtx;
    std::string pub, priv;

    static http::Headers headers() { return {{"User-Agent", DESKTOP_UA}}; }

    static std::string videoId(const std::string& url) {
        std::string path = substringBefore(substringBefore(url, "#"), "?");
        while (!path.empty() && path.back() == '/') path.pop_back();
        return substringAfterLast(path, "/");
    }

    std::pair<std::string, std::string> config() {
        std::lock_guard<std::mutex> lock(mtx);
        if (pub.empty()) {
            html::Document d(fetch(baseUrl() + "/list"), baseUrl() + "/list");
            std::string script = scriptWith(d, {"injectJson"});
            std::string s = substringAfter(script, "\"pConfig\":");
            auto e = s.find('}');
            json pc = parseJson(e == std::string::npos ? "" : s.substr(0, e + 1));
            pub = js(pc, "publicKey");
            const json& pk = jget(pc, "privateKey");
            if (pk.is_array() && !pk.empty() && pk[0].is_string()) priv = pk[0].get<std::string>();
            if (pub.empty()) throw http::Error("Configurazione di Iyf non trovata");
        }
        return {pub, priv};
    }

    static std::string encode(const Params& p) {
        std::string out;
        for (auto& kv : p) out += (out.empty() ? "" : "&") + http::urlEncode(kv.first) + "=" + http::urlEncode(kv.second);
        return out;
    }

    std::string sign(const Params& p) {
        auto cfg = config();
        std::string decoded;
        for (auto& kv : p) decoded += (decoded.empty() ? "" : "&") + kv.first + "=" + kv.second;
        return crypto::toHex(crypto::md5(cfg.first + "&" + lowerAscii(decoded) + "&" + cfg.second));
    }

    json api(const std::string& base, const Params& params) {
        std::string vv = sign(params);
        std::string url = base + "?" + encode(params) + "&vv=" + vv + "&pub=" + config().first;
        return parseJson(fetch(url, headers()));
    }

    Page filterSearch(int page, const std::string& orderby) {
        Params params = {{"cinema", "1"}, {"page", std::to_string(page)}, {"size", "32"}, {"orderby", orderby},
                         {"desc", "1"}, {"cid", "0,1,6"}, {"isIndex", "-1"}, {"isfree", "-1"}};
        std::string vv = sign(params);
        return parseResult(fetch("https://m10.iyf.tv/api/list/Search?" + encode(params) + "&vv=" + vv + "&pub=" + config().first, headers()));
    }

    static Page parseResult(const std::string& body) {
        json j = parseJson(body);
        const json& info = jget(jget(j, "data"), "info");
        Page p;
        if (!info.is_array() || info.empty()) return p;
        const json& result = jget(info[0], "result");
        for (auto& it : result) {
            Anime a;
            std::string key = js(it, "key");
            if (key.empty()) key = js(it, "contxt");
            a.url = "/play/" + key + "#" + js(it, "videoClassID");
            a.title = js(it, "title");
            a.thumbnail = js(it, "image");
            if (a.thumbnail.empty()) a.thumbnail = js(it, "imgPath");
            if (!a.title.empty() && !key.empty()) p.animes.push_back(a);
        }
        p.hasNextPage = result.is_array() && result.size() >= 32;
        return p;
    }
};

// ---------------------------------------------------------------------------------------- Nivod (泥视频)
class Nivod : public AsiaSource {
  public:
    Nivod() : AsiaSource({"zh.nivod", "泥视频", "https://www.nivod.cc", "zh", false, true}) {}

    Page popular(int) override { return layout(1); }
    Page latestPage(int) override { return layout(0); }

    Page search(const std::string& q, int page) override {
        if (q.empty()) return popular(page);
        std::string url = "https://e.kortw.cc/vodsearch/-------------.html?keyword=" + http::urlEncode(q);
        html::Document d(fetch(url), url);
        Page p;
        for (auto& it : d.select(".qy-list-img.vertical")) {
            Anime a;
            a.url = it.selectFirst("a.qy-mod-link").attr("href");
            a.title = html::textOf(it.select(".title-wrap .main a"));
            a.thumbnail = baseUrl() + substringBefore(substringAfter(it.selectFirst("div.qy-mod-cover").attr("style"), "url("), ")");
            if (!a.url.empty() && !a.title.empty()) p.animes.push_back(a);
        }
        return p;
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details det;
        det.thumbnail = baseUrl() + d->selectFirst(".left-img-c img").attr("src");
        det.title = html::textOf(d->select(".right-title"));
        det.genre = joinText(d->select(".right-label-c .right-label"));
        for (auto& c : d->select(".right-type-c")) {
            int n = nthChild(c);
            std::string t = html::textOf(c.select(".right-label"));
            if (n == 4) det.author = t;
            else if (n == 6) det.description = t;
        }
        int i = 0;
        for (auto& a : d->select(".list-ruku a")) {
            Episode e;
            e.url = a.attr("href");
            e.name = html::textOf(a.select(".item"));
            if (e.name.empty()) e.name = a.text();
            e.number = ++i;
            det.episodes.push_back(e);
        }
        std::reverse(det.episodes.begin(), det.episodes.end());
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        auto d = doc(abs(episodeUrl));
        std::string script = scriptWith(*d, {"xhr_playinfo"});
        std::string path = substringBefore(substringAfter(script, "url = '"), "'");
        if (script.empty() || path.empty()) throw http::Error("Informazioni di riproduzione non trovate");
        json j = parseJson(fetch(abs(path)));
        std::vector<Video> out;
        for (auto& it : jget(j, "pdatas")) {
            std::string u = js(it, "playurl");
            if (u.empty()) continue;
            std::string from = js(it, "from");
            std::string name = from.size() >= 2 ? from.substr(0, 2) : from;
            for (auto& c : name) c = (char)std::toupper((unsigned char)c);
            out.push_back(simpleVideo(u, name + "云", baseUrl() + "/"));
        }
        return ensure(out);
    }

  private:
    Page layout(size_t index) {
        auto d = doc(baseUrl() + "/class.html?channel=anime");
        auto layouts = d->select(".tl-layout");
        Page p;
        if (index >= layouts.size()) return p;
        for (auto& it : layouts[index].select(".qy-mod-img.vertical")) {
            Anime a;
            a.url = it.selectFirst("a.qy-mod-link").attr("href");
            a.title = html::textOf(it.select(".title-wrap .main a"));
            a.thumbnail = baseUrl() + it.selectFirst("picture img").attr("src");
            if (!a.url.empty() && !a.title.empty()) p.animes.push_back(a);
        }
        return p;
    }
};

// ---------------------------------------------------------------------------------------- Xiaoxintv (小宝影院)
class Xiaoxintv : public AsiaSource {
  public:
    Xiaoxintv() : AsiaSource({"zh.xiaoxintv", "小宝影院", "https://xiaoxintv.cc", "zh", false, true}) {}

    Page popular(int page) override { return filterList(baseUrl() + "/index.php/vod/show/by/hits/id/5" + pageSuffix(page)); }
    Page latestPage(int page) override { return filterList(baseUrl() + "/index.php/vod/show/id/5" + pageSuffix(page)); }

    Page search(const std::string& q, int page) override {
        if (trim(q).empty()) return popular(page);
        std::string url = page > 1 ? baseUrl() + "/index.php/vod/search/page/" + std::to_string(page) + "/wd/" + http::urlEncode(q)
                                   : baseUrl() + "/index.php/vod/search?wd=" + http::urlEncode(q);
        auto d = doc(url);
        Page p;
        for (auto& li : d->select("#searchList li")) {
            html::Node t = li.selectFirst("a.myui-vodlist__thumb");
            Anime a;
            a.url = rel(t.attr("href"));
            a.thumbnail = t.attr("data-original");
            a.title = t.attr("title");
            if (!a.url.empty() && !a.title.empty()) p.animes.push_back(a);
        }
        p.hasNextPage = hasNext(*d, url);
        return p;
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details det;
        det.thumbnail = d->selectFirst(".myui-vodlist__thumb.picture img").attr("data-original");
        det.title = html::textOf(d->select(".myui-content__detail .title"));
        auto datas = d->select("p.data");
        det.author = firstContaining(datas, "主演：").text();
        std::string dir = firstContaining(datas, "导演：").text();
        html::Node intro = firstContaining(datas, "简介：");
        det.description = intro ? ownText(intro) : "";
        if (!dir.empty()) det.description = trim(det.description + "\n\n" + dir);
        auto items = d->select("#playlist1 ul li");
        for (size_t i = 0; i < items.size(); i++) {
            Episode e;
            html::Node a = items[i].selectFirst("a");
            e.url = a.attr("href");
            e.name = items[i].attr("title");
            if (e.name.empty()) e.name = a.text();
            e.number = (double)i;
            det.episodes.push_back(e);
        }
        std::reverse(det.episodes.begin(), det.episodes.end());
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        auto d = doc(abs(episodeUrl));
        std::string u = macCmsPlayerUrl(*d);
        if (!startsWith(u, "http")) throw http::Error("Video non trovato");
        return {simpleVideo(u, "小宝影院", baseUrl() + "/")};
    }

  private:
    static std::string pageSuffix(int page) { return page > 1 ? "/page/" + std::to_string(page) : ""; }

    static bool hasNext(const html::Document& d, const std::string& requestUrl) {
        for (auto& a : d.select(".myui-page a"))
            if (contains(a.text(), "下一页")) {
                std::string href = a.attr("href");
                return !href.empty() && !endsWith(requestUrl, href);
            }
        return false;
    }

    Page filterList(const std::string& url) {
        auto d = doc(url);
        Page p;
        for (auto& box : d->select(".myui-vodlist__box")) {
            html::Node t = box.selectFirst(".myui-vodlist__thumb");
            Anime a;
            a.url = rel(t.attr("href"));
            a.thumbnail = t.attr("data-original");
            a.title = t.attr("title");
            if (!a.url.empty() && !a.title.empty()) p.animes.push_back(a);
        }
        p.hasNextPage = hasNext(*d, url);
        return p;
    }
};

// ---------------------------------------------------------------------------------------- Hanime1.me (18+)
class Hanime1 : public AsiaSource {
  public:
    Hanime1() : AsiaSource({"zh.hanime1", "Hanime1.me", "https://hanime1.me", "zh", true, true}) {}

    Page popular(int page) override { return searchPage("", "本週排行", page); }
    Page latestPage(int page) override { return searchPage("", "", page); }
    Page search(const std::string& q, int page) override { return searchPage(q, "", page); }

    Details details(const std::string& url) override {
        std::string pageUrl = abs(url);
        auto d = doc(pageUrl);
        Details det;
        std::vector<std::string> tags;
        for (auto& t : d->select(".single-video-tag")) {
            if (t.hasAttr("data-toggle")) continue;
            std::string s = removePrefix(t.text(), "# ");
            // " \(\d+\)$"
            auto p = s.rfind(" (");
            if (p != std::string::npos && endsWith(s, ")") && s.size() > p + 3) {
                std::string inner = s.substr(p + 2, s.size() - p - 3);
                if (!inner.empty() && digitsOnly(inner) == inner) s = s.substr(0, p);
            }
            tags.push_back(s);
        }
        det.genre = joinStr(tags);
        det.author = html::textOf(d->select("#video-artist-name"));
        det.title = html::textOf(d->select("#shareBtn-title"));
        if (det.title.empty()) det.title = d->selectFirst("meta[property=og:title]").attr("content");
        det.description = d->selectFirst("meta[property=og:description]").attr("content");
        det.thumbnail = d->selectFirst("meta[property=og:image]").attr("content");

        html::Node playlist = d->selectFirst("#playlist-scroll");
        std::vector<html::Node> nodes;
        for (auto& c : playlist.children())
            if (c.tag() == "div") nodes.push_back(c);
        for (size_t i = 0; i < nodes.size(); i++) {
            std::string href = nodes[i].selectFirst(".thumb-container a").attr("href");
            if (href.empty()) continue;
            Episode e;
            e.url = rel(href);
            e.number = (double)(nodes.size() - i);
            e.name = html::textOf(nodes[i].select(".video-title"));
            det.episodes.push_back(e);
        }
        if (det.episodes.empty()) {
            Episode e;
            e.url = rel(pageUrl);
            e.name = det.title;
            e.number = 1;
            det.episodes.push_back(e);
        }
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        auto d = doc(abs(episodeUrl));
        std::vector<Video> out;
        for (auto& s : d->select("video source")) {
            std::string src = s.attr("src");
            if (src.empty()) continue;
            out.push_back(simpleVideo(src, s.attr("size") + "P", baseUrl() + "/"));
        }
        if (out.empty()) {
            std::string script = scriptWith(*d, {"source"});
            std::string u = substringBefore(afterOr(script, "source = '"), "'");
            if (!u.empty()) out.push_back(simpleVideo(u, "Raw", baseUrl() + "/"));
        }
        preferFirst(out, "1080P");
        return ensure(out);
    }

  private:
    Page searchPage(const std::string& q, const std::string& sort, int page) {
        std::string url = baseUrl() + "/search";
        std::vector<std::string> params;
        if (!q.empty()) params.push_back("query=" + http::urlEncode(q));
        if (!sort.empty()) params.push_back("sort=" + http::urlEncode(sort));
        if (page > 1) params.push_back("page=" + std::to_string(page));
        if (!params.empty()) url += "?" + joinStr(params, "&");
        auto d = doc(url);
        Page p;
        std::set<std::string> seen;
        for (auto& it : d->select(".horizontal-row .video-item-container")) {
            if (it.selectFirst("a.video-link[target]")) continue;
            Anime a;
            a.url = rel(d->absUrl(it.selectFirst("a.video-link"), "href"));
            a.thumbnail = d->absUrl(it.selectFirst(".main-thumb"), "src");
            a.title = html::textOf(it.select(".title")) + "​";
            if (!a.url.empty() && seen.insert(a.url).second) p.animes.push_back(a);
        }
        if (p.animes.empty()) {
            for (auto& it : d->select(".search-videos")) {
                html::Node parent = it.parent();
                if (parent.tag() != "a" || parent.hasAttr("target")) continue;
                Anime a;
                a.url = rel(d->absUrl(parent, "href"));
                a.thumbnail = it.selectFirst("img").attr("src");
                a.title = html::textOf(it.select(".home-rows-videos-title")) + "​";
                if (!a.url.empty() && seen.insert(a.url).second) p.animes.push_back(a);
            }
        }
        p.hasNextPage = d->selectFirst("li.page-item a.page-link[rel=next]").valid();
        return p;
    }
};

}  // namespace

// ####################################################################################################
// ######################################## COREANO / SERBO / UCRAINO / HINDI #########################
// ####################################################################################################

namespace {

// ---------------------------------------------------------------------------------------- Aniweek (ko)
class Aniweek : public AsiaSource {
  public:
    Aniweek() : AsiaSource({"ko.aniweek", "Aniweek", "https://aniweek.com", "ko", false, false}) {}

    Page popular(int page) override {
        return listing(baseUrl() + "/bbs/board.php?bo_table=ing" + (page > 1 ? "&page=" + std::to_string(page) : std::string()));
    }

    Page search(const std::string& q, int page) override {
        if (trim(q).empty()) return popular(page);
        return listing(baseUrl() + "/bbs/search.php?sfl=wr_subject&stx=" + http::urlEncode(q) +
                       "&sop=and&gr_id=&srows=24&onetable=&page=" + std::to_string(page));
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details det;
        det.title = d->selectFirst("div.view-title").text();
        det.thumbnail = fixThumb(d->selectFirst("div.view-info > div.image img").attr("src"));
        std::vector<std::string> rows;
        for (auto& p : d->select("div.view-info > div.list > p")) {
            std::vector<std::string> parts;
            for (auto& s : p.select("span")) parts.push_back(s.text());
            rows.push_back(joinStr(parts, ": "));
        }
        det.description = joinStr(rows, "\n");
        for (auto& li : d->select("div.serial-list > ul.list-body > li")) {
            html::Node a = li.selectFirst("a");
            if (!a) continue;
            Episode e;
            e.number = toNumber(li.selectFirst("div.wr-num").text(), 1);
            e.name = a.text();
            e.url = rel(a.attr("href"));
            det.episodes.push_back(e);
        }
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        auto d = doc(abs(episodeUrl));
        html::Node form = d->selectFirst("form.tt");
        if (!form) throw http::Error("Failed to generate form");
        std::string postUrl = d->absUrl(form, "action");
        std::vector<std::pair<std::string, std::string>> fields;
        for (auto& in : form.select("input[type=hidden][name][value]")) fields.push_back({in.attr("name"), in.attr("value")});
        http::Headers ph = {{"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8"},
                            {"Origin", baseUrl()},
                            {"Referer", baseUrl() + "/"}};
        html::Document nd(post(postUrl, formBody(fields), ph), postUrl);
        std::string iframeUrl = nd.selectFirst("iframe").attr("src");
        if (iframeUrl.empty()) throw http::Error("Failed to extract iframe");
        iframeUrl = fixUrl(iframeUrl, postUrl);
        std::string host = http::hostOf(iframeUrl);

        http::Response ir = http::request("GET", iframeUrl,
                                          {{"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"},
                                           {"Referer", baseUrl() + "/"},
                                           {"Sec-Fetch-Dest", "iframe"},
                                           {"Sec-Fetch-Mode", "navigate"},
                                           {"Sec-Fetch-Site", "cross-site"},
                                           {"Upgrade-Insecure-Requests", "1"}});
        std::vector<Video::Track> subs;
        {
            html::Document idoc(ir.body, iframeUrl);
            std::string script = scriptWith(idoc, {"playerjsSubtitle"});
            std::string s = substringBefore(afterOr(script, "var playerjsSubtitle = \""), "\"");
            if (!s.empty() && contains(s, "https:"))
                subs.push_back({"https:" + substringAfter(s, "https:"), substringBefore(s, "https:")});
        }
        std::string cookie;
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (savedCookie.empty()) savedCookie = substringBefore(ir.header("set-cookie"), ";");
            cookie = savedCookie;
        }
        std::string hash = contains(iframeUrl, "/video/") ? substringAfter(iframeUrl, "/video/") : substringAfter(iframeUrl, "data=");
        http::Headers vh = {{"Accept", "*/*"},
                            {"Content-Type", "application/x-www-form-urlencoded; charset=UTF-8"},
                            {"Origin", "https://" + host},
                            {"Referer", iframeUrl},
                            {"X-Requested-With", "XMLHttpRequest"}};
        if (!cookie.empty()) vh.push_back({"Cookie", cookie});
        http::Response pr = http::request("POST", "https://" + host + "/player/index.php?data=" + hash + "&do=getVideo", vh,
                                          "hash=" + hash + "&r=" + http::urlEncode(baseUrl() + "/"));
        json j = parseJson(pr.body);
        if (!jget(j, "hls").is_boolean() || !j["hls"].get<bool>()) throw http::Error("Nessun video HLS disponibile");
        http::Headers extra;
        if (!cookie.empty()) extra.push_back({"Cookie", cookie});
        auto out = hlsVideos(js(j, "videoSource"), "https://" + host + "/", "", subs, extra);
        sortByQuality(out, "1080");
        return ensure(out);
    }

  private:
    std::mutex mtx;
    std::string savedCookie;

    std::string fixThumb(const std::string& t) const { return startsWith(t, "..") ? baseUrl() + substringAfter(t, "..") : t; }

    Page listing(const std::string& url) {
        auto d = doc(url);
        Page p = list(*d, "div.list-board > div.list-body > div.list-row", [&](const html::Node& el, Anime& a) {
            a.url = el.selectFirst("a").attr("href");
            a.thumbnail = fixThumb(el.selectFirst("img").attr("src"));
            a.title = el.selectFirst("div.post-title").text();
        });
        // "ul.pagination > li.active ~ li:not(.disabled):matches(.)"
        bool afterActive = false;
        for (auto& li : d->selectFirst("ul.pagination").children()) {
            std::string cls = " " + li.attr("class") + " ";
            if (contains(cls, " active ")) {
                afterActive = true;
                continue;
            }
            if (afterActive && !contains(cls, " disabled ") && !trim(li.text()).empty()) {
                p.hasNextPage = true;
                break;
            }
        }
        return p;
    }
};

// ---------------------------------------------------------------------------------------- AnimeSrbija (sr)
class AnimeSrbija : public AsiaSource {
  public:
    AnimeSrbija() : AsiaSource({"sr.animesrbija", "Anime Srbija", "https://www.animesrbija.com", "sr", false, true}) {}

    Page popular(int page) override { return filterPage(baseUrl() + "/filter?sort=popular&page=" + std::to_string(page)); }

    Page latestPage(int) override {
        auto d = doc(baseUrl());
        json props = pageProps(*d);
        Page p;
        for (auto& ep : jget(props, "newEpisodes")) addAnime(p, jget(ep, "anime"));
        return p;
    }

    Page search(const std::string& q, int page) override {
        std::string url = baseUrl() + "/filter?page=" + std::to_string(page) + "&sort=popular";
        if (!trim(q).empty()) url += "&search=" + http::urlEncode(q);
        return filterPage(url);
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        json anime = jget(pageProps(*d), "anime");
        Details det;
        det.title = js(anime, "title");
        det.thumbnail = imgUrl(js(anime, "img"));
        std::string st = js(anime, "status");
        det.status = st == "Završeno" ? "Completato" : st == "Emituje se" ? "In corso" : "";
        std::vector<std::string> studios, genres;
        for (auto& s : jget(anime, "studios"))
            if (s.is_string()) studios.push_back(s.get<std::string>());
        for (auto& s : jget(anime, "genres"))
            if (s.is_string()) genres.push_back(s.get<std::string>());
        det.author = joinStr(studios);
        det.genre = joinStr(genres);
        std::string desc;
        if (!js(anime, "season").empty()) desc += "Sezona: " + js(anime, "season") + "\n";
        if (!js(anime, "aired").empty()) desc += "Datum: " + js(anime, "aired") + "\n";
        if (!js(anime, "subtitle").empty()) desc += "Alternativni naziv: " + js(anime, "subtitle") + "\n";
        if (!js(anime, "desc").empty()) desc += "\n\n" + js(anime, "desc");
        det.description = trim(desc);
        for (auto& ep : jget(anime, "episodes")) {
            Episode e;
            e.url = "/epizoda/" + js(ep, "slug");
            e.number = toNumber(js(ep, "number"), -1);
            e.name = "Epizoda " + js(ep, "number");
            if (jget(ep, "filler").is_boolean() && ep["filler"].get<bool>()) e.name += " (filler)";
            det.episodes.push_back(e);
        }
        sortEpisodesDesc(det.episodes);
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        auto d = doc(abs(episodeUrl));
        json ep = jget(pageProps(*d), "episode");
        std::vector<Video> out;
        for (const char* k : {"player1", "player2", "player3", "player4", "player5"}) {
            std::string u = trimChars(js(ep, k), "!");
            if (u.empty()) continue;
            try {
                if (contains(u, "filemoon")) append(out, moon(u, baseUrl(), "Filemoon - "));
                else if (contains(u, ".m3u8")) out.push_back(simpleVideo(u, "Internal Player", baseUrl() + "/"));
            } catch (const std::exception&) {
            }
        }
        return ensure(out);
    }

  private:
    std::string imgUrl(const std::string& img) const { return baseUrl() + "/_next/image?url=" + img + "&w=1080&q=75"; }

    static json pageProps(const html::Document& d) {
        json j = parseJson(d.selectFirst("script#__NEXT_DATA__").data());
        return jget(jget(j, "props"), "pageProps");
    }

    void addAnime(Page& p, const json& a) const {
        Anime an;
        an.url = "/anime/" + js(a, "slug");
        an.title = js(a, "title");
        an.thumbnail = imgUrl(js(a, "img"));
        if (!an.title.empty() && !js(a, "slug").empty()) p.animes.push_back(an);
    }

    Page filterPage(const std::string& url) {
        auto d = doc(url);
        Page p;
        for (auto& a : jget(pageProps(*d), "anime")) addAnime(p, a);
        for (auto& s : d->select("ul.pagination span.next-page"))
            if (!contains(" " + s.attr("class") + " ", " disabled ")) p.hasNextPage = true;
        return p;
    }
};

// ---------------------------------------------------------------------------------------- UAKino (uk)
class UAKino : public AsiaSource {
  public:
    UAKino() : AsiaSource({"uk.uakino", "UAKino", "https://uakino.best", "uk", false, true}) {}

    Page popular(int page) override {
        return listing(baseUrl() + "/animeukr/f/c.year=1921,2026/sort=rating;desc/page/" + std::to_string(page), "div.movie-item");
    }
    Page latestPage(int page) override {
        return listing(baseUrl() + "/animeukr" + (page > 1 ? "/page/" + std::to_string(page) : std::string()), "div.movie-item");
    }

    Page search(const std::string& q, int) override {
        std::string body = formBody({{"do", "search"}, {"subaction", "search"}, {"story", q}});
        auto d = postDoc(baseUrl() + "/ua/", body, {{"Referer", baseUrl() + "/"}, {"User-Agent", "Mozilla/5.0"}});
        return parse(*d, "div.movie-item.short-item");
    }

    Details details(const std::string& url) override {
        std::string pageUrl = abs(url);
        auto d = doc(pageUrl);
        Details det;
        det.title = html::textOf(d->select("h1 span.solototle"));
        det.thumbnail = fixUrl(d->absUrl(d->selectFirst("a[data-fancybox=gallery]"), "href"), baseUrl());
        det.description = html::textOf(d->select("div.full-text[itemprop=description]"));

        std::string titleId = d->selectFirst("input[id=post_id]").attr("value");
        if (trim(titleId).empty()) return det;
        std::string listUrl = baseUrl() + "/engine/ajax/playlists.php?news_id=" + http::urlEncode(titleId) + "&xfield=playlist";
        json parsed = parseJson(http::request("GET", listUrl,
                                              {{"Referer", baseUrl() + "/"}, {"X-Requested-With", "XMLHttpRequest"}, {"User-Agent", "Mozilla/5.0"}})
                                    .body);
        std::vector<Episode> eps;
        if (jget(parsed, "success").is_boolean() && parsed["success"].get<bool>()) {
            html::Document pd(js(parsed, "response"), baseUrl());
            for (auto& li : pd.select("div.playlists-videos li")) {
                std::string file = li.attr("data-file");
                if (file.empty()) continue;
                Episode e;
                e.url = fixUrl(file, baseUrl());
                e.name = trim(li.text() + " " + li.attr("data-voice"));
                eps.push_back(e);
            }
        } else {
            std::string playerUrl = fixUrl(d->selectFirst("iframe#pre").attr("src"), baseUrl());
            if (contains(playerUrl, "/serial/")) {
                html::Document sd(http::getText(playerUrl), playerUrl);
                std::string scripts = allScripts(sd);
                json voices = parseJson(fileValue(scripts));
                for (auto& voice : voices)
                    for (auto& season : jget(voice, "folder"))
                        for (auto& video : jget(season, "folder")) {
                            Episode e;
                            e.name = js(season, "title") + " " + js(video, "title") + " " + js(voice, "title");
                            e.url = js(video, "file");
                            if (!e.url.empty()) eps.push_back(e);
                        }
            } else if (!playerUrl.empty()) {
                Episode e;
                e.url = playerUrl;
                e.name = det.title + " Фільм";
                eps.push_back(e);
            }
        }
        for (size_t i = 0; i < eps.size(); i++) {
            eps[i].number = firstNumber(eps[i].name, (double)(i + 1));
        }
        std::reverse(eps.begin(), eps.end());
        det.episodes = eps;
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::string m3u8 = episodeUrl;
        if (!contains(episodeUrl, ".m3u8")) {
            html::Document d(http::getText(episodeUrl), episodeUrl);
            for (auto& s : d.select("script")) {
                std::string f = fileValue(s.data());
                if (!f.empty()) {
                    m3u8 = f;
                    break;
                }
            }
        }
        if (!contains(m3u8, ".m3u8")) throw http::Error("Nessun video trovato");
        return ensure(hlsVideos(fixUrl(m3u8), baseUrl(), ""));
    }

  private:
    /** file\s*:\s*["']([^"']+)["'] (per le serie il valore e' un JSON tra apici singoli) */
    static std::string fileValue(const std::string& text) {
        size_t p = 0;
        while ((p = text.find("file", p)) != std::string::npos) {
            size_t i = p + 4;
            while (i < text.size() && std::isspace((unsigned char)text[i])) i++;
            if (i < text.size() && text[i] == ':') {
                i++;
                while (i < text.size() && std::isspace((unsigned char)text[i])) i++;
                if (i < text.size() && (text[i] == '"' || text[i] == '\'')) {
                    char q = text[i];
                    auto e = text.find(q, i + 1);
                    if (e != std::string::npos && e > i + 1) return text.substr(i + 1, e - i - 1);
                }
            }
            p += 4;
        }
        return "";
    }

    Page listing(const std::string& url, const std::string& sel) {
        auto d = doc(url);
        return parse(*d, sel);
    }

    Page parse(const html::Document& d, const std::string& sel) {
        Page p = list(d, sel, [&](const html::Node& el, Anime& a) {
            html::Node t = el.selectFirst("a.movie-title");
            a.url = t.attr("href");
            a.title = t.text();
            a.thumbnail = fixUrl(d.absUrl(el.selectFirst("div.movie-img img"), "src"), baseUrl());
        });
        for (auto& a : d.select("a"))
            if (contains(a.text(), "Далі")) p.hasNextPage = true;
        return p;
    }
};

// ---------------------------------------------------------------------------------------- UFDub (uk)
class UFDub : public AsiaSource {
  public:
    UFDub() : AsiaSource({"uk.ufdub", "UFDub", "https://ufdub.com", "uk", false, true}) {}

    Page popular(int page) override {
        std::string body = formBody({{"dlenewssortby", "rating"},
                                     {"dledirection", "desc"},
                                     {"set_new_sort", "dle_sort_cat_12"},
                                     {"set_direction_sort", "dle_direction_cat_12"}});
        auto d = postDoc(baseUrl() + "/anime/page/" + std::to_string(page), body);
        return parse(*d);
    }

    Page latestPage(int page) override {
        auto d = doc(baseUrl() + "/anime/page/" + std::to_string(page));
        return parse(*d);
    }

    Page search(const std::string& q, int) override {
        std::string body = formBody({{"do", "search"}, {"subaction", "search"}, {"full_search", "1"}, {"result_from", "1"}, {"story", q}});
        auto d = postDoc(baseUrl() + "/index.php?do=search", body);
        Page p = parse(*d);
        p.hasNextPage = false;
        return p;
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details det;
        det.thumbnail = baseUrl() + d->selectFirst("div.f-poster img").attr("src");
        det.title = html::textOf(d->select("h1.top-title"));
        det.description = html::textOf(d->select("div.full-text p"));
        for (auto& ele : d->select("div.full-desc .full-info div.fi-col-item")) {
            std::string label = html::textOf(ele.select("span"));
            if (label == "Студія:") det.author = html::textOf(ele.select("a"));
            else if (label == "Жанр:") det.genre = replaceAll(html::textOf(ele.select("a")), " ", ", ");
        }
        std::string playerUrl;
        for (auto& in : d->select("input[value]"))
            if (contains(in.attr("value"), "https://video.ufdub.com")) {
                playerUrl = in.attr("value");
                break;
            }
        if (playerUrl.empty()) return det;
        html::Document pd(http::getText(playerUrl), playerUrl);
        std::string scripts = allScripts(pd);
        const std::string marker = "https://ufdub.com/video/VIDEOS.php?";
        size_t p = 0;
        int n = 0;
        while ((p = scripts.find(marker, p)) != std::string::npos) {
            auto e = scripts.find('\'', p);
            if (e == std::string::npos) break;
            std::string u = scripts.substr(p, e - p);
            p = e;
            Episode ep;
            ep.url = u;
            ep.name = urlDecode(http::queryParam(u, "Seriya"));
            if (ep.name.empty()) ep.name = "Серія " + std::to_string(n + 1);
            ep.number = ++n;
            det.episodes.push_back(ep);
        }
        std::reverse(det.episodes.begin(), det.episodes.end());
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::string finalUrl = episodeUrl;
        try {
            http::Response r = http::request("HEAD", episodeUrl, {{"Referer", baseUrl() + "/"}}, "", 30, true);
            if (!r.finalUrl.empty()) finalUrl = r.finalUrl;
        } catch (const std::exception&) {
        }
        return {simpleVideo(replaceAll(finalUrl, "dl=1", "raw=1"), "Quality")};
    }

  private:
    Page parse(const html::Document& d) {
        return list(d, "div.short", [&](const html::Node& el, Anime& a) {
            a.url = el.selectFirst("div.m-views").attr("data-link");
            a.thumbnail = baseUrl() + el.selectFirst("div.short-i > img").attr("src");
            a.title = html::textOf(el.select("div.short-t-or"));
        }, "div.pagi-nav a");
    }
};

// ---------------------------------------------------------------------------------------- YoMovies (hi, 18+)
class YoMovies : public AsiaSource {
  public:
    YoMovies() : AsiaSource({"hi.yomovies", "YoMovies", "https://yomovies.town", "hi", true, false}) {}

    Page popular(int page) override { return listing(addPage(baseUrl() + "/most-favorites/", page)); }

    Page search(const std::string& q, int page) override {
        if (trim(q).empty()) return popular(page);
        std::string url = page == 1 ? baseUrl() + "/?s=" + http::urlEncode(q)
                                    : baseUrl() + "/page/" + std::to_string(page) + "?s=" + http::urlEncode(q);
        return listing(url);
    }

    Details details(const std::string& url) override {
        std::string pageUrl = abs(url);
        auto d = doc(pageUrl);
        Details det;
        html::Node info = d->selectFirst("div.mvi-content");
        det.title = info.selectFirst("h3").text();
        if (det.title.empty()) det.title = d->selectFirst("meta[property=og:title]").attr("content");
        det.thumbnail = d->absUrl(info.selectFirst("img"), "src");
        det.description = info.selectFirst("p.f-desc").text();
        for (auto& p : info.select("div.mvici-left > p")) {
            std::string t = p.text();
            if (contains(t, "Genre:")) det.genre = joinText(p.select("a"));
            if (contains(t, "Studio:")) det.author = joinText(p.select("a"));
        }
        auto seasons = d->select("div#seasons > div.tvseason");
        if (seasons.empty()) {
            Episode e;
            e.url = rel(pageUrl);
            e.name = "Movie";
            e.number = 1;
            det.episodes.push_back(e);
            return det;
        }
        for (auto& season : seasons) {
            std::string seasonText = trim(season.selectFirst("div.les-title").text());
            auto eps = season.select("div.les-content > a");
            for (size_t i = 0; i < eps.size(); i++) {
                std::string epNumber = substringAfter(trim(eps[i].text()), "pisode ");
                Episode e;
                e.url = rel(d->absUrl(eps[i], "href"));
                e.name = seasonText + " Ep. " + epNumber;
                e.number = toNumber(epNumber, (double)(i + 1));
                det.episodes.push_back(e);
            }
        }
        std::reverse(det.episodes.begin(), det.episodes.end());
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        auto d = doc(abs(episodeUrl));
        std::vector<Video> out;
        for (auto& server : d->select("div[id*=tab]")) {
            html::Node iframe = server.selectFirst("div.movieplay > iframe");
            if (!iframe) continue;
            std::string id = server.attr("id");
            std::string name;
            for (auto& li : d->select("ul.idTabs > li"))
                if (li.selectFirst("a[href=\"#" + id + "\"]")) {
                    std::string t = li.selectFirst("div.les-title").text();
                    if (!t.empty()) name = "[" + t + "] - ";
                    break;
                }
            std::string src = d->absUrl(iframe, "src");
            if (src.empty()) src = d->absUrl(iframe, "data-src");
            try {
                if (contains(src, "speedostream")) append(out, speedostream(src, name));
                else if (contains(src, "movembed.cc")) append(out, movembed(src));
                else if (contains(src, "minoplres")) append(out, minoplres(src, name));
            } catch (const std::exception&) {
            }
        }
        sortByQuality(out, "1080");
        return ensure(out);
    }

  private:
    static std::string addPage(const std::string& url, int page) {
        if (page == 1) return url;
        std::string u = url;
        if (endsWith(u, "/")) u.pop_back();
        return u + "/page/" + std::to_string(page);
    }

    Page listing(const std::string& url) {
        auto d = doc(url);
        Page p = list(*d, "div.movies-list > div.ml-item", [&](const html::Node& el, Anime& a) {
            a.url = el.selectFirst("a[href]").attr("href");
            a.thumbnail = d->absUrl(el.selectFirst("img[data-original]"), "data-original");
            a.title = el.selectFirst("div.qtip-title").text();
        });
        // "ul.pagination > li.active + li"
        auto lis = d->selectFirst("ul.pagination").children();
        for (size_t i = 0; i + 1 < lis.size(); i++)
            if (contains(" " + lis[i].attr("class") + " ", " active ") && lis[i + 1].tag() == "li") p.hasNextPage = true;
        return p;
    }

    std::vector<Video> speedostream(const std::string& url, const std::string& prefix) {
        html::Document d(http::getText(url, {{"Referer", baseUrl() + "/"}}), url);
        std::string script = scriptWith(d, {"file:"});
        if (script.empty()) return {};
        std::string master = substringBefore(substringAfter(substringAfter(script, "file:"), "\""), "\"");
        if (!startsWith(master, "http")) return {};
        return hlsVideos(master, "https://" + http::hostOf(url) + "/", prefix + "Speedostream - ");
    }

    std::vector<Video> minoplres(const std::string& url, const std::string& name) {
        html::Document d(http::getText(url, {{"Referer", url}}), url);
        std::string script = scriptWith(d, {"sources:"});
        if (script.empty()) return {};
        std::string master = substringBefore(substringAfter(script, "file:\""), "\"");
        if (!startsWith(master, "http")) return {};
        return hlsVideos(master, url, name + " Minoplres - ");
    }

    std::vector<Video> movembed(const std::string& url) {
        html::Document d(http::getText(url), url);
        std::vector<Video> out;
        for (auto& li : d.select("ul.list-server-items > li.linkserver")) {
            std::string iframe = d.absUrl(li, "data-video");
            if (iframe.empty()) continue;
            try {
                if (contains(iframe, "mixdrop") || contains(iframe, "mixdroop")) {
                    std::vector<Video::Track> subs;
                    std::string s1 = urlDecode(http::queryParam(iframe, "sub1"));
                    if (!s1.empty()) {
                        std::string label = urlDecode(http::queryParam(iframe, "sub1_label"));
                        subs.push_back({s1, label.empty() ? "English" : label});
                    }
                    append(out, mixDrop(iframe, "(movembed) - ", subs));
                } else if (startsWith(iframe, "https://doo")) {
                    auto v = dood(iframe, "(movembed) ");
                    std::string c1 = urlDecode(http::queryParam(iframe, "c1_file"));
                    if (!c1.empty()) {
                        std::string label = urlDecode(http::queryParam(iframe, "c1_label"));
                        for (auto& x : v) x.subtitles.push_back({c1, label.empty() ? "English" : label});
                    }
                    append(out, v);
                }
            } catch (const std::exception&) {
            }
        }
        return out;
    }
};

}  // namespace

// ####################################################################################################
// ########################################### MULTILINGUA ############################################
// ####################################################################################################

namespace {

// ---------------------------------------------------------------------------------------- AnimeWorld India
class AnimeWorldIndia : public AsiaSource {
  public:
    AnimeWorldIndia(Info i, std::string siteLang, std::string language)
        : AsiaSource(i), siteLang(std::move(siteLang)), language(std::move(language)) {}

    Page popular(int page) override { return listing(advanced(page) + "?s_lang=" + siteLang + "&s_orderby=viewed"); }
    Page latestPage(int page) override { return listing(advanced(page) + "?s_lang=" + siteLang + "&s_orderby=update"); }
    Page search(const std::string& q, int page) override {
        return listing(advanced(page) + "?s_keyword=" + http::urlEncode(q) + "&s_lang=" + siteLang +
                       "&s_type=all&s_status=all&s_sub_type=all&s_year=all&s_orderby=default&s_genre=");
    }

    Details details(const std::string& url) override {
        http::Response r = req("GET", abs(url));
        check(r);
        html::Document d(r.body, abs(url));
        Details det;
        det.title = d.selectFirst("h2.text-4xl").text();
        det.genre = joinText(d.select("span.leading-6 a[class*=border-opacity-30]"));
        det.description = d.selectFirst("div[data-synopsis]").text();
        det.author = d.selectFirst("span.leading-6 a[href*=producer]").text();
        det.thumbnail = d.absUrl(d.selectFirst("img[src*=wp-content]"), "src");
        bool isMovie = d.selectFirst("nav li > a[href*=\"type/movies/\"]").valid();

        std::string raw = trim(substringBefore(afterOr(r.body, "var season_list = "), "var season_label ="));
        if (!raw.empty() && raw.back() == ';') raw.pop_back();
        json seasons = parseJson(raw);
        if (!seasons.is_array()) return det;
        bool single = seasons.size() == 1;
        double fallback = 1;
        for (size_t s = 0; s < seasons.size(); s++) {
            std::string seasonName = single ? "" : "Season " + std::to_string(s + 1);
            json all = jget(jget(seasons[s], "episodes"), "all");
            if (!all.is_array()) continue;
            for (auto it = all.rbegin(); it != all.rend(); ++it) {
                const json& meta = jget(*it, "metadata");
                std::string title = js(meta, "title");
                int epNum = std::atoi(js(meta, "number").c_str());
                if (epNum == 0 && js(meta, "number") != "0") epNum = (int)fallback;
                Episode e;
                if (isMovie) {
                    e.name = "Movie";
                } else {
                    e.name = (seasonName.empty() ? "" : seasonName + " - ") + "Episode " + std::to_string(epNum) +
                             (trim(title).empty() ? "" : " - " + title);
                }
                e.number = single ? epNum : fallback;
                fallback++;
                e.url = "/wp-json/kiranime/v1/episode?id=" + js(*it, "id");
                det.episodes.push_back(e);
            }
        }
        sortEpisodesDesc(det.episodes);
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::string body = fetch(abs(episodeUrl));
        std::string arr = trim(substringBefore(substringAfterLast(body, "\"players\":"), ",\"noplayer\":"));
        json players = parseJson(arr);
        std::vector<Video> out;
        bool any = false;
        for (auto& p : players) {
            if (js(p, "type") != "stream" || trim(js(p, "url")).empty()) continue;
            any = true;
            if (!language.empty() && js(p, "language") != language) continue;
            if (js(p, "server") != "Mystream") continue;
            try {
                append(out, myStream(js(p, "url"), js(p, "language")));
            } catch (const std::exception&) {
            }
        }
        if (!any) throw http::Error("No streams available!");
        sortByQuality(out, "1080");
        return ensure(out);
    }

  private:
    std::string siteLang, language;

    std::string advanced(int page) const { return baseUrl() + "/advanced-search/page/" + std::to_string(page) + "/"; }

    Page listing(const std::string& url) {
        auto d = doc(url);
        Page p = list(*d, "div.col-span-1", [&](const html::Node& el, Anime& a) {
            a.url = el.selectFirst("a").attr("href");
            a.thumbnail = d->absUrl(el.selectFirst("img"), "src");
            a.title = el.selectFirst("div.font-medium.line-clamp-2.mb-3").text();
        });
        // "ul.page-numbers li:has(span.current) + li"
        auto lis = d->select("ul.page-numbers li");
        for (size_t i = 0; i + 1 < lis.size(); i++)
            if (lis[i].selectFirst("span.current") && lis[i + 1].parent().raw() == lis[i].parent().raw()) p.hasNextPage = true;
        return p;
    }

    std::vector<Video> myStream(const std::string& url, const std::string& lang) {
        std::string host = substringBefore(url, "/watch");
        http::Response r = http::request("GET", url, {{"Referer", baseUrl() + "/"}});
        std::string code = substringBefore(substringAfter(substringAfter(r.body, "sniff("), ", \""), "\"");
        if (code.empty()) return {};
        std::string streamUrl = host + "/m3u8/" + code + "/master.txt?s=1&cache=1";
        std::string cookie = cookiesOf(r, "PHPSESSID");
        http::Headers extra = {{"Accept", "*/*"}};
        if (!cookie.empty()) extra.push_back({"Cookie", cookie});
        return hlsVideos(streamUrl, "", "[" + lang + "] MyStream: ", {}, extra);
    }
};

// ---------------------------------------------------------------------------------------- AnimeOnsen
class AnimeOnsen : public AsiaSource {
  public:
    AnimeOnsen() : AsiaSource({"all.animeonsen", "AnimeOnsen", "https://www.animeonsen.xyz", "all", false, false}) {}

    Page popular(int page) override { return index(page); }

    Page search(const std::string& q, int page) override {
        if (trim(q).empty()) return popular(page);
        std::string token = searchToken();
        http::Headers h = apiHeaders();
        h.push_back({"Content-Type", "application/json"});
        h.push_back({"Authorization", "Bearer " + token});
        http::Response r = http::request("POST", std::string(SEARCH) + "/indexes/content/search", h, json{{"q", q}}.dump());
        check(r);
        Page p;
        for (auto& it : jget(parseJson(r.body), "hits")) addItem(p, it);
        return p;
    }

    Details details(const std::string& url) override {
        json dt = parseJson(api(std::string(API) + "/content/" + url + "/extensive"));
        Details det;
        det.title = js(dt, "content_title");
        if (det.title.empty()) det.title = js(dt, "content_title_en");
        const json& mal = jget(dt, "mal_data");
        det.status = js(mal, "status") == "finished_airing" ? "Completato" : "In corso";
        std::vector<std::string> studios, genres;
        for (auto& s : jget(mal, "studios")) studios.push_back(js(s, "name"));
        for (auto& g : jget(mal, "genres")) genres.push_back(js(g, "name"));
        det.author = joinStr(studios);
        det.genre = joinStr(genres);
        det.thumbnail = std::string(API) + "/image/210x300/" + js(dt, "content_id");
        std::string desc;
        std::string score = js(mal, "mean_score");
        if (!score.empty()) {
            int stars = std::max(0, std::min(5, (int)(std::atof(score.c_str()) / 2.0 + 0.5)));
            std::string s;
            for (int i = 0; i < 5; i++) s += i < stars ? "★" : "☆";
            desc += s + " " + score + "\n\n";
        }
        desc += js(mal, "synopsis");
        std::string rating = js(mal, "rating");
        if (!rating.empty()) {
            rating = replaceAll(rating, "_", " ");
            for (auto& c : rating) c = (char)std::toupper((unsigned char)c);
            desc += "\n\nRating: " + rating;
        }
        if (!js(dt, "mal_id").empty()) desc += "\nhttps://myanimelist.net/anime/" + js(dt, "mal_id");
        det.description = trim(desc);

        json eps = parseJson(api(std::string(API) + "/content/" + url + "/episodes"));
        if (eps.is_object()) {
            for (auto it = eps.begin(); it != eps.end(); ++it) {
                Episode e;
                e.url = url + "/video/" + it.key();
                e.number = toNumber(it.key(), -1);
                std::string jp = js(it.value(), "contentTitle_episode_jp");
                std::string en = js(it.value(), "contentTitle_episode_en");
                e.name = "Episode " + it.key() + ": " + (jp.empty() ? en : jp);
                det.episodes.push_back(e);
            }
        }
        sortEpisodesDesc(det.episodes);
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        json j = parseJson(api(std::string(API) + "/content/" + episodeUrl));
        const json& uri = jget(j, "uri");
        std::string stream = js(uri, "stream");
        if (stream.empty()) throw http::Error("Nessun video trovato");
        const json& langs = jget(jget(j, "metadata"), "subtitles");
        Video v = simpleVideo(stream, "Default (720p)", std::string(BASE) + "/",
                              {{"User-Agent", UA}, {"Origin", BASE}});
        std::vector<Video::Track> preferred, others;
        const json& subs = jget(uri, "subtitles");
        if (subs.is_object())
            for (auto it = subs.begin(); it != subs.end(); ++it) {
                std::string name = js(langs, it.key().c_str());
                if (name.empty() || !it.value().is_string()) continue;
                Video::Track t{it.value().get<std::string>(), name};
                (contains(it.key(), "en-US") ? preferred : others).push_back(t);
            }
        v.subtitles = preferred;
        v.subtitles.insert(v.subtitles.end(), others.begin(), others.end());
        return {v};
    }

  private:
    static constexpr const char* BASE = "https://www.animeonsen.xyz";
    static constexpr const char* API = "https://api.animeonsen.xyz/v4";
    static constexpr const char* SEARCH = "https://search.animeonsen.xyz";
    static constexpr const char* UA =
        "Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/134.0.0.0 Mobile Safari/537.3";

    std::mutex mtx;
    std::string token, sToken;

    static http::Headers apiHeaders() {
        return {{"User-Agent", UA},
                {"Accept", "application/json, text/plain, */*"},
                {"Accept-Language", "en-US,en;q=0.9"},
                {"Referer", std::string(BASE) + "/"},
                {"Origin", BASE},
                {"Sec-Fetch-Dest", "empty"},
                {"Sec-Fetch-Mode", "cors"},
                {"Sec-Fetch-Site", "same-site"}};
    }

    std::string fetchToken() {
        std::string body = formBody({{"client_id", "f296be26-28b5-4358-b5a1-6259575e23b7"},
                                     {"client_secret", "349038c4157d0480784753841217270c3c5b35f4281eaee029de21cb04084235"},
                                     {"grant_type", "client_credentials"}});
        try {
            http::Response r = http::request("POST", "https://auth.animeonsen.xyz/oauth/token",
                                             {{"User-Agent", UA},
                                              {"Accept", "application/json"},
                                              {"Origin", BASE},
                                              {"Referer", std::string(BASE) + "/"},
                                              {"Content-Type", "application/x-www-form-urlencoded"}},
                                             body);
            if (trim(r.body).empty() || trim(r.body)[0] == '<') return "";
            return js(parseJson(r.body), "access_token");
        } catch (const std::exception&) {
            return "";
        }
    }

    std::string api(const std::string& url) {
        std::string tok;
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (token.empty()) token = fetchToken();
            tok = token;
        }
        auto call = [&](const std::string& t) {
            http::Headers h = apiHeaders();
            if (!t.empty()) h.push_back({"Authorization", "Bearer " + t});
            return http::request("GET", url, h);
        };
        http::Response r = call(tok);
        if (r.status == 401) {
            std::lock_guard<std::mutex> lock(mtx);
            token = fetchToken();
            tok = token;
            r = call(tok);
        }
        check(r);
        return r.body;
    }

    std::string searchToken() {
        std::lock_guard<std::mutex> lock(mtx);
        if (sToken.empty()) {
            try {
                html::Document d(http::getText(BASE, {{"User-Agent", UA}}), BASE);
                sToken = d.selectFirst("meta[name=ao-search-token]").attr("content");
            } catch (const std::exception&) {
            }
        }
        return sToken;
    }

    static void addItem(Page& p, const json& it) {
        Anime a;
        std::string id = js(it, "content_id");
        a.url = id;
        a.title = js(it, "content_title");
        if (a.title.empty()) a.title = js(it, "content_title_jp");
        if (a.title.empty()) a.title = js(it, "content_title_en");
        a.thumbnail = js(it, "thumbnail");
        if (a.thumbnail.empty()) a.thumbnail = js(it, "content_image");
        if (a.thumbnail.empty()) a.thumbnail = std::string(API) + "/image/210x300/" + id;
        if (!id.empty() && !a.title.empty()) p.animes.push_back(a);
    }

    Page index(int page) {
        json j = parseJson(api(std::string(API) + "/content/index?start=" + std::to_string((page - 1) * 30) + "&limit=30"));
        Page p;
        for (auto& it : jget(j, "content")) addItem(p, it);
        const json& next = jget(jget(j, "cursor"), "next");
        p.hasNextPage = next.is_array() && !next.empty() && next[0].is_boolean() && next[0].get<bool>();
        return p;
    }

  public:
    http::Headers imageHeaders() const override { return {{"Referer", std::string(BASE) + "/"}, {"User-Agent", UA}}; }
};

// ---------------------------------------------------------------------------------------- StreamingUnity
class StreamingCommunity : public AsiaSource {
  public:
    StreamingCommunity(Info i, std::string lang, std::string showType)
        : AsiaSource(i), siteLang(std::move(lang)), showType(std::move(showType)) {}

    Page popular(int page) override {
        std::string url;
        if (page == 1) url = site() + "/browse/top10?type=" + showType;
        else if (page == 2) url = site() + "/browse/trending?type=" + showType;
        else url = site() + "/archive?type=" + showType + "&sort=views&page=" + std::to_string(page - 2);
        return browse(url, page);
    }

    Page latestPage(int page) override {
        return browse(site() + "/browse/latest?type=" + showType + "&page=" + std::to_string(page), page);
    }

    Page search(const std::string& q, int page) override {
        return browse(site() + "/archive?search=" + http::urlEncode(q) + "&type=" + showType + "&page=" + std::to_string(page), page);
    }

    Details details(const std::string& url) override {
        json props = getData(site() + "/titles/" + url);
        const json& t = jget(props, "title");
        if (!t.is_object()) throw http::Error("Anime details parsing error: title is null.");
        Details det;
        det.title = js(t, "name");
        det.thumbnail = poster(jget(t, "images"));
        std::string st = js(t, "status");
        det.status = (st == "Ended" || st == "Released") ? "Completato" : st == "Returning Series" ? "In corso" : "";
        std::vector<std::string> genres, directors, actors;
        for (auto& g : jget(t, "genres")) genres.push_back(js(g, "name"));
        for (auto& g : jget(t, "main_directors")) directors.push_back(js(g, "name"));
        for (auto& g : jget(t, "main_actors")) actors.push_back(js(g, "name"));
        det.genre = joinStr(genres);
        det.author = joinStr(directors);
        bool it = siteLang == "it";
        std::string desc;
        std::string score = js(t, "score");
        if (!score.empty()) {
            int stars = std::max(0, std::min(5, (int)(std::atof(score.c_str()) / 2.0 + 0.5)));
            for (int i = 0; i < 5; i++) desc += i < stars ? "★" : "☆";
            desc += " " + score + "\n";
        }
        if (!js(t, "plot").empty()) desc += js(t, "plot") + "\n\n";
        desc += std::string(it ? "Titolo originale" : "Original name") + ": " + js(t, "original_name");
        desc += std::string("\n") + (it ? "Qualità" : "Quality") + ": " + js(t, "quality");
        if (!js(t, "runtime").empty()) desc += std::string(" - ") + (it ? "Durata" : "Run time") + ": " + js(t, "runtime") + "m";
        if (!js(t, "release_date").empty()) desc += std::string("\n") + (it ? "Data di uscita" : "Release date") + ": " + js(t, "release_date");
        if (!js(t, "age").empty()) desc += std::string("\n") + (it ? "Età" : "Rating") + ": " + js(t, "age") + "+";
        if (!actors.empty()) desc += std::string("\n\n") + (it ? "Cast" : "Cast") + ": " + joinStr(actors);
        det.description = trim(desc);

        std::string titleId = js(t, "id");
        const json& loaded = jget(props, "loadedSeason");
        std::vector<Episode> eps;
        if (!loaded.is_object()) {
            Episode e;
            e.name = "Film";
            e.url = titleId;
            eps.push_back(e);
        } else {
            std::string seasonWord = it ? "Stagione" : "Season", epWord = it ? "Episodio" : "Episode";
            for (auto& season : jget(t, "seasons")) {
                json episodes;
                if (js(season, "id") == js(loaded, "id")) {
                    episodes = jget(loaded, "episodes");
                } else {
                    try {
                        episodes = jget(jget(getData(site() + "/titles/" + url + "/season-" + js(season, "number")), "loadedSeason"), "episodes");
                    } catch (const std::exception&) {
                    }
                }
                for (auto& ep : episodes) {
                    Episode e;
                    e.name = seasonWord + " " + js(season, "number") + " " + epWord + " " + js(ep, "number") + " - " + js(ep, "name");
                    e.url = titleId + "?episode_id=" + js(ep, "id") + "&next_episode=1";
                    eps.push_back(e);
                }
            }
        }
        for (size_t i = 0; i < eps.size(); i++) eps[i].number = (double)(i + 1);
        std::reverse(eps.begin(), eps.end());
        det.episodes = eps;
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::string iframeUrl = episodeUrl;
        if (!startsWith(episodeUrl, "https://")) {
            std::string u = site() + "/iframe/" + episodeUrl;
            http::Response r = req("GET", u);
            check(r);
            html::Document d(r.body, r.finalUrl.empty() ? u : r.finalUrl);
            iframeUrl = d.absUrl(d.selectFirst("iframe[src]"), "src");
            if (iframeUrl.empty()) throw http::Error("Failed to extract iframe");
        }
        return ensure(vixCloud(iframeUrl));
    }

  private:
    std::string siteLang, showType;
    std::mutex mtx;
    std::string redirectedHome, imageCdn;

    std::string home() const {
        std::string b = baseUrl();
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mtx));
        return redirectedHome.empty() ? b : redirectedHome;
    }
    std::string site() const { return home() + "/" + siteLang; }

  protected:
    http::Headers baseHeaders() const override { return {{"Origin", home()}, {"Referer", home() + "/"}}; }

  private:
    /** Pagina Inertia: JSON di div#app[data-page] (o risposta JSON) -> props. Aggiorna il dominio in caso di redirect. */
    json getData(const std::string& url) {
        http::Response r = req("GET", url);
        check(r);
        if (!r.finalUrl.empty()) {
            std::string origin = http::originOf(r.finalUrl);
            if (!origin.empty() && origin != home()) {
                std::lock_guard<std::mutex> lock(mtx);
                redirectedHome = origin;
            }
        }
        std::string data;
        if (contains(r.header("content-type"), "application/json")) {
            data = r.body;
        } else {
            html::Document d(r.body, url);
            data = d.selectFirst("div#app[data-page]").attr("data-page");
            if (data.empty()) throw http::Error("Failed to extract data-page");
        }
        json j = parseJson(data);
        if (!j.is_object()) throw http::Error("Risposta non valida");
        return jget(j, "props");
    }

    std::string cdn() {
        std::lock_guard<std::mutex> lock(mtx);
        if (!imageCdn.empty()) return imageCdn;
        std::string h = http::hostOf(redirectedHome.empty() ? baseUrl() : redirectedHome);
        return "https://cdn." + h + "/images/";
    }

    std::string poster(const json& images) {
        std::string base = cdn();
        for (const char* type : {"poster", "cover", "cover_mobile", "background"})
            for (auto& im : images)
                if (js(im, "type") == type) return base + js(im, "filename");
        return "";
    }

    Page browse(const std::string& url, int page) {
        json props = getData(url);
        std::string c = js(props, "cdn_url");
        if (!trim(c).empty()) {
            std::lock_guard<std::mutex> lock(mtx);
            imageCdn = c + "/images/";
        }
        Page p;
        for (auto& t : jget(props, "titles")) {
            Anime a;
            a.title = js(t, "name");
            a.url = js(t, "id") + "-" + js(t, "slug");
            a.thumbnail = poster(jget(t, "images"));
            if (!a.title.empty()) p.animes.push_back(a);
        }
        std::string path = http::pathOf(url);
        if (contains(path, "/browse/top10") || contains(path, "/browse/trending")) p.hasNextPage = true;
        else if (p.animes.size() < 60) p.hasNextPage = false;
        else if (contains(substringBefore(path, "?"), "/archive")) {
            int pg = std::atoi(http::queryParam(url, "page").c_str());
            p.hasNextPage = (pg > 0 ? pg : 1) < 20;
        } else p.hasNextPage = true;
        (void)page;
        return p;
    }

    std::vector<Video> vixCloud(const std::string& iframeUrl) {
        html::Document d(http::getText(iframeUrl, {{"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"},
                                                   {"Referer", site() + "/"}}),
                         iframeUrl);
        std::string script = scriptWith(d, {"masterPlaylist"});
        if (script.empty()) throw http::Error("Failed to extract masterPlaylist script");
        auto quoted = [&](const std::string& key) -> std::string {
            // key: ?'valore'
            auto p = script.find(key);
            if (p == std::string::npos) return "";
            p += key.size();
            while (p < script.size() && script[p] == ' ') p++;
            if (p >= script.size() || script[p] != '\'') return "";
            auto e = script.find('\'', p + 1);
            return e == std::string::npos ? "" : script.substr(p + 1, e - p - 1);
        };
        std::string playlist = quoted("url:");
        std::string token = quoted("'token':");
        std::string expires = quoted("'expires':");
        if (playlist.empty() || token.empty()) throw http::Error("Failed to extract playlist URL");
        std::string fhd = trim(substringBefore(substringBefore(afterOr(script, "window.canPlayFHD"), ";"), "\n"));
        bool canFhd = contains(fhd, "true");
        auto build = [&](const std::string& base) {
            return base + (contains(base, "?") ? "&" : "?") + (canFhd ? "h=1&" : "") + "token=" + token + "&expires=" + expires +
                   "&lang=" + siteLang;
        };
        // window.streams = [{"name":"Server1","active":false,"url":"..."}, ...]
        std::vector<std::pair<bool, std::string>> servers;
        size_t p = 0;
        while ((p = script.find("{\"name\":\"", p)) != std::string::npos) {
            std::string obj = script.substr(p, std::min<size_t>(1024, script.size() - p));
            obj = substringBefore(obj, "}");
            p += 9;
            std::string active = substringBefore(afterOr(obj, "\"active\":"), ",");
            std::string u = substringBefore(afterOr(obj, "\"url\":\""), "\"");
            if (u.empty()) continue;
            u = replaceAll(replaceAll(u, "\\/", "/"), "\\u0026", "&");
            servers.push_back({active == "true" || active == "1", build(u)});
        }
        std::stable_sort(servers.begin(), servers.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        servers.push_back({true, build(playlist)});
        for (auto& s : servers) {
            try {
                auto v = hlsVideos(s.second, "", "VixCloud - ");
                if (!v.empty()) {
                    sortByQuality(v, "1080p");
                    return v;
                }
            } catch (const std::exception&) {
            }
        }
        return {};
    }
};

// ---------------------------------------------------------------------------------------- AniZone (Livewire)
class AniZone : public AsiaSource {
  public:
    AniZone() : AsiaSource({"all.anizone", "AniZone", "https://anizone.to", "all", false, true}) {}

    Page popular(int page) override { return animeList("/anime?sort=title-asc", page); }
    Page latestPage(int page) override { return animeList("/", page); }
    Page search(const std::string& q, int page) override {
        return animeList("/anime?search=" + http::urlEncode(q) + "&sort=title-asc", page);
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details det;
        det.thumbnail = d->absUrl(d->selectFirst("div.flex.items-start img"), "src");
        std::string xData;
        for (auto& el : d->select("[x-data]"))
            if (contains(el.attr("x-data"), "anmTitles")) {
                xData = el.attr("x-data");
                break;
            }
        std::string fallback = d->selectFirst("h1").text();
        if (fallback.empty()) fallback = substringBefore(d->selectFirst("title").text(), " — AniZone");
        det.title = preferredTitle(xData, fallback, "anmTitles");
        for (auto& s : d->select("span.inline-block")) {
            std::string t = lower(s.text());
            if (t == "completed") det.status = "Completato";
            else if (t == "ongoing") det.status = "In corso";
            else if (t == "upcoming" || t == "cancelled") det.status = "";
            else continue;
            break;
        }
        det.genre = joinText(d->select("a[href*=/tag/]"));
        for (auto& h3 : d->select("div > h3"))
            if (contains(h3.text(), "Synopsis")) {
                for (auto& c : h3.parent().children())
                    if (c.tag() == "div") {
                        det.description = c.text();
                        break;
                    }
                break;
            }

        // episodi: stato Livewire separato dalla lista degli anime
        std::lock_guard<std::mutex> lock(mtx);
        State st;
        st.slug = rel(abs(url));
        html::Node root = updateState(*d, st);
        std::string items = rawXDataArg(root, "items");
        std::vector<Episode> eps;
        std::set<std::string> seen;
        auto addItems = [&](const json& arr) {
            for (auto& it : arr) {
                Episode e;
                if (!episodeFrom(it, e)) continue;
                if (seen.insert(e.url).second) eps.push_back(e);
            }
        };
        addItems(parseXDataList(items, "summary"));
        bool hasMore = hasLoadMore(root);
        std::string cursor = xDataCursor(root);
        int guard = 0;
        while (hasMore && !cursor.empty() && guard++ < 50) {
            json resp = livewire(st, json::array({{{"path", ""}, {"method", "loadPage"}, {"params", json::array({cursor})}}}));
            json dispatch = itemsLoaded(resp);
            addItems(jget(jget(dispatch, "params"), "items"));
            std::string next = js(jget(dispatch, "params"), "nextCursor");
            const json& more = jget(jget(dispatch, "params"), "hasMore");
            hasMore = more.is_boolean() && more.get<bool>() && next != cursor && !next.empty();
            cursor = next;
        }
        // speciali (S01, Special, Recap) prima, poi gli episodi normali: ciascun gruppo dal piu' recente
        std::vector<Episode> specials, regular;
        for (auto& e : eps) {
            std::string base = lower(substringBefore(e.name, " - "));
            bool special = contains(base, "special") || contains(base, "recap");
            for (size_t i = 0; i + 1 < base.size() && !special; i++)
                if (base[i] == 's' && std::isdigit((unsigned char)base[i + 1])) special = true;
            (special ? specials : regular).push_back(e);
        }
        sortEpisodesDesc(specials);
        sortEpisodesDesc(regular);
        det.episodes = specials;
        det.episodes.insert(det.episodes.end(), regular.begin(), regular.end());
        return det;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::string pageUrl = abs(episodeUrl);
        auto d = doc(pageUrl);
        std::lock_guard<std::mutex> lock(mtx);
        State st;
        st.slug = rel(pageUrl);
        updateState(*d, st);
        struct Server {
            std::string name, id;
            bool isDefault;
        };
        std::vector<Server> servers;
        for (auto& btn : d->select("button")) {
            std::string click = btn.attr("wire:click");
            if (!contains(click, "setVideo")) continue;
            std::string id = digitsOnly(substringBefore(substringAfter(click, "setVideo("), ")"));
            std::string name = btn.selectFirst("div.text-lg").text();
            if (name.empty()) name = btn.text();
            servers.push_back({name, id.empty() ? "0" : id, btn.hasAttr("disabled")});
        }
        if (servers.empty()) servers.push_back({"AniZone", "0", true});
        std::vector<Video> out;
        for (auto& s : servers) {
            try {
                if (s.isDefault) {
                    append(out, extract(*d, s.name));
                } else {
                    json resp = livewire(st, json::array({{{"path", ""}, {"method", "setVideo"},
                                                           {"params", json::array({std::atoi(s.id.c_str())})}}}));
                    std::string htmlStr = componentHtml(resp, st);
                    html::Document vd(htmlStr, baseUrl());
                    append(out, extract(vd, s.name));
                }
            } catch (const std::exception&) {
            }
        }
        sortByQuality(out, "1080");
        return ensure(out);
    }

  private:
    struct State {
        std::string snapshot, slug;
    };
    std::mutex mtx;
    std::string token;
    State listState;
    std::string nextCursor;
    std::set<std::string> seenUrls;

    // ---- stato Livewire
    html::Node updateState(const html::Document& d, State& st) {
        std::string csrf = d.selectFirst("script[data-csrf]").attr("data-csrf");
        if (!csrf.empty()) token = csrf;
        for (auto& el : d.select("main > div, main > ul"))
            if (el.hasAttr("wire:snapshot")) {
                st.snapshot = el.attr("wire:snapshot");
                return el;
            }
        return d.selectFirst("body");
    }

    json livewire(State& st, const json& calls) {
        for (int attempt = 0; attempt < 2; attempt++) {
            if (st.snapshot.empty() || token.empty()) {
                auto d = doc(baseUrl() + st.slug);
                updateState(*d, st);
                if (token.empty()) throw http::Error("Failed to get csrf token");
            }
            json payload = {{"_token", token},
                            {"components", json::array({{{"snapshot", st.snapshot}, {"updates", json::object()}, {"calls", calls}}})}};
            http::Response r = req("POST", baseUrl() + "/livewire/update",
                                   {{"Accept", "*/*"},
                                    {"Content-Type", "application/json"},
                                    {"X-Livewire", ""},
                                    {"Origin", baseUrl()},
                                    {"Referer", baseUrl() + st.slug}},
                                   payload.dump());
            if (r.status == 419) {
                token.clear();
                st.snapshot.clear();
                continue;
            }
            check(r);
            json j = parseJson(r.body);
            const json& comps = jget(j, "components");
            if (comps.is_array() && !comps.empty()) {
                std::string snap = js(comps[0], "snapshot");
                if (!snap.empty()) st.snapshot = replaceAll(snap, "\\\"", "\"");
            }
            return j;
        }
        throw http::Error("Sessione AniZone scaduta");
    }

    static json itemsLoaded(const json& resp) {
        const json& comps = jget(resp, "components");
        if (!comps.is_array() || comps.empty()) return json();
        for (auto& dsp : jget(jget(comps[0], "effects"), "dispatches"))
            if (js(dsp, "name") == "items-loaded") return dsp;
        return json();
    }

    static std::string componentHtml(const json& resp, State&) {
        const json& comps = jget(resp, "components");
        if (!comps.is_array() || comps.empty()) return "";
        return replaceAll(replaceAll(js(jget(comps[0], "effects"), "html"), "\\\"", "\""), "\\n", "");
    }

    // ---- x-data di Alpine: key: JSON.parse('...')
    static std::string rawXDataArg(const html::Node& root, const std::string& key) {
        std::vector<html::Node> nodes = root.select("[x-data]");
        if (root.hasAttr("x-data")) nodes.insert(nodes.begin(), root);
        for (auto& el : nodes) {
            std::string x = el.attr("x-data");
            size_t p = 0;
            while ((p = x.find("JSON.parse('", p)) != std::string::npos) {
                size_t k = p;
                while (k > 0 && (x[k - 1] == ' ' || x[k - 1] == ':')) k--;
                size_t ks = k;
                while (ks > 0 && (std::isalnum((unsigned char)x[ks - 1]) || x[ks - 1] == '_')) ks--;
                size_t start = p + 12;
                size_t i = start;
                while (i < x.size() && x[i] != '\'') i += x[i] == '\\' ? 2 : 1;
                if (x.substr(ks, k - ks) == key && i <= x.size()) return x.substr(start, std::min(i, x.size()) - start);
                p = start;
            }
        }
        return "";
    }

    static std::string unescapeXData(std::string s) {
        s = replaceAll(s, "\\u0022", "\"");
        s = replaceAll(s, "\\u0026", "&");
        s = replaceAll(s, "\\'", "'");
        s = replaceAll(s, "\\/", "/");
        s = replaceAll(s, "\\\\", "\\");
        return s;
    }

    static std::string sanitizeEscapes(const std::string& s) {
        std::string out;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] != '\\') {
                out += s[i];
                continue;
            }
            char n = i + 1 < s.size() ? s[i + 1] : 0;
            if (n && std::strchr("\"\\/bfnrt", n)) {
                out += s[i];
                out += n;
                i++;
            } else if (n == 'u' && i + 5 < s.size() && std::isxdigit((unsigned char)s[i + 2]) && std::isxdigit((unsigned char)s[i + 3]) &&
                       std::isxdigit((unsigned char)s[i + 4]) && std::isxdigit((unsigned char)s[i + 5])) {
                out += s.substr(i, 6);
                i += 5;
            } else if (n == 'x' && i + 3 < s.size() && std::isxdigit((unsigned char)s[i + 2]) && std::isxdigit((unsigned char)s[i + 3])) {
                out += "\\u00" + s.substr(i + 2, 2);
                i += 3;
            } else if (n == '0') {
                out += "\\u0000";
                i++;
            }
            // altrimenti la barra non valida viene scartata
        }
        return out;
    }

    /** Divide "[{...},{...}]" negli oggetti di primo livello e li analizza uno per uno (scartando quelli rotti). */
    static json parseXDataList(const std::string& raw, const std::string& stripField = "") {
        json out = json::array();
        if (raw.empty()) return out;
        int depth = 0;
        size_t start = std::string::npos;
        for (size_t i = 0; i < raw.size(); i++) {
            if (raw[i] == '{') {
                if (depth == 0) start = i;
                depth++;
            } else if (raw[i] == '}') {
                depth--;
                if (depth == 0 && start != std::string::npos) {
                    std::string el = raw.substr(start, i - start + 1);
                    start = std::string::npos;
                    if (!stripField.empty()) {
                        std::string k = "\\u0022" + stripField + "\\u0022:\\u0022";
                        auto p = el.find(k);
                        if (p != std::string::npos) {
                            size_t vs = p + k.size();
                            auto e1 = el.find("\\u0022,\\u0022", vs);
                            auto e2 = el.find("\\u0022}", vs);
                            auto e = std::min(e1, e2);
                            if (e != std::string::npos) el = el.substr(0, vs) + el.substr(e);
                        }
                    }
                    json j = parseJson(sanitizeEscapes(unescapeXData(el)));
                    if (j.is_object()) out.push_back(j);
                }
            }
        }
        return out;
    }

    static std::string clean(std::string s) {
        s = replaceAll(s, "&amp;", "&");
        s = replaceAll(s, "&quot;", "\"");
        s = replaceAll(s, "&#039;", "'");
        s = replaceAll(s, "&#39;", "'");
        s = replaceAll(s, "\\/", "/");
        s = replaceAll(s, "`", "'");
        return trim(s);
    }

    static std::string titleFromList(const json& tl) {
        if (!tl.is_object()) return "";
        for (const char* k : {"1", "5"}) {
            std::string v = js(tl, k);
            if (!trim(v).empty()) return v;
        }
        return "";
    }

    std::string preferredTitle(const std::string& xData, const std::string& fallbackText, const std::string& key) {
        std::string fallback;
        // getTitle(..., 'X')
        auto p = xData.find("getTitle(");
        if (p != std::string::npos) {
            auto c = xData.find(",", p);
            auto q = c == std::string::npos ? c : xData.find('\'', c);
            auto e = q == std::string::npos ? q : xData.find('\'', q + 1);
            if (e != std::string::npos) fallback = clean(xData.substr(q + 1, e - q - 1));
        }
        if (fallback.empty()) fallback = fallbackText;
        std::string raw;
        {
            auto k = xData.find(key);
            if (k != std::string::npos) raw = substringBefore(afterOr(xData.substr(k), "JSON.parse('"), "')");
        }
        std::string t = titleFromList(parseJson(sanitizeEscapes(unescapeXData(raw))));
        return clean(t.empty() ? fallback : t);
    }

    static std::string relUrl(const std::string& u) {
        std::string s = replaceAll(u, "\\/", "/");
        if (startsWith(s, "http")) s = http::pathOf(s);
        while (!s.empty() && s[0] == '/') s.erase(0, 1);
        return s.empty() ? "" : "/" + s;
    }

    static bool animeFrom(const json& it, Anime& a) {
        const json& obj = jget(it, "anime").is_object() ? jget(it, "anime") : it;
        a.url = relUrl(js(obj, "url"));
        if (a.url.empty()) return false;
        a.title = titleFromList(jget(obj, "title_list"));
        if (a.title.empty()) a.title = js(obj, "main_title");
        a.thumbnail = js(it, "cover");
        if (a.thumbnail.empty()) a.thumbnail = js(it, "snapshot");
        if (a.thumbnail.empty()) a.thumbnail = js(it, "teaser");
        return !trim(a.title).empty();
    }

    static bool episodeFrom(const json& it, Episode& e) {
        e.url = relUrl(js(it, "url"));
        if (e.url.empty()) return false;
        std::string slug = js(it, "slug");
        std::string t = titleFromList(jget(it, "title_list"));
        e.name = "Episode " + slug + (!trim(t).empty() && t != "Unknown" ? " - " + t : "");
        e.number = toNumber(slug, -1);
        return true;
    }

    static bool hasLoadMore(const html::Node& root) {
        for (auto& d : root.select("div[x-intersect]"))
            if (contains(d.attr("x-intersect"), "loadMore")) return true;
        return false;
    }

    static std::string xDataCursor(const html::Node& root) {
        std::vector<html::Node> nodes = root.select("[x-data]");
        if (root.hasAttr("x-data")) nodes.insert(nodes.begin(), root);
        for (auto& el : nodes) {
            std::string x = el.attr("x-data");
            auto p = x.find("nextCursor:");
            if (p == std::string::npos) continue;
            p += 11;
            while (p < x.size() && x[p] == ' ') p++;
            if (p < x.size() && x[p] == '\'') {
                auto e = x.find('\'', p + 1);
                if (e != std::string::npos) return x.substr(p + 1, e - p - 1);
            }
        }
        return "";
    }

    /** Anime dalle schede HTML (quando la pagina non ha la lista JSON). */
    static void htmlAnimes(const html::Document& d, const html::Node& root, std::vector<Anime>& out) {
        for (auto& el : root.select(".grid > div, .grid > li, li.space-y-3")) {
            html::Node link;
            for (auto& a : el.select("a[href*=/anime/]")) {
                std::string path = trimChars(substringAfter(a.attr("href"), "/anime/"), "/");
                if (!path.empty() && !contains(path, "/")) {
                    link = a;
                    break;
                }
            }
            if (!link) link = el.selectFirst("a[href*=/anime/]");
            if (!link) continue;
            Anime a;
            a.url = rel(d.absUrl(link, "href"));
            html::Node span = el.selectFirst("span[x-text*=AnimeTitle]");
            a.title = span ? span.text() : link.attr("title");
            if (a.title.empty()) a.title = link.text();
            a.title = clean(a.title);
            a.thumbnail = d.absUrl(el.selectFirst("img"), "src");
            if (!a.url.empty() && !a.title.empty()) out.push_back(a);
        }
    }

    Page animeList(const std::string& slug, int page) {
        std::lock_guard<std::mutex> lock(mtx);
        std::vector<Anime> raw;
        bool hasMore = false;
        if (page == 1 || listState.slug != slug) {
            listState = State();
            listState.slug = slug;
            seenUrls.clear();
            nextCursor.clear();
            token.clear();
            auto d = doc(baseUrl() + slug);
            html::Node root = updateState(*d, listState);
            json items = parseXDataList(rawXDataArg(root, "items"));
            for (auto& it : items) {
                Anime a;
                if (animeFrom(it, a)) raw.push_back(a);
            }
            if (raw.empty()) htmlAnimes(*d, root, raw);
            nextCursor = xDataCursor(root);
            hasMore = !nextCursor.empty() || hasLoadMore(root);
        } else {
            if (nextCursor.empty()) return Page();
            json resp = livewire(listState, json::array({{{"path", ""}, {"method", "loadPage"}, {"params", json::array({nextCursor})}}}));
            json dispatch = itemsLoaded(resp);
            for (auto& it : jget(jget(dispatch, "params"), "items")) {
                Anime a;
                if (animeFrom(it, a)) raw.push_back(a);
            }
            if (raw.empty()) {
                std::string h = componentHtml(resp, listState);
                html::Document hd(h, baseUrl());
                htmlAnimes(hd, hd.selectFirst("body"), raw);
            }
            nextCursor = js(jget(dispatch, "params"), "nextCursor");
            const json& more = jget(jget(dispatch, "params"), "hasMore");
            hasMore = !nextCursor.empty() && (more.is_boolean() ? more.get<bool>() : true);
        }
        Page p;
        for (auto& a : raw)
            if (seenUrls.insert(a.url).second) p.animes.push_back(a);
        if (page > 1 && p.animes.empty() && !raw.empty()) {
            p.animes = raw;
            p.hasNextPage = false;
            return p;
        }
        p.hasNextPage = hasMore;
        return p;
    }

    std::vector<Video> extract(const html::Document& d, const std::string& hoster) {
        std::string src;
        std::vector<Video::Track> subs;
        for (auto& el : d.select("[x-data]")) {
            std::string x = el.attr("x-data");
            if (!contains(x, "vidstackPlayer")) continue;
            auto p = x.find("JSON.parse('");
            if (p == std::string::npos) break;
            size_t start = p + 12, i = start;
            while (i < x.size() && x[i] != '\'') i += x[i] == '\\' ? 2 : 1;
            std::string raw = x.substr(start, std::min(i, x.size()) - start);
            raw = replaceAll(replaceAll(replaceAll(replaceAll(raw, "\\u0022", "\""), "\\u0026", "&"), "\\'", "'"), "\\/", "/");
            json cfg = parseJson(raw);
            src = js(cfg, "src");
            for (auto& s : jget(cfg, "subtitles")) subs.push_back({replaceAll(js(s, "file"), "\\/", "/"), js(s, "title")});
            break;
        }
        if (src.empty()) {
            src = d.selectFirst("media-player").attr("src");
            for (auto& t : d.select("track[kind=subtitles]")) subs.push_back({replaceAll(t.attr("src"), "\\/", "/"), t.attr("label")});
        }
        if (src.empty()) return {};
        // come l'estensione: prima i sottotitoli inglesi, al massimo 2 tracce
        std::vector<Video::Track> chosen;
        for (auto& s : subs)
            if (containsCI(s.lang, "english") || containsCI(s.lang, "eng")) chosen.push_back(s);
        for (auto& s : subs) {
            if (chosen.size() >= 2) break;
            bool dup = false;
            for (auto& c : chosen)
                if (c.url == s.url) dup = true;
            if (!dup) chosen.push_back(s);
        }
        if (chosen.size() > 2) chosen.resize(2);
        return hlsVideos(src, baseUrl() + "/", hoster + " - ", chosen);
    }
};

}  // namespace

// ####################################################################################################

std::vector<std::shared_ptr<Source>> makeAsiaOtherSources() {
    std::vector<std::shared_ptr<Source>> list;
    // indonesiano
    list.push_back(std::make_shared<OtakuDesu>());
    list.push_back(std::make_shared<AnimposxSource>(AnimposxSource::Cfg{
        {"id.samehadaku", "Samehadaku", "https://v2.samehadaku.how", "id", false, true}, "daftar-anime-2", true}));
    list.push_back(std::make_shared<Kuramanime>());
    list.push_back(std::make_shared<Kuronime>());
    list.push_back(std::make_shared<AnimposxSource>(AnimposxSource::Cfg{
        {"id.oploverz", "Oploverz", "https://oploverz.media", "id", false, true}, "anime-list", false}));
    list.push_back(std::make_shared<NeoNime>());
    list.push_back(std::make_shared<NimeGami>());
    // cinese
    list.push_back(std::make_shared<Anime1>());
    list.push_back(std::make_shared<Xfani>());
    list.push_back(std::make_shared<Cycity>());
    list.push_back(std::make_shared<Iyf>());
    list.push_back(std::make_shared<Nivod>());
    list.push_back(std::make_shared<Xiaoxintv>());
    list.push_back(std::make_shared<Hanime1>());
    // coreano, serbo, ucraino, hindi
    list.push_back(std::make_shared<Aniweek>());
    list.push_back(std::make_shared<AnimeSrbija>());
    list.push_back(std::make_shared<UAKino>());
    list.push_back(std::make_shared<UFDub>());
    list.push_back(std::make_shared<YoMovies>());
    // multilingua
    list.push_back(std::make_shared<AnimeWorldIndia>(
        Info{"all.animeworldindia", "AnimeWorld India", "https://anime-world.co", "all", false, true}, "all", ""));
    list.push_back(std::make_shared<AnimeWorldIndia>(
        Info{"hi.animeworldindia", "AnimeWorld India (Hindi)", "https://anime-world.co", "hi", false, true}, "hi", "hindi"));
    list.push_back(std::make_shared<AniZone>());
    list.push_back(std::make_shared<AnimeOnsen>());
    list.push_back(std::make_shared<StreamingCommunity>(
        Info{"it.streamingcommunity.tv", "StreamingUnity (Tv)", "https://streamingunity.vip", "it", false, true}, "it", "tv"));
    list.push_back(std::make_shared<StreamingCommunity>(
        Info{"it.streamingcommunity.movie", "StreamingUnity (Movie)", "https://streamingunity.vip", "it", false, true}, "it", "movie"));
    list.push_back(std::make_shared<StreamingCommunity>(
        Info{"en.streamingcommunity.tv", "StreamingUnity (Tv)", "https://streamingunity.vip", "en", false, true}, "en", "tv"));
    list.push_back(std::make_shared<StreamingCommunity>(
        Info{"en.streamingcommunity.movie", "StreamingUnity (Movie)", "https://streamingunity.vip", "en", false, true}, "en", "movie"));
    return list;
}

}  // namespace src
