// Porting in C++ delle estensioni Aniyomi spagnole (src/es/*) che non usano i temi animestream/dooplay,
// piu' il tema multisrc "pelisplus" (PelisPlusHD, PelisPlusPH, PelisPlusTo).
// Gli estrattori degli hoster non presenti in extractors.hpp (YourUpload, Uqload, MixDrop, BurstCloud, Fastream,
// Upstream, Sendvid, Streamlare, Lulu, PixelDrain, Vudeo, GoodStream, MediaFire, VK, Amazon) sono qui sotto.

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <thread>

#include <gumbo.h>

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

/** Prima sequenza numerica (anche decimale) nel testo, -1 se assente. */
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

/** Decodifica le sequenze \uXXXX (unescapeJava) e \/ . */
std::string unescapeJs(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 5 < s.size() + 0 && s[i + 1] == 'u' && i + 6 <= s.size()) {
            unsigned cp = (unsigned)std::strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16);
            if (cp < 0x80) out += (char)cp;
            else if (cp < 0x800) {
                out += (char)(0xC0 | (cp >> 6));
                out += (char)(0x80 | (cp & 0x3F));
            } else {
                out += (char)(0xE0 | (cp >> 12));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
            }
            i += 5;
        } else if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '/') {
            out += '/';
            i++;
        } else {
            out += s[i];
        }
    }
    return out;
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

/** Tutti gli URL http(s) presenti nel testo (fetchUrls). */
std::vector<std::string> allUrls(const std::string& text) {
    std::vector<std::string> out;
    size_t pos = 0;
    while ((pos = text.find("http", pos)) != std::string::npos) {
        size_t p = pos + 4;
        if (p < text.size() && text[p] == 's') p++;
        if (text.compare(p, 3, "://") != 0) {
            pos += 4;
            continue;
        }
        size_t e = p + 3;
        while (e < text.size()) {
            unsigned char c = text[e];
            if (std::isalnum(c) || std::strchr("_-.,@?^=%&:/~+#", c)) e++;
            else break;
        }
        std::string u = text.substr(pos, e - pos);
        while (!u.empty() && std::strchr(".,:", u.back())) u.pop_back();
        out.push_back(u);
        pos = e;
    }
    return out;
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

/** Ordina: lingua preferita, server preferito, qualita' preferita, poi qualita' decrescente. */
void sortVideos(std::vector<Video>& v, const std::string& server, const std::string& langPref = "", int quality = 1080) {
    std::string q = std::to_string(quality);
    auto key = [&](const Video& x) {
        int l = !langPref.empty() && containsCI(x.title, langPref) ? 1 : 0;
        int s = !server.empty() && containsCI(x.title, server) ? 1 : 0;
        int qq = contains(x.title, q) ? 1 : 0;
        int h = x.quality ? x.quality : qualityOf(x.title);
        return std::make_tuple(l, s, qq, h);
    };
    std::stable_sort(v.begin(), v.end(), [&](const Video& a, const Video& b) { return key(a) > key(b); });
}

// =============================================================================================== hoster

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

std::vector<Video> burstCloud(const std::string& url, const std::string& prefix) {
    const std::string base = "https://www.burstcloud.co";
    http::Response r = http::request("GET", url, {{"Referer", base}});
    html::Document doc(r.body, r.finalUrl.empty() ? url : r.finalUrl);
    std::string fileId = doc.selectFirst("div#player").attr("data-file-id");
    if (fileId.empty()) return {};
    http::Headers ph = {{"Referer", doc.url()}, {"Content-Type", "application/x-www-form-urlencoded"}};
    http::Response pr = http::request("POST", base + "/file/play-request/", ph, "fileId=" + http::urlEncode(fileId));
    json j = json::parse(pr.body, nullptr, false);
    if (!j.is_object() || !j.contains("purchase") || !j["purchase"].is_object()) return {};
    std::string cdn = j["purchase"].value("cdnUrl", "");
    if (cdn.empty()) return {};
    return {simpleVideo(cdn, prefix + "BurstCloud", base)};
}

std::vector<Video> fastream(const std::string& url, const std::string& prefix) {
    const std::string base = "https://fastream.to";
    http::Headers h = {{"Referer", base + "/"}, {"Origin", base}};
    html::Document first(http::getText(url, h), url);
    std::string script;
    auto inputs = first.select("input[name]");
    if (!inputs.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5100));  // il sito richiede almeno 5 secondi
        std::string form;
        for (auto& in : inputs) form += (form.empty() ? "" : "&") + http::urlEncode(in.attr("name")) + "=" + http::urlEncode(in.attr("value"));
        http::Headers ph = h;
        ph.push_back({"Content-Type", "application/x-www-form-urlencoded"});
        http::Response r = http::request("POST", url, ph, form);
        html::Document doc(r.body, url);
        script = scriptAll(doc, {"jwplayer", "vplayer"});
    } else {
        script = scriptAll(first, {"jwplayer", "vplayer"});
    }
    if (script.empty()) return {};
    if (contains(script, "eval(function(")) script = unpacker::unpackAndCombine(script);
    std::string videoUrl = trim(substringBefore(substringAfter(script, "file:\""), "\""));
    if (!startsWith(videoUrl, "http")) return {};
    if (contains(videoUrl, ".m3u8")) return hlsVideos(videoUrl, base + "/", prefix + "Fastream:");
    return {simpleVideo(videoUrl, prefix + "Fastream", base + "/", {{"Origin", base}})};
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

std::vector<Video> streamlare(const std::string& url, const std::string& prefix) {
    std::string id = substringAfterLast(url, "/");
    http::Response r = http::request("POST", "https://slwatch.co/api/video/stream/get", {{"Content-Type", "application/json"}},
                                     "{\"id\":\"" + id + "\"}");
    std::string playlist = r.body;
    std::string type = substringBefore(substringAfter(playlist, "\"type\":\""), "\"");
    std::string pre = trim(prefix).empty() ? "" : trim(prefix) + " ";
    if (type == "hls") {
        std::string master = replaceAll(substringBefore(substringAfter(playlist, "\"file\":\""), "\""), "\\/", "/");
        return hlsVideos(master, "", pre + "Streamlare:");
    }
    std::vector<Video> out;
    const std::string sep = "\"label\":\"";
    auto parts = split(substringAfter(playlist, sep), sep);
    for (auto& it : parts) {
        std::string quality = substringBefore(it, "\",");
        std::string api = replaceAll(substringBefore(substringAfter(it, "\"file\":\""), "\","), "\\", "");
        if (!startsWith(api, "http")) continue;
        try {
            http::Response pr = http::request("POST", api, {}, "", 20);
            std::string videoUrl = pr.finalUrl.empty() ? api : pr.finalUrl;
            out.push_back(simpleVideo(videoUrl, pre + "Streamlare:" + quality));
        } catch (const std::exception&) {
        }
    }
    return out;
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

std::vector<Video> pixelDrain(const std::string& url, const std::string& prefix) {
    auto p = url.find("/u/");
    if (p == std::string::npos) return {simpleVideo(url, prefix + "PixelDrain")};
    std::string id = url.substr(p + 3);
    std::string api = "https://pixeldrain.com/api/file/" + id + "?download";
    return {simpleVideo(api, prefix + "PixelDrain")};
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

std::vector<Video> goodStream(const std::string& url, const std::string& name, const http::Headers& headers = {}) {
    html::Document doc(http::getText(url, headers), url);
    std::vector<Video> out;
    for (auto& s : doc.select("script")) {
        std::string d = s.data();
        if (!contains(d, "file") && !contains(d, "player")) continue;
        auto p = d.find("file: \"https://");
        if (p == std::string::npos) continue;
        std::string link = substringBefore(d.substr(p + 7), "\"");
        out.push_back(simpleVideo(link, name, headerValue(headers, "Referer")));
    }
    return out;
}

std::vector<Video> mediafire(const std::string& url, const std::string& prefix) {
    html::Document doc(http::getText(url), url);
    std::string dl = doc.selectFirst("a#downloadButton").attr("href");
    if (dl.empty()) return {};
    return {simpleVideo(dl, prefix + "MediaFire")};
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

std::vector<Video> amazon(const std::string& url, const std::string& prefix) {
    if (containsCI(url, "disable")) return {};
    html::Document doc(http::getText(url), url);
    std::string script = scriptWith(doc, {"var shareId"});
    if (script.empty()) return {};
    std::string shareId = substringBefore(substringAfter(script, "shareId = \""), "\"");
    std::string j1 = http::getText("https://www.amazon.com/drive/v1/shares/" + shareId +
                                   "?resourceVersion=V2&ContentType=JSON&asset=ALL");
    std::string epId = substringBefore(substringAfter(j1, "\"id\":\""), "\"");
    std::string j2 = http::getText("https://www.amazon.com/drive/v1/nodes/" + epId +
                                   "/children?resourceVersion=V2&ContentType=JSON&limit=200&sort=%5B%22kind+DESC%22%2C+%22modifiedDate+DESC%22%5D&asset=ALL&tempLink=true&shareId=" +
                                   shareId);
    std::string videoUrl = substringBefore(substringAfter(substringAfter(j2, "\"FOLDER\":"), "tempLink\":\""), "\"");
    if (!startsWith(videoUrl, "http")) return {};
    std::string name = contains(videoUrl, "&ext=es") ? "AmazonES" : "Amazon";
    if (contains(videoUrl, ".m3u8")) return hlsVideos(videoUrl, "", prefix + name + ":");
    return {simpleVideo(videoUrl, prefix + name)};
}

/**
 * Risolutore generico (serverVideoResolver delle estensioni spagnole): individua l'hoster dall'URL
 * (o dal nome del server) e chiama l'estrattore giusto. Gli hoster che richiedono una WebView
 * (UniversalExtractor) o un motore JS (VidGuard) non sono supportati e restituiscono una lista vuota.
 */
struct Convention {
    const char* key;
    std::vector<const char*> names;
};

const std::vector<Convention>& conventions() {
    static const std::vector<Convention> list = {
        {"voe", {"voe", "tubelessceliolymph", "simpulumlamerop", "urochsunloath", "nathanfromsubject", "yip.", "metagnathtuggers", "donaldlineelse"}},
        {"okru", {"ok.ru", "okru"}},
        {"filemoon", {"filemoon", "moonplayer", "moviesm4u", "files.im"}},
        {"amazon", {"amazon", "amz"}},
        {"uqload", {"uqload.", "uqload"}},
        {"mp4upload", {"mp4upload"}},
        {"pixeldrain", {"pixeldrain"}},
        {"player.zilla", {"player.zilla"}},
        {"filelions", {"filelions", "fviplions", "lion"}},
        {"streamwish", {"wishembed", "streamwish", "strwish", "wish", "kswplayer", "swhoi", "multimovies", "uqloads", "neko-stream", "swdyu", "iplayerhls", "streamgg", "sfastwish", "playerwish", "hlswish", "embedwish"}},
        {"doodstream", {"doodstream", "dood.", "ds2play", "doods.", "ds2video", "dooood", "d000d", "d0000d", "d-s.io", "dsvplay"}},
        {"streamlare", {"streamlare", "slmaxed"}},
        {"yourupload", {"yourupload", "upload"}},
        {"burstcloud", {"burstcloud", "burst"}},
        {"upstream", {"upstream"}},
        {"streamtape", {"streamtape", "stp", "stape", "shavetape"}},
        {"vidhide", {"ahvsh", "streamhide", "guccihide", "streamvid", "vidhide", "kinoger", "smoothpre", "dhtpre", "peytonepre", "earnvids", "ryderjet", "dintezuvio", "callistanise"}},
        {"vidguard", {"vembed", "guard", "listeamed", "bembed", "vgfplay"}},
        {"mixdrop", {"mixdrop", "mxdrop", "mdbekjwqa"}},
        {"lulu", {"luluvdo", "lulu"}},
        {"fastream", {"fastream"}},
        {"sendvid", {"sendvid"}},
        {"vudeo", {"vudeo"}},
        {"mediafire", {"mediafire"}},
        {"dailymotion", {"dailymotion"}},
        {"vk", {"vk.com", "vkvideo"}},
        {"goodstream", {"goodstream"}},
    };
    return list;
}

std::string matchServer(const std::string& url, const std::string& serverName = "") {
    std::string u = lower(url), s = lower(serverName);
    for (auto& c : conventions())
        for (auto* n : c.names)
            if (contains(u, n)) return c.key;
    if (!s.empty())
        for (auto& c : conventions())
            for (auto* n : c.names)
                if (contains(s, n)) return c.key;
    return "";
}

std::vector<Video> resolveByKey(const std::string& key, const std::string& url, const std::string& prefix, const std::string& siteUrl) {
    if (key == "voe") return voe(url, prefix);
    if (key == "okru") return okru(url, prefix);
    if (key == "filemoon") return moon(url, siteUrl, prefix + "Filemoon:");
    if (key == "amazon") return amazon(url, prefix);
    if (key == "uqload") return uqload(url, prefix);
    if (key == "mp4upload") return mp4upload(url, prefix);
    if (key == "pixeldrain") return pixelDrain(url, prefix);
    if (key == "player.zilla") {
        std::string m3u = replaceAll(url, "play/", "m3u8/");
        return hlsVideos(m3u, "", prefix + "HLS - ");
    }
    if (key == "filelions") {
        auto v = streamWish(url, prefix);
        if (v.empty()) v = vidHide(url, prefix);
        for (auto& x : v) x.title = replaceAll(replaceAll(x.title, "StreamWish", "FileLions"), "VidHide", "FileLions");
        return v;
    }
    if (key == "streamwish") return streamWish(url, prefix);
    if (key == "doodstream") return dood(replaceAll(url, "d-s.io", "dsvplay.com"), prefix);
    if (key == "streamlare") return streamlare(url, prefix);
    if (key == "yourupload") return yourUpload(url, prefix);
    if (key == "burstcloud") return burstCloud(url, prefix);
    if (key == "upstream") return upstream(url, prefix);
    if (key == "streamtape") return streamtape(url, prefix);
    if (key == "vidhide") return vidHide(url, prefix);
    if (key == "mixdrop") return mixDrop(url, prefix);
    if (key == "lulu") return lulu(url, prefix);
    if (key == "fastream") return fastream(url, prefix);
    if (key == "sendvid") return sendvid(url, prefix);
    if (key == "vudeo") return vudeo(url, prefix);
    if (key == "mediafire") return mediafire(url, prefix);
    if (key == "dailymotion") return dailymotion(url, prefix + "Dailymotion - ");
    if (key == "vk") return vk(url, prefix + "VK - ");
    if (key == "goodstream") return goodStream(url, prefix + "GoodStream");
    return {};  // vidguard / universal: non supportati
}

std::vector<Video> resolve(const std::string& rawUrl, const std::string& prefix, const std::string& siteUrl,
                           const std::string& serverName = "") {
    std::string url = fixUrl(trim(rawUrl));
    if (!startsWith(url, "http")) return {};
    std::string key = matchServer(url, serverName);
    if (key.empty()) return {};
    try {
        return resolveByKey(key, url, prefix, siteUrl);
    } catch (const std::exception&) {
        return {};
    }
}

/** Risolve una lista di (url, prefisso, nome server) ignorando gli errori dei singoli hoster. */
struct Link {
    std::string url;
    std::string prefix;
    std::string server;
};

std::vector<Video> resolveAll(const std::vector<Link>& links, const std::string& siteUrl) {
    std::vector<Video> out;
    std::set<std::string> seen;
    for (auto& l : links) {
        if (!seen.insert(l.prefix + "|" + l.url).second) continue;
        append(out, resolve(l.url, l.prefix, siteUrl, l.server));
    }
    return out;
}

// =============================================================================================== base

struct Info {
    const char* id;
    const char* name;
    const char* url;
    bool nsfw;
};

class EsSource : public Source {
  public:
    explicit EsSource(Info i) : info(i) {}
    std::string id() const override { return info.id; }
    std::string name() const override { return info.name; }
    std::string defaultBaseUrl() const override { return info.url; }
    std::string lang() const override { return "es"; }
    bool nsfw() const override { return info.nsfw; }

  protected:
    Info info;

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

    std::string fetch(const std::string& url, const http::Headers& extra = {}) const {
        http::Response r = req("GET", url, extra);
        if (r.status < 200 || r.status >= 300)
            throw http::Error(std::string(info.name) + " ha risposto HTTP " + std::to_string(r.status));
        return std::move(r.body);
    }

    std::string postForm(const std::string& url, const std::string& body, const http::Headers& extra = {}) const {
        http::Headers h = mergeHeaders({{"Content-Type", "application/x-www-form-urlencoded"}}, extra);
        http::Response r = req("POST", url, h, body);
        if (r.status < 200 || r.status >= 300)
            throw http::Error(std::string(info.name) + " ha risposto HTTP " + std::to_string(r.status));
        return std::move(r.body);
    }

    DocPtr doc(const std::string& url, const http::Headers& extra = {}) const {
        http::Response r = req("GET", url, extra);
        if (r.status < 200 || r.status >= 300)
            throw http::Error(std::string(info.name) + " ha risposto HTTP " + std::to_string(r.status));
        return std::make_unique<html::Document>(r.body, r.finalUrl.empty() ? url : r.finalUrl);
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
        std::string t = lower(s);
        if (contains(t, "finaliz") || contains(t, "conclu") || contains(t, "complet") || contains(t, "terminad")) return "Completato";
        if (contains(t, "emisi") || contains(t, "estreno") || contains(t, "en curso") || contains(t, "ongoing") ||
            contains(t, "por estrenar") || contains(t, "emision"))
            return "In corso";
        return "";
    }

    static std::vector<Video> ensure(std::vector<Video> v) {
        if (v.empty()) throw http::Error("Nessun video trovato");
        return v;
    }

    /** Film senza episodi: un unico episodio che punta alla pagina stessa. */
    static Episode movieEpisode(const std::string& url, const std::string& name = "Película") {
        Episode e;
        e.url = url;
        e.name = name;
        e.number = 1;
        return e;
    }

    static void sortEpisodesDesc(std::vector<Episode>& eps) {
        std::stable_sort(eps.begin(), eps.end(), [](const Episode& a, const Episode& b) { return a.number > b.number; });
    }
};

// =============================================================================================== utilita' (2)

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

json parseJson(const std::string& s) { return json::parse(s, nullptr, false); }

std::string jstr(const json& j, const char* key, const std::string& def = "") {
    if (!j.is_object() || !j.contains(key)) return def;
    const json& v = j[key];
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number()) return numStr(v.get<double>());
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    return def;
}

double jnum(const json& j, const char* key, double def = -1) {
    if (!j.is_object() || !j.contains(key)) return def;
    const json& v = j[key];
    if (v.is_number()) return v.get<double>();
    if (v.is_string()) return firstNumber(v.get<std::string>(), def);
    return def;
}

/** Valore di una chiave JS/JSON non quotata o quotata: key:"value" / key: 'value' / "key":"value". */
std::string jsString(const std::string& text, const std::string& key, size_t from = 0) {
    for (const std::string& k : {"\"" + key + "\"", key}) {
        size_t p = from;
        while ((p = text.find(k, p)) != std::string::npos) {
            size_t q = p + k.size();
            while (q < text.size() && std::isspace((unsigned char)text[q])) q++;
            if (q < text.size() && text[q] == ':') {
                q++;
                while (q < text.size() && std::isspace((unsigned char)text[q])) q++;
                if (q < text.size() && (text[q] == '"' || text[q] == '\'')) {
                    char c = text[q];
                    auto e = text.find(c, q + 1);
                    if (e != std::string::npos) return text.substr(q + 1, e - q - 1);
                }
            }
            p = q;
        }
    }
    return "";
}

std::string queryEnc(const std::string& q) { return http::urlEncode(trim(q)); }

std::string trimChars(std::string s, const std::string& chars) {
    while (!s.empty() && chars.find(s.front()) != std::string::npos) s.erase(s.begin());
    while (!s.empty() && chars.find(s.back()) != std::string::npos) s.pop_back();
    return s;
}

std::string pathSegment(const std::string& url, int idx) {
    auto parts = split(trimChars(substringBefore(http::pathOf(url), "?"), "/"), "/");
    if (idx < 0) idx += (int)parts.size();
    return idx >= 0 && idx < (int)parts.size() ? parts[idx] : "";
}

void reverseEps(std::vector<Episode>& e) { std::reverse(e.begin(), e.end()); }

// =============================================================================================== AnimeFLV

// FACTORY: out.push_back(std::make_shared<AnimeFlv>());
class AnimeFlv : public EsSource {
  public:
    AnimeFlv() : EsSource({"es.animeflv", "AnimeFLV", "https://www4.animeflv.net", false}) {}

    Page popular(int page) override { return browse(baseUrl() + "/browse?order=rating&page=" + std::to_string(page)); }
    Page latest(int) override {
        auto d = doc(baseUrl());
        return list(*d, "div.Container ul.ListEpisodios li a.fa-play", [&](const html::Node& el, Anime& a) {
            std::string href = replaceAll(d->absUrl(el, "href"), "/ver/", "/anime/");
            a.url = substringBeforeLast(href, "-");
            a.title = el.selectFirst("strong.Title").text();
            a.thumbnail = replaceAll(d->absUrl(el.selectFirst("span.Image img"), "src"), "thumbs", "covers");
        });
    }
    Page search(const std::string& q, int page) override {
        return browse(baseUrl() + "/browse?q=" + queryEnc(q) + "&page=" + std::to_string(page));
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details r;
        r.thumbnail = d->absUrl(d->selectFirst("div.AnimeCover div.Image figure img"), "src");
        r.title = d->selectFirst("div.Ficha.fchlt div.Container .Title").text();
        r.description = removeSurrounding(d->selectFirst("div.Description").text(), '"');
        r.genre = joinText(d->select("nav.Nvgnrs a"));
        std::string st = textOf(d->select("span.fa-tv"));
        r.status = contains(st, "En emision") ? "In corso" : contains(st, "Finalizado") ? "Completato" : "";
        std::string script = scriptWith(*d, {"var anime_info ="});
        if (!script.empty()) {
            json info = parseJson("[" + substringBefore(substringAfter(script, "var anime_info = ["), "];") + "]");
            std::string uri = info.is_array() && info.size() > 2 && info[2].is_string() ? info[2].get<std::string>() : "";
            std::string eps = balancedAfter(script, "var episodes =");
            json arr = parseJson(eps);
            if (arr.is_array() && !uri.empty()) {
                for (auto& e : arr) {
                    if (!e.is_array() || e.empty()) continue;
                    std::string n = e[0].is_number() ? numStr(e[0].get<double>()) : e[0].is_string() ? e[0].get<std::string>() : "";
                    if (n.empty()) continue;
                    Episode ep;
                    ep.url = "/ver/" + uri + "-" + n;
                    ep.name = "Episodio " + n;
                    ep.number = parseNumber(n, 0);
                    r.episodes.push_back(ep);
                }
            }
        }
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::string script = scriptWith(*d, {"var videos = {"});
        json j = parseJson(balancedAfter(script, "var videos ="));
        std::vector<Video> out;
        if (j.is_object() && j.contains("SUB") && j["SUB"].is_array()) {
            for (auto& s : j["SUB"]) {
                std::string title = jstr(s, "title"), code = jstr(s, "code");
                if (code.empty()) continue;
                try {
                    if (title == "Stape") append(out, streamtape(code, ""));
                    else if (title == "Okru") append(out, okru(code, ""));
                    else if (title == "YourUpload") append(out, yourUpload(code, ""));
                    else if (title == "SW") append(out, streamWish(code, "", baseHeaders()));
                    else append(out, resolve(code, "", baseUrl(), title));
                } catch (const std::exception&) {
                }
            }
        }
        sortVideos(out, "StreamWish", "", 720);
        return ensure(out);
    }

  protected:
    http::Headers baseHeaders() const override {
        return {{"Referer", baseUrl() + "/"}, {"Origin", baseUrl()}, {"User-Agent", DESKTOP_UA}};
    }

  private:
    Page browse(const std::string& url) {
        auto d = doc(url);
        return list(*d, "div.Container ul.ListAnimes li article", [&](const html::Node& el, Anime& a) {
            a.url = d->absUrl(el.selectFirst("div.Description a.Button"), "href");
            a.title = el.selectFirst("a h3").text();
            html::Node img = el.selectFirst("a div.Image figure img");
            a.thumbnail = img.attr("src").empty() ? img.attr("data-cfsrc") : d->absUrl(img, "src");
        }, "ul.pagination li a[rel=\"next\"]");
    }
};

// =============================================================================================== JKAnime

// FACTORY: out.push_back(std::make_shared<JkAnime>());
class JkAnime : public EsSource {
  public:
    JkAnime() : EsSource({"es.jkanime", "Jkanime", "https://jkanime.net", false}) {}

    Page popular(int page) override { return directory(baseUrl() + "/directorio?filtro=popularidad&p=" + std::to_string(page)); }
    Page latest(int page) override {
        if (page > 1) return directory(baseUrl() + "/directorio?p=" + std::to_string(page - 1));
        auto d = doc(baseUrl());
        Page p = list(*d, "div.trending_div div.custom_thumb_home a", [&](const html::Node& el, Anime& a) {
            a.url = trimChars(d->absUrl(el, "href"), "/");
            a.title = el.selectFirst("img").attr("alt");
            a.thumbnail = d->absUrl(el.selectFirst("img"), "src");
        });
        p.hasNextPage = true;
        return p;
    }
    Page search(const std::string& q, int page) override {
        if (page > 1) return {};
        std::string url = baseUrl() + "/buscar/" + http::urlEncode(replaceAll(trim(q), " ", "_"));
        auto d = doc(url);
        if (!startsWith(http::pathOf(d->url()), "/buscar")) {
            if (startsWith(http::pathOf(d->url()), "/directorio")) return parseDirectory(*d);
            return {};
        }
        Page p;
        for (auto& el : d->select("div.row div.row.page_directorio div.anime__item")) {
            html::Node a = el.selectFirst("div.anime__item__text a");
            if (!a) continue;
            Anime an;
            an.title = a.text();
            an.thumbnail = d->absUrl(el.selectFirst("div.g-0"), "data-setbg");
            an.url = rel(d->absUrl(a, "href"));
            if (!an.url.empty()) p.animes.push_back(an);
        }
        return p;
    }

    Details details(const std::string& url) override {
        std::string animeUrl = trimChars(abs(url), "/");
        http::Response resp = req("GET", animeUrl);
        if (resp.status < 200 || resp.status >= 300) throw http::Error("Jkanime ha risposto HTTP " + std::to_string(resp.status));
        html::Document d(resp.body, animeUrl);
        Details r;
        r.thumbnail = d.absUrl(d.selectFirst("div.anime__details__content div.anime_pic img"), "src");
        r.title = d.selectFirst("div.anime__details__content div.anime_info h3").text();
        r.description = d.selectFirst("div.anime__details__content div.anime_info p.scroll").text();
        for (auto& li : d.select("div.anime__details__content div.anime_data.pc li")) {
            std::string data = textOf(li.select("span"));
            if (contains(data, "Generos:")) r.genre = joinText(li.select("a"));
            if (contains(data, "Estado")) {
                std::string st = textOf(li.select("div"));
                r.status = contains(st, "Concluido") ? "Completato" : (contains(st, "En emision") || contains(st, "Por estrenar")) ? "In corso" : "";
            }
            if (contains(data, "Studios")) r.author = textOf(li.select("a"));
        }

        std::string token = d.selectFirst("meta[name=csrf-token]").attr("content");
        std::string animeId = d.selectFirst("div.anime__details__content div.pc div#guardar-anime").attr("data-anime");
        if (token.empty() || animeId.empty()) return r;
        std::string cookie;
        for (auto it = resp.headers.equal_range("set-cookie"); it.first != it.second; ++it.first)
            cookie += (cookie.empty() ? "" : " ") + substringBeforeLast(it.first->second, ";") + ";";
        http::Headers h = {{"X-Requested-With", "XMLHttpRequest"}, {"Referer", animeUrl}};
        if (!cookie.empty()) h.push_back({"Cookie", cookie});
        json j = parseJson(postForm(baseUrl() + "/ajax/episodes/" + animeId + "/1", "_token=" + http::urlEncode(token), h));
        if (!j.is_object()) return r;
        std::string base = rel(animeUrl);
        auto add = [&](int n) {
            Episode e;
            e.url = base + "/" + std::to_string(n);
            e.name = "Episodio " + std::to_string(n);
            e.number = n;
            r.episodes.push_back(e);
        };
        if (j.contains("data") && j["data"].is_array())
            for (auto& ep : j["data"]) add((int)jnum(ep, "number", 0));
        int from = (int)jnum(j, "from", 1), to = (int)jnum(j, "to", 0), total = (int)jnum(j, "total", 0);
        for (int i = to + 1; i <= total + from - 1; i++) add(i);
        reverseEps(r.episodes);
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::string script = scriptWith(*d, {"var video = [];"});
        if (script.empty()) throw http::Error("Nessun video trovato");
        struct L {
            std::string url, lang, name;
        };
        std::vector<L> links;
        auto langOf = [](int n) -> std::string { return n == 1 ? "[JAP]" : n == 3 ? "[LAT]" : n == 4 ? "[CHIN]" : ""; };
        bool isRemote = containsCI(script, "= remote+'");
        std::string jsServer = substringBefore(substringAfter(script, "var remote = '"), "'");
        std::string jsPath = substringBefore(substringAfter(script, "= remote+'"), "'");
        std::string jsLinks;
        try {
            if (isRemote && !jsServer.empty() && jsServer != script) jsLinks = http::getText(jsServer + jsPath);
            else jsLinks = balancedAfter(script, "var servers =");
            if (jsLinks.empty()) jsLinks = balancedAfter(script, "var servers=");
        } catch (const std::exception&) {
        }
        json arr = parseJson(jsLinks);
        if (arr.is_array())
            for (auto& it : arr) {
                std::string remote = jstr(it, "remote");
                if (remote.empty()) continue;
                links.push_back({base64Decode(remote), langOf((int)jnum(it, "lang", 0)), jstr(it, "server")});
            }
        for (auto& a : d->select("div.bg-servers a")) {
            std::string sid = a.attr("data-id");
            std::string cls = substringBefore(substringAfter(a.attr("class"), "lg_"), " ");
            std::string lang = cls.empty() ? "" : langOf(std::atoi(cls.c_str()));
            std::string u = substringBefore(substringAfter(script, "video[" + sid + "] = '<iframe class=\"player_conte\" src=\""), "\"");
            if (u == script) continue;
            u = replaceAll(u, "/jkokru.php?u=", "http://ok.ru/videoembed/");
            u = replaceAll(u, "/jkvmixdrop.php?u=", "https://mixdrop.ag/e/");
            u = replaceAll(u, "/jksw.php?u=", "https://sfastwish.com/e/");
            u = replaceAll(u, "/jk.php?u=", baseUrl() + "/");
            if (startsWith(u, "/")) u = baseUrl() + u;
            links.push_back({u, lang, a.text()});
        }

        static const std::vector<std::pair<const char*, std::vector<const char*>>> matching = {
            {"voe", {"voe", "tubelessceliolymph", "simpulumlamerop", "urochsunloath", "nathanfromsubject", "yip.", "metagnathtuggers", "donaldlineelse"}},
            {"okru", {"ok.ru", "okru"}},
            {"filemoon", {"filemoon", "moonplayer", "moviesm4u", "files.im"}},
            {"streamtape", {"streamtape", "stp", "stape", "shavetape"}},
            {"mixdrop", {"mixdrop", "mxdrop", "mdbekjwqa"}},
            {"streamwish", {"sfastwish", "wishembed", "streamwish", "strwish", "wish", "kswplayer", "swhoi", "multimovies", "uqloads", "neko-stream", "swdyu", "iplayerhls", "streamgg"}},
            {"doostream", {"d-s.io", "dsvplay"}},
            {"desuka", {"stream/jkmedia"}},
            {"nozomi", {"jkplayer/um2?", "um2.php", "nozomi"}},
            {"desu", {"jkplayer/um?", "um.php"}},
            {"magi", {"jkplayer/umv?"}},
            {"mega", {"mega.nz"}},
        };
        std::vector<Video> out;
        for (auto& l : links) {
            std::string lu = lower(l.url), matched;
            for (auto& m : matching) {
                for (auto* n : m.second)
                    if (contains(lu, n)) matched = m.first;
                if (!matched.empty()) break;
            }
            if (matched.empty()) matched = lower(l.name);
            std::string p = l.lang.empty() ? "" : l.lang + " ";
            try {
                if (matched == "okru") append(out, okru(l.url, p));
                else if (matched == "voe") append(out, voe(l.url, p));
                else if (matched == "filemoon") append(out, moon(l.url, baseUrl(), p + "Filemoon:"));
                else if (matched == "streamtape") append(out, streamtape(l.url, p));
                else if (matched == "mp4upload") append(out, mp4upload(l.url, p));
                else if (matched == "mixdrop") append(out, mixDrop(l.url, p));
                else if (matched == "streamwish") append(out, streamWish(l.url, p));
                else if (matched == "doostream" || matched == "doodstream")
                    append(out, dood(replaceAll(l.url, "d-s.io", "dsvplay.com"), p));
                else if (matched == "vidhide") append(out, vidHide(l.url, p + "- "));
                else if (matched == "mediafire") append(out, mediafire(l.url, p));
                else if (matched == "desuka") append(out, desuka(l.url, p));
                else if (matched == "nozomi") append(out, nozomi(l.url, p));
                else if (matched == "desu") append(out, dpPlayer(l.url, p + "Desu"));
                else if (matched == "magi") append(out, magi(l.url, p));
                else if (matched != "mega") append(out, resolve(l.url, p, baseUrl(), l.name));
            } catch (const std::exception&) {
            }
        }
        sortVideos(out, "Okru", "[JAP]", 1080);
        return ensure(out);
    }

  private:
    Page directory(const std::string& url) {
        auto d = doc(url);
        return parseDirectory(*d);
    }

    Page parseDirectory(const html::Document& d) {
        std::string script = scriptWith(d, {"var animes = "});
        json j = parseJson(balancedAfter(script, "var animes ="));
        Page p;
        if (!j.is_object() || !j.contains("data") || !j["data"].is_array()) return p;
        for (auto& a : j["data"]) {
            Anime an;
            an.url = rel(jstr(a, "url"));
            an.title = jstr(a, "title");
            an.thumbnail = jstr(a, "image");
            if (!an.url.empty()) p.animes.push_back(an);
        }
        p.hasNextPage = !jstr(j, "next_page_url").empty();
        return p;
    }

    std::vector<Video> dpPlayer(const std::string& url, const std::string& title) {
        html::Document d(http::getText(url), url);
        std::string s = scriptWith(d, {"new DPlayer({"});
        if (s.empty()) return {};
        std::string u = substringBefore(substringAfter(s, "url: '"), "'");
        if (!startsWith(u, "http")) return {};
        return {simpleVideo(u, title, u)};
    }

    std::vector<Video> desuka(const std::string& url, const std::string& p) {
        http::Response h = http::request("HEAD", url);
        if (startsWith(h.header("content-type"), "video/")) {
            std::string real = h.finalUrl.empty() ? url : h.finalUrl;
            return {simpleVideo(real, p + "Desuka", real)};
        }
        return dpPlayer(url, p + "Desuka");
    }

    std::vector<Video> magi(const std::string& url, const std::string& p) {
        html::Document d(http::getText(url), url);
        std::string u = d.selectFirst("source[src*=\".m3u8\"]").attr("src");
        if (u.empty()) return {};
        return {simpleVideo(u, p + "Magi", u)};
    }

    std::vector<Video> nozomi(const std::string& url, const std::string& p) {
        http::Headers h = {{"Referer", url}};
        html::Document d(http::getText(url, h), url);
        std::string key = d.selectFirst("form input[value]").attr("value");
        http::Headers ph = {{"Referer", url}, {"Content-Type", "application/x-www-form-urlencoded"}};
        http::Response r = http::request("POST", "https://jkanime.net/gsplay/redirect_post.php", ph, "data=" + key);
        std::string postKey = substringAfter(r.finalUrl, "player.html#");
        if (postKey.empty() || postKey == r.finalUrl) return {};
        http::Response r2 = http::request("POST", "https://jkanime.net/gsplay/api.php",
                                          {{"Content-Type", "application/x-www-form-urlencoded"}}, "v=" + postKey);
        std::string file = jstr(parseJson(r2.body), "file");
        if (file.empty()) return {};
        return {simpleVideo(file, p + "Nozomi", file)};
    }
};

// =============================================================================================== AnimeFenix

// FACTORY: out.push_back(std::make_shared<AnimeFenix>());
class AnimeFenix : public EsSource {
  public:
    AnimeFenix() : EsSource({"es.animefenix", "AnimeFenix", "https://animefenix2.tv", false}) {}
    bool supportsLatest() const override { return false; }

    Page popular(int page) override { return dir(baseUrl() + "/directorio/anime?p=" + std::to_string(page) + "&estado=2"); }
    Page latest(int page) override { return popular(page); }
    Page search(const std::string& q, int page) override {
        return dir(baseUrl() + "/directorio/anime?q=" + queryEnc(q) + "&p=" + std::to_string(page));
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details r;
        r.title = ownText(d->selectFirst("h1.text-4xl"));
        std::string st = lower(textOf(d->select(".relative .rounded")));
        r.status = contains(st, "finalizado") ? "Completato" : contains(st, "emision") ? "In corso" : "";
        r.description = d->selectFirst(".mb-6 p.text-gray-300").text();
        r.genre = joinText(d->select(".flex-wrap a"));
        r.thumbnail = imgUrl(*d, d->selectFirst("#anime_image"));
        for (auto& a : d->select(".divide-y li > a")) {
            Episode e;
            e.name = a.selectFirst(".font-semibold").text();
            e.number = parseNumber(trim(substringAfter(e.name, "Episodio")), 0);
            e.url = rel(d->absUrl(a, "href"));
            r.episodes.push_back(e);
        }
        sortEpisodesDesc(r.episodes);
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::string script = scriptWith(*d, {"var tabsArray"});
        std::vector<Link> links;
        std::vector<Video> out;
        auto parts = split(substringAfter(script, "<iframe"), "src='");
        for (size_t i = 1; i < parts.size(); i++) {
            std::string u = trim(substringAfter(substringBefore(parts[i], "'"), "redirect.php?id="));
            if (contains(u, "/stream/fl.php")) {
                std::string v = substringAfter(u, "/stream/fl.php?v=");
                try {
                    if (http::request("HEAD", v).status == 200) out.push_back(simpleVideo(v, "FireLoad", v));
                } catch (const std::exception&) {
                }
                continue;
            }
            links.push_back({u, "", ""});
        }
        append(out, resolveAll(links, baseUrl()));
        sortVideos(out, "Mp4Upload");
        return ensure(out);
    }

  private:
    Page dir(const std::string& url) {
        auto d = doc(url);
        Page p = list(*d, ".grid-animes li article a", [&](const html::Node& el, Anime& a) {
            a.url = d->absUrl(el, "href");
            a.title = el.selectFirst("p:not(.gray)").text();
            a.thumbnail = imgUrl(*d, el.selectFirst(".main-img img"));
        });
        p.hasNextPage = d->selectFirst(".right:not(.disabledd)").valid();
        return p;
    }
};

// =============================================================================================== AnimeAV1

// FACTORY: out.push_back(std::make_shared<AnimeAv1>());
class AnimeAv1 : public EsSource {
  public:
    AnimeAv1() : EsSource({"es.animeav1", "AnimeAv1", "https://animeav1.com", false}) {}

    Page popular(int page) override { return catalog(baseUrl() + "/catalogo?order=popular&page=" + std::to_string(page)); }
    Page latest(int page) override { return catalog(baseUrl() + "/catalogo?order=latest_released&page=" + std::to_string(page)); }
    Page search(const std::string& q, int page) override {
        return catalog(baseUrl() + "/catalogo?search=" + queryEnc(q) + "&page=" + std::to_string(page));
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details r;
        r.title = d->selectFirst("h1.line-clamp-2").text();
        r.description = d->selectFirst(".entry > p").text();
        r.genre = joinText(d->select("header > .items-center > a"));
        r.thumbnail = d->absUrl(d->selectFirst("img.object-cover"), "src");
        for (auto& s : d->select("header > .items-center.text-sm span")) {
            std::string t = s.text();
            if (contains(t, "Finalizado")) r.status = "Completato";
            else if (contains(t, "En emisión")) r.status = "In corso";
        }
        std::string script = scriptWith(*d, {"node_ids"});
        std::string base = rel(substringBefore(substringBefore(d->url(), "?"), "#"));
        size_t p = script.find("episodes:");
        if (p == std::string::npos) p = script.find("episodes :");
        if (p != std::string::npos) {
            auto lb = script.find('[', p), rb = script.find(']', lb == std::string::npos ? p : lb);
            if (lb != std::string::npos && rb != std::string::npos) {
                std::string arr = script.substr(lb, rb - lb);
                size_t q = 0;
                while ((q = arr.find("number:", q)) != std::string::npos) {
                    q += 7;
                    size_t e = q;
                    while (e < arr.size() && (std::isdigit((unsigned char)arr[e]) || arr[e] == '.')) e++;
                    std::string n = arr.substr(q, e - q);
                    if (n.empty()) continue;
                    Episode ep;
                    ep.name = "Episodio " + n;
                    ep.number = parseNumber(n, 0);
                    ep.url = base + "/" + n;
                    r.episodes.push_back(ep);
                }
            }
        }
        reverseEps(r.episodes);
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::string script = scriptWith(*d, {"node_ids"});
        std::vector<Link> links;
        std::set<std::string> seen;
        for (const char* type : {"DUB", "SUB"}) {
            std::string key = std::string(type) + ":[";
            size_t p = 0;
            while ((p = script.find(key, p)) != std::string::npos) {
                p += key.size();
                auto end = script.find(']', p);
                if (end == std::string::npos) break;
                std::string block = script.substr(p, end - p);
                size_t q = 0;
                while ((q = block.find("server:\"", q)) != std::string::npos) {
                    q += 8;
                    std::string server = block.substr(q, block.find('"', q) - q);
                    std::string u = substringBefore(substringBefore(substringAfter(block.substr(q), "url:\""), "\""), "?embed");
                    if (u.empty() || !seen.insert(std::string(type) + u).second) continue;
                    links.push_back({u, std::string(type) + " ", server});
                }
                p = end;
            }
        }
        std::vector<Video> out;
        for (auto& l : links) {
            std::string s = lower(l.server), u = lower(l.url);
            try {
                if (contains(u, "player.zilla")) {
                    std::string m3u = replaceAll(l.url, "play/", "m3u8/");
                    out.push_back(simpleVideo(m3u, l.prefix + "HLS", m3u));
                } else if (contains(s, "pixeldrain") || contains(s, "pdrain") || contains(u, "pixeldrain")) {
                    append(out, pixelDrain(l.url, l.prefix));
                } else {
                    append(out, resolve(l.url, l.prefix, baseUrl(), l.server));
                }
            } catch (const std::exception&) {
            }
        }
        sortVideos(out, "PixelDrain", "SUB", 1080);
        return ensure(out);
    }

  private:
    Page catalog(const std::string& url) {
        auto d = doc(url);
        Page p = list(*d, "article[class*=\"group/item\"]", [&](const html::Node& el, Anime& a) {
            a.url = d->absUrl(el.selectFirst("a"), "href");
            a.title = textOf(el.select("header h3"));
            a.thumbnail = d->absUrl(el.selectFirst(".bg-current img"), "src");
        });
        // ".pointer-events-none:not(.max-sm:hidden) ~ a": pagina corrente seguita da un altro link
        for (auto& cur : d->select(".pointer-events-none")) {
            if (contains(cur.attr("class"), "max-sm:hidden")) continue;
            bool after = false;
            for (auto& sib : cur.parent().children()) {
                if (sib.raw() == cur.raw()) after = true;
                else if (after && sib.tag() == "a") p.hasNextPage = true;
            }
        }
        return p;
    }
};

// =============================================================================================== helper selettori

/** "X.active ~ Y:has(a)": un fratello successivo di un elemento che soddisfa sel contiene "inner". */
bool siblingAfterHas(const html::Document& d, const std::string& sel, const std::string& inner) {
    for (auto& cur : d.select(sel)) {
        bool after = false;
        for (auto& sib : cur.parent().children()) {
            if (sib.raw() == cur.raw()) after = true;
            else if (after && (inner.empty() || sib.selectFirst(inner).valid())) return true;
        }
    }
    return false;
}

/** "a:has(span:containsOwn(text))" */
bool anyContainsText(const html::Document& d, const std::string& sel, const std::string& text) {
    for (auto& n : d.select(sel))
        if (contains(n.text(), text)) return true;
    return false;
}

// =============================================================================================== Latanime

// FACTORY: out.push_back(std::make_shared<Latanime>());
class Latanime : public EsSource {
  public:
    Latanime() : EsSource({"es.latanime", "Latanime", "https://latanime.org", false}) {}
    bool supportsLatest() const override { return false; }

    Page popular(int page) override { return grid(baseUrl() + "/emision?p=" + std::to_string(page)); }
    Page latest(int page) override { return popular(page); }
    Page search(const std::string& q, int page) override {
        return grid(baseUrl() + "/buscar?q=" + queryEnc(q) + "&p=" + std::to_string(page));
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details r;
        r.title = textOf(d->select("div.row > div > h2"));
        std::vector<std::string> g;
        for (auto& a : d->select("div.row > div > a"))
            if (a.selectFirst("div.btn")) g.push_back(a.text());
        r.genre = joinStr(g);
        r.description = d->selectFirst("div.row > div > p.my-2").text();
        r.thumbnail = imgUrl(*d, d->selectFirst("div.row img"));
        for (auto& a : d->select("div.row > div > div.row > div > a")) {
            Episode e;
            std::string t = a.text();
            e.number = parseNumber(trim(substringBefore(substringAfter(t, "Capitulo "), " ")), 0);
            e.name = replaceAll(t, "- ", "");
            e.url = rel(d->absUrl(a, "href"));
            r.episodes.push_back(e);
        }
        reverseEps(r.episodes);
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::vector<Link> links;
        for (auto& a : d->select("li#play-video > a.play-video")) {
            std::string u = base64Decode(a.attr("data-player"));
            std::string server = ownText(a);
            if (!u.empty()) links.push_back({u, server + " - ", server});
        }
        auto out = resolveAll(links, baseUrl());
        sortVideos(out, "", "", 1080);
        return ensure(out);
    }

  private:
    Page grid(const std::string& url) {
        auto d = doc(url);
        Page p;
        for (auto& el : d->select("div.row > div")) {
            html::Node a = el.selectFirst("a");
            html::Node h = el.selectFirst("div.seriedetails > h3");
            if (!a || !h) continue;
            Anime an;
            an.url = rel(d->absUrl(a, "href"));
            an.title = h.text();
            an.thumbnail = imgUrl(*d, el.selectFirst("img"));
            p.animes.push_back(an);
        }
        p.hasNextPage = siblingAfterHas(*d, "ul.pagination > li.active", "a");
        return p;
    }
};

// =============================================================================================== MonosChinos

// FACTORY: out.push_back(std::make_shared<MonosChinos>());
class MonosChinos : public EsSource {
  public:
    MonosChinos() : EsSource({"es.monoschinos", "MonosChinos", "https://monoschinos.st", false}) {}

    Page popular(int page) override { return grid(baseUrl() + "/animes?p=" + std::to_string(page)); }
    Page latest(int page) override {
        auto d = doc(page == 1 ? baseUrl() : baseUrl() + "?page=" + std::to_string(page));
        Page p = list(*d, "ul.row.row-cols-xl-4.row-cols-lg-4.row-cols-md-3.row-cols-2 > li.col.mb-4", [&](const html::Node& el, Anime& a) {
            html::Node link = el.selectFirst("a");
            if (!link) return;
            std::string slug = substringBefore(substringAfter(d->absUrl(link, "href"), "/ver/"), "?");
            auto p = slug.rfind("-episodio-");
            if (p != std::string::npos) slug = slug.substr(0, p);
            a.url = "/anime/" + slug + "-sub-espanol";
            a.title = el.selectFirst("h2.fs-5").text();
            a.thumbnail = img(*d, el.selectFirst("img.lazy"));
        });
        p.hasNextPage = anyContainsText(*d, ".pagination a span", "»");
        return p;
    }
    Page search(const std::string& q, int page) override {
        return grid(baseUrl() + "/buscar?q=" + queryEnc(q) + "&p=" + std::to_string(page));
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details r;
        r.title = d->selectFirst("h1.fs-2.text-capitalize.text-light").text();
        r.description = d->selectFirst("#profile-tab-pane .mb-3 p").text();
        r.genre = joinText(d->select("#profile-tab-pane .badge.bg-secondary"));
        r.thumbnail = img(*d, d->selectFirst(".d-none.d-sm-flex img.lazy"));
        for (auto& col : d->select(".col")) {
            bool ok = false;
            for (auto& m : col.select(".text-muted"))
                if (contains(m.text(), "Estado")) ok = true;
            if (!ok) continue;
            auto divs = col.select("div.ms-2 div");
            if (divs.empty()) continue;
            std::string st = divs.back().text();
            r.status = (st == "Estreno" || st == "En emisión") ? "In corso" : st == "Finalizado" ? "Completato" : "";
        }
        std::string referer = d->url();
        std::string ajax = d->selectFirst("section.caplist").attr("data-ajax");
        if (ajax.empty()) return r;
        if (!startsWith(ajax, "http")) ajax = baseUrl() + ajax;
        std::string csrf = d->selectFirst("meta[name='csrf-token']").attr("content");
        std::string slug;
        html::Node ver = d->selectFirst("a[href^='/ver/']");
        if (ver) slug = substringBefore(substringAfter(ver.attr("href"), "/ver/"), "-episodio-");
        else {
            slug = substringBefore(substringBefore(substringAfter(referer, "/anime/"), "?"), "#");
            if (endsWith(slug, "-sub-espanol")) slug = slug.substr(0, slug.size() - 12);
        }
        if (trim(slug).empty()) return r;
        for (int page = 1; page <= 200; page++) {
            std::string u = page == 1 ? ajax : ajax + (contains(ajax, "?") ? "&" : "?") + "page=" + std::to_string(page);
            json j;
            try {
                j = parseJson(postForm(u, "_token=" + http::urlEncode(csrf),
                                       {{"Referer", referer}, {"X-Requested-With", "XMLHttpRequest"},
                                        {"Accept", "application/json, text/javascript, */*; q=0.01"}}));
            } catch (const std::exception&) {
                break;
            }
            if (!j.is_object() || !j.contains("eps") || !j["eps"].is_array()) break;
            for (auto& e : j["eps"]) {
                std::string n = jstr(e, "num");
                double num = parseNumber(n, -1);
                if (n.empty() || num < 0) continue;
                Episode ep;
                std::string urlNum = num == (long long)num ? std::to_string((long long)num) : n;
                ep.name = "Episodio " + urlNum;
                ep.number = num;
                ep.url = "/ver/" + slug + "-episodio-" + urlNum;
                r.episodes.push_back(ep);
            }
            int per = (int)jnum(j, "perpage", 0);
            if (per == 0 || (int)j["eps"].size() < per) break;
        }
        sortEpisodesDesc(r.episodes);
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::vector<Link> links;
        for (auto& b : d->select("button.play-video[data-player]")) {
            std::string u = base64Decode(b.attr("data-player"));
            if (u.empty()) continue;
            std::string server = b.attr("data-server");
            if (server.empty()) server = b.text();
            links.push_back({u, "", server});
        }
        std::vector<Video> out;
        for (auto& l : links) {
            std::string s = lower(l.server);
            try {
                if (s == "lulu" || contains(s, "lulu")) append(out, lulu(l.url, ""));
                else if (contains(s, "filemoon")) append(out, moon(l.url, baseUrl(), "Filemoon:"));
                else if (contains(s, "dood")) append(out, dood(l.url, "DoodStream:"));
                else append(out, resolve(l.url, "", baseUrl(), l.server));
            } catch (const std::exception&) {
            }
        }
        sortVideos(out, "Filemoon");
        return ensure(out);
    }

  private:
    static std::string img(const html::Document& d, const html::Node& n) {
        if (!n) return "";
        for (const char* a : {"data-src", "data-lazy-src", "srcset", "src"}) {
            std::string v = trim(n.attr(a));
            if (v.empty() || contains(v, "anime.png")) continue;
            if (std::string(a) == "srcset") v = substringBefore(v, " ");
            return http::resolve(d.url(), v);
        }
        return "";
    }

    Page grid(const std::string& url) {
        auto d = doc(url);
        Page p = list(*d, "li.ficha_efecto a", [&](const html::Node& el, Anime& a) {
            a.title = el.selectFirst("h3").text();
            a.thumbnail = img(*d, el.selectFirst("img"));
            a.url = d->absUrl(el, "href");
        });
        p.hasNextPage = anyContainsText(*d, ".pagination a span", "»");
        return p;
    }
};

// =============================================================================================== AnimeID

// FACTORY: out.push_back(std::make_shared<AnimeId>());
class AnimeId : public EsSource {
  public:
    AnimeId() : EsSource({"es.animeid", "AnimeID", "https://www.animeid.tv", false}) {}

    Page popular(int page) override { return grid(baseUrl() + "/series?sort=views&pag=" + std::to_string(page)); }
    Page latest(int page) override { return grid(baseUrl() + "/series?sort=newest&pag=" + std::to_string(page)); }
    Page search(const std::string& q, int page) override {
        return grid(baseUrl() + "/buscar?q=" + queryEnc(q) + "&pag=" + std::to_string(page) + "&sort=");
    }

    Details details(const std::string& url) override {
        std::string pageUrl = abs(url);
        auto d = doc(pageUrl);
        Details r;
        r.thumbnail = d->absUrl(d->selectFirst("#anime figure img.cover"), "src");
        r.title = d->selectFirst("#anime section hgroup h1").text();
        r.description = removeSurrounding(d->selectFirst("#anime section p.sinopsis").text(), '"');
        r.genre = joinText(d->select("#anime section ul.tags li a"));
        std::string st = textOf(d->select("div.main div section div.status-left div.cuerpo div span"));
        r.status = contains(st, "En emisión") ? "In corso" : contains(st, "Finalizada") ? "Completato" : "";
        std::string animeId = d->selectFirst("#ord").attr("data-id");
        if (animeId.empty()) return r;
        http::Headers h = {{"Referer", pageUrl}, {"sec-fetch-site", "same-origin"}, {"x-requested-with", "XMLHttpRequest"},
                           {"User-Agent", "Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:96.0) Gecko/20100101 Firefox/96.0"},
                           {"Accept-Language", "es-MX,es-419;q=0.9,es;q=0.8,en;q=0.7"}};
        for (int page = 1; page <= 100; page++) {
            std::string body = fetch("https://www.animeid.tv/ajax/caps?id=" + animeId + "&ord=DESC&pag=" + std::to_string(page), h);
            if (contains(body, "<body>")) body = substringBefore(substringAfter(body, "<body>"), "</body>");
            json j = parseJson(body);
            if (!j.is_object() || !j.contains("list") || !j["list"].is_array() || j["list"].empty()) break;
            for (auto& c : j["list"]) {
                std::string n = jstr(c, "numero");
                Episode e;
                e.number = parseNumber(n, 0);
                e.name = "Episodio " + n;
                e.url = rel(jstr(c, "href"));
                if (!startsWith(e.url, "/")) e.url = "/" + e.url;
                r.episodes.push_back(e);
            }
        }
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::vector<Link> links;
        for (auto& tab : d->select("#partes div.container li.subtab")) {
            std::string tabId = tab.attr("data-tab-id");
            std::string server = trim(ownText(d->selectFirst("#mirrors [data-tab-id=\"" + tabId + "\"]")));
            for (auto& part : tab.select("div.parte")) {
                std::string data = replaceAll(unescapeJs(part.attr("data")), "\\", "");
                auto urls = allUrls(data);
                if (urls.empty()) continue;
                links.push_back({urls.front(), "", server});
            }
        }
        std::vector<Video> out;
        for (auto& l : links) {
            std::string u = lower(l.url), s = lower(l.server);
            try {
                if (contains(u, "shg") || contains(s, "shg")) append(out, streamtape(l.url, ""));
                else if (contains(u, "fviplions") || contains(s, "fviplions")) append(out, streamWish(l.url, ""));
                else append(out, resolve(l.url, "", baseUrl(), l.server));
            } catch (const std::exception&) {
            }
        }
        return ensure(out);
    }

  private:
    Page grid(const std::string& url) {
        auto d = doc(url);
        Page p = list(*d, "#result article.item", [&](const html::Node& el, Anime& a) {
            a.url = el.selectFirst("a").attr("href");
            if (!startsWith(a.url, "/") && !startsWith(a.url, "http")) a.url = "/" + a.url;
            a.title = el.selectFirst("a header").text();
            a.thumbnail = el.selectFirst("a figure img").attr("src");
        });
        auto lis = d->select("#paginas ul li");
        p.hasNextPage = lis.size() >= 2 && lis[lis.size() - 2].selectFirst("a").valid();
        return p;
    }
};

// =============================================================================================== helper (3)

std::vector<Video> solidFiles(const std::string& url, const std::string& prefix) {
    html::Document d(http::getText(url), url);
    std::vector<Video> out;
    for (auto& s : d.select("script")) {
        std::string data = s.data();
        if (!contains(data, "\"downloadUrl\":")) continue;
        std::string u = replaceAll(substringBefore(substringAfter(data, "\"downloadUrl\":"), ","), "\"", "");
        if (startsWith(trim(u), "http")) out.push_back(simpleVideo(trim(u), prefix + "SolidFiles", trim(u)));
    }
    return out;
}

/** Element.closest(selettore semplice ".classe" o "tag") */
html::Node closest(html::Node n, const std::string& cls) {
    while (n.valid()) {
        if (startsWith(cls, ".")) {
            std::string c = " " + n.attr("class") + " ";
            if (contains(c, " " + cls.substr(1) + " ")) return n;
        } else if (n.tag() == cls) {
            return n;
        }
        n = n.parent();
    }
    return {};
}

std::string hexToString(const std::string& hex) { return crypto::fromHex(trim(hex)); }

/** Formatta un numero come Float.toString() di Kotlin ("1.0", "1.5"). */
std::string kotlinFloat(double n) {
    std::string s = numStr(n);
    return contains(s, ".") ? s : s + ".0";
}

// =============================================================================================== AnimeLatinoHD

// FACTORY: out.push_back(std::make_shared<AnimeLatinoHd>());
class AnimeLatinoHd : public EsSource {
  public:
    AnimeLatinoHd() : EsSource({"es.animelatinohd", "AnimeLatinoHD", "https://www.animelatinohd.com", false}) {}

    Page popular(int) override { return parse(baseUrl() + "/animes/populares", "popular_today"); }
    Page latest(int page) override { return parse(baseUrl() + "/animes?page=" + std::to_string(page) + "&status=1", "data"); }
    Page search(const std::string& q, int page) override {
        return parse(baseUrl() + "/animes?page=" + std::to_string(page) + "&search=" + queryEnc(q), "data");
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        json data = pageData(*d);
        Details r;
        if (!data.is_object()) return r;
        r.title = jstr(data, "name");
        r.genre = joinStr(split(jstr(data, "genres"), ","));
        r.description = jstr(data, "overview");
        std::string st = jstr(data, "status");
        r.status = contains(st, "1") ? "In corso" : contains(st, "0") ? "Completato" : "";
        r.thumbnail = "https://image.tmdb.org/t/p/w600_and_h900_bestv2" + jstr(data, "poster");
        std::string slug = jstr(data, "slug");
        if (data.contains("episodes") && data["episodes"].is_array())
            for (auto& e : data["episodes"]) {
                double n = jnum(e, "number", 0);
                Episode ep;
                ep.number = n;
                ep.name = "Episodio " + kotlinFloat(n);
                ep.url = "/ver/" + slug + "/" + kotlinFloat(n);
                r.episodes.push_back(ep);
            }
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        json data = pageData(*d);
        std::vector<Video> out;
        if (!data.is_object() || !data.contains("players")) return ensure(out);
        std::vector<json> players;
        if (data["players"].is_array()) for (auto& p : data["players"]) players.push_back(p);
        else if (data["players"].is_object()) for (auto& p : data["players"].items()) players.push_back(p.value());
        for (auto& servers : players) {
            if (!servers.is_array()) continue;
            for (auto& item : servers) {
                std::string id = jstr(item, "id");
                if (id.empty()) continue;
                std::string language = jstr(item, "languaje") == "1" ? "[LAT]" : "[SUB]";
                try {
                    http::Response r = http::request("GET", "https://api.animelatinohd.com/stream/" + id,
                                                     {{"Referer", "https://www.animelatinohd.com/"}, {"upgrade-insecure-requests", "1"}});
                    std::string u = r.finalUrl;
                    std::string e = lower(u);
                    if (contains(e, "filemoon")) append(out, moon(u, baseUrl(), language + " Filemoon:"));
                    else if (contains(e, "filelions") || contains(e, "lion")) {
                        auto v = streamWish(u, language + " ");
                        for (auto& x : v) x.title = replaceAll(x.title, "StreamWish", "FileLions");
                        append(out, v);
                    } else if (contains(e, "streamtape")) append(out, streamtape(u, language + " "));
                    else if (contains(e, "dood")) append(out, dood(u, language + " "));
                    else if (contains(e, "okru") || contains(e, "ok.ru")) append(out, okru(u, language + " "));
                    else if (contains(e, "solidfiles")) append(out, solidFiles(u, language + " "));
                    else if (contains(e, "od.lk")) out.push_back(simpleVideo(u, language + "Od.lk", u));
                    else if (contains(e, "cldup.com")) out.push_back(simpleVideo(u, language + "CldUp", u));
                } catch (const std::exception&) {
                }
            }
        }
        sortVideos(out, "FileLions", "[LAT]", 1080);
        return ensure(out);
    }

  private:
    static json nextData(const html::Document& d) {
        for (auto& s : d.select("script")) {
            std::string data = s.data();
            if (contains(data, "{\"props\":{\"pageProps\":")) return parseJson(data);
        }
        return json();
    }
    static json pageData(const html::Document& d) {
        json j = nextData(d);
        if (!j.is_object() || !j.contains("props")) return json();
        json pp = j["props"].value("pageProps", json::object());
        return pp.is_object() && pp.contains("data") ? pp["data"] : json();
    }

    Page parse(const std::string& url, const char* key) {
        auto d = doc(url);
        Page p;
        json data = pageData(*d);
        if (data.is_object() && data.contains(key) && data[key].is_array())
            for (auto& it : data[key]) {
                Anime a;
                a.url = "/anime/" + jstr(it, "slug");
                a.thumbnail = "https://image.tmdb.org/t/p/w200" + jstr(it, "poster");
                a.title = jstr(it, "name");
                if (!a.title.empty()) p.animes.push_back(a);
            }
        for (auto& a : d->select("div[class*=\"Animes_paginate\"] a")) {
            auto sibs = a.parent().children();
            if (!sibs.empty() && sibs.back().raw() == a.raw() && a.selectFirst("svg")) p.hasNextPage = true;
        }
        return p;
    }
};

// =============================================================================================== MundoDonghua

// FACTORY: out.push_back(std::make_shared<MundoDonghua>());
class MundoDonghua : public EsSource {
  public:
    MundoDonghua() : EsSource({"es.mundodonghua", "MundoDonghua", "https://www.mundodonghua.com", false}) {}

    Page popular(int page) override { return grid(baseUrl() + "/lista-donghuas/" + std::to_string(page), false); }
    Page latest(int page) override { return grid(baseUrl() + "/lista-episodios/" + std::to_string(page), true); }
    Page search(const std::string& q, int page) override {
        if (page > 1) return {};
        return grid(baseUrl() + "/busquedas/" + http::urlEncode(trim(q)), false);
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details r;
        r.thumbnail = d->absUrl(d->selectFirst("div.md-detail-poster img"), "src");
        r.title = d->selectFirst("h1.md-detail-title").text();
        r.description = d->selectFirst("p.md-detail-synopsis").text();
        r.genre = joinText(d->select("div.md-genres-block a.md-genre-tag"));
        std::string st = lower(d->selectFirst("span.md-emision-badge").text());
        r.status = contains(st, "en emisión") ? "In corso" : contains(st, "finalizada") ? "Completato" : contains(st, "cancelada") ? "Cancellato" : "";
        for (auto& a : d->select("ul.md-episode-list li.md-episode-item a.md-ep-link")) {
            Episode e;
            std::string href = a.attr("href");
            e.number = parseNumber(substringAfterLast(trimChars(href, "/"), "/"), 0);
            e.name = "Episodio " + numStr(e.number);
            e.url = rel(d->absUrl(a, "href"));
            r.episodes.push_back(e);
        }
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::vector<Video> out;
        for (auto& s : d->select("script")) {
            std::string data = s.data();
            if (!contains(data, "eval(function(p,a,c,k,e")) continue;
            std::string un = unpacker::unpackAndCombine(data);
            if (un.empty()) continue;
            auto urls = allUrls(un);
            for (auto& u : urls) {
                try {
                    if (contains(un, "amagi_tab")) append(out, voe(u, ""));
                    if (contains(un, "fmoon_tab")) append(out, moon(u, baseUrl(), "Filemoon:"));
                    if (contains(un, "vhide_tab") && contains(u, "vidhide"))
                        append(out, vidHide(u, "", {{"Referer", baseUrl() + "/"}, {"Origin", baseUrl()}}));
                    if (contains(un, "swish_tab") && contains(u, "embedwish"))
                        append(out, streamWish(u, "", {{"Referer", baseUrl() + "/"}, {"Origin", baseUrl()}}));
                    if (contains(un, "asura_tab") && contains(u, "redirector"))
                        append(out, hlsVideos(u, baseUrl() + "/", "Asura:"));
                } catch (const std::exception&) {
                }
            }
        }
        return ensure(out);
    }

  private:
    Page grid(const std::string& url, bool latestMode) {
        auto d = doc(url);
        Page p = list(*d, "div.md-card-grid > div.md-card > a", [&](const html::Node& el, Anime& a) {
            a.url = d->absUrl(el, "href");
            if (latestMode) a.url = substringBeforeLast(replaceAll(a.url, "/ver/", "/donghua/"), "/");
            a.title = el.selectFirst(".md-card-title").text();
            a.thumbnail = d->absUrl(el.selectFirst("div.md-card-img img"), "src");
        }, "nav.md-pagination > a");
        return p;
    }
};

// =============================================================================================== VerAni.me

// FACTORY: out.push_back(std::make_shared<VerAnime>());
class VerAnime : public EsSource {
  public:
    VerAnime() : EsSource({"es.veranime", "VerAni.me", "https://verani.me", false}) {}

    Page popular(int page) override { return grid(baseUrl() + "/animes/page/" + std::to_string(page) + "/?orderby=popular"); }
    Page latest(int page) override { return grid(baseUrl() + "/animes/page/" + std::to_string(page) + "/"); }
    Page search(const std::string& q, int page) override {
        return grid(baseUrl() + "/page/" + std::to_string(page) + "/?s=" + queryEnc(q));
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details r;
        r.title = trim(d->selectFirst("h1").text());
        if (r.title.empty()) r.title = "Anime";
        r.description = d->selectFirst(".anime-hero-description, .sinopsis, .description, p.desc, .info p, .pelicula-overview p").text();
        std::vector<std::string> g;
        for (auto& a : d->select("a[href*=\"categoria\"]")) {
            std::string t = trim(a.text());
            if (std::find(g.begin(), g.end(), t) == g.end()) g.push_back(t);
        }
        r.genre = joinStr(g);
        std::string st;
        for (auto& lbl : d->select("div.anime-info-label")) {
            if (!contains(lbl.text(), "Estado")) continue;
            html::Node sp = lbl.selectFirst("span");
            st = sp ? sp.text() : lbl.text();
            break;
        }
        if (st.empty()) st = d->selectFirst(".status").text();
        st = lower(trim(replaceAll(replaceAll(st, "Estado", ""), "estado", "")));
        r.status = (st == "en emision" || st == "en emisión") ? "In corso" : st == "finalizado" ? "Completato" : "";
        if (r.status.empty() && contains(d->url(), "/pelicula/")) r.status = "Completato";
        r.thumbnail = d->selectFirst("meta[property=og:image]").attr("content");

        const std::string epSel = ".capitulo-card-link, a[href*=\"capitulo\"], a[href*=\"episodio\"]";
        auto groups = d->select(".temporada-group");
        if (!groups.empty()) {
            bool fmt = groups.size() > 1;
            for (auto& g2 : groups) {
                std::string prefix;
                int n = (int)firstNumber(g2.selectFirst(".temporada-badge, .temporada-name").text(), -1);
                if (fmt && n >= 0) {
                    prefix = std::string("S") + (n < 10 ? "0" : "") + std::to_string(n) + " ";
                }
                for (auto& a : g2.select(epSel)) addEp(*d, a, prefix, r.episodes);
            }
        } else {
            for (auto& a : d->select(epSel)) addEp(*d, a, "", r.episodes);
        }
        if (r.episodes.empty() && d->selectFirst(".iframe-wrapper")) r.episodes.push_back(movieEpisode(rel(d->url())));
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::vector<Video> out;
        for (auto& f : d->select("iframe[src], iframe[data-src]")) {
            std::string src = d->absUrl(f, "src");
            if (src.empty()) src = d->absUrl(f, "data-src");
            if (src.empty()) continue;
            std::string language;
            html::Node item = closest(f, ".iframe-item");
            if (item)
                for (auto& sp : item.select("span"))
                    if (contains(sp.text(), "Idioma:")) {
                        language = trim(substringAfter(sp.text(), "Idioma:"));
                        break;
                    }
            try {
                auto v = resolveVer(src);
                if (!language.empty())
                    for (auto& x : v) x.title = "[" + language + "] " + x.title;
                append(out, v);
            } catch (const std::exception&) {
            }
        }
        return ensure(out);
    }

  private:
    static void addEp(const html::Document& d, const html::Node& a, const std::string& prefix, std::vector<Episode>& eps) {
        std::string href = d.absUrl(a, "href");
        std::string text = trim(a.text());
        if (href.empty() || contains(href, "proximos-capitulos") || contains(" " + a.attr("class") + " ", " ver-ahora ") ||
            containsCI(text, "ver ahora"))
            return;
        Episode e;
        e.name = prefix + (text.empty() ? "Capítulo" : text);
        e.url = rel(href);
        std::string lt = lower(text);
        for (const char* k : {"capitulo", "episodio"}) {
            auto p = lt.find(k);
            if (p == std::string::npos) continue;
            std::string rest = trim(lt.substr(p + std::strlen(k)));
            if (!rest.empty() && std::isdigit((unsigned char)rest[0])) {
                e.number = std::atof(rest.c_str());
                break;
            }
        }
        if (e.number < 0)
            for (const char* k : {"capitulo-", "episodio-"}) {
                auto p = e.url.find(k);
                if (p != std::string::npos && p + std::strlen(k) < e.url.size() && std::isdigit((unsigned char)e.url[p + std::strlen(k)])) {
                    e.number = std::atof(e.url.c_str() + p + std::strlen(k));
                    break;
                }
            }
        eps.push_back(e);
    }

    std::vector<Video> resolveVer(const std::string& url) {
        std::string u = lower(url);
        auto any = [&](std::initializer_list<const char*> l) {
            for (auto* s : l)
                if (contains(u, s)) return true;
            return false;
        };
        if (any({"ok.ru", "okru"})) return okru(url, "");
        if (any({"filelions", "lion", "fviplions"})) {
            auto v = streamWish(url, "");
            for (auto& x : v) x.title = replaceAll(x.title, "StreamWish", "FileLions");
            return v;
        }
        if (any({"wishembed", "streamwish", "strwish", "wish"})) return streamWish(url, "");
        if (contains(u, "animeav1.uns.bio")) return unsBio(url, "");
        if (any({"vidhide", "streamhide", "guccihide", "streamvid"})) return vidHide(url, "");
        std::string host = lower(http::hostOf(url));
        if (host == "voe.sx" || endsWith(host, ".voe.sx")) return voe(url, "");
        if (host == "yourupload.com" || endsWith(host, ".yourupload.com")) return yourUpload(url, "");
        if (contains(u, "zilla-networks")) {
            if (!contains(url, "/play/")) return {};
            std::string base = substringBefore(url, "/play/");
            std::string id = substringBefore(substringAfter(url, "/play/"), "?");
            std::string m3u8 = base + "/m3u8/" + id;
            return {simpleVideo(m3u8, "Zilla-Networks", base + "/")};
        }
        if (contains(u, "mp4upload.com")) return mp4upload(url, "");
        if (contains(u, "pixeldrain.com")) return pixelDrain(url, "");
        return resolve(url, "", baseUrl());
    }

  public:
    /** Estrattore animeav1.uns.bio: payload esadecimale cifrato AES-CBC con chiave/IV fissi (derivati nel JS). */
    static std::vector<Video> unsBio(const std::string& url, const std::string& prefix) {
        std::string swarmId = substringAfter(url, "#");
        http::Headers h = {{"Referer", "https://animeav1.com/"}};
        std::string payload = trim(http::getText("https://animeav1.uns.bio/api/v1/video?id=" + swarmId, h));
        std::string plain = crypto::aesCbcDecrypt(crypto::fromHex(payload), "kiemtienmua911ca", "1234567890oiuytr");
        json j = parseJson(plain);
        std::vector<Video> out;
        std::string tt = jstr(j, "hlsVideoTiktok");
        if (!tt.empty()) out.push_back(simpleVideo("https://animeav1.uns.bio" + tt + "?v=1766826492", prefix + "Tiktok HLS", "https://animeav1.com/"));
        std::string cf = jstr(j, "cf");
        if (!cf.empty()) out.push_back(simpleVideo(cf, prefix + "Cloudflare HLS", "https://animeav1.com/"));
        std::string src = jstr(j, "source");
        if (!src.empty()) out.push_back(simpleVideo(src, prefix + "In-House HLS", "https://animeav1.com/"));
        return out;
    }

  private:
    Page grid(const std::string& url) {
        auto d = doc(url);
        Page p;
        std::set<std::string> seen;
        for (auto& el : d->select(".anime-card a, article a")) {
            std::string href = d->absUrl(el, "href");
            if (href.empty() || contains(href, "/page/") || contains(href, "/animes/") || !contains(href, "verani.me")) continue;
            Anime a;
            a.url = rel(href);
            html::Node h3 = el.selectFirst("h3");
            a.title = h3 ? trim(h3.text()) : trim(el.selectFirst("img").attr("alt"));
            if (a.title.empty()) a.title = "Anime";
            a.thumbnail = d->absUrl(el.selectFirst("img"), "src");
            if (seen.insert(a.url).second) p.animes.push_back(a);
        }
        p.hasNextPage = d->selectFirst(".pagination .next, a.next").valid();
        return p;
    }
};

// =============================================================================================== VerAnimes

// FACTORY: out.push_back(std::make_shared<VerAnimes>());
class VerAnimes : public EsSource {
  public:
    VerAnimes() : EsSource({"es.veranimes", "VerAnimes", "https://wwv.veranimes.net", false}) {}

    Page popular(int page) override { return grid(baseUrl() + "/animes?orden=desc&pag=" + std::to_string(page)); }
    Page latest(int page) override { return grid(baseUrl() + "/animes?estado=en-emision&orden=desc&pag=" + std::to_string(page)); }
    Page search(const std::string& q, int page) override {
        return grid(baseUrl() + "/animes?buscar=" + queryEnc(q) + "&pag=" + std::to_string(page));
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details r;
        r.title = trim(d->selectFirst(".ti h1").text());
        r.description = d->selectFirst(".r .tx p").text();
        r.genre = joinText(d->select(".gn li a"));
        r.thumbnail = d->absUrl(d->selectFirst(".info figure img"), "data-src");
        r.status = d->selectFirst(".em") ? "In corso" : d->selectFirst(".fi") ? "Completato" : "";
        for (auto& li : d->select(".info .u:not(.sp) > li")) {
            std::string t = li.text();
            if (contains(t, "Estudio")) r.author = trim(substringAfter(t, "Estudio(s):"));
        }
        std::string script = scriptWith(*d, {"var eps ="});
        std::string slug = d->selectFirst("*[data-sl]").attr("data-sl");
        json arr = parseJson(trim(substringBefore(substringAfter(script, "var eps = "), ";")));
        if (arr.is_array())
            for (auto& it : arr) {
                std::string n = it.is_string() ? it.get<std::string>() : it.is_number() ? numStr(it.get<double>()) : "";
                if (n.empty()) continue;
                Episode e;
                e.number = parseNumber(n, 0);
                e.name = "Episodio " + n;
                e.url = "/ver/" + slug + "-" + n;
                r.episodes.push_back(e);
            }
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        std::string pageUrl = abs(url);
        auto d = doc(pageUrl);
        std::string opt = d->selectFirst(".opt").attr("data-encrypt");
        std::string body = postForm(baseUrl() + "/process", "acc=opt&i=" + opt,
                                    {{"Content-Type", "application/x-www-form-urlencoded; charset=UTF-8"},
                                     {"Referer", d->url()}, {"X-Requested-With", "XMLHttpRequest"}});
        html::Document sd(body, baseUrl());
        std::vector<Video> out;
        for (auto& li : sd.select("li")) {
            std::string link = hexToString(li.attr("encrypt"));
            if (link.empty()) continue;
            std::string u = lower(link);
            try {
                if (contains(u, "ok.ru") || contains(u, "okru")) append(out, okru(link, ""));
                else if (contains(u, "filelions") || contains(u, "lion")) {
                    auto v = streamWish(link, "");
                    for (auto& x : v) x.title = replaceAll(x.title, "StreamWish", "FileLions");
                    append(out, v);
                } else if (contains(u, "wish")) append(out, streamWish(link, ""));
                else if (contains(u, "vidhide") || contains(u, "streamhide") || contains(u, "guccihide") || contains(u, "streamvid"))
                    append(out, vidHide(link, ""));
                else if (contains(u, "voe")) append(out, voe(link, ""));
                else if (contains(u, "upload")) append(out, yourUpload(link, ""));
                else append(out, resolve(link, "", baseUrl()));
            } catch (const std::exception&) {
            }
        }
        sortVideos(out, "Voe");
        return ensure(out);
    }

  private:
    Page grid(const std::string& url) {
        auto d = doc(url);
        return list(*d, "article.li figure a", [&](const html::Node& el, Anime& a) {
            a.url = d->absUrl(el, "href");
            a.title = el.attr("title");
            a.thumbnail = d->absUrl(el.selectFirst("img"), "data-src");
        }, ".pag li a[title*=Siguiente]");
    }
};

// =============================================================================================== helper (4)

/** Tutti gli src degli <iframe ...> contenuti in uno script (video[1] = '<iframe src="...">'). */
std::vector<std::string> iframeSrcs(const std::string& script) {
    std::vector<std::string> out;
    size_t p = 0;
    while ((p = script.find("<iframe", p)) != std::string::npos) {
        auto end = script.find('>', p);
        auto s = script.find("src=", p);
        p += 7;
        if (s == std::string::npos || (end != std::string::npos && s > end)) continue;
        s += 4;
        if (s >= script.size()) break;
        char q = script[s];
        if (q != '"' && q != '\'' && !(q == '\\' && s + 1 < script.size())) continue;
        if (q == '\\') {  // src=\"...\"
            s++;
            q = script[s];
        }
        auto e = script.find(q, s + 1);
        if (e == std::string::npos) break;
        std::string u = script.substr(s + 1, e - s - 1);
        if (!u.empty() && u.back() == '\\') u.pop_back();
        if (startsWith(u, "//")) u = "https:" + u;
        out.push_back(u);
    }
    return out;
}

/** Decifratura CryptoJS con sale esadecimale (CryptoAES.decryptWithSalt). */
std::string decryptWithSalt(const std::string& ctB64, const std::string& saltHex, const std::string& password) {
    try {
        std::string kv = crypto::evpBytesToKey(password, crypto::fromHex(saltHex), 32, 16);
        return crypto::aesCbcDecrypt(base64Decode(ctB64), kv.substr(0, 32), kv.substr(32, 16));
    } catch (const std::exception&) {
        return "";
    }
}

// =============================================================================================== ZeroAnime

// FACTORY: out.push_back(std::make_shared<ZeroAnime>());
class ZeroAnime : public EsSource {
  public:
    ZeroAnime() : EsSource({"es.zeroanime", "zeroanime", "https://www4.zeroanime.xyz", false}) {}
    bool supportsLatest() const override { return false; }

    Page popular(int page) override {
        return grid(baseUrl() + "/search?q=&letra=&genero=ALL&years=ALL&estado=2&orden=desc&p=" + std::to_string(page));
    }
    Page latest(int page) override { return popular(page); }
    Page search(const std::string& q, int page) override {
        return grid(baseUrl() + "/search?q=" + queryEnc(q) + "&p=" + std::to_string(page));
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details r;
        r.title = textOf(d->select("h1.htitle"));
        r.description = textOf(d->select("div.vraven_text.single"));
        r.genre = joinText(d->select("div.single_data div.list a"));
        r.thumbnail = d->absUrl(d->selectFirst("div.hentai_cover img"), "src");
        std::string st = lower(textOf(d->select("div.data")));
        r.status = contains(st, "emisión") ? "In corso" : contains(st, "finalizado") ? "Completato" : "";
        for (auto& li : d->select("li.hentai__chapter")) {
            Episode e;
            e.name = textOf(li.select("div.chapter_info span.title"));
            e.number = parseNumber(trim(substringAfter(e.name, "Episodio ")), 0);
            e.url = rel(d->absUrl(li.selectFirst("a"), "href"));
            r.episodes.push_back(e);
        }
        sortEpisodesDesc(r.episodes);
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::vector<Video> out;
        for (auto& b : d->select("button[id^=embed-]")) {
            std::string videoUrl = trim(b.attr("data-url"));
            if (videoUrl.empty()) continue;
            if (startsWith(videoUrl, "../redirect.php?")) videoUrl = baseUrl() + replaceAll(videoUrl, "../redirect.php?", "/redirect.php?");
            try {
                std::string finalUrl = refreshTarget(req("GET", videoUrl).header("refresh"));
                if (finalUrl.empty()) {
                    auto p = videoUrl.find("url=");
                    if (p != std::string::npos) finalUrl = videoUrl.substr(p + 4);
                }
                if (finalUrl.empty()) continue;
                if (startsWith(finalUrl, "../video/")) finalUrl = baseUrl() + replaceAll(finalUrl, "../video/", "/video/");
                std::string target = refreshTarget(req("GET", finalUrl).header("refresh"));
                if (target.empty()) continue;
                std::string e = lower(target);
                if (contains(e, "streamtape")) append(out, streamtape(target, ""));
                else if (contains(e, "filemoon")) append(out, moon(target, baseUrl(), "Filemoon:"));
                else if (contains(e, "mp4upload")) append(out, mp4upload(target, ""));
                else if (contains(e, "streamvid")) append(out, vidHide(target, ""));
                else append(out, resolve(target, "", baseUrl(), b.text()));
            } catch (const std::exception&) {
            }
        }
        sortVideos(out, "mp4upload");
        return ensure(out);
    }

  private:
    static std::string refreshTarget(const std::string& refresh) {
        std::string l = lower(refresh);
        auto p = l.find("url=");
        if (p == std::string::npos) return "";
        return trim(refresh.substr(p + 4));
    }

    Page grid(const std::string& url) {
        auto d = doc(url);
        return list(*d, "ul.animes.list-unstyled.row li.col-6.col-sm-4.col-md-3.col-xl-2", [&](const html::Node& el, Anime& a) {
            a.url = el.selectFirst("a").attr("href");
            a.title = textOf(el.select("div.title"));
            a.thumbnail = el.selectFirst("div.thumb img").attr("src");
        }, "ul.pagination li.page-item:not(.active) a");
    }
};

// =============================================================================================== Katanime

// FACTORY: out.push_back(std::make_shared<Katanime>());
class Katanime : public EsSource {
  public:
    Katanime() : EsSource({"es.katanime", "Katanime", "https://katanime.net", false}) {}

    Page popular(int) override { return grid(baseUrl() + "/populares"); }
    Page latest(int page) override {
        std::time_t t = std::time(nullptr);
        std::tm tm = *std::localtime(&t);
        return grid(baseUrl() + "/animes?fecha=" + std::to_string(tm.tm_year + 1900) + "&p=" + std::to_string(page));
    }
    Page search(const std::string& q, int page) override {
        return grid(baseUrl() + "/buscar?q=" + queryEnc(q) + "&p=" + std::to_string(page));
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details r;
        r.title = ownText(d->selectFirst(".comics-title"));
        r.description = ownText(d->selectFirst("#sinopsis p"));
        r.genre = joinText(d->select(".anime-genres a"));
        std::string st = lower(textOf(d->select(".details-by #estado")));
        r.status = contains(st, "finalizado") ? "Completato" : contains(st, "emision") ? "In corso" : "";
        r.thumbnail = imgUrl(*d, d->selectFirst("#animeinfo img, .comics-img img, img.lozad"));
        html::Node pag = d->selectFirst("._pagination");
        std::string token = d->selectFirst("[name=\"csrf-token\"]").attr("content");
        if (!pag || token.empty()) return r;
        std::string purl = d->absUrl(pag, "data-url");
        if (purl.empty()) purl = pag.attr("data-url");
        if (purl.empty()) return r;
        int pages = 1;
        for (int page = 1; page <= pages && page <= 100; page++) {
            try {
                json j = parseJson(postForm(purl, "_token=" + http::urlEncode(token) + "&pagina=" + std::to_string(page),
                                            {{"Origin", baseUrl()}, {"Referer", d->url()}}));
                json ep = j.is_object() && j.contains("ep") ? j["ep"] : json();
                if (!ep.is_object()) break;
                if (page == 1) {
                    if (ep.contains("last_page") && ep["last_page"].is_number()) pages = ep["last_page"].get<int>();
                    else {
                        double total = jnum(ep, "total", 1), per = jnum(ep, "per_page", 1e9);
                        pages = (int)std::ceil(total / per);
                    }
                }
                if (ep.contains("data") && ep["data"].is_array())
                    for (auto& e : ep["data"]) {
                        std::string u = jstr(e, "url");
                        if (u.empty()) continue;
                        Episode x;
                        std::string n = jstr(e, "numero");
                        x.name = n.empty() ? "Episodio" : "Episodio " + n;
                        x.number = parseNumber(n, 0);
                        x.url = rel(u);
                        r.episodes.push_back(x);
                    }
            } catch (const std::exception&) {
                break;
            }
        }
        reverseEps(r.episodes);
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::vector<Video> out;
        for (auto& el : d->select("[data-player]:not([data-player-name=\"Mega\"])")) {
            std::string server = trim(ownText(el));
            try {
                auto pd = doc(baseUrl() + "/reproductor?url=" + el.attr("data-player"));
                std::string s = scriptWith(*pd, {"var e ="});
                std::string enc = substringBefore(substringAfter(s, "var e = '"), "';");
                json j = parseJson(enc);
                std::string link = replaceAll(replaceAll(decryptWithSalt(jstr(j, "ct"), jstr(j, "s"), "hanabi"), "\\/", "/"), "\"", "");
                if (!startsWith(link, "http")) continue;
                std::string l = lower(link), sv = lower(server);
                auto has = [&](const char* k) { return contains(l, k) || contains(sv, k); };
                if (has("wish") || has("streamw") || has("swdyu") || has("iplayerhls")) append(out, streamWish(link, ""));
                else if (has("dood") || has("ds2play") || has("d000d")) append(out, dood(link, "DoodStream"));
                else if (has("streamtape") || has("stape") || has("shavetape")) append(out, streamtape(link, ""));
                else if (has("filemoon") || has("moonplayer")) append(out, moon(link, baseUrl(), "Filemoon:"));
                else if (has("mp4upload")) append(out, mp4upload(link, ""));
                else if (has("sendvid")) append(out, sendvid(link, ""));
                else if (has("lulu")) {
                    html::Document ld(http::getText(link), link);
                    std::string un = unpacker::unpackAndCombine(scriptWith(ld, {"eval"}));
                    std::string pl = substringBefore(substringAfter(un, "file:\""), "\"");
                    if (startsWith(pl, "http")) append(out, hlsVideos(pl, pl, "LuluStream:"));
                }
            } catch (const std::exception&) {
            }
        }
        sortVideos(out, "VidGuard");
        return ensure(out);
    }

  protected:
    http::Headers baseHeaders() const override {
        return {{"Referer", baseUrl() + "/"},
                {"User-Agent", "Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/135.0.0.0 Mobile Safari/537.36"}};
    }

  private:
    Page grid(const std::string& url) {
        auto d = doc(url);
        Page p = list(*d, "#article-div .full > a", [&](const html::Node& el, Anime& a) {
            a.url = d->absUrl(el, "href");
            a.title = el.selectFirst("img").attr("alt");
            a.thumbnail = imgUrl(*d, el.selectFirst("img"));
        });
        p.hasNextPage = false;
        for (auto& cur : d->select(".pagination .active")) {
            bool after = false;
            for (auto& sib : cur.parent().children()) {
                if (sib.raw() == cur.raw()) after = true;
                else if (after && sib.tag() == "li" && !contains(" " + sib.attr("class") + " ", " disabled ")) p.hasNextPage = true;
            }
        }
        return p;
    }
};

// =============================================================================================== LegionAnime

const char* LEGION_JSON =
    "{\"mob3\":\"wj2fea7esGZ44ef\",\"mob\":\"ca-app-pub-8704883736496335~2640452466\",\"mob2\":\"ca-app-pub-8704883736496335/7509635763\",\"laltx\":\"ca-app-pub-7457591504273346/96408526573970637unleinunleba\",\"language\":\"es\",\"isDeb\":false,\"mobx\":\"ca-app-pub-7457591504273346~4714978974\",\"loadLvl\":1,\"code_name\":\"emu64xa\",\"vcode\":\"2.0.2.6\",\"platform\":\"13\",\"lalt\":\"ca-app-pub-8704883736496335/71217888083970637unleinunleba\",\"kind_device\":\"0\",\"manufacturer\":\"Google\",\"som\":\"android\",\"device_name\":\"Sdk_gphone64_x86_64\",\"player_id\":\"50dfea02-9f24-4116-902b-a726146da421\",\"mobf\":\"ca-app-pub-8704883736496335/2195575817\",\"ipv6\":\"FE80::6898:34FF:FE32:E13E\",\"root\":\"0\",\"eth\":\"02:00:00:00:00:00\",\"tel\":\"XXX-XXX-XXX\",\"UUID\":\"00000000-0001-a657-0001-aad30001b11d\",\"yek\":\"bqUgI4l339bqQbnz\",\"moned\":false,\"lvl_sign\":1,\"mobfx\":\"ca-app-pub-7457591504273346/1377852271\",\"device_id\":\"\",\"orp\":false,\"modelo\":\"sdk_gphone64_x86_64\",\"market_name\":\"Sdk_gphone64_x86_64\",\"token\":\"es\",\"isSign\":true,\"malt\":\"ca-app-pub-8704883736496335/7121788808\",\"maltx\":\"ca-app-pub-7457591504273346/9640852657\",\"api_lvl\":\"33\",\"package_version\":\"50\",\"kind_release\":0,\"ipLocal\":\"10.0.2.15\",\"mob2x\":\"ca-app-pub-7457591504273346/9640852657\",\"wlan\":\"02:00:00:00:00:00\",\"inmo\":\"a3c41c881e7f4bc982db32a889eb9e57\",\"inst\":\"\",\"package_name\":\"aplicaciones.paleta.legionanimefull\",\"androidID\":\"b70a95e4fda18f3c\"}";

// FACTORY: out.push_back(std::make_shared<LegionAnime>());
class LegionAnime : public EsSource {
  public:
    LegionAnime() : EsSource({"es.legionanime", "LegionAnime", "https://legionanime.club/api", false}) {}

    Page popular(int page) override { return directory("4", "", page, true); }
    Page latest(int page) override { return directory("2", "", page, true); }
    Page search(const std::string& q, int page) override { return directory("", queryEnc(q), page, false); }

    Details details(const std::string& url) override {
        json j = parseJson(fetch(abs(url)));
        Details r;
        json resp = j.is_object() && j.contains("response") ? j["response"] : json();
        json anime = resp.is_object() && resp.contains("anime") ? resp["anime"] : json();
        r.title = jstr(anime, "name");
        r.description = jstr(anime, "synopsis");
        r.genre = jstr(anime, "genres");
        std::string st = jstr(anime, "status");
        r.status = st == "En emisión" ? "In corso" : st == "Finalizado" ? "Completato" : "";
        std::string mal = jstr(anime, "mal_id");
        if (!mal.empty()) {
            try {
                json jk = parseJson(http::getText("https://api.jikan.moe/v4/anime/" + mal));
                r.thumbnail = jk["data"]["images"]["jpg"].value("large_image_url", "");
            } catch (const std::exception&) {
            }
        }
        if (resp.is_object() && resp.contains("episodes") && resp["episodes"].is_array())
            for (auto& e : resp["episodes"]) {
                Episode ep;
                std::string n = jstr(e, "name");
                ep.name = "Episodio " + n;
                ep.number = firstNumber(n, -1);
                ep.url = "/v2/episode_links/" + jstr(e, "id");
                r.episodes.push_back(ep);
            }
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        json j = parseJson(postForm(abs(url), "apyki=pM7VYr2bBG2plWQp"));
        std::vector<Video> out;
        json resp = j.is_object() && j.contains("response") ? j["response"] : json();
        if (!resp.is_object() || !resp.contains("players") || !resp["players"].is_array()) return ensure(out);
        for (auto& p : resp["players"]) {
            std::string server = jstr(p, "option");
            std::string pre = jstr(p, "name");
            std::string u = substringAfter(pre, "-");
            if (!startsWith(pre, "F-")) std::reverse(u.begin(), u.end());
            try {
                if (contains(u, "streamwish")) append(out, streamWish(u, "StreamWish"));
                else if (contains(u, "mediafire")) {
                    auto v = mediafire(u, "");
                    for (auto& x : v) x.title = server + "-MediaFire";
                    append(out, v);
                } else if (contains(u, "streamtape")) {
                    auto v = streamtape(u, "");
                    for (auto& x : v) x.title = server;
                    append(out, v);
                } else if (contains(u, "jkanime")) {
                    html::Document dd(http::getText(u), u);
                    std::string s = scriptWith(dd, {"var parts = {"});
                    std::string su = substringBefore(substringAfter(s, "url: '"), "'");
                    if (startsWith(su, "http")) out.push_back(simpleVideo(su, "Desu", su));
                } else if (contains(u, "/stream/amz.php?")) {
                    std::string au = replaceAll(u, ".com", ".tv");
                    html::Document dd(http::getText(au), au);
                    std::string s = scriptWith(dd, {"sources: ["});
                    std::string vu = replaceAll(substringBefore(substringAfter(s, "[{\"file\":\""), "\","), "\\", "");
                    if (startsWith(vu, "http")) out.push_back(simpleVideo(vu, server, vu));
                } else if (contains(u, "yourupload")) append(out, yourUpload(u, ""));
                else if (contains(u, "mp4upload")) append(out, mp4upload(u, ""));
                else if (contains(u, "dood")) append(out, dood(u, ""));
                else if (contains(u, "ok.ru")) append(out, okru(u, ""));
                else if (startsWith(u, "http") && contains(u, "flvvideo") && (endsWith(u, ".m3u8") || endsWith(u, ".mp4")))
                    out.push_back(simpleVideo(u, "VideoFLV", u));
                else if (startsWith(u, "http") && contains(u, "cdnlat4animecen") &&
                         (endsWith(u, ".class") || endsWith(u, ".m3u8") || endsWith(u, ".mp4")))
                    out.push_back(simpleVideo(u, "AnimeCen", u));
                else if (contains(u, "uqload")) append(out, uqload(u, ""));
            } catch (const std::exception&) {
            }
        }
        return ensure(out);
    }

  protected:
    http::Headers baseHeaders() const override {
        return {{"json", LEGION_JSON}, {"User-Agent", "android l3gi0n4N1mE %E6%9C%AC%E7%89%A9"}};
    }

  private:
    Page directory(const std::string& orderBy, const std::string& query, int page, bool next) {
        std::string url = baseUrl() + "/v2/directories?studio=0&not_genre=&year=&orderBy=" + orderBy +
                          "&language=&type=&duration=&search=" + query + "&letter=0&limit=24&genre=&season=&page=" +
                          std::to_string((page - 1) * 24) + "&status=";
        json j = parseJson(postForm(url, "apyki=pM7VYr2bBG2plWQp"));
        Page p;
        if (!j.is_object() || !j.contains("response") || !j["response"].is_array()) return p;
        static const char* aip[] = {"https://la-space-4.sfo2.digitaloceanspaces.com/", "https://la-space-5.sfo2.digitaloceanspaces.com/"};
        int i = 0;
        for (auto& a : j["response"]) {
            Anime an;
            an.title = jstr(a, "nombre");
            an.url = "/v1/episodes/" + jstr(a, "id");
            an.thumbnail = std::string(aip[(i++) % 2]) + jstr(a, "img_url");
            p.animes.push_back(an);
        }
        p.hasNextPage = next && !p.animes.empty();
        return p;
    }
};

// =============================================================================================== AnimeBum

// FACTORY: out.push_back(std::make_shared<AnimeBum>());
class AnimeBum : public EsSource {
  public:
    AnimeBum() : EsSource({"es.animebum", "AnimeBum", "https://www.animebum.net", false}) {}
    bool supportsLatest() const override { return false; }

    Page popular(int page) override {
        auto d = doc(baseUrl() + "/series?page=" + std::to_string(page));
        return list(*d, "article.serie", [&](const html::Node& el, Anime& a) {
            html::Node t = el.selectFirst("div.title h3 a");
            a.title = t.attr("title").empty() ? "Sin título" : t.attr("title");
            a.url = t.attr("href");
            a.thumbnail = el.selectFirst("figure.image img").attr("src");
        }, "ul.pagination li a[rel=next]");
    }
    Page latest(int page) override { return popular(page); }
    Page search(const std::string& q, int page) override {
        auto d = doc(baseUrl() + "/search?s=" + queryEnc(q) + "&page=" + std::to_string(page));
        return list(*d, "div.search-results__item", [&](const html::Node& el, Anime& a) {
            a.title = el.selectFirst("div.search-results__left a h2").text();
            a.url = el.selectFirst("div.search-results__left a").attr("href");
            a.thumbnail = el.selectFirst("div.search-results__img a img").attr("src");
        }, "a.next.page-numbers");
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details r;
        r.title = d->selectFirst("h1").text();
        r.description = d->selectFirst("div.description p").text();
        if (r.description.empty()) r.description = "Sin sinopsis";
        html::Node st = d->selectFirst("p.datos-serie strong.emision");
        if (!st) st = d->selectFirst("p.datos-serie strong.fin");
        std::string s = st.text();
        r.status = s == "En emisión" ? "In corso" : s == "Finalizado" ? "Completato" : "";
        r.genre = joinText(d->select("div.boom-categories a"));
        r.thumbnail = imgUrl(*d, d->selectFirst("figure.image img, div.poster img"));
        for (auto& li : d->select("ul.list-episodies li")) {
            html::Node a = li.selectFirst("a");
            Episode e;
            e.name = trim(ownText(a));
            e.url = rel(a.attr("href"));
            e.number = 1;
            auto p = e.name.find("Episodio ");
            if (p != std::string::npos && p + 9 < e.name.size() && std::isdigit((unsigned char)e.name[p + 9]))
                e.number = std::atof(e.name.c_str() + p + 9);
            r.episodes.push_back(e);
        }
        sortEpisodesDesc(r.episodes);
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::string script = scriptWith(*d, {"var video = []"});
        std::vector<Video> out;
        for (auto& u : iframeSrcs(script)) {
            std::string l = lower(u);
            try {
                if (contains(l, "vidhide") || contains(l, "luluvdo")) append(out, vidHide(u, ""));
                else if (contains(l, "streamwish")) append(out, streamWish(u, ""));
                else if (contains(l, "ok.ru")) append(out, okru(u, ""));
            } catch (const std::exception&) {
            }
        }
        sortVideos(out, "Voe");
        return ensure(out);
    }
};

// =============================================================================================== helper (5)

/** Testo del nodo fratello successivo (Element.nextSibling().toString() per un nodo di testo). */
std::string nextSiblingText(const html::Node& n) {
    GumboNode* g = n.raw();
    if (!g || !g->parent) return "";
    GumboVector& ch = g->parent->v.element.children;
    size_t idx = g->index_within_parent + 1;
    if (idx >= ch.length) return "";
    auto* s = (GumboNode*)ch.data[idx];
    if (s->type == GUMBO_NODE_TEXT || s->type == GUMBO_NODE_WHITESPACE) return trim(s->v.text.text);
    return "";
}

/** Nodo di testo precedente (previousSibling().toString()). */
std::string prevSiblingText(const html::Node& n) {
    GumboNode* g = n.raw();
    if (!g || !g->parent || g->index_within_parent == 0) return "";
    GumboVector& ch = g->parent->v.element.children;
    auto* s = (GumboNode*)ch.data[g->index_within_parent - 1];
    if (s->type == GUMBO_NODE_TEXT || s->type == GUMBO_NODE_WHITESPACE) return trim(s->v.text.text);
    return "";
}

/** Nodi di testo diretti (Element.textNodes()). */
std::vector<std::string> textNodes(const html::Node& n) {
    std::vector<std::string> out;
    GumboNode* g = n.raw();
    if (!g || g->type != GUMBO_NODE_ELEMENT) return out;
    GumboVector& ch = g->v.element.children;
    for (unsigned i = 0; i < ch.length; i++) {
        auto* s = (GumboNode*)ch.data[i];
        if (s->type == GUMBO_NODE_TEXT) out.push_back(trim(s->v.text.text));
    }
    return out;
}

/** Minuscolo senza accenti (vocali accentate e ñ in UTF-8). */
std::string stripAccents(const std::string& in) {
    static const std::pair<const char*, char> map[] = {
        {"á", 'a'}, {"é", 'e'}, {"í", 'i'}, {"ó", 'o'}, {"ú", 'u'}, {"ü", 'u'}, {"ñ", 'n'},
        {"Á", 'a'}, {"É", 'e'}, {"Í", 'i'}, {"Ó", 'o'}, {"Ú", 'u'}, {"Ü", 'u'}, {"Ñ", 'n'}};
    std::string s = in;
    for (auto& m : map) s = replaceAll(s, m.first, std::string(1, m.second));
    return lower(trim(s));
}

// =============================================================================================== AnimeMovil

// FACTORY: out.push_back(std::make_shared<AnimeMovil>());
class AnimeMovil : public EsSource {
  public:
    AnimeMovil() : EsSource({"es.animemovil", "AnimeMovil", "https://animemeow.xyz", false}) {}
    bool supportsLatest() const override { return false; }

    Page popular(int page) override { return grid(baseUrl() + "/directorio/?p=" + std::to_string(page)); }
    Page latest(int page) override { return popular(page); }
    Page search(const std::string& q, int page) override {
        return grid(baseUrl() + "/directorio/?p=" + std::to_string(page) + "&q=" + queryEnc(q));
    }

    Details details(const std::string& url) override {
        std::string pageUrl = abs(url);
        http::Response resp = req("GET", pageUrl);
        html::Document d(resp.body, pageUrl);
        Details r;
        r.title = d.selectFirst(".banner-info div.titles h1").text();
        r.description = textOf(d.select("#sinopsis"));
        r.thumbnail = d.absUrl(d.selectFirst("#anime_image"), "src");
        r.genre = joinText(d.select(".generos-wrap .item"));
        std::string st = trim(textOf(d.select(".banner-img .estado")));
        r.status = st == "Finalizado" ? "Completato" : st == "En emision" ? "In corso" : "";

        auto seasons = d.select(".temporadas-lista .btn-temporada");
        if (!seasons.empty()) {
            std::string token;
            for (auto it = resp.headers.equal_range("set-cookie"); it.first != it.second; ++it.first)
                if (startsWith(it.first->second, "csrftoken"))
                    token = replaceAll(substringBefore(substringAfter(it.first->second, "="), ";"), "%3D", "=");
            std::string host = http::hostOf(pageUrl);
            std::reverse(seasons.begin(), seasons.end());
            for (auto& s : seasons) {
                std::string sid = s.attr("data-sid"), t = s.attr("data-t");
                try {
                    http::Headers h = {{"origin", "https://" + host}, {"referer", pageUrl}, {"x-csrftoken", token},
                                       {"x-requested-with", "XMLHttpRequest"}, {"cookie", "csrftoken=" + token},
                                       {"Content-Type", "application/json"}};
                    http::Response er = req("POST", "https://animemeow.xyz/api/obtener_episodios_x_temporada/", h,
                                            "{\"show\": \"" + sid + "\",\"temporada\": \"" + t + "\"}");
                    json j = parseJson(er.body);
                    if (!j.is_object() || !j.contains("episodios") || !j["episodios"].is_array()) continue;
                    int idx = 0;
                    for (auto& e : j["episodios"]) {
                        Episode ep;
                        ep.url = rel(jstr(e, "url"));
                        ep.name = "T" + t + " - " + trim(replaceAll(jstr(e, "ep_nombre"), "Ver", ""));
                        ep.number = idx++;
                        r.episodes.push_back(ep);
                    }
                } catch (const std::exception&) {
                }
            }
        } else {
            auto eps = d.select("#eps li > a");
            std::reverse(eps.begin(), eps.end());
            int idx = 0;
            for (auto& a : eps) {
                Episode ep;
                ep.url = rel(d.absUrl(a, "href"));
                ep.name = trim(replaceAll(ownText(a.selectFirst("p")), "Ver", ""));
                ep.number = idx++;
                r.episodes.push_back(ep);
            }
        }
        reverseEps(r.episodes);
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::vector<Video> out;
        for (auto& b : d->select("#fuentes button")) {
            std::string u = trim(substringAfter(b.attr("data-url"), "redirect.php?id="));
            if (u.empty()) continue;
            try {
                if (contains(u, "php?id=")) {
                    std::string server = trim(ownText(b));
                    html::Document sd(http::getText(u), u);
                    std::string fileData = scriptWith(sd, {"sources: [{file:"});
                    auto files = allUrls(fileData);
                    if (!files.empty()) {
                        for (auto& f : files) {
                            std::string type = contains(f, ".m3u8") ? ":HLS" : contains(f, ".mp4") ? ":MP4" : "";
                            out.push_back(simpleVideo(f, server + type));
                        }
                        continue;
                    }
                }
                std::string e = lower(u);
                if (contains(e, "voe")) append(out, voe(u, ""));
                else if (contains(e, "filemoon") || contains(e, "moonplayer")) append(out, moon(u, baseUrl(), "Filemoon:"));
                else if (contains(e, "uqload")) append(out, uqload(u, ""));
                else if (contains(e, "mp4upload")) append(out, mp4upload(u, ""));
                else if (contains(e, "wish")) append(out, streamWish(u, "", {{"Referer", baseUrl() + "/"}}));
                else if (contains(e, "doodstream") || contains(e, "dood.")) append(out, dood(u, "DoodStream"));
                else if (contains(e, "streamlare")) append(out, streamlare(u, ""));
                else if (contains(e, "yourupload")) append(out, yourUpload(u, ""));
                else if (contains(e, "burst")) append(out, burstCloud(u, ""));
                else if (contains(e, "fastream")) append(out, fastream(u, ""));
                else if (contains(e, "upstream")) append(out, upstream(u, ""));
                else if (contains(e, "streamtape")) append(out, streamtape(u, ""));
                else if (contains(e, "lion")) {
                    auto v = streamWish(u, "");
                    for (auto& x : v) x.title = replaceAll(x.title, "StreamWish", "FileLions");
                    append(out, v);
                } else append(out, resolve(u, "", baseUrl()));
            } catch (const std::exception&) {
            }
        }
        sortVideos(out, "Voe");
        return ensure(out);
    }

  private:
    Page grid(const std::string& url) {
        auto d = doc(url);
        return list(*d, ".grid-animes article", [&](const html::Node& el, Anime& a) {
            a.url = d->absUrl(el.selectFirst("a"), "href");
            a.title = el.selectFirst("a > p").text();
            a.thumbnail = d->absUrl(el.selectFirst("a .main-img img"), "src");
        }, ".pagination .right:not(.disabledd) .page-link");
    }
};

// =============================================================================================== AnimeYT

// FACTORY: out.push_back(std::make_shared<AnimeYt>());
class AnimeYt : public EsSource {
  public:
    AnimeYt() : EsSource({"es.animeyt", "AnimeYT", "https://ytanime.tv", false}) {}

    Page popular(int page) override { return grid(baseUrl() + "/mas-populares?page=" + std::to_string(page)); }
    Page latest(int page) override { return grid(baseUrl() + "/ultimos-animes?page=" + std::to_string(page)); }
    Page search(const std::string& q, int page) override {
        return grid(baseUrl() + "/search?q=" + queryEnc(q) + "&page=" + std::to_string(page));
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details r;
        r.thumbnail = d->selectFirst("div.sa-series-dashboard__poster figure.sa-poster__fig img").attr("src");
        r.title = d->selectFirst("#info div.sa-layout__line div div.sa-title-series__title span").text();
        r.description = removeSurrounding(d->selectFirst("#info div.sa-layout__line p.sa-text").text(), '"');
        std::string st = textOf(d->select("#info > div > button"));
        r.status = contains(st, "En Emisión") ? "In corso" : contains(st, "Finalizado") ? "Completato" : "";
        for (auto& a : d->select("#caps ul.list-group li.list-group-item a")) {
            Episode e;
            std::string n = digitsOnly(textOf(a.select("span.sa-series-link__number")));
            e.number = n.empty() ? 1 : parseNumber(n, 1);
            e.name = "Episodio " + kotlinFloat(e.number);
            e.url = rel(a.attr("href"));
            r.episodes.push_back(e);
        }
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::vector<Video> out;
        for (auto& f : d->select("#plays iframe")) {
            std::string u = f.attr("src");
            std::string server = substringBefore(replaceAll(replaceAll(u, "https://", ""), "http://", ""), ".");
            if (server != "fastream") continue;
            if (contains(u, "emb.html")) u = "https://fastream.to/embed-" + substringAfterLast(u, "/") + ".html";
            try {
                append(out, fastream(u, ""));
            } catch (const std::exception&) {
            }
        }
        return ensure(out);
    }

  private:
    Page grid(const std::string& url) {
        auto d = doc(url);
        Page p = list(*d, "div.video-block div.row div.col-md-2 div.video-card", [&](const html::Node& el, Anime& a) {
            html::Node t = el.selectFirst("div.video-card-body div.video-title a");
            a.url = t.attr("href");
            a.title = t.text();
            auto ch = el.selectFirst("div.video-card-image").children();
            html::Node img = ch.size() > 1 ? ch[1].selectFirst("img") : el.selectFirst("div.video-card-image img");
            a.thumbnail = img.attr("src");
        });
        auto lis = d->select("ul.pagination li.page-item");
        p.hasNextPage = !lis.empty() && lis.back().selectFirst("a").valid();
        return p;
    }
};

// =============================================================================================== BeatZ Anime

// FACTORY: out.push_back(std::make_shared<BeatZAnime>());
class BeatZAnime : public EsSource {
  public:
    BeatZAnime() : EsSource({"es.beatzanime", "BeatZ Anime", "https://www.beatz-anime.net", false}) {}

    Page popular(int page) override {
        if (page > 1) return {};
        auto d = doc(baseUrl());
        return list(*d, "article.top-views-card", [&](const html::Node& el, Anime& a) {
            html::Node anchor = el.selectFirst("a.top-views-poster");
            a.url = anchor.attr("href");
            a.thumbnail = d->absUrl(anchor.selectFirst("img"), "src");
            a.title = el.selectFirst("h3.top-views-title").text();
        });
    }
    Page latest(int page) override {
        auto d = doc(page > 1 ? baseUrl() + "/index.php?pagina=" + std::to_string(page) : baseUrl() + "/");
        Page p;
        std::set<std::string> seen;
        for (auto& a : d->select(".row > div a.titulo-largo")) {
            Anime an;
            an.url = rel(d->absUrl(a, "href"));
            an.title = a.text();
            std::string slug = substringAfterLast(a.attr("href"), "/");
            if (endsWith(slug, ".html")) slug = slug.substr(0, slug.size() - 5);
            an.thumbnail = baseUrl() + "/img/img-anime/" + slug + "/" + slug + "-port.jpg";
            if (seen.insert(an.url).second) p.animes.push_back(an);
        }
        for (auto& cur : d->select("ul.pagination > li.active")) {
            auto sibs = cur.parent().children();
            for (size_t i = 0; i + 1 < sibs.size(); i++)
                if (sibs[i].raw() == cur.raw() && !contains(" " + sibs[i + 1].attr("class") + " ", " disabled ")) p.hasNextPage = true;
        }
        return p;
    }
    Page search(const std::string& q, int page) override {
        if (page > 1) return {};
        auto d = doc(baseUrl() + "/lista-animes/");
        std::string query = stripAccents(q);
        Page p;
        for (auto& el : d->select("div.anime-card")) {
            if (!query.empty() && !contains(stripAccents(el.attr("data-name")), query)) continue;
            html::Node anchor = el.selectFirst("a.anime-poster-link");
            Anime a;
            a.url = rel(anchor.attr("href"));
            a.thumbnail = d->absUrl(anchor.selectFirst("img.anime-poster"), "src");
            html::Node t = el.selectFirst("span.overlay-title-link");
            a.title = t ? t.text() : anchor.attr("title");
            if (!a.url.empty()) p.animes.push_back(a);
        }
        return p;
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details r;
        r.title = d->selectFirst("h1").text();
        r.thumbnail = d->absUrl(d->selectFirst("div.poster-card img"), "src");
        html::Node post = d->selectFirst("p.post-text");
        std::string syn;
        for (auto& sp : post.select("span")) {
            std::string b = textOf(sp.select("b"));
            if (contains(b, "Generos")) r.genre = ownText(sp);
            if (contains(b, "Sinónimos")) syn = ownText(sp);
        }
        for (auto& div : d->select("div")) {
            bool ok = false;
            for (auto& h : div.children())
                if (h.tag() == "h5" && contains(h.text(), "Estado")) ok = true;
            if (!ok) continue;
            std::string st = lower(div.selectFirst("a").text());
            r.status = st == "finalizado" ? "Completato" : (st == "en emisión" || st == "en emsión") ? "In corso" : "";
            break;
        }
        std::string desc = joinStr(textNodes(post), "\n\n");
        if (!syn.empty()) desc += "\n\nSinónimos: " + syn;
        r.description = trim(desc);

        bool isMovie = false;
        for (auto& st : d->select("div.stat-item")) {
            if (!contains(textOf(st.select("h5")), "Tipo")) continue;
            isMovie = lower(st.selectFirst("a").text()) == "pelicula";
        }
        std::vector<Episode> regular, special;
        int index = 0;
        for (auto& row : d->select("#collapseExampled tbody tr")) {
            index++;
            auto cells = row.select("td");
            if (cells.size() < 5) continue;
            std::string format = lower(cells[1].text()), type = lower(cells[2].text());
            if (!(type == "video" || format == "mp4" || format == "mkv")) continue;
            std::string fileUrl = d->absUrl(cells[4].selectFirst("a.btn-descarga-premium"), "href");
            if (fileUrl.empty()) continue;
            std::string raw = cells[0].text();
            if (trim(raw).empty()) raw = trim(urlDecode(substringBeforeLast(substringAfterLast(fileUrl, "/"), ".")));
            Episode e;
            e.name = raw;
            e.url = fileUrl;
            e.number = episodeNumber(raw, index);
            if (isSpecial(raw)) {
                special.push_back(e);
            } else {
                regular.push_back(e);
            }
        }
        if (isMovie && regular.size() + special.size() == 1) {
            Episode& m = regular.empty() ? special[0] : regular[0];
            m.name = r.title;
            m.number = 1;
            if (!special.empty()) {
                regular.push_back(special[0]);
                special.clear();
            }
        }
        auto byNum = [](const Episode& a, const Episode& b) { return a.number < b.number; };
        std::stable_sort(regular.begin(), regular.end(), byNum);
        std::stable_sort(special.begin(), special.end(), byNum);
        for (auto& s : special) s.number = -1;
        std::vector<Episode> all = special;
        all.insert(all.end(), regular.begin(), regular.end());
        std::reverse(all.begin(), all.end());
        r.episodes = all;
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        std::string label;
        std::smatch m;
        static const std::regex res("\\d{3,4}p", std::regex::icase);
        std::string name = url.substr(0, std::min<size_t>(url.size(), 1024));
        if (std::regex_search(name, m, res)) label = m.str();
        else label = [&] { std::string e = substringAfterLast(url, "."); for (auto& c : e) c = (char)std::toupper((unsigned char)c); return e; }();
        Video v = simpleVideo(url, label, baseUrl() + "/");
        v.headers.push_back({"Accept", "video/webm,video/ogg,video/*;q=0.9,application/ogg;q=0.7,audio/*;q=0.6,*/*;q=0.5"});
        return {v};
    }

  private:
    static bool isSpecial(const std::string& name) {
        static const std::regex re("\\b(?:oad|ova|especiales|especial|peliculas?|pel\xC3\xAD" "culas?|ncopv|ncop|nced)\\b", std::regex::icase);
        std::string s = name.substr(0, std::min<size_t>(name.size(), 1024));
        return std::regex_search(s, re);
    }
    static double episodeNumber(const std::string& raw, int index) {
        std::string s = raw.substr(0, std::min<size_t>(raw.size(), 1024));
        std::smatch m;
        static const std::regex sxe("[Ss]\\d+[Ee](\\d+)"), trailing("(\\d+)\\s*$");
        if (std::regex_search(s, m, sxe)) return std::atof(m[1].str().c_str());
        // Exx non preceduto da Sxx
        for (size_t i = 0; i + 1 < s.size(); i++) {
            if ((s[i] != 'E' && s[i] != 'e') || !std::isdigit((unsigned char)s[i + 1])) continue;
            size_t b = i;
            while (b > 0 && std::isdigit((unsigned char)s[b - 1])) b--;
            if (b < i && b > 0 && (s[b - 1] == 'S' || s[b - 1] == 's')) continue;
            return std::atof(s.c_str() + i + 1);
        }
        if (std::regex_search(s, m, trailing)) return std::atof(m[1].str().c_str());
        return index;
    }
};

// =============================================================================================== az-animex

// FACTORY: out.push_back(std::make_shared<Azanimex>());
class Azanimex : public EsSource {
  public:
    Azanimex() : EsSource({"es.azanimex", "az-animex", "https://www.az-animex.com", false}) {}
    bool supportsLatest() const override { return false; }

    Page popular(int page) override {
        auto d = doc(page > 1 ? baseUrl() + "/?query-22-page=" + std::to_string(page) : baseUrl());
        return list(*d, "li.wp-block-post", [&](const html::Node& el, Anime& a) {
            html::Node t = el.selectFirst("h2.wp-block-post-title a");
            a.url = t.attr("href");
            a.title = trim(substringBefore(t.text(), "["));
            a.thumbnail = el.selectFirst("figure.wp-block-post-featured-image img").attr("data-src");
        }, "a.page-numbers:not(.prev):not(.next)");
    }
    Page latest(int page) override { return popular(page); }
    Page search(const std::string& q, int page) override {
        auto d = doc(page > 1 ? baseUrl() + "/page/" + std::to_string(page) + "/?s=" + queryEnc(q) : baseUrl() + "/?s=" + queryEnc(q));
        return list(*d, "li.wp-block-post", [&](const html::Node& el, Anime& a) {
            html::Node t = el.selectFirst("h2.wp-block-post-title a");
            a.url = t.attr("href");
            a.title = t.text().empty() ? "Título desconocido" : t.text();
            a.thumbnail = el.selectFirst("figure.gs-hover-scale-img img").attr("src");
            if (a.thumbnail.empty()) a.thumbnail = el.selectFirst("img").attr("data-src");
        }, "a.page-numbers:not(.prev):not(.next)");
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details r;
        std::map<std::string, std::string> info;
        for (auto& sp : d->select("span.post-info")) {
            std::string label = trim(sp.text());
            info[label] = nextSiblingText(sp);
            if (contains(label, "Título") && r.title.empty()) r.title = info[label];
        }
        auto get = [&](const std::string& k) {
            for (auto& kv : info)
                if (contains(kv.first, k)) return kv.second;
            return std::string();
        };
        if (r.title.empty()) r.title = d->selectFirst("h1").text();
        r.description = trim(d->selectFirst("div.su-spoiler-content").text());
        r.genre = substringBefore(get("Géneros"), ".");
        r.author = get("Estudio");
        std::string eps = get("Episodios");
        if (!eps.empty()) r.status = trim(substringAfter(eps, "de ")) == trim(substringBefore(eps, " de")) ? "Completato" : "In corso";
        std::string extra;
        for (const char* k : {"Año", "Episodios", "Duración", "Fansub", "Versión", "Resolución", "Formato"}) {
            std::string v = get(k);
            if (!trim(v).empty()) extra += std::string("\n• ") + k + ": " + v;
        }
        if (!extra.empty()) r.description += "\n\nInformación:" + extra;

        html::Node btn;
        for (auto& a : d->select("a.su-button[href*='.az-animex.com']")) {
            btn = a;
            break;
        }
        if (!btn) throw http::Error("URL degli episodi non trovato");
        std::string mainUrl = btn.attr("href");
        if (contains(mainUrl, "series-am")) mainUrl = replaceAll(mainUrl, "series-am", "series-am2");
        else if (contains(mainUrl, "series-nz")) mainUrl = replaceAll(mainUrl, "series-nz", "series-nz2");
        std::string clean = replaceAll(replaceAll(mainUrl, "https://", ""), "http://", "");
        std::string path = replaceAll(substringAfter(clean, "/"), "//", "/");
        auto p = path.find("es/");
        if (p != std::string::npos) path.erase(p, 3);
        std::string host = substringBefore(substringAfter(mainUrl, "https://"), "/");
        json j = parseJson(http::getText("https://" + host + "/api?path=" + path));
        auto epName = [](const std::string& n) { return substringBeforeLast(substringAfter(n, "] "), " ["); };
        auto epNum = [](const std::string& n) {
            auto b = n.rfind('[');
            if (b == std::string::npos) return 0.0;
            std::string pre = trim(n.substr(0, b));
            auto dash = pre.rfind('-');
            if (dash == std::string::npos) return 0.0;
            std::string num = trim(pre.substr(dash + 1));
            return digitsOnly(num) == num && !num.empty() ? std::atof(num.c_str()) : 0.0;
        };
        if (j.is_object() && j.contains("file") && j["file"].is_object()) {
            std::string n = jstr(j["file"], "name");
            if (endsWith(n, ".mp4")) {
                Episode e;
                e.url = "https://" + host + "/api/raw/?path=" + path;
                e.name = epName(n);
                e.number = epNum(n);
                r.episodes.push_back(e);
            }
        }
        if (j.is_object() && j.contains("folder") && j["folder"].is_object() && j["folder"].contains("value") &&
            j["folder"]["value"].is_array())
            for (auto& f : j["folder"]["value"]) {
                std::string n = jstr(f, "name");
                if (!endsWith(n, ".mp4")) continue;
                Episode e;
                e.url = "https://" + host + "/api/raw/?path=" + path + "/" + replaceAll(http::urlEncode(n), "%20", "+");
                e.name = epName(n);
                e.number = epNum(n);
                r.episodes.push_back(e);
            }
        std::stable_sort(r.episodes.begin(), r.episodes.end(), [](const Episode& a, const Episode& b) { return a.name > b.name; });
        return r;
    }

    std::vector<Video> videos(const std::string& url) override { return {simpleVideo(url, "az-animex")}; }
};

// =============================================================================================== Doramasflix

// FACTORY: out.push_back(std::make_shared<Doramasflix>());
class Doramasflix : public EsSource {
  public:
    Doramasflix() : EsSource({"es.doramasflix", "Doramasflix", "https://doramasflix.in", false}) {}

    Page popular(int page) override { return pagination(listDoramas(page, "POPULARITY_DESC", false)); }
    Page latest(int page) override { return pagination(listDoramas(page, "CREATEDAT_DESC", false)); }
    Page search(const std::string& q, int page) override {
        if (page > 1) return {};
        json body = {{"operationName", "searchAll"},
                     {"variables", {{"input", replaceAll(trim(q), "+", " ")}}},
                     {"query",
                      "query searchAll($input: String!) {\n  searchDorama(input: $input, limit: 32) {\n    _id\n    slug\n    name\n    "
                      "name_es\n    poster_path\n    poster\n    __typename\n  }\n  searchMovie(input: $input, limit: 32) {\n    _id\n    "
                      "name\n    name_es\n    slug\n    poster_path\n    poster\n    __typename\n  }\n}\n"}};
        json j = gql(body);
        Page p;
        json data = j.is_object() ? j.value("data", json::object()) : json::object();
        for (const char* k : {"searchDorama", "searchMovie"})
            if (data.contains(k) && data[k].is_array())
                for (auto& it : data[k]) p.animes.push_back(toAnime(it));
        return p;
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details r;
        json apollo = apolloState(*d);
        json dorama = findDorama(apollo);
        if (dorama.is_object()) {
            r.title = jstr(dorama, "name") + " (" + jstr(dorama, "name_es") + ")";
            r.description = trim(jstr(dorama, "overview"));
            std::vector<std::string> genres;
            std::string network;
            for (auto& kv : apollo.items()) {
                if (contains(kv.key(), "genres")) genres.push_back(jstr(kv.value(), "name"));
                if (network.empty() && contains(kv.key(), "networks")) network = jstr(kv.value(), "name");
            }
            r.genre = joinStr(genres);
            r.author = network;
            if (lower(jstr(dorama, "__typename")) == "movie") r.status = "Completato";
            std::string img = jstr(dorama, "poster_path");
            if (img.empty()) img = jstr(dorama, "poster");
            if (!img.empty()) r.thumbnail = image(img, false);
        }
        if (contains(url, "peliculas-online")) {
            r.episodes.push_back(movieEpisode(url));
            return r;
        }
        std::string id = substringAfter(url, "?id=");
        json seasons = gql({{"operationName", "listSeasons"},
                            {"variables", {{"serie_id", id}}},
                            {"query",
                             "query listSeasons($serie_id: MongoID!) {\n  listSeasons(sort: NUMBER_ASC, filter: {serie_id: $serie_id}) "
                             "{\n    slug\n    season_number\n    poster_path\n    air_date\n    serie_name\n    poster\n    backdrop\n    "
                             "__typename\n  }\n}\n"}},
                           "?id=" + id);
        json list = seasons.is_object() && seasons.contains("data") ? seasons["data"].value("listSeasons", json::array()) : json::array();
        for (auto& s : list) {
            json sn = s.contains("season_number") ? s["season_number"] : json(1);
            try {
                json eps = gql({{"operationName", "listEpisodes"},
                                {"variables", {{"serie_id", id}, {"season_number", sn}}},
                                {"query",
                                 "query listEpisodes($season_number: Float!, $serie_id: MongoID!) {\n  listEpisodes(sort: NUMBER_ASC, "
                                 "filter: {type_serie: \"dorama\", serie_id: $serie_id, season_number: $season_number}) {\n    _id\n    "
                                 "name\n    slug\n    serie_name\n    serie_name_es\n    serie_id\n    still_path\n    air_date\n    "
                                 "season_number\n    episode_number\n    languages\n    poster\n    backdrop\n    __typename\n  }\n}\n"}});
                json arr = eps.is_object() && eps.contains("data") ? eps["data"].value("listEpisodes", json::array()) : json::array();
                int idx = 0;
                for (auto& e : arr) {
                    Episode ep;
                    std::string en = jstr(e, "episode_number");
                    std::string nm = jstr(e, "name");
                    ep.name = "T" + jstr(e, "season_number") + " - E" + en + " " + (nm.empty() ? "- Capítulo " + en : "- " + nm);
                    ep.number = en.empty() ? idx : parseNumber(en, idx);
                    ep.url = "/episodios/" + jstr(e, "slug");
                    r.episodes.push_back(ep);
                    idx++;
                }
            } catch (const std::exception&) {
            }
        }
        reverseEps(r.episodes);
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        json apollo = apolloState(*d);
        json episode;
        for (auto& kv : apollo.items())
            if (contains(kv.key(), "Episode:")) {
                episode = kv.value();
                break;
            }
        if (!episode.is_object()) episode = findDorama(apollo);
        json links;
        auto linksOf = [](const json& o) -> json {
            if (o.is_object() && o.contains("links_online") && o["links_online"].is_object() && o["links_online"].contains("json") &&
                o["links_online"]["json"].is_array())
                return o["links_online"]["json"];
            return json();
        };
        links = linksOf(episode);
        if (links.is_null())
            for (auto& kv : apollo.items())
                if (contains(kv.key(), "ROOT_QUERY.getMovieLinks(")) {
                    links = linksOf(kv.value());
                    break;
                }
        std::vector<std::pair<std::string, std::string>> items;
        if (links.is_array()) {
            for (auto& l : links) items.push_back({jstr(l, "link"), langOf(jstr(l, "lang"))});
        } else {
            std::set<std::string> seen;
            for (auto& kv : apollo.items()) {
                if (!contains(kv.key(), "ROOT_QUERY.listProblems(")) continue;
                const json& v = kv.value();
                if (!v.is_object() || !v.contains("server") || !v["server"].is_object()) continue;
                json server = v["server"].value("json", json::object());
                std::string link = jstr(server, "link");
                if (link.empty() || !seen.insert(link).second) continue;
                try {
                    items.push_back({realLink(link), langOf(jstr(server, "lang"))});
                } catch (const std::exception&) {
                }
            }
        }
        std::vector<Video> out;
        for (auto& it : items) {
            try {
                append(out, resolveFlix(it.first, it.second));
            } catch (const std::exception&) {
            }
        }
        sortVideos(out, "Voe", "[LAT]", 1080);
        return ensure(out);
    }

  private:
    static constexpr const char* API = "https://sv1.fluxcedene.net/api/gql";

    json gql(const json& body, const std::string& query = "") {
        http::Headers h = {{"accept", "application/json, text/plain, */*"},
                           {"content-type", "application/json;charset=UTF-8"},
                           {"origin", "https://doramasflix.in"},
                           {"referer", "https://doramasflix.in/"},
                           {"platform", "doramasflix"},
                           {"authorization", "Bear"},
                           {"x-access-jwt-token", ""},
                           {"x-access-platform", "RxARncfg1S_MdpSrCvreoLu_SikCGMzE1NzQzODc3NjE2MQ=="}};
        http::Response r = http::request("POST", std::string(API) + query, h, body.dump());
        if (r.status < 200 || r.status >= 300) throw http::Error("Doramasflix ha risposto HTTP " + std::to_string(r.status));
        return parseJson(r.body);
    }

    json listDoramas(int page, const std::string& sort, bool tvShow) {
        return gql({{"operationName", "listDoramas"},
                    {"variables", {{"page", page}, {"sort", sort}, {"perPage", 32}, {"filter", {{"isTVShow", tvShow}}}}},
                    {"query",
                     "query listDoramas($page: Int, $perPage: Int, $sort: SortFindManyDoramaInput, $filter: FilterFindManyDoramaInput) {\n  "
                     "paginationDorama(page: $page, perPage: $perPage, sort: $sort, filter: $filter) {\n    count\n    pageInfo {\n      "
                     "currentPage\n      hasNextPage\n      hasPreviousPage\n      __typename\n    }\n    items {\n      _id\n      name\n      "
                     "name_es\n      slug\n      cast\n      names\n      overview\n      languages\n      created_by\n      popularity\n      "
                     "poster_path\n      vote_average\n      backdrop_path\n      first_air_date\n      episode_run_time\n      isTVShow\n      "
                     "poster\n      backdrop\n      genres {\n        name\n        slug\n        __typename\n      }\n      networks {\n        "
                     "name\n        slug\n        __typename\n      }\n      __typename\n    }\n    __typename\n  }\n}\n"}});
    }

    static std::string image(const std::string& u, bool thumb) {
        if (contains(u, "https")) return u;
        return (thumb ? "https://image.tmdb.org/t/p/w220_and_h330_face" : "https://image.tmdb.org/t/p/w500") + u;
    }

    static std::string urlByType(const std::string& type, const std::string& slug, const std::string& id) {
        std::string t = lower(type);
        if (t == "dorama") return "/doramas-online/" + slug + "?id=" + id;
        if (t == "movie") return "/peliculas-online/" + slug + "?id=" + id;
        if (t == "episode") return "/episodios/" + slug;
        return "";
    }

    static Anime toAnime(const json& it) {
        Anime a;
        a.title = jstr(it, "name") + " (" + jstr(it, "name_es") + ")";
        std::string img = jstr(it, "poster_path");
        if (img.empty()) img = jstr(it, "poster");
        a.thumbnail = img.empty() ? "" : image(img, true);
        a.url = urlByType(jstr(it, "__typename"), jstr(it, "slug"), jstr(it, "_id"));
        return a;
    }

    static Page pagination(const json& j) {
        Page p;
        json data = j.is_object() ? j.value("data", json::object()) : json::object();
        json pg = data.contains("paginationDorama") ? data["paginationDorama"] : data.value("paginationMovie", json::object());
        if (!pg.is_object()) return p;
        if (pg.contains("items") && pg["items"].is_array())
            for (auto& it : pg["items"]) {
                Anime a = toAnime(it);
                if (!a.url.empty()) p.animes.push_back(a);
            }
        p.hasNextPage = pg.contains("pageInfo") && pg["pageInfo"].is_object() && pg["pageInfo"].value("hasNextPage", false);
        return p;
    }

    static json apolloState(const html::Document& d) {
        for (auto& s : d.select("script")) {
            std::string data = s.data();
            if (!contains(data, "{\"props\":{\"pageProps\":{")) continue;
            json j = parseJson(data);
            try {
                return j["props"]["pageProps"]["apolloState"];
            } catch (const std::exception&) {
                return json();
            }
        }
        return json();
    }

    static json findDorama(const json& apollo) {
        if (!apollo.is_object()) return json();
        for (auto& kv : apollo.items()) {
            const std::string& k = kv.key();
            std::string rest;
            if (startsWith(k, "Movie:")) rest = k.substr(6);
            else if (startsWith(k, "Dorama:")) rest = k.substr(7);
            else continue;
            bool ok = !rest.empty();
            for (char c : rest)
                if (!std::isalnum((unsigned char)c)) ok = false;
            if (ok) return kv.value();
        }
        return json();
    }

    static std::string langOf(const std::string& id) {
        static const std::map<std::string, std::string> m = {
            {"36", "[ENG]"}, {"37", "[CAST]"}, {"38", "[LAT]"}, {"192", "[SUB]"}, {"1327", "[POR]"}, {"13109", "[COR]"},
            {"13110", "[JAP]"}, {"13111", "[MAN]"}, {"13112", "[TAI]"}, {"13113", "[FIL]"}, {"13114", "[IND]"}, {"343422", "[VIET]"}};
        auto it = m.find(id);
        return it == m.end() ? "" : it->second;
    }

    std::string realLink(const std::string& link) {
        if (!contains(link, "fkplayer.xyz")) return link;
        html::Document d(http::getText(link), link);
        std::string data = scriptWith(d, {"{\"props\":{\"pageProps\":{"});
        json t = parseJson(data);
        std::string token;
        try {
            token = t["props"]["pageProps"].value("token", "");
            if (token.empty()) token = t["query"].value("token", "");
        } catch (const std::exception&) {
        }
        http::Response r = http::request("POST", "https://fkplayer.xyz/api/decoding",
                                         {{"origin", "https://" + http::hostOf(link)}, {"Content-Type", "application/json"}},
                                         json({{"token", token}}).dump());
        return base64Decode(jstr(parseJson(r.body), "link"));
    }

    std::vector<Video> resolveFlix(const std::string& url, const std::string& prefix) {
        std::string e = lower(url);
        std::string p = prefix.empty() ? "" : prefix + " ";
        if (contains(e, "voe")) return voe(url, p);
        if (contains(e, "ok.ru") || contains(e, "okru")) return okru(url, p);
        if (contains(e, "filemoon") || contains(e, "moonplayer")) return moon(url, baseUrl(), p + "Filemoon:");
        if (contains(e, "uqload")) return uqload(url, prefix);
        if (contains(e, "mp4upload")) return mp4upload(url, p);
        if (contains(e, "doodstream") || contains(e, "dood.")) return dood(replaceAll(url, "https://doodstream.com/e/", "https://dood.to/e/"), p);
        if (contains(e, "streamlare")) return streamlare(url, prefix);
        if (contains(e, "upload")) return yourUpload(url, p);
        if (contains(e, "wish")) return streamWish(url, p, {{"Origin", "https://streamwish.to"}, {"Referer", "https://streamwish.to/"}});
        if (contains(e, "burst")) return burstCloud(url, p);
        if (contains(e, "fastream")) return fastream(url, p);
        if (contains(e, "upstream")) return upstream(url, p);
        if (contains(e, "streamtape") || contains(e, "stp") || contains(e, "stape")) return streamtape(url, p);
        if (contains(e, "ahvsh") || contains(e, "streamhide")) return vidHide(url, p);
        if (contains(e, "lion")) {
            auto v = streamWish(url, p);
            for (auto& x : v) x.title = replaceAll(x.title, "StreamWish", "FileLions");
            return v;
        }
        if (contains(e, "vudeo") || contains(e, "vudea")) return vudeo(url, p);
        return {};
    }
};

// =============================================================================================== Doramasyt

std::string imgSkip(const html::Document& d, const html::Node& n, const std::string& bad) {
    if (!n) return "";
    for (const char* a : {"data-src", "data-lazy-src", "srcset", "src"}) {
        if (!n.hasAttr(a)) continue;
        std::string v = trim(n.attr(a));
        if (v.empty() || contains(v, bad)) continue;
        if (std::string(a) == "srcset") v = substringBefore(v, " ");
        return http::resolve(d.url(), v);
    }
    return "";
}

// FACTORY: out.push_back(std::make_shared<Doramasyt>());
class Doramasyt : public EsSource {
  public:
    Doramasyt() : EsSource({"es.doramasyt", "Doramasyt", "https://www.doramasyt.com", false}) {}

    Page popular(int page) override { return grid(baseUrl() + "/doramas?p=" + std::to_string(page)); }
    Page latest(int page) override { return grid(baseUrl() + "/emision?p=" + std::to_string(page)); }
    Page search(const std::string& q, int page) override {
        if (page > 1) return {};
        return grid(baseUrl() + "/buscar?q=" + queryEnc(q));
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details r;
        r.title = d->selectFirst(".flex-column h1.text-capitalize").text();
        r.description = d->selectFirst(".h-100 .mb-3 p").text();
        r.genre = joinText(d->select(".lh-lg span"));
        r.thumbnail = imgSkip(*d, d->selectFirst(".gap-3 img"), "anime.png");
        std::string st = textOf(d->select(".lh-sm .ms-2"));
        r.status = contains(st, "Finalizado") ? "Completato" : contains(st, "Estreno") ? "In corso" : "";
        std::string token = d->selectFirst("meta[name='csrf-token']").attr("content");
        std::string capList = d->selectFirst(".caplist").attr("data-ajax");
        if (capList.empty()) return r;
        std::string referer = d->url();
        http::Headers h = {{"accept", "application/json, text/javascript, */*; q=0.01"}, {"accept-language", "es-419,es;q=0.8"},
                           {"content-type", "application/x-www-form-urlencoded; charset=UTF-8"}, {"origin", baseUrl()},
                           {"referer", referer}, {"x-requested-with", "XMLHttpRequest"}};
        json det = parseJson(postForm(capList, "_token=" + http::urlEncode(token), h));
        if (!det.is_object()) return r;
        double per = jnum(det, "perpage", 0);
        size_t total = det.contains("eps") && det["eps"].is_array() ? det["eps"].size() : 0;
        if (per <= 0) return r;
        int pages = (int)std::ceil(total / per);
        std::string paginate = jstr(det, "paginate_url");
        for (int page = 1; page <= pages; page++) {
            try {
                json pj = parseJson(postForm(paginate, "_token=" + http::urlEncode(token) + "&p=" + std::to_string(page), h));
                if (!pj.is_object() || !pj.contains("caps") || !pj["caps"].is_array()) continue;
                int idx = 0;
                for (auto& c : pj["caps"]) {
                    idx++;
                    int n = c.contains("episodio") && c["episodio"].is_number() ? c["episodio"].get<int>() : idx;
                    Episode e;
                    e.name = "Capítulo " + std::to_string(n);
                    e.number = n;
                    e.url = rel(jstr(c, "url"));
                    r.episodes.push_back(e);
                }
            } catch (const std::exception&) {
            }
        }
        reverseEps(r.episodes);
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::vector<Video> out;
        for (auto& el : d->select("[data-player]")) {
            std::string u = base64Decode(el.attr("data-player"));
            std::string e = lower(u);
            try {
                if (contains(e, "voe")) append(out, voe(u, ""));
                else if (contains(e, "uqload")) append(out, uqload(u, ""));
                else if (contains(e, "ok.ru") || contains(e, "okru")) append(out, okru(u, ""));
                else if (contains(e, "filemoon") || contains(e, "moonplayer")) append(out, moon(u, baseUrl(), "Filemoon:"));
                else if (contains(e, "wish")) append(out, streamWish(u, ""));
                else if (contains(e, "streamtape") || contains(e, "stp") || contains(e, "stape")) append(out, streamtape(u, ""));
                else if (contains(e, "doodstream") || contains(e, "dood.") || contains(e, "ds2play") || contains(e, "doods.")) append(out, dood(u, ""));
                else if (contains(e, "lion")) {
                    auto v = streamWish(u, "");
                    for (auto& x : v) x.title = replaceAll(x.title, "StreamWish", "FileLions");
                    append(out, v);
                } else if (contains(e, "mix")) append(out, mixDrop(u, ""));
                else append(out, resolve(u, "", baseUrl()));
            } catch (const std::exception&) {
            }
        }
        sortVideos(out, "Filemoon");
        return ensure(out);
    }

  private:
    Page grid(const std::string& url) {
        auto d = doc(url);
        return list(*d, ".ficha_efecto a", [&](const html::Node& el, Anime& a) {
            a.title = el.selectFirst(".title_cap").text();
            a.thumbnail = imgSkip(*d, el.selectFirst("img"), "anime.png");
            a.url = d->absUrl(el, "href");
        }, ".pagination [rel=\"next\"]");
    }
};

// =============================================================================================== EstrenosDoramas

// FACTORY: out.push_back(std::make_shared<EstrenosDoramas>());
class EstrenosDoramas : public EsSource {
  public:
    EstrenosDoramas() : EsSource({"es.estrenosdoramas", "EstrenosDoramas", "https://estrenosdoramas.es", false}) {}

    Page popular(int page) override { return grid(baseUrl() + "/temporadas/?page=" + std::to_string(page) + "&order=popular"); }
    Page latest(int page) override { return grid(baseUrl() + "/temporadas/?page=" + std::to_string(page) + "&order=latest"); }
    Page search(const std::string& q, int page) override {
        return grid(baseUrl() + "/page/" + std::to_string(page) + "/?s=" + queryEnc(q));
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details r;
        r.title = trim(d->selectFirst(".entry-title").text());
        r.description = trim(d->selectFirst(".mindesc").text());
        r.genre = joinText(d->select(".genxed a"));
        r.thumbnail = d->absUrl(d->selectFirst(".thumb img"), "src");
        for (auto& sp : d->select(".spe > span")) {
            std::string t = textOf(sp.select("b"));
            if (contains(t, "Estado")) {
                std::string s = ownText(sp);
                r.status = contains(s, "Ongoing") ? "In corso" : contains(s, "Completed") ? "Completato" : "";
            } else if (contains(t, "Network")) {
                r.author = joinText(sp.select("a"));
            }
        }
        int idx = 0;
        for (auto& a : d->select("#myList li a")) {
            idx++;
            Episode e;
            e.name = trim(textOf(a.select(".epl-title")));
            e.number = firstNumber(e.name, idx);
            e.url = rel(d->absUrl(a, "href"));
            r.episodes.push_back(e);
        }
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::vector<Video> out;
        for (auto& el : d->select("[data-embed]")) {
            std::string link = el.attr("data-embed");
            try {
                http::Response r = http::request("GET", link);
                std::string real = r.finalUrl.empty() ? link : r.finalUrl;
                std::string e = lower(real);
                auto any = [&](std::initializer_list<const char*> l) {
                    for (auto* s : l)
                        if (contains(e, s)) return true;
                    return false;
                };
                if (any({"ok.ru", "okru"})) append(out, okru(real, ""));
                else if (any({"filelions", "lion", "fviplions"})) {
                    auto v = streamWish(real, "");
                    for (auto& x : v) x.title = replaceAll(x.title, "StreamWish", "FileLions");
                    append(out, v);
                } else if (any({"wishembed", "streamwish", "strwish", "wish"})) append(out, streamWish(real, ""));
                else if (any({"vidhide", "streamhide", "guccihide", "streamvid"})) append(out, vidHide(real, ""));
                else if (any({"voe", "robertordercharacter", "donaldlineelse"})) append(out, voe(real, ""));
                else if (any({"yourupload", "upload"})) append(out, yourUpload(real, ""));
            } catch (const std::exception&) {
            }
        }
        sortVideos(out, "Voe");
        return ensure(out);
    }

  private:
    Page grid(const std::string& url) {
        auto d = doc(url);
        return list(*d, ".listupd article a", [&](const html::Node& el, Anime& a) {
            a.url = d->absUrl(el, "href");
            a.title = el.attr("title");
            a.thumbnail = d->absUrl(el.selectFirst("img"), "src");
        }, ".hpage .r, .pagination .next");
    }
};

// =============================================================================================== Pandrama

// FACTORY: out.push_back(std::make_shared<Pandrama>());
class Pandrama : public EsSource {
  public:
    Pandrama() : EsSource({"es.pandrama", "Pandrama", "https://pandrama.com", false}) {}

    Page popular(int page) override { return grid(baseUrl() + "/explorar/Dramas--------" + std::to_string(page) + "---/"); }
    Page latest(int page) override { return grid(baseUrl() + "/explorar/Dramas--hits------" + std::to_string(page) + "---/"); }
    Page search(const std::string& q, int page) override {
        return grid(baseUrl() + "/buscar/media/" + queryEnc(q) + "----------" + std::to_string(page) + "---/");
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details r;
        r.title = d->selectFirst("h1, h3.slide-info-title").text();
        r.thumbnail = d->absUrl(d->selectFirst(".detail-pic img, .this-pic img"), "data-src");
        r.description = ownText(d->selectFirst("#height_limit"));
        r.genre = joinText(d->select(".this-desc-labels a"));
        for (auto& el : d->select(".this-info"))
            if (contains(textOf(el.select("strong")), "Director:")) r.author = el.selectFirst("a").text();
        std::vector<std::pair<std::string, json>> groups;
        for (auto& a : d->select(".anthology-list-play li a")) {
            std::string key = trim(a.text());
            auto it = std::find_if(groups.begin(), groups.end(), [&](const std::pair<std::string, json>& g) { return g.first == key; });
            if (it == groups.end()) {
                groups.push_back({key, json::array()});
                it = groups.end() - 1;
            }
            it->second.push_back(d->absUrl(a, "href"));
        }
        for (auto& g : groups) {
            Episode e;
            std::string n = trim(substringAfter(g.first, "Ep."));
            e.name = "Episodio " + n;
            e.number = parseNumber(n, 0);
            e.url = g.second.dump();
            r.episodes.push_back(e);
        }
        reverseEps(r.episodes);
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        json list = parseJson(url);
        std::vector<Video> out;
        if (!list.is_array()) return ensure(out);
        for (auto& it : list) {
            if (!it.is_string()) continue;
            try {
                auto d = doc(it.get<std::string>());
                std::string s = scriptWith(*d, {"var player_aaaa"});
                json player = parseJson(balancedAfter(s, "var player_aaaa="));
                std::string u = jstr(player, "url");
                u = jnum(player, "encrypt", 0) == 2 ? urlDecode(base64Decode(u)) : urlDecode(u);
                std::string e = lower(u);
                if (contains(e, "ok.ru") || contains(e, "okru")) append(out, okru(u, ""));
                else if (contains(e, "vk.")) append(out, vk(u, "Vk - "));
            } catch (const std::exception&) {
            }
        }
        sortVideos(out, "Vk");
        return ensure(out);
    }

  private:
    Page grid(const std::string& url) {
        auto d = doc(url);
        return list(*d, "a.public-list-exp", [&](const html::Node& el, Anime& a) {
            std::string tag = trim(textOf(el.select(".public-prt")));
            std::string prefix = contains(tag, "Español") ? "[LAT] " : contains(tag, "Castellano") ? "[CAST] " : "";
            a.title = trim(prefix + " " + el.attr("title"));
            a.thumbnail = d->absUrl(el.selectFirst("img"), "data-src");
            a.url = d->absUrl(el, "href");
        }, "[title=\"Página siguiente\"]");
    }
};

// =============================================================================================== LACartoons

// FACTORY: out.push_back(std::make_shared<LaCartoons>());
class LaCartoons : public EsSource {
  public:
    LaCartoons() : EsSource({"es.lacartoons", "LACartoons", "https://www.lacartoons.com", false}) {}
    bool supportsLatest() const override { return false; }

    Page popular(int page) override { return grid(baseUrl() + "/?page=" + std::to_string(page)); }
    Page latest(int page) override { return popular(page); }
    Page search(const std::string& q, int page) override {
        if (page > 1) return {};
        return grid(baseUrl() + "/?utf8=%E2%9C%93&Titulo=" + queryEnc(q));
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details r;
        r.title = ownText(d->selectFirst(".subtitulo-serie-seccion"));
        r.author = d->selectFirst(".marcador-cartoon").text();
        r.genre = r.author;
        r.status = "Completato";
        for (auto& p : d->select(".informacion-serie-seccion > p"))
            if (contains(p.text(), "Reseña")) r.description = p.selectFirst("span").text();
        r.thumbnail = d->absUrl(d->selectFirst("div.h-thumb figure img"), "src");
        auto panels = d->select(".episodio-panel");
        int real = 1;
        auto seasons = d->select(".estilo-temporada");
        for (size_t i = 0; i < seasons.size() && i < panels.size(); i++) {
            std::string sn = digitsOnly(ownText(seasons[i]));
            for (auto& ep : panels[i].select("ul > li > a")) {
                Episode e;
                e.number = real++;
                e.name = "T" + sn + " - E" + digitsOnly(textOf(ep.select("span"))) + " - " + trim(ownText(ep));
                e.url = rel(d->absUrl(ep, "href"));
                r.episodes.push_back(e);
            }
        }
        reverseEps(r.episodes);
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::vector<Video> out;
        for (auto& f : d->select("iframe")) {
            std::string u = fixUrl(f.attr("src"));
            std::string e = lower(u);
            try {
                if (contains(e, "ok.ru") || contains(e, "okru")) append(out, okru(u, ""));
                else if (contains(e, "lion")) {
                    auto v = streamWish(u, "");
                    for (auto& x : v) x.title = replaceAll(x.title, "StreamWish", "FileLions");
                    append(out, v);
                } else if (contains(e, "wish")) append(out, streamWish(u, "", {{"Origin", "https://streamwish.to"}, {"Referer", "https://streamwish.to/"}}));
                else if (contains(e, "vidhide") || contains(e, "streamhide") || contains(e, "guccihide") || contains(e, "streamvid")) append(out, vidHide(u, ""));
                else if (contains(e, "voe")) append(out, voe(u, ""));
                else if (contains(e, "upload")) append(out, yourUpload(u, ""));
                else if (contains(e, "sendvid")) append(out, sendvid(u, ""));
                else append(out, resolve(u, "", baseUrl()));
            } catch (const std::exception&) {
            }
        }
        sortVideos(out, "FileLions");
        return ensure(out);
    }

  private:
    Page grid(const std::string& url) {
        auto d = doc(url);
        return list(*d, ".conjuntos-series a", [&](const html::Node& el, Anime& a) {
            a.url = d->absUrl(el, "href");
            a.title = el.selectFirst(".serie .informacion-serie .nombre-serie").text();
            a.thumbnail = d->absUrl(el.selectFirst(".serie img"), "src");
        }, ".pagination a[rel=next]");
    }
};

// =============================================================================================== TioAnime / TioHentai

// FACTORY: out.push_back(std::make_shared<TioAnimeH>(Info{"es.tioanime", "TioAnime", "https://tioanime.com", false}));
// FACTORY: out.push_back(std::make_shared<TioAnimeH>(Info{"es.tiohentai", "TioHentai", "https://tiohentai.com", true}));
class TioAnimeH : public EsSource {
  public:
    explicit TioAnimeH(Info i) : EsSource(i) {}

    Page popular(int page) override { return grid(baseUrl() + "/directorio?p=" + std::to_string(page)); }
    Page latest(int) override {
        auto d = doc(baseUrl());
        std::string slug = contains(baseUrl(), "hentai") ? "/hentai/" : "/anime/";
        return list(*d, ".episodes li", [&](const html::Node& el, Anime& a) {
            a.title = textOf(el.select("article a h3"));
            a.thumbnail = baseUrl() + el.selectFirst("article a div figure img").attr("src");
            std::string href = el.selectFirst("article a").attr("href");
            a.url = replaceAll(substringBeforeLast(href, "-"), "/ver/", slug);
        });
    }
    Page search(const std::string& q, int page) override {
        return grid(baseUrl() + "/directorio?q=" + queryEnc(q) + "&p=" + std::to_string(page));
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details r;
        r.title = textOf(d->select("h1.title"));
        r.description = ownText(d->selectFirst("p.sinopsis"));
        r.genre = joinText(d->select("p.genres span.btn.btn-sm.btn-primary.rounded-pill a"));
        r.thumbnail = d->absUrl(d->selectFirst(".thumb img"), "src");
        std::string st = textOf(d->select("a.btn.btn-success.btn-block.status"));
        r.status = contains(st, "En emision") ? "In corso" : contains(st, "Finalizado") ? "Completato" : "";
        std::string script = scriptWith(*d, {"var episodes = "});
        std::string eps = substringBefore(substringAfter(script, "episodes = ["), "];");
        if (script.empty() || trim(eps).empty()) return r;
        auto info = split(replaceAll(substringBefore(substringAfter(script, "anime_info = ["), "];"), "\"", ""), ",");
        if (info.size() < 2) return r;
        for (auto& n : split(eps, ",")) {
            std::string t = trim(n);
            if (t.empty()) continue;
            Episode e;
            e.name = "Episodio " + t;
            e.url = "/ver/" + info[1] + "-" + t;
            e.number = parseNumber(t, 0);
            r.episodes.push_back(e);
        }
        sortEpisodesDesc(r.episodes);
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::string script = scriptWith(*d, {"var videos ="});
        std::string list = replaceAll(substringBefore(substringAfter(script, "var videos = [["), "]];"), "\"", "");
        std::vector<Video> out;
        if (script.empty()) return ensure(out);
        for (auto& it : split(list, "],[")) {
            auto parts = split(it, ",");
            if (parts.size() < 2) continue;
            std::string server = lower(parts[0]);
            std::string u = replaceAll(parts[1], "\\/", "/");
            try {
                if (server == "voe") append(out, voe(u, ""));
                else if (server == "okru") append(out, okru(u, ""));
                else if (server == "yourupload") append(out, yourUpload(u, ""));
                else if (server == "mixdrop") append(out, mixDrop(u, ""));
                else append(out, resolve(u, "", baseUrl(), server));
            } catch (const std::exception&) {
            }
        }
        sortVideos(out, "Voe");
        return ensure(out);
    }

  private:
    Page grid(const std::string& url) {
        auto d = doc(url);
        Page p = list(*d, "ul.animes.list-unstyled.row li.col-6.col-sm-4.col-md-3.col-xl-2", [&](const html::Node& el, Anime& a) {
            a.url = el.selectFirst("article a").attr("href");
            a.title = textOf(el.select("article a h3"));
            a.thumbnail = baseUrl() + el.selectFirst("article a div figure img").attr("src");
        });
        for (auto& cur : d->select(".pagination .active")) {
            bool after = false;
            for (auto& sib : cur.parent().children()) {
                if (sib.raw() == cur.raw()) after = true;
                else if (after && sib.tag() == "li" && !contains(" " + sib.attr("class") + " ", " disabled ")) p.hasNextPage = true;
            }
        }
        return p;
    }
};

// =============================================================================================== Animejl (18+)

// FACTORY: out.push_back(std::make_shared<Animejl>());
class Animejl : public EsSource {
  public:
    Animejl() : EsSource({"es.animejl", "Animejl", "https://www.anime-jl.net", true}) {}

    Page popular(int page) override { return grid(baseUrl() + "/animes?order=rating&page=" + std::to_string(page)); }
    Page latest(int page) override { return grid(baseUrl() + "/animes?order=updated&page=" + std::to_string(page)); }
    Page search(const std::string& q, int page) override {
        return grid(baseUrl() + "/animes?q=" + queryEnc(q) + "&page=" + std::to_string(page));
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details r;
        r.thumbnail = d->absUrl(d->selectFirst("div.AnimeCover div.Image figure img"), "src");
        r.title = d->selectFirst("div.Ficha.fchlt div.Container .Title").text();
        r.description = removeSurrounding(d->selectFirst("div.Description").text(), '"');
        r.genre = joinText(d->select("nav.Nvgnrs a"));
        std::string st = textOf(d->select("span.fa-tv"));
        r.status = contains(st, "En emision") ? "In corso" : contains(st, "Finalizado") ? "Completato" : "";
        std::string script = scriptWith(*d, {"var episodes ="});
        json eps = parseJson(balancedAfter(script, "var episodes ="));
        auto info = split(substringBefore(substringAfter(script, "var anime_info = ["), "];"), ",");
        for (auto& s : info) s = trimChars(trim(s), "\"");
        std::string animeId = info.size() > 0 ? info[0] : "", slug = info.size() > 2 ? info[2] : "";
        if (eps.is_array())
            for (auto& e : eps) {
                if (!e.is_array() || e.empty()) continue;
                int n = e[0].is_number() ? e[0].get<int>() : std::atoi(e[0].is_string() ? e[0].get<std::string>().c_str() : "0");
                Episode ep;
                ep.url = "/anime/" + animeId + "/" + slug + "/episodio-" + std::to_string(n);
                ep.number = n;
                ep.name = "Episodio " + std::to_string(n);
                r.episodes.push_back(ep);
            }
        sortEpisodesDesc(r.episodes);
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::string script = scriptWith(*d, {"var video = ["});
        std::vector<Video> out;
        for (auto& u : iframeSrcs(script)) {
            try {
                if (contains(u, "streamtape")) append(out, streamtape(u, ""));
                else if (contains(u, "ok.ru")) append(out, okru(u, ""));
                else if (contains(u, "yourupload")) append(out, yourUpload(u, ""));
                else if (contains(u, "streamwish") || contains(u, "playerwish")) append(out, streamWish(u, ""));
                else if (contains(u, "streamhidevid")) append(out, vidHide(u, ""));
                else if (contains(u, "voe")) append(out, voe(u, ""));
                else if (contains(u, "uqload")) append(out, uqload(u, ""));
                else if (contains(u, "mp4upload")) append(out, mp4upload(u, ""));
                else append(out, resolve(u, "", baseUrl()));
            } catch (const std::exception&) {
            }
        }
        sortVideos(out, "StreamWish", "", 720);
        return ensure(out);
    }

  private:
    Page grid(const std::string& url) {
        auto d = doc(url);
        return list(*d, "div.Container ul.ListAnimes li article", [&](const html::Node& el, Anime& a) {
            a.url = d->absUrl(el.selectFirst("div.Description a.Button"), "href");
            a.title = el.selectFirst("a h3").text();
            std::string img = el.selectFirst("a div.Image figure img").attr("src");
            a.thumbnail = startsWith(img, "/storage") ? baseUrl() + img : img;
        }, "ul.pagination li a[rel=\"next\"]");
    }
};

// =============================================================================================== Hentaijk (18+)

// FACTORY: out.push_back(std::make_shared<Hentaijk>());
class Hentaijk : public EsSource {
  public:
    Hentaijk() : EsSource({"es.hentaijk", "Hentaijk", "https://hentaijk.com", true}) {}

    Page popular(int page) override {
        if (page > 1) return {};
        auto d = doc(baseUrl() + "/top/");
        return list(*d, "div.col-lg-12 div.list", [&](const html::Node& el, Anime& a) {
            html::Node l = el.selectFirst("div#conb a");
            a.url = l.attr("href");
            a.title = l.attr("title");
            a.thumbnail = l.selectFirst("img").attr("src");
        });
    }
    Page latest(int page) override {
        return parseSearch(baseUrl() + "/directorio/" + std::to_string(page) +
                           "/?filtro=fecha&tipo=none&estado=none&fecha=none&temporada=none&orden=desc");
    }
    Page search(const std::string& q, int page) override {
        return parseSearch(baseUrl() + "/buscar/" + queryEnc(q) + "/" + std::to_string(page) + "/?filtro=fecha&tipo=none&estado=none&orden=desc");
    }

    Details details(const std::string& url) override {
        std::string pageUrl = trimChars(abs(url), "/");
        auto d = doc(pageUrl);
        Details r;
        r.thumbnail = d->selectFirst("div.col-lg-3 div.anime__details__pic.set-bg").attr("data-setbg");
        r.title = d->selectFirst("div.anime__details__text div.anime__details__title h3").text();
        r.description = ownText(d->selectFirst("div.col-lg-9 div.anime__details__text p"));
        for (auto& li : d->select("div.row div.col-lg-6.col-md-6 ul li")) {
            std::string data = textOf(li.select("span"));
            if (contains(data, "Genero")) r.genre = joinText(li.select("a"));
            if (contains(data, "Estado"))
                r.status = contains(data, "Concluido") ? "Completato" : (contains(data, "En emision") || contains(data, "Por estrenar")) ? "In corso" : "";
            if (contains(data, "Studios")) r.author = textOf(li.select("a"));
        }
        std::string script = scriptWith(*d, {"var invertir ="});
        std::string animeId = substringBefore(substringAfter(script, "'/ajax/last_episode/"), "/',");
        if (script.empty() || animeId == script || animeId.empty()) return r;
        auto pages = d->select("div.anime__pagination a");
        std::string first = pages.empty() ? "" : replaceAll(pages.front().attr("href"), "#pag", "");
        std::string last = pages.empty() ? "" : replaceAll(pages.back().attr("href"), "#pag", "");
        std::string base = rel(pageUrl);
        auto add = [&](const std::string& n) {
            Episode e;
            e.number = parseNumber(n, 0);
            e.name = "Episodio " + n;
            e.url = base + "/" + n;
            r.episodes.push_back(e);
        };
        if (first != last) {
            int lp = std::atoi(last.c_str());
            for (int i = 1; i < lp; i++)
                for (int j = 1; j <= 12; j++) add(std::to_string(j + (i - 1) * 12));
        }
        std::string body = fetch(baseUrl() + "/ajax/pagination_episodes/" + animeId + "/" + last);
        for (auto& part : split(replaceAll(body, "}]", ""), "}")) {
            std::string n = substringBefore(substringAfter(part, "\"number\":\""), "\"");
            if (!n.empty() && n != part && digitsOnly(n) == n) add(n);
        }
        reverseEps(r.episodes);
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        std::string pageUrl = abs(url);
        auto d = doc(pageUrl);
        std::string script = scriptWith(*d, {"var video = [];"});
        std::vector<Video> out;
        for (auto& a : d->select("div.col-lg-12.rounded.bg-servers.text-white.p-3.mt-2 a")) {
            std::string server = a.text(), sid = a.attr("data-id");
            std::string u = substringBefore(substringAfter(script, "video[" + sid + "] = '<iframe class=\"player_conte\" src=\""), "\"");
            if (u == script || u.empty()) continue;
            u = replaceAll(u, baseUrl() + "/jkokru.php?u=", "http://ok.ru/videoembed/");
            u = replaceAll(u, baseUrl() + "/jkvmixdrop.php?u=", "https://mixdrop.co/e/");
            u = replaceAll(u, baseUrl() + "/jk.php?u=", baseUrl() + "/");
            try {
                if (contains(u, "um2")) {
                    html::Document fd(http::getText(u, {{"Referer", pageUrl}}), u);
                    std::string key = fd.selectFirst("form input[value]").attr("value");
                    http::Response pr = http::request("POST", baseUrl() + "/gsplay/redirect_post.php",
                                                      {{"Referer", u}, {"Content-Type", "application/x-www-form-urlencoded"},
                                                       {"Origin", baseUrl()}},
                                                      "data=" + http::urlEncode(key), 30, false);
                    std::string loc = pr.header("location");
                    if (!loc.empty()) {
                        std::string postKey = replaceAll(loc, "/gsplay/player.html#", "");
                        http::Response nr = http::request("POST", baseUrl() + "/gsplay/api.php",
                                                          {{"Content-Type", "application/x-www-form-urlencoded"}}, "v=" + postKey);
                        for (auto& f : split(nr.body, "}")) {
                            std::string fu = replaceAll(substringBefore(substringAfter(f, "\"file\":\""), "\""), "\\", "");
                            if (!trim(fu).empty() && !contains(fu, "{") && fu != f) out.push_back(simpleVideo(fu, server, fu));
                        }
                    }
                }
                if (contains(u, "ok")) append(out, okru(u, ""));
                else if (contains(u, "stream/jkmedia")) out.push_back(simpleVideo(u, "Xtreme S", u));
                else if (contains(u, "um.php")) {
                    html::Document ud(http::getText(u), u);
                    std::string s = scriptWith(ud, {"var parts = {"});
                    std::string su = substringBefore(substringAfter(s, "url: '"), "'");
                    if (startsWith(su, "http")) out.push_back(simpleVideo(su, server, su));
                }
            } catch (const std::exception&) {
            }
        }
        return ensure(out);
    }

  private:
    Page parseSearch(const std::string& url) {
        auto d = doc(url);
        Page p;
        for (auto& el : d->select(".col-lg-2.col-md-6.col-sm-6")) {
            Anime a;
            a.title = el.selectFirst("div.anime__item #ainfo div.title").text();
            a.thumbnail = el.selectFirst("div.anime__item a div.anime__item__pic").attr("data-setbg");
            a.url = rel(el.selectFirst("div.anime__item a").attr("href"));
            if (!a.url.empty()) p.animes.push_back(a);
        }
        if (p.animes.empty())
            for (auto& el : d->select(".card.mb-3.custom_item2")) {
                Anime a;
                html::Node t = el.selectFirst("div.row div.col-md-7 div.card-body h5.card-title a");
                a.title = t.text();
                a.url = rel(t.attr("href"));
                a.thumbnail = el.selectFirst("div.row div.custom_thumb2 a img").attr("src");
                if (!a.url.empty()) p.animes.push_back(a);
            }
        p.hasNextPage = d->selectFirst("div.navigation a.nav-next").valid();
        return p;
    }
};

// =============================================================================================== Hentaila (18+)

// FACTORY: out.push_back(std::make_shared<Hentaila>());
class Hentaila : public EsSource {
  public:
    Hentaila() : EsSource({"es.hentaila", "Hentaila", "https://hentaila.com", true}) {}

    Page popular(int page) override { return catalog("order=popular&page=" + std::to_string(page)); }
    Page latest(int page) override { return catalog("order=latest_released&page=" + std::to_string(page)); }
    Page search(const std::string& q, int page) override {
        return catalog("page=" + std::to_string(page) + "&search=" + queryEnc(q));
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details r;
        bool ongoing = false;
        for (auto& s : d->select("div.flex.flex-wrap.items-center.text-sm span"))
            if (s.text() == "En emisión") ongoing = true;
        r.status = ongoing ? "In corso" : "Completato";
        r.thumbnail = d->selectFirst("img.object-cover.w-full.aspect-poster").attr("src");
        r.title = d->selectFirst(".grid.items-start h1.text-lead").text();
        r.description = textOf(d->select(".entry.text-lead.text-sm p"));
        std::vector<std::string> g;
        for (auto& b : d->select(".flex-wrap.items-center .btn.btn-xs.rounded-full"))
            if (!contains(b.attr("class"), "sm:w-auto")) g.push_back(b.text());
        r.genre = joinStr(g);
        std::string animeId = lower(substringBefore(substringBefore(substringAfter(url, "media/"), "/"), "?"));
        for (auto& art : d->select("article[class~=\"group/item\"]")) {
            std::string n = textOf(art.select("div.bg-line.text-subs span"));
            if (n.empty() || parseNumber(n, -1) < 0) continue;
            Episode e;
            e.number = parseNumber(n, 0);
            e.name = "Episodio " + n;
            e.url = "/media/" + animeId + "/" + n;
            r.episodes.push_back(e);
        }
        reverseEps(r.episodes);
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        json j = parseJson(fetch(baseUrl() + url + "/__data.json"));
        std::vector<std::pair<std::string, std::string>> servers;
        if (j.is_object() && j.contains("nodes") && j["nodes"].is_array())
            for (auto& node : j["nodes"]) {
                if (!node.is_object() || !node.contains("uses") || !node["uses"].is_object()) continue;
                json params = node["uses"].value("params", json());
                if (!params.is_array() || params.empty() || params[0] != "number") continue;
                json data = node.value("data", json());
                auto at = [&](const json& idx) -> json {
                    if (!idx.is_number_integer()) return json();
                    int i = idx.get<int>();
                    return i >= 0 && i < (int)data.size() ? data[i] : json();
                };
                if (!data.is_array() || data.empty()) continue;
                json result = data[0];
                json embeds = at(result.value("embeds", json()));
                if (!embeds.is_object()) continue;
                json sub = at(embeds.value("SUB", json()));
                if (!sub.is_array()) continue;
                for (auto& code : sub) {
                    json src = at(code);
                    if (!src.is_object()) continue;
                    json name = at(src.value("server", json())), u = at(src.value("url", json()));
                    if (name.is_string() && u.is_string()) servers.push_back({name.get<std::string>(), u.get<std::string>()});
                }
            }
        std::vector<Video> out;
        for (auto& s : servers) {
            std::string n = lower(s.first), u = s.second;
            try {
                if (n == "vip") append(out, hlsVideos(replaceAll(u, "/play/", "/m3u8/"), "", "VIP - "));
                else if (n == "streamwish") append(out, streamWish(u, ""));
                else if (n == "mp4upload") append(out, mp4upload(u, ""));
                else if (n == "voe") append(out, voe(u, ""));
                else if (n == "arc") out.push_back(simpleVideo(substringAfter(u, "#"), "Arc"));
                else if (n == "yupi" || n == "yourupload") append(out, yourUpload(u, ""));
                else if (n == "burst") append(out, burstCloud(u, ""));
                else if (n == "sendvid") append(out, sendvid(u, ""));
                else if (n == "mediafire") append(out, mediafire(u, ""));
                else if (n == "fireload") {
                    html::Document fd(http::getText(u), u);
                    std::string s2 = scriptWith(fd, {"dlink"});
                    std::string dl = substringBefore(substringAfter(s2, "dlink\" : \""), "\",");
                    if (startsWith(dl, "http")) out.push_back(simpleVideo(dl, "FireLoad"));
                } else if (n == "vidhide") append(out, vidHide(u, ""));
            } catch (const std::exception&) {
            }
        }
        sortVideos(out, "VidHide");
        return ensure(out);
    }

  private:
    Page catalog(const std::string& query) {
        json j = parseJson(fetch(baseUrl() + "/catalogo/__data.json?" + query));
        Page p;
        if (!j.is_object() || !j.contains("nodes") || !j["nodes"].is_array()) return p;
        for (auto& node : j["nodes"]) {
            if (!node.is_object() || !node.contains("uses") || !node["uses"].is_object() || !node["uses"].contains("search_params"))
                continue;
            json data = node.value("data", json());
            if (!data.is_array() || data.empty() || !data[0].is_object()) continue;
            auto at = [&](const json& idx) -> json {
                if (!idx.is_number_integer()) return json();
                int i = idx.get<int>();
                return i >= 0 && i < (int)data.size() ? data[i] : json();
            };
            auto str = [&](const json& obj, const char* key) -> std::string {
                json v = at(obj.value(key, json()));
                if (v.is_string()) return v.get<std::string>();
                if (v.is_number_integer()) return std::to_string(v.get<long long>());
                return "";
            };
            json result = data[0];
            json ids = at(result.value("results", json()));
            if (!ids.is_array()) continue;
            for (auto& id : ids) {
                json obj = at(id);
                if (!obj.is_object()) continue;
                Anime a;
                a.title = str(obj, "title");
                std::string slug = str(obj, "slug");
                if (a.title.empty() || slug.empty()) continue;
                a.url = "/media/" + slug;
                std::string aid = str(obj, "id");
                if (!aid.empty()) a.thumbnail = "https://cdn.hentaila.com/covers/" + aid + ".jpg";
                p.animes.push_back(a);
            }
            json pag = at(result.value("pagination", json()));
            if (pag.is_object()) {
                json cur = at(pag.value("currentPage", json())), tot = at(pag.value("totalPages", json()));
                if (cur.is_number() && tot.is_number()) p.hasNextPage = cur.get<int>() < tot.get<int>();
            }
            return p;
        }
        return p;
    }
};

// =============================================================================================== HentaiTk (18+)

// FACTORY: out.push_back(std::make_shared<HentaiTk>());
class HentaiTk : public EsSource {
  public:
    HentaiTk() : EsSource({"es.hentaitk", "HentaiTk", "https://hentaitk.net", true}) {}
    bool supportsLatest() const override { return false; }

    Page popular(int page) override { return grid(baseUrl() + "/hentais/page/" + std::to_string(page)); }
    Page latest(int page) override { return popular(page); }
    Page search(const std::string& q, int page) override {
        return grid(baseUrl() + "/page/" + std::to_string(page) + "/?s=" + queryEnc(q));
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details r;
        r.title = d->selectFirst(".video-info h1").text();
        r.description = d->selectFirst(".video-details .post-entry p:not([style])").text();
        r.genre = joinText(d->select(".video-details .meta a"));
        r.thumbnail = d->selectFirst("meta[property=og:image]").attr("content");
        Episode e = movieEpisode(rel(d->url()), "Episode");
        r.episodes.push_back(e);
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::vector<std::string> urls;
        auto pages = d->select(".post-tape .page-link");
        if (!pages.empty()) {
            for (auto& p : pages) {
                try {
                    auto vd = doc(d->absUrl(p, "href"));
                    urls.push_back(vd->selectFirst(".embed-responsive-item iframe").attr("src"));
                } catch (const std::exception&) {
                }
            }
        } else {
            for (auto& f : d->select(".embed-responsive-item iframe")) urls.push_back(f.attr("src"));
        }
        std::vector<Video> out;
        for (auto& u : urls) {
            std::string e = lower(u);
            try {
                if (contains(e, "ok.ru") || contains(e, "okru")) append(out, okru(u, ""));
                else if (contains(e, "lion")) {
                    auto v = streamWish(u, "");
                    for (auto& x : v) x.title = replaceAll(x.title, "StreamWish", "FileLions");
                    append(out, v);
                } else if (contains(e, "wish")) append(out, streamWish(u, ""));
                else if (contains(e, "vidhide") || contains(e, "streamhide") || contains(e, "guccihide") || contains(e, "streamvid")) append(out, vidHide(u, ""));
                else if (contains(e, "voe")) append(out, voe(u, ""));
                else if (contains(e, "upload")) append(out, yourUpload(u, ""));
                else if (contains(e, "dood") || contains(e, "d000d") || contains(e, "ds2play")) append(out, dood(u, ""));
                else if (contains(e, "streamtape") || contains(e, "stp") || contains(e, "stape")) append(out, streamtape(u, ""));
                else append(out, resolve(u, "", baseUrl()));
            } catch (const std::exception&) {
            }
        }
        sortVideos(out, "FileLions");
        return ensure(out);
    }

  private:
    static std::string bestSrcset(const html::Node& img) {
        if (!img.hasAttr("srcset")) return img.attr("src");
        std::string best;
        int bw = -1;
        for (auto& part : split(img.attr("srcset"), ", ")) {
            auto bits = split(trim(part), " ");
            if (bits.size() < 2) continue;
            int w = std::atoi(bits[1].c_str());
            if (w > bw) {
                bw = w;
                best = bits[0];
            }
        }
        return best.empty() ? img.attr("src") : best;
    }
    Page grid(const std::string& url) {
        auto d = doc(url);
        return list(*d, ".video-section .item", [&](const html::Node& el, Anime& a) {
            html::Node t = el.selectFirst(".post-header .post-title a");
            a.url = d->absUrl(t, "href");
            a.title = t.text();
            a.thumbnail = bestSrcset(el.selectFirst(".item-img a img"));
        }, ".pagination .page-item .next");
    }
};

// =============================================================================================== Jkhentai (18+)

// FACTORY: out.push_back(std::make_shared<Jkhentai>());
class Jkhentai : public EsSource {
  public:
    Jkhentai() : EsSource({"es.jkhentai", "Jkhentai", "https://www.jkhentai.net", true}) {}
    bool supportsLatest() const override { return false; }

    Page popular(int page) override { return grid(baseUrl() + "/lista/" + std::to_string(page)); }
    Page latest(int page) override { return popular(page); }
    Page search(const std::string& q, int page) override {
        return grid(baseUrl() + "/buscador.php?search=" + queryEnc(q) + "&page=" + std::to_string(page));
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details r;
        const std::string pre = "div#contenedor div.items.ptts div#movie div.post div.headingder div.datos ";
        r.thumbnail = d->selectFirst(pre + "div.imgs.tsll a img").attr("src");
        r.title = d->selectFirst(pre + "div.dataplus h1").text();
        r.description = "Titulo Original: " + ownText(d->selectFirst(pre + "div.dataplus span.original"));
        r.genre = joinText(d->select(pre + "div.dataplus div#dato-1.data-content div.xmll p.xcsd strong a"));
        r.status = "Completato";
        std::string animeId = replaceAll(replaceAll(pathSegment(d->url(), -1), "-sub-espanol", ""), "-080p", "-1080p");
        for (auto& li : d->select("div#contenedor div.items.ptts div#movie div.post div#cssmenu ul li.has-sub.open ul li")) {
            std::string n = replaceAll(li.selectFirst("a").attr("href"), "https://www.jkhentai.net/ver/" + animeId + "-", "");
            Episode e;
            e.number = parseNumber(n, 0);
            e.name = "Episodio " + n;
            e.url = "/ver/" + animeId + "-" + n;
            r.episodes.push_back(e);
        }
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        const std::string pre = "div#contenedor div.items.ptts div#movie div.post div#player-container ";
        auto names = d->select(pre + "ul.player-menu li");
        auto plays = d->select(pre + "div.play-c");
        std::vector<Video> out;
        for (size_t i = 0; i < plays.size(); i++) {
            std::string u = plays[i].selectFirst("div.player-content iframe").attr("src");
            std::string server = i < names.size() ? textOf(names[i].select("a")) : "";
            try {
                if (server == "StreamTape" || contains(u, "streamtape")) append(out, streamtape(u, ""));
                else if (server == "Upload" || contains(u, "yourupload")) append(out, yourUpload(u, ""));
                else append(out, resolve(u, "", baseUrl(), server));
            } catch (const std::exception&) {
            }
        }
        return ensure(out);
    }

  private:
    Page grid(const std::string& url) {
        auto d = doc(url);
        return list(*d, "div#contenedor div.items div#directorio div#box_movies div.movie", [&](const html::Node& el, Anime& a) {
            a.url = el.selectFirst("div.imagen a").attr("href");
            a.title = textOf(el.select("h2"));
            a.thumbnail = el.selectFirst("div.imagen img").attr("src");
        }, "a.page.larger");
    }
};

// =============================================================================================== VeoHentai (18+)

// FACTORY: out.push_back(std::make_shared<VeoHentai>());
class VeoHentai : public EsSource {
  public:
    VeoHentai() : EsSource({"es.veohentai", "VeoHentai", "https://veohentai.com", true}) {}

    Page popular(int page) override { return grid(baseUrl() + "/mas-visitados/page/" + std::to_string(page)); }
    Page latest(int page) override { return grid(baseUrl() + "/page/" + std::to_string(page)); }
    Page search(const std::string& q, int page) override {
        return grid(baseUrl() + "/page/" + std::to_string(page) + "/?s=" + queryEnc(q));
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details r;
        r.title = trim(d->selectFirst(".pb-2 h1").text());
        r.description = joinText(d->select(".entry-content p"));
        r.genre = joinText(d->select(".tags a"));
        r.thumbnail = imgUrl(*d, d->selectFirst("#thumbnail-post img"));
        for (auto& div : d->select(".gap-4 div")) {
            std::string t = div.text();
            if (contains(t, "Marca")) r.author = trim(substringAfter(t, "Marca"));
        }
        r.episodes.push_back(movieEpisode(rel(d->url()), "Capítulo"));
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        html::Node frame = d->selectFirst("iframe[webkitallowfullscreen]");
        std::string link = d->absUrl(frame, "src");
        if (link.empty() || startsWith(link, "about")) link = frame.attr("data-litespeed-src");
        if (link.empty() || startsWith(link, "about")) throw http::Error("Nessun video trovato");
        http::Response pr = http::request("GET", link, {{"Referer", baseUrl() + "/"}});
        html::Document player(pr.body, pr.finalUrl.empty() ? link : pr.finalUrl);
        std::string dataId = player.selectFirst("[data-id]").attr("data-id");
        if (dataId.empty()) throw http::Error("Nessun video trovato");
        std::string host = http::hostOf(player.url());
        std::string realUrl = "https://" + host + dataId;
        html::Document real(http::getText(realUrl, {{"Referer", player.url()}}), realUrl);
        std::string script = scriptWith(real, {"jwplayer.key"});
        std::vector<Video::Track> subs;
        std::string tracks = substringBefore(substringAfter(script, "tracks:"), "]");
        for (auto& item : braceItems(tracks)) {
            std::string file = fieldOf(item, "file"), label = fieldOf(item, "label");
            if (!file.empty()) subs.push_back({file, label});
        }
        std::vector<Video> out;
        for (auto& item : braceItems(substringBefore(substringAfter(script, "sources:"), "]"))) {
            std::string file = fieldOf(item, "file");
            if (file.empty()) continue;
            std::string type = contains(file, ".m3u") ? "HSL" : contains(file, ".mp4") ? "MP4" : "";
            Video v = simpleVideo(file, "VeoHentai:" + type, realUrl);
            v.subtitles = subs;
            out.push_back(v);
        }
        return ensure(out);
    }

  private:
    static std::vector<std::string> braceItems(const std::string& s) {
        std::vector<std::string> out;
        size_t p = 0;
        while ((p = s.find('{', p)) != std::string::npos) {
            auto e = s.find('}', p);
            if (e == std::string::npos) break;
            out.push_back(s.substr(p + 1, e - p - 1));
            p = e + 1;
        }
        return out;
    }
    static std::string fieldOf(const std::string& item, const std::string& key) {
        std::string v = substringAfter(item, key + "\": \"");
        if (v == item) v = substringAfter(item, key + ": \"");
        if (v == item) return "";
        return substringBefore(v, "\"");
    }
    Page grid(const std::string& url) {
        auto d = doc(url);
        Page p = list(*d, ".gap-6 a", [&](const html::Node& el, Anime& a) {
            a.title = trim(el.selectFirst("h2").text());
            a.thumbnail = imgUrl(*d, el.selectFirst("img:not([class*=cover])"));
            a.url = d->absUrl(el, "href");
        });
        p.hasNextPage = anyContainsText(*d, ".nav-links a", "Next");
        return p;
    }
};

// =============================================================================================== Samato's Den: Videos (18+)

// FACTORY: out.push_back(std::make_shared<SamatoDenVideos>());
class SamatoDenVideos : public EsSource {
  public:
    SamatoDenVideos() : EsSource({"es.samatodenvideos", "Samato's Den: Videos", "https://samatoden.blogspot.com", true}) {}
    bool supportsLatest() const override { return false; }

    Page popular(int page) override { return feed(page, ""); }
    Page latest(int page) override { return popular(page); }
    Page search(const std::string& q, int page) override { return feed(page, trim(q)); }

    Details details(const std::string& url) override {
        json entry = single(url);
        Details r;
        r.title = jt(entry.value("title", json()));
        std::string html = jt(entry.value("content", json()));
        r.thumbnail = thumbOf(entry, html);
        std::vector<std::string> cats;
        if (entry.contains("category") && entry["category"].is_array())
            for (auto& c : entry["category"]) cats.push_back(jstr(c, "term"));
        r.genre = joinStr(cats);
        r.status = "Completato";
        std::string referer = linkHref(entry, "alternate");
        if (referer.empty()) referer = baseUrl() + "/";
        size_t pl = html.find("playlist:");
        if (pl == std::string::npos) pl = html.find("playlist :");
        std::string playlist = pl == std::string::npos ? "" : balancedAfter(html, "", html.find('[', pl));
        int idx = 0;
        if (!playlist.empty()) {
            size_t p = 0;
            while ((p = playlist.find('{', p)) != std::string::npos) {
                std::string block = balancedAfter(playlist, "", p);
                if (block.empty()) break;
                p += block.size();
                std::string file = jsString(block, "file");
                if (file.empty()) continue;
                idx++;
                std::string title = jsString(block, "title");
                Episode e;
                e.name = title.empty() ? r.title + " " + std::to_string(idx) : title;
                e.number = idx;
                e.url = json({{"v", file}, {"r", referer}}).dump();
                r.episodes.push_back(e);
            }
        }
        if (r.episodes.empty()) {
            std::string file = jsString(html, "file");
            if (!file.empty()) {
                Episode e;
                e.name = r.title.empty() ? "Video" : r.title;
                e.number = 1;
                e.url = json({{"v", file}, {"r", referer}}).dump();
                r.episodes.push_back(e);
            }
        }
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        json j = parseJson(url);
        std::string v = jstr(j, "v"), ref = jstr(j, "r");
        if (v.empty()) throw http::Error("Nessun video trovato");
        if (ref.empty()) ref = baseUrl() + "/";
        return {simpleVideo(v, "Video", ref, {{"Origin", http::originOf(ref)}, {"Accept", "*/*"}})};
    }

  protected:
    http::Headers baseHeaders() const override {
        return {{"Referer", baseUrl() + "/"}, {"Accept", "application/json, text/html, */*"}};
    }

  private:
    static std::string jt(const json& o) { return o.is_object() ? jstr(o, "$t") : ""; }

    static std::string linkHref(const json& entry, const std::string& relName) {
        if (!entry.contains("link") || !entry["link"].is_array()) return "";
        for (auto& l : entry["link"])
            if (jstr(l, "rel") == relName) return jstr(l, "href");
        return "";
    }

    static std::string feedUrl(const std::string& u) { return contains(u, "alt=json") ? u : u + (contains(u, "?") ? "&" : "?") + "alt=json"; }

    static std::string thumbOf(const json& entry, const std::string& html) {
        auto p = html.find("<img");
        if (p != std::string::npos) {
            std::string tag = html.substr(p, std::min<size_t>(html.size() - p, 1024));
            std::string src = substringBefore(substringAfter(tag, "src=\""), "\"");
            if (src != tag && !src.empty()) return src;
        }
        auto pl = html.find("playlist");
        if (pl != std::string::npos) {
            std::string img = jsString(html, "image", pl);
            if (!img.empty()) return img;
        }
        if (entry.contains("media$thumbnail")) return substringBefore(jstr(entry["media$thumbnail"], "url"), "=s72");
        return "";
    }

    json single(const std::string& url) {
        json j = parseJson(fetch(feedUrl(abs(url))));
        if (j.is_object() && j.contains("entry")) return j["entry"];
        if (j.is_object() && j.contains("feed") && j["feed"].contains("entry") && j["feed"]["entry"].is_array() && !j["feed"]["entry"].empty())
            return j["feed"]["entry"][0];
        throw http::Error("Nessuna voce trovata nella risposta di Blogger");
    }

    Page feed(int page, const std::string& q) {
        std::string url = baseUrl() + "/feeds/posts/default/-/videos?alt=json&max-results=30&start-index=" + std::to_string((page - 1) * 30 + 1);
        if (!q.empty()) url += "&q=" + http::urlEncode(q);
        json j = parseJson(fetch(url));
        Page p;
        json f = j.is_object() ? j.value("feed", json()) : json();
        if (!f.is_object()) return p;
        if (f.contains("entry") && f["entry"].is_array())
            for (auto& e : f["entry"]) {
                Anime a;
                a.url = feedUrl(linkHref(e, "self"));
                a.title = jt(e.value("title", json()));
                a.thumbnail = thumbOf(e, jt(e.value("content", json())));
                if (!a.title.empty()) p.animes.push_back(a);
            }
        int total = (int)parseNumber(jt(f.value("openSearch$totalResults", json())), (double)p.animes.size());
        int start = (int)parseNumber(jt(f.value("openSearch$startIndex", json())), 1);
        int per = (int)parseNumber(jt(f.value("openSearch$itemsPerPage", json())), (double)p.animes.size());
        p.hasNextPage = start + per - 1 < total;
        return p;
    }
};

// =============================================================================================== tema PelisPlus

/** Base comune del tema multisrc "pelisplus": risolutore dei server e ordinamento. */
class PelisPlusBase : public EsSource {
  public:
    explicit PelisPlusBase(Info i) : EsSource(i) {}
    bool supportsLatest() const override { return false; }
    Page latest(int page) override { return popular(page); }

  protected:
    /** serverVideoResolver del tema: abbina per nome server (se presente) o per URL. */
    std::vector<Video> resolvePP(const std::string& url, const std::string& prefix, const std::string& serverName = "") {
        std::string source = lower(serverName.empty() ? url : serverName);
        static const std::vector<std::pair<const char*, std::vector<const char*>>> conv = {
            {"voe", {"voe", "tubelessceliolymph", "simpulumlamerop", "urochsunloath", "nathanfromsubject", "yip.", "metagnathtuggers", "donaldlineelse"}},
            {"okru", {"ok.ru", "okru"}},
            {"filemoon", {"filemoon", "moonplayer", "moviesm4u", "files.im"}},
            {"amazon", {"amazon", "amz"}},
            {"uqload", {"uqload"}},
            {"mp4upload", {"mp4upload"}},
            {"streamwish", {"wishembed", "streamwish", "strwish", "wish", "kswplayer", "swhoi", "multimovies", "uqloads", "neko-stream", "swdyu", "iplayerhls", "streamgg"}},
            {"doodstream", {"doodstream", "dood.", "ds2play", "doods.", "ds2video", "dooood", "d000d", "d0000d"}},
            {"streamlare", {"streamlare", "slmaxed"}},
            {"yourupload", {"yourupload", "upload"}},
            {"burstcloud", {"burstcloud", "burst"}},
            {"fastream", {"fastream"}},
            {"upstream", {"upstream"}},
            {"streamsilk", {"streamsilk"}},
            {"streamtape", {"streamtape", "stp", "stape", "shavetape"}},
            {"vidhide", {"ahvsh", "streamhide", "guccihide", "streamvid", "vidhide", "kinoger", "smoothpre", "dhtpre", "peytonepre", "earnvids", "ryderjet"}},
            {"vidguard", {"vembed", "guard", "listeamed", "bembed", "vgfplay"}},
        };
        std::string matched;
        for (auto& c : conv) {
            for (auto* n : c.second)
                if (contains(source, n)) matched = c.first;
            if (!matched.empty()) break;
        }
        std::string p = prefix.empty() ? "" : prefix + " ";
        if (matched == "voe") return voe(url, p);
        if (matched == "okru") return okru(url, p);
        if (matched == "filemoon") return moon(url, baseUrl(), p + "Filemoon:");
        if (matched == "amazon") return amazon(url, p);
        if (matched == "uqload") return uqload(url, p);
        if (matched == "mp4upload") return mp4upload(url, p);
        if (matched == "streamwish") return streamWish(url, p);
        if (matched == "doodstream") return dood(url, p);
        if (matched == "streamlare") return streamlare(url, prefix);
        if (matched == "yourupload") return yourUpload(url, p);
        if (matched == "burstcloud") return burstCloud(url, p);
        if (matched == "fastream") return fastream(url, p);
        if (matched == "upstream") return upstream(url, p);
        if (matched == "streamtape") return streamtape(url, p);
        if (matched == "vidhide") return vidHide(url, p + "- ");
        if (matched == "vidguard" || matched == "streamsilk") return {};  // richiedono un motore JS
        return resolve(url, p, baseUrl(), serverName);  // al posto di UniversalExtractor (WebView)
    }

    static std::string langOf(const std::string& s) {
        std::string l = lower(s);
        if (contains(l, "0") || contains(l, "lat")) return "[LAT]";
        if (contains(l, "1") || contains(l, "cast")) return "[CAST]";
        if (contains(l, "2") || contains(l, "eng") || contains(l, "sub")) return "[SUB]";
        return "";
    }

    static void finish(std::vector<Video>& out, const std::string& lang = "") { sortVideos(out, "VidHide", lang, 1080); }
};

// ------------------------------------------------------------------------------ PelisPlusHD

// FACTORY: out.push_back(std::make_shared<PelisPlusHd>());
class PelisPlusHd : public PelisPlusBase {
  public:
    PelisPlusHd() : PelisPlusBase({"es.pelisplushd", "PelisPlusHD", "https://pelisplushd.bz", false}) {}

    Page popular(int page) override { return grid(baseUrl() + "/series?page=" + std::to_string(page)); }
    Page search(const std::string& q, int page) override {
        return grid(baseUrl() + "/search?s=" + queryEnc(q) + "&page=" + std::to_string(page));
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details r;
        r.title = d->selectFirst("h1.m-b-5").text();
        r.thumbnail = replaceAll(d->selectFirst("div.card-body div.row div.col-sm-3 img.img-fluid").attr("src"), "/w154/", "/w500/");
        r.description = ownText(d->selectFirst("div.col-sm-4 div.text-large"));
        r.genre = joinText(d->select("div.p-v-20.p-h-15.text-center a span"));
        r.status = "Completato";
        if (contains(d->url(), "/pelicula/")) {
            r.episodes.push_back(movieEpisode(rel(d->url()), "PELÍCULA"));
        } else {
            int idx = 0;
            for (auto& a : d->select("div.tab-content div a")) {
                Episode e;
                e.number = ++idx;
                e.name = a.text();
                e.url = rel(d->absUrl(a, "href"));
                r.episodes.push_back(e);
            }
            reverseEps(r.episodes);
        }
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::string data = scriptWith(*d, {"video[1] = "});
        std::vector<Video> out;
        for (auto& opt : quotedUrls(data, "embed69.org", '\'')) {
            try {
                auto od = doc(opt);
                std::string crypto = scriptWith(*od, {"let dataLink"});
                if (!trim(crypto).empty()) {
                    json arr = parseJson(substringBefore(substringAfter(crypto, "let dataLink ="), "];") + "]");
                    if (!arr.is_array()) continue;
                    for (auto& item : arr) {
                        json embeds = item.value("sortedEmbeds", json::array());
                        std::string lng = jstr(item, "video_language");
                        json links = json::array();
                        for (auto& e : embeds)
                            if (e.is_object() && e.contains("link") && e["link"].is_string()) links.push_back(e["link"]);
                        http::Response pr = http::request("POST", "https://embed69.org/api/decrypt", {{"Content-Type", "application/json"}},
                                                          json({{"links", links}}).dump());
                        json dec = parseJson(pr.body);
                        if (!dec.is_object() || !dec.contains("links") || !dec["links"].is_array()) continue;
                        for (auto& l : dec["links"]) {
                            std::string link = jstr(l, "link");
                            if (link.empty()) continue;
                            int idx = (int)jnum(l, "index", -1);
                            std::string server = idx >= 0 && idx < (int)embeds.size() ? jstr(embeds[idx], "servername") : "Embed69";
                            if (server.empty()) server = "Embed69";
                            try {
                                append(out, resolvePP(link, lng, server));
                            } catch (const std::exception&) {
                            }
                        }
                    }
                } else {
                    for (auto& li : od->select("li[onclick]"))
                        for (auto& u : allUrls(li.attr("onclick"))) {
                            try {
                                append(out, resolvePP(u, ""));
                            } catch (const std::exception&) {
                            }
                        }
                }
            } catch (const std::exception&) {
            }
        }
        finish(out);
        return ensure(out);
    }

  private:
    Page grid(const std::string& url) {
        auto d = doc(url);
        return list(*d, "div.Posters a.Posters-link", [&](const html::Node& el, Anime& a) {
            a.url = d->absUrl(el, "href");
            a.title = textOf(el.select("div.listing-content p"));
            a.thumbnail = replaceAll(el.selectFirst("img").attr("src"), "/w154/", "/w200/");
        }, "a.page-link");
    }
};

// ------------------------------------------------------------------------------ PelisPlusPh

// FACTORY: out.push_back(std::make_shared<PelisPlusPh>());
class PelisPlusPh : public PelisPlusBase {
  public:
    PelisPlusPh() : PelisPlusBase({"es.pelisplusph", "PelisPlusPh", "https://www.pelisplushd.la", false}) {}

    Page popular(int page) override { return grid(baseUrl() + "/peliculas?page=" + std::to_string(page)); }
    Page search(const std::string& q, int page) override {
        return grid(baseUrl() + "/search?s=" + queryEnc(q) + "&page=" + std::to_string(page));
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details r;
        r.title = d->selectFirst(".card-body h1").text();
        r.thumbnail = d->absUrl(d->selectFirst(".card-body img"), "src");
        auto ps = d->select(".card-body p");
        for (size_t i = 0; i < ps.size(); i++) {
            auto& p = ps[i];
            if (contains(p.text(), "Sinopsis:")) {
                // nextElementSibling(): primo elemento fratello successivo
                auto sibs = p.parent().children();
                for (size_t k = 0; k + 1 < sibs.size(); k++)
                    if (sibs[k].raw() == p.raw()) r.description = sibs[k + 1].text();
            }
            std::string ct = textOf(p.select(".content-type"));
            if (contains(ct, "Géneros:")) r.genre = joinText(p.select(".content-type-a a"));
        }
        bool movie = contains(d->url(), "/pelicula/");
        r.status = contains(d->url(), "/serie/") ? "" : "Completato";
        if (movie) {
            r.episodes.push_back(movieEpisode(rel(d->url()), "PELÍCULA"));
        } else {
            int idx = 0;
            for (auto& a : d->select(".tab-content a")) {
                Episode e;
                e.number = ++idx;
                e.name = ownText(a);
                e.url = rel(d->absUrl(a, "href"));
                r.episodes.push_back(e);
            }
            reverseEps(r.episodes);
        }
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::vector<Video> out;
        for (auto& li : d->select(".TbVideoNv li")) {
            std::string ln = li.attr("data-name");
            std::string lang = contains(ln, "Subtitulado") ? "[SUB]" : contains(ln, "Latino") ? "[LAT]" : "[CAST]";
            try {
                append(out, resolvePP(li.attr("data-url"), lang, li.text()));
            } catch (const std::exception&) {
            }
        }
        finish(out, "[LAT]");
        return ensure(out);
    }

  private:
    Page grid(const std::string& url) {
        auto d = doc(url);
        Page p = list(*d, ".Posters-link", [&](const html::Node& el, Anime& a) {
            a.url = d->absUrl(el, "href");
            a.title = textOf(el.select(".listing-content > p"));
            a.thumbnail = d->absUrl(el.selectFirst("img"), "src");
        });
        p.hasNextPage = !p.animes.empty();  // popularAnimeNextPageSelector = "body"
        return p;
    }
};

// ------------------------------------------------------------------------------ PelisPlusTo

// FACTORY: out.push_back(std::make_shared<PelisPlusTo>());
class PelisPlusTo : public PelisPlusBase {
  public:
    PelisPlusTo() : PelisPlusBase({"es.pelisplusto", "PelisPlusTo", "https://tioplus.app", false}) {}

    Page popular(int page) override { return grid(baseUrl() + "/peliculas?page=" + std::to_string(page)); }
    Page search(const std::string& q, int page) override {
        if (page > 1) return {};
        return grid(baseUrl() + "/api/search/" + queryEnc(q));
    }

    Details details(const std::string& url) override {
        std::string pageUrl = abs(url);
        auto d = doc(pageUrl);
        Details r;
        r.title = d->selectFirst(".home__slider_content div h1.slugh1").text();
        r.description = d->selectFirst(".home__slider_content .description").text();
        std::vector<std::string> g;
        for (auto& a : d->select(".home__slider_content div > a"))
            if (contains(a.attr("href"), "genero")) g.push_back(a.text());
        r.genre = joinStr(g);
        r.thumbnail = d->selectFirst("meta[property=og:image]").attr("content");
        r.status = "Completato";
        if (contains(pageUrl, "/pelicula/")) {
            r.episodes.push_back(movieEpisode(rel(d->url()), "PELÍCULA"));
            return r;
        }
        std::string script = scriptWith(*d, {"const seasonUrl ="});
        json seasons = parseJson(balancedAfter(script, "seasonsJson ="));
        if (!seasons.is_object()) return r;
        int index = 0;
        std::string base = rel(pageUrl);
        for (auto& kv : seasons.items()) {
            if (!kv.value().is_array()) continue;
            std::vector<json> eps(kv.value().begin(), kv.value().end());
            std::reverse(eps.begin(), eps.end());
            for (auto& e : eps) {
                Episode ep;
                ep.number = ++index;
                std::string s = jstr(e, "season"), n = jstr(e, "episode");
                ep.name = "T" + s + " - E" + n + " - " + jstr(e, "title");
                ep.url = base + "/season/" + s + "/episode/" + n;
                r.episodes.push_back(ep);
            }
        }
        reverseEps(r.episodes);
        return r;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::vector<Video> out;
        for (auto& li : d->select(".bg-tabs ul li")) {
            std::string prefix = langOf(lower(ownText(li.parent().parent().selectFirst("button"))));
            std::string ds = li.attr("data-server");
            std::string decoded = base64Decode(ds);
            std::string u = allUrls(decoded).empty() ? baseUrl() + "/player/" + crypto::base64Encode(ds) : decoded;
            try {
                std::string videoUrl = u;
                if (contains(u, "/player/")) {
                    auto pd = doc(u);
                    auto urls = allUrls(scriptWith(*pd, {"window.onload"}));
                    videoUrl = urls.empty() ? "" : urls.front();
                }
                videoUrl = replaceAll(videoUrl, "https://sblanh.com", "https://lvturbo.com");
                auto p = videoUrl.find("=https://ww3.pelisplus.to");
                if (p != std::string::npos) {
                    size_t b = p;
                    while (b > 0 && (std::isalnum((unsigned char)videoUrl[b - 1]) || videoUrl[b - 1] == '_' || videoUrl[b - 1] == '-')) b--;
                    videoUrl = videoUrl.substr(0, b);
                }
                if (videoUrl.empty()) continue;
                append(out, resolvePP(videoUrl, prefix));
            } catch (const std::exception&) {
            }
        }
        finish(out);
        return ensure(out);
    }

  private:
    Page grid(const std::string& url) {
        auto d = doc(url);
        return list(*d, "article.item", [&](const html::Node& el, Anime& a) {
            a.url = d->absUrl(el.selectFirst("a"), "href");
            a.title = textOf(el.select("a h2"));
            a.thumbnail = el.selectFirst("a .item__image picture img").attr("data-src");
        }, "a[rel=\"next\"]");
    }
};

}  // namespace

std::vector<std::shared_ptr<Source>> makeSpanishSources() {
    std::vector<std::shared_ptr<Source>> out;
    out.push_back(std::make_shared<AnimeFlv>());
    out.push_back(std::make_shared<JkAnime>());
    out.push_back(std::make_shared<AnimeFenix>());
    out.push_back(std::make_shared<AnimeAv1>());
    out.push_back(std::make_shared<Latanime>());
    out.push_back(std::make_shared<MonosChinos>());
    out.push_back(std::make_shared<AnimeId>());
    out.push_back(std::make_shared<AnimeLatinoHd>());
    out.push_back(std::make_shared<MundoDonghua>());
    out.push_back(std::make_shared<VerAnime>());
    out.push_back(std::make_shared<VerAnimes>());
    out.push_back(std::make_shared<ZeroAnime>());
    out.push_back(std::make_shared<Katanime>());
    out.push_back(std::make_shared<LegionAnime>());
    out.push_back(std::make_shared<AnimeBum>());
    out.push_back(std::make_shared<AnimeMovil>());
    out.push_back(std::make_shared<AnimeYt>());
    out.push_back(std::make_shared<BeatZAnime>());
    out.push_back(std::make_shared<Azanimex>());
    out.push_back(std::make_shared<Doramasflix>());
    out.push_back(std::make_shared<Doramasyt>());
    out.push_back(std::make_shared<EstrenosDoramas>());
    out.push_back(std::make_shared<Pandrama>());
    out.push_back(std::make_shared<LaCartoons>());
    out.push_back(std::make_shared<TioAnimeH>(Info{"es.tioanime", "TioAnime", "https://tioanime.com", false}));
    out.push_back(std::make_shared<TioAnimeH>(Info{"es.tiohentai", "TioHentai", "https://tiohentai.com", true}));
    out.push_back(std::make_shared<Animejl>());
    out.push_back(std::make_shared<Hentaijk>());
    out.push_back(std::make_shared<Hentaila>());
    out.push_back(std::make_shared<HentaiTk>());
    out.push_back(std::make_shared<Jkhentai>());
    out.push_back(std::make_shared<VeoHentai>());
    out.push_back(std::make_shared<SamatoDenVideos>());
    out.push_back(std::make_shared<PelisPlusHd>());
    out.push_back(std::make_shared<PelisPlusPh>());
    out.push_back(std::make_shared<PelisPlusTo>());
    return out;
}

}  // namespace src
