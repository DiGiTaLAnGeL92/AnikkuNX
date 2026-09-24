// Porting in C++ del tema multisrc Aniyomi "DooPlay" (lib-multisrc/dooplay) e delle estensioni che lo usano:
//   pt: AnimePlay, AnimePlayer, AnimeQ, AnimesDrive, Q1N (animesgratis), Animes Online CC, Animes Online Cloud,
//       Animes ROLL, BetterAnimeIo, Pi Fansubs
//   es: Animenix, AnimeOnline.Ninja, Cineplus123, DeTodoPeliculas, FlixLatam, SoloLatino, VerPelisTop
//   fr: JetAnime, VoirCartoon, HDS           de: Kinoking
// Il tema e' implementato una volta sola (classe DooPlay) con le personalizzazioni dei singoli siti.
// Gli estrattori degli hoster non presenti in extractors.hpp (Blogger, MixDrop, Uqload, YourUpload, BurstCloud,
// Fastream, Upstream, Streamlare, Amazon, HexLoad, Ruplay, NOA, AnrollOnline/Blembed, ComedyShow,
// Sentinel/Hdsplay, BetterAnimeIo API) sono qui sotto, nello spazio dei nomi anonimo.

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <random>
#include <set>
#include <thread>
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

bool ok(const http::Response& r) { return r.status >= 200 && r.status < 300; }

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

std::string upper(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

std::string urlDecode(const std::string& s, bool plusIsSpace = true) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
            out += (char)std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else if (s[i] == '+' && plusIsSpace) {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

/** Decodifica \uXXXX e \/ (stringhe JSON/JS incorporate nell'HTML). */
std::string unescapeJs(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 5 < s.size() && s[i + 1] == 'u' && std::isxdigit((unsigned char)s[i + 2])) {
            unsigned cp = (unsigned)std::strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16);
            if (cp < 0x80) {
                out += (char)cp;
            } else if (cp < 0x800) {
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

void appendAll(std::vector<Video>& out, const std::vector<Video>& v) { out.insert(out.end(), v.begin(), v.end()); }

/** Stringhe tra virgolette (" o ') che iniziano con http(s) e contengono "needle". */
std::vector<std::string> quotedUrls(const std::string& text, const std::string& needle) {
    std::vector<std::string> out;
    for (char quote : {'"', '\''}) {
        size_t pos = 0;
        while ((pos = text.find(quote, pos)) != std::string::npos) {
            auto end = text.find(quote, pos + 1);
            if (end == std::string::npos) break;
            std::string c = text.substr(pos + 1, end - pos - 1);
            if ((startsWith(c, "http") || startsWith(c, "//")) && contains(c, needle) && c.size() < 2048) {
                c = fixUrl(replaceAll(c, "\\/", "/"));
                if (std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
                pos = end + 1;
            } else {
                pos = pos + 1;
            }
        }
    }
    return out;
}

std::string formEncode(std::initializer_list<std::pair<const char*, std::string>> fields) {
    std::string out;
    for (auto& f : fields) out += std::string(out.empty() ? "" : "&") + f.first + "=" + http::urlEncode(f.second);
    return out;
}

std::string jsonString(const json& j, const char* key) {
    if (!j.is_object() || !j.contains(key) || !j[key].is_string()) return "";
    return j[key].get<std::string>();
}

/** Come formatBytes() di keiyoushi.utils ("1.2 GB"). */
std::string formatBytes(long long bytes) {
    if (bytes <= 0) return "";
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024 && u < 4) {
        v /= 1024;
        u++;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
    return buf;
}

/** Numero finale di una stringa (Regex("(\\d+)$") o "(\\d+(?:\\.\\d+)?)$"), "0" se assente. */
std::string trailingNumber(const std::string& raw, bool allowDecimal) {
    std::string s = trim(raw);
    size_t e = s.size(), b = e;
    while (b > 0 && std::isdigit((unsigned char)s[b - 1])) b--;
    if (b == e) return "0";
    if (allowDecimal && b >= 2 && s[b - 1] == '.' && std::isdigit((unsigned char)s[b - 2])) {
        size_t b2 = b - 1;
        while (b2 > 0 && std::isdigit((unsigned char)s[b2 - 1])) b2--;
        b = b2;
    }
    return s.substr(b, e - b);
}

std::string firstScriptWithAny(const html::Document& doc, std::initializer_list<const char*> needles) {
    for (auto& s : doc.select("script")) {
        std::string d = s.data();
        for (auto* n : needles)
            if (contains(d, n)) return d;
    }
    return "";
}

std::string scriptAll(const html::Document& doc, std::initializer_list<const char*> needles) {
    for (auto& s : doc.select("script")) {
        std::string d = s.data();
        bool okAll = true;
        for (auto* n : needles)
            if (!contains(d, n)) okAll = false;
        if (okAll) return d;
    }
    return "";
}

/** Primo "src" di un <iframe> in un frammento HTML (anche con virgolette escape JSON). */
std::string iframeSrc(const std::string& htmlText) {
    size_t pos = 0;
    while ((pos = htmlText.find("<iframe", pos)) != std::string::npos) {
        auto end = htmlText.find('>', pos);
        std::string tag = htmlText.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos += 7;
        for (const char* attr : {" src=", " data-src=", " data-lazy-src="}) {
            auto a = tag.find(attr);
            if (a == std::string::npos) continue;
            size_t p = a + std::strlen(attr);
            if (p < tag.size() && tag[p] == '\\') p++;
            if (p >= tag.size() || (tag[p] != '"' && tag[p] != '\'')) continue;
            char q = tag[p];
            size_t s = p + 1, e = s;
            while (e < tag.size() && tag[e] != q && tag[e] != '\\') e++;
            std::string v = trim(tag.substr(s, e - s));
            if (!v.empty()) return v;
        }
    }
    return "";
}

/** Campo "embed_url" della risposta del player DooPlay (JSON, oppure testo grezzo come in Kotlin). */
std::string embedUrlOf(const std::string& body) {
    std::string url;
    json j = json::parse(body, nullptr, false);
    if (j.is_object()) url = jsonString(j, "embed_url");
    if (url.empty()) {
        auto p = body.find("\"embed_url\"");
        if (p != std::string::npos) {
            p = body.find(':', p);
            if (p != std::string::npos) p = body.find('"', p);
            if (p != std::string::npos) {
                size_t e = p + 1;
                while (e < body.size() && !(body[e] == '"' && body[e - 1] != '\\')) e++;
                url = replaceAll(body.substr(p + 1, e - p - 1), "\\", "");
            }
        }
    }
    url = trim(url);
    // alcuni siti restituiscono direttamente il codice dell'iframe
    if (contains(url, "<iframe")) url = iframeSrc(url);
    if (startsWith(url, "//")) url = "https:" + url;
    return url;
}

// =============================================================================================== hoster

// ---- Blogger (lib/bloggerextractor)
std::string bloggerQuality(const std::string& f) {
    std::string t = trim(f);
    if (t == "7") return "240p";
    if (t == "18") return "360p";
    if (t == "22") return "720p";
    if (t == "37") return "1080p";
    return "Unknown";
}

std::string decodeJsonString(const std::string& v) {
    json j = json::parse("\"" + v + "\"", nullptr, false);
    return j.is_string() ? j.get<std::string>() : "";
}

std::vector<Video> blogger(const std::string& url, const std::string& referer, const std::string& suffix = "") {
    http::Response r = http::request("GET", url, {{"Referer", referer}});
    if (!ok(r)) return {};
    const std::string& body = r.body;
    auto title = [&](const std::string& q) {
        std::string t = "Blogger - " + q + " " + suffix;
        while (!t.empty() && t.back() == ' ') t.pop_back();
        return t;
    };
    std::vector<Video> out;
    if (!contains(body, "errorContainer")) {
        std::string streams = substringBefore(after(body, "\"streams\":[", ""), "]");
        for (auto& it : splitStr(streams, "},")) {
            if (!contains(it, "\"play_url\":\"")) continue;
            std::string v = unescapeJs(substringBefore(substringAfter(it, "\"play_url\":\""), "\""));
            if (trim(v).empty()) continue;
            std::string fmt = substringBefore(substringAfter(it, "\"format_id\":"), "}");
            out.push_back(simpleVideo(v, title(bloggerQuality(fmt)), referer));
        }
    }
    if (!out.empty()) return out;

    // variante RPC (batchexecute)
    std::string token = urlDecode(http::queryParam(url, "token"));
    if (trim(token).empty()) return {};
    std::string sid = substringBefore(substringAfter(body, "FdrFJe\":\""), "\"");
    std::string bl = substringBefore(substringAfter(body, "cfb2h\":\""), "\"");
    std::string reqId = std::to_string((long long)std::time(nullptr) % 86400);
    const std::string base = "https://www.blogger.com/";
    std::string rpcUrl = base + "_/BloggerVideoPlayerUi/data/batchexecute?rpcids=WcwnYd&source-path=%2Fvideo.g&f.sid=" +
                         http::urlEncode(sid) + "&bl=" + http::urlEncode(bl) + "&hl=en-US&_reqid=" + reqId + "&rt=c";
    std::string rpcBody = "f.req=%5B%5B%5B%22WcwnYd%22%2C%22%5B%5C%22" + token +
                          "%5C%22%2C%5C%22%5C%22%2C0%5D%22%2Cnull%2C%22generic%22%5D%5D%5D&";
    http::Headers rh = {
        {"Accept", "*/*"},
        {"Accept-Language", "en-US,en;q=0.9"},
        {"Content-Type", "application/x-www-form-urlencoded;charset=UTF-8"},
        {"Sec-Fetch-Dest", "empty"},
        {"Sec-Fetch-Mode", "cors"},
        {"Sec-Fetch-Site", "same-origin"},
        {"X-Same-Domain", "1"},
        {"Referer", base},
    };
    http::Response rr = http::request("POST", rpcUrl, rh, rpcBody);
    const std::string& rpc = rr.body;
    if (!contains(rpc, "https://")) return {};
    std::string s = "\\\"" + substringBefore(after(rpc, "[[\\\"", ""), "]]]") + "]";
    for (auto& it : splitStr(s, "],[")) {
        std::string v = substringBefore(after(it, "\\\"", ""), "\\\"");
        if (trim(v).empty()) continue;
        std::string first = decodeJsonString(v);
        std::string videoUrl = first.empty() ? "" : decodeJsonString(first);
        if (!startsWith(videoUrl, "http")) continue;
        std::string fmt = substringBefore(substringAfter(it, "["), "]");
        out.push_back(simpleVideo(videoUrl, title(bloggerQuality(fmt)), referer));
    }
    return out;
}

// ---- MixDrop (lib/mixdropextractor)
std::vector<Video> mixDrop(const std::string& url, const std::string& prefix) {
    const std::string referer = "https://mixdrop.co/";
    http::Headers h = {{"Referer", referer}, {"User-Agent", DESKTOP_UA}};
    html::Document doc(http::getText(url, h), url);
    std::string packed = scriptAll(doc, {"eval", "MDCore"});
    if (packed.empty()) return {};
    std::string un = unpacker::unpackAndCombine(packed);
    if (un.empty()) return {};
    std::string wurl = substringBefore(substringAfter(un, "Core.wurl=\""), "\"");
    if (wurl.empty()) return {};
    Video v = simpleVideo(fixUrl(wurl), prefix + "MixDrop", referer);
    v.userAgent = DESKTOP_UA;
    if (contains(un, "Core.remotesub=\"")) {
        std::string sub = substringBefore(substringAfter(un, "Core.remotesub=\""), "\"");
        if (!trim(sub).empty()) v.subtitles.push_back({urlDecode(sub), "sub"});
    }
    return {v};
}

// ---- Uqload (lib/uqloadextractor)
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
        if (startsWith(u, "http")) sources.push_back(u);
    }
    if (sources.empty()) {
        std::string packed = scriptWith(doc, {"eval(function(p,a,c,k,e,d)"});
        if (packed.empty()) return {};
        std::string un = unpacker::unpackAndCombine(packed);
        for (auto& u : quotedUrls(un, ".m3u8")) sources.push_back(u);
        for (auto& u : quotedUrls(un, ".mp4")) sources.push_back(u);
    }
    std::vector<Video> out;
    for (auto& s : sources) {
        if (contains(s, ".m3u8")) appendAll(out, hlsVideos(s, fixed, quality + " - "));
        else out.push_back(simpleVideo(s, quality, fixed));
    }
    return out;
}

// ---- YourUpload (lib/youruploadextractor)
std::vector<Video> yourUpload(const std::string& url, const std::string& prefix) {
    http::Headers h = {{"Referer", "https://www.yourupload.com/"}};
    html::Document doc(http::getText(url, h), url);
    std::string data = scriptWith(doc, {"jwplayerOptions"});
    if (data.empty()) return {};
    std::string file = substringBefore(substringAfter(data, "file: '"), "',");
    if (!startsWith(file, "http")) return {};
    return {simpleVideo(file, prefix + "YourUpload", "https://www.yourupload.com/")};
}

// ---- BurstCloud (lib/burstcloudextractor)
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
    std::string cdn = jsonString(j["purchase"], "cdnUrl");
    if (cdn.empty()) return {};
    return {simpleVideo(cdn, prefix + "BurstCloud", base)};
}

// ---- Fastream (lib/fastreamextractor)
std::vector<Video> fastream(const std::string& url, const std::string& prefix) {
    const std::string base = "https://fastream.to";
    http::Headers h = {{"Referer", base + "/"}, {"Origin", base}};
    html::Document first(http::getText(url, h), url);
    std::string script;
    auto inputs = first.select("input[name]");
    if (!inputs.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5100));  // il sito richiede almeno 5 secondi
        std::string form;
        for (auto& in : inputs)
            form += (form.empty() ? "" : "&") + http::urlEncode(in.attr("name")) + "=" + http::urlEncode(in.attr("value"));
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
    if (contains(videoUrl, ".m3u8")) return hlsVideos(videoUrl, base + "/", prefix + "Fastream - ");
    return {simpleVideo(videoUrl, prefix + "Fastream", base + "/", {{"Origin", base}})};
}

// ---- Upstream (lib/upstreamextractor)
std::vector<Video> upstream(const std::string& url, const std::string& prefix) {
    html::Document doc(http::getText(url), url);
    std::string js = scriptWith(doc, {"eval"});
    if (js.empty()) return {};
    std::string un = unpacker::unpackAndCombine(js);
    std::string master = substringBefore(substringAfter(un, "{file:\""), "\"}");
    if (!startsWith(master, "http")) return {};
    return hlsVideos(master, "", prefix + "Upstream - ");
}

// ---- Streamlare (lib/streamlareextractor)
std::vector<Video> streamlare(const std::string& url, const std::string& prefix) {
    std::string id = afterLast(url, "/");
    http::Response r = http::request("POST", "https://slwatch.co/api/video/stream/get", {{"Content-Type", "application/json"}},
                                     "{\"id\":\"" + id + "\"}");
    const std::string& playlist = r.body;
    std::string type = substringBefore(substringAfter(playlist, "\"type\":\""), "\"");
    if (type == "hls") {
        std::string master = replaceAll(substringBefore(substringAfter(playlist, "\"file\":\""), "\""), "\\/", "/");
        return hlsVideos(master, "", prefix + "Streamlare - ");
    }
    std::vector<Video> out;
    const std::string sep = "\"label\":\"";
    for (auto& it : splitStr(substringAfter(playlist, sep), sep)) {
        std::string quality = substringBefore(it, "\",");
        std::string api = replaceAll(substringBefore(substringAfter(it, "\"file\":\""), "\","), "\\", "");
        if (!startsWith(api, "http")) continue;
        try {
            http::Response pr = http::request("POST", api, {}, "", 20);
            out.push_back(simpleVideo(pr.finalUrl.empty() ? api : pr.finalUrl, prefix + "Streamlare - " + quality));
        } catch (const std::exception&) {
        }
    }
    return out;
}

// ---- Amazon Drive (FlixLatam.extractAmazonVideo)
std::vector<Video> amazon(const std::string& url, const std::string& prefix) {
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
    return {simpleVideo(videoUrl, prefix + "Amazon")};
}

// ---- HexLoad (verpelistop/HexloadExtractor)
std::vector<Video> hexload(const std::string& url, const std::string& prefix, const http::Headers& headers) {
    // Regex("embed-(\\w+?)/")
    auto p = url.find("embed-");
    if (p == std::string::npos) return {};
    size_t s = p + 6, e = s;
    while (e < url.size() && (std::isalnum((unsigned char)url[e]) || url[e] == '_')) e++;
    if (e == s || e >= url.size() || url[e] != '/') return {};
    std::string id = url.substr(s, e - s);
    http::Headers h = mergeHeaders(headers, {{"Content-Type", "application/x-www-form-urlencoded"}});
    http::Response r = http::request("POST", "https://hexload.com/download", h,
                                     formEncode({{"op", "download3"}, {"id", id}, {"ajax", "1"}, {"method_free", "1"}, {"dataType", "json"}}));
    json j = json::parse(r.body, nullptr, false);
    if (!j.is_object() || !j.contains("result") || !j["result"].is_object()) return {};
    const json& res = j["result"];
    std::string videoUrl = jsonString(res, "url");
    if (videoUrl.empty()) return {};
    long long size = 0;
    if (res.contains("size")) {
        if (res["size"].is_number()) size = res["size"].get<long long>();
        else if (res["size"].is_string()) size = std::atoll(res["size"].get<std::string>().c_str());
    }
    std::string sz = formatBytes(size);
    return {simpleVideo(videoUrl, sz.empty() ? prefix : prefix + ": " + sz, "", headers)};
}

// ---- Ruplay (animesgratis/extractors/RuplayExtractor)
std::vector<Video> ruplay(const std::string& url) {
    std::string body = http::getText(url);
    std::string file = substringBefore(substringAfter(substringAfter(body, "Playerjs({"), "file:\""), "\"");
    std::vector<Video> out;
    for (auto& it : splitStr(file, ",")) {
        std::string videoUrl = contains(it, "]") ? substringAfter(it, "]") : it;
        if (!startsWith(videoUrl, "http")) continue;
        std::string quality = substringBefore(after(it, "[", ""), "]");
        if (quality.empty()) quality = "Default";
        out.push_back(simpleVideo(videoUrl, "Ruplay - " + quality, videoUrl));
    }
    return out;
}

// ---- NOA (animesgratis/extractors/NoaExtractor)
std::vector<Video> noa(const std::string& url, const std::string& referer, const std::string& name = "NOA") {
    std::string body = http::getText(url);
    std::vector<Video> out;
    if (contains(body, "file: jw.file")) {
        std::string videoUrl = replaceAll(substringBefore(substringAfter(substringAfter(body, "file"), ":\""), "\""), "\\", "");
        if (startsWith(videoUrl, "http")) out.push_back(simpleVideo(videoUrl, name, referer));
    } else if (contains(body, "sources:")) {
        std::string block = substringBefore(substringAfter(body, "sources: ["), "]");
        auto parts = splitStr(block, "{");
        for (size_t i = 1; i < parts.size(); i++) {
            const std::string& it = parts[i];
            std::string label = "Default";
            auto lp = it.find("label");
            if (lp != std::string::npos) {
                auto q = it.find(":\"", lp);
                if (q != std::string::npos) {
                    std::string l = substringBefore(it.substr(q + 2), "\"");
                    if (!l.empty()) label = l;
                }
            }
            std::string videoUrl =
                replaceAll(substringBefore(substringAfter(substringAfter(substringAfter(it, "file"), ":"), "\""), "\""), "\\", "");
            if (startsWith(videoUrl, "http")) out.push_back(simpleVideo(videoUrl, name + " - " + label, referer));
        }
    }
    return out;
}

/** API "kaken" comune ad AnrollOnline (animesroll) e Blembed (pifansubs): {sources:[{file,label}]}. */
std::vector<Video> kakenSources(const std::string& apiUrl, const http::Headers& headers, const std::string& prefix,
                                const std::string& referer) {
    http::Response r = http::request("GET", apiUrl, headers);
    json j = json::parse(r.body, nullptr, false);
    std::vector<Video> out;
    if (!j.is_object() || !j.contains("sources") || !j["sources"].is_array()) return out;
    for (auto& s : j["sources"]) {
        std::string file = jsonString(s, "file");
        if (file.empty()) continue;
        std::string label = jsonString(s, "label");
        std::string title = trim(prefix).empty() ? label : prefix + (label.empty() ? "" : " - " + label);
        if (contains(file, ".m3u8")) appendAll(out, hlsVideos(file, referer, title + " - "));
        else out.push_back(simpleVideo(file, title, referer));
    }
    return out;
}

// ---- AnrollOnline (animesroll/extractors/AnrollOnlineExtractor)
std::vector<Video> anrollOnline(const std::string& url, const std::string& prefix) {
    html::Document doc(http::getText(url), url);
    std::string packed = scriptAll(doc, {"eval", "p,a,c,k,e,d"});
    if (packed.empty()) return {};
    std::string script = unpacker::unpackAndCombine(packed);
    std::string kaken = substringBefore(substringAfter(after(script, "kaken", ""), "\""), "\"");
    if (kaken.empty()) return {};
    long long now = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string api = "https://" + http::hostOf(url) + "/api?" + kaken + "&_=" + std::to_string(now);
    return kakenSources(api, {}, prefix, "");
}

// ---- Blembed (pifansubs/extractors/BlembedExtractor)
std::vector<Video> blembed(const std::string& url, const http::Headers& headers, const std::string& referer) {
    html::Document doc(http::getText(url, headers), url);
    std::string script = scriptWith(doc, {"player ="});
    if (script.empty()) return {};
    std::string token = substringBefore(substringAfter(script, "kaken = \""), "\"");
    long long now = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string api = "https://blembed.com/api/?" + token + "&_=" + std::to_string(now);
    return kakenSources(api, mergeHeaders(headers, {{"X-Requested-With", "XMLHttpRequest"}}), "Blembed", referer);
}

// ---- ComedyShow (voircartoon/extractors/ComedyShowExtractor)
std::vector<Video> comedyShow(const std::string& url) {
    const std::string host = "https://comedyshow.to";
    std::string id = afterLast(url, "/");
    http::Headers h = {{"X-Requested-With", "XMLHttpRequest"},
                       {"Referer", host},
                       {"Origin", host},
                       {"Content-Type", "application/x-www-form-urlencoded"}};
    http::Response r = http::request("POST", host + "/player/index.php?data=" + id + "&do=getVideo", h,
                                     formEncode({{"hash", id}, {"r", ""}}));
    std::string master = replaceAll(substringBefore(substringAfter(r.body, "videoSource\":\""), "\""), "\\", "");
    if (!startsWith(master, "http")) return {};
    return hlsVideos(master, host, "ComedyShow - ", {}, {{"X-Requested-With", "XMLHttpRequest"}});
}

// ---- Sentinel / Hdsplay (jetanime/extractors)
std::vector<Video> jetPlayer(const std::string& url, const std::string& server, const std::string& name) {
    html::Document doc(http::getText(url), url);
    std::string script = firstScriptWithAny(doc, {"m3u8", "mp4"});
    if (script.empty()) return {};
    if (contains(script, "eval(function(p,a,c,k,e,d)")) {
        std::string un = unpacker::unpackAndCombine(script);
        if (!un.empty()) script = un;
    }
    // Regex("file: ?\"(...)\"") senza std::regex
    auto quotedFile = [&](std::initializer_list<const char*> exts, size_t& at) -> std::string {
        size_t pos = 0;
        while ((pos = script.find("file:", pos)) != std::string::npos) {
            size_t p = pos + 5;
            pos = p;
            if (p < script.size() && script[p] == ' ') p++;
            if (p >= script.size() || script[p] != '"') continue;
            auto e = script.find('"', p + 1);
            if (e == std::string::npos) break;
            std::string v = script.substr(p + 1, e - p - 1);
            for (auto* x : exts)
                if (contains(v, x)) {
                    at = e;
                    return v;
                }
        }
        return "";
    };
    size_t at = 0;
    std::string videoUrl = quotedFile({"m3u8", "mp4"}, at);
    if (!startsWith(videoUrl, "http")) return {};
    std::vector<Video::Track> subs;
    size_t subAt = 0;
    std::string sub = quotedFile({"vtt", "ass", "srt"}, subAt);
    if (!sub.empty()) {
        std::string rest = script.substr(subAt);
        auto lp = rest.find("label:");
        if (lp != std::string::npos) {
            size_t p = lp + 6;
            if (p < rest.size() && rest[p] == ' ') p++;
            if (p < rest.size() && rest[p] == '"') subs.push_back({fixUrl(sub, url), substringBefore(rest.substr(p + 1), "\"")});
        }
    }
    std::vector<Video> out;
    if (contains(videoUrl, ".m3u8")) {
        out = hlsVideos(videoUrl, url, server + ": ", subs);
        for (auto& v : out) v.title += " (" + name + ")";
    } else {
        Video v = simpleVideo(videoUrl, "Sentinel: Video (" + name + ")");
        v.subtitles = subs;
        out.push_back(v);
    }
    return out;
}

// ---- BetterAnimeIo (BetterAnimeIoExtractor)
std::vector<Video> betterAnimeApi(const std::string& encodedSource) {
    http::Response r = http::request("GET", "https://api.myblogapi.site/api/v1/decode/blogg/" + encodedSource);
    json j = json::parse(r.body, nullptr, false);
    std::vector<Video> out;
    if (!j.is_object() || jsonString(j, "status") != "success" || !j.contains("play") || !j["play"].is_array()) return out;
    for (auto& p : j["play"]) {
        std::string src = trim(jsonString(p, "src"));
        while (!src.empty() && src.back() == '\\') src.pop_back();
        if (src.empty()) continue;
        std::string label = jsonString(p, "sizeText");
        if (label.empty()) label = "Default";
        out.push_back(simpleVideo(src, label));
    }
    return out;
}

// =============================================================================================== risolutore hoster

/** Convenzioni nome-hoster delle estensioni spagnole (unione delle liste dei singoli siti). */
struct Conv {
    const char* key;
    std::vector<const char*> names;
};

const std::vector<Conv>& conventions() {
    static const std::vector<Conv> list = {
        {"saidochesto", {"saidochesto", "multiserver"}},
        {"voe", {"voe", "tubelessceliolymph", "simpulumlamerop", "urochsunloath", "nathanfromsubject", "yip.", "metagnathtuggers", "donaldlineelse"}},
        {"okru", {"ok.ru", "okru"}},
        {"filemoon", {"filemoon", "moonplayer", "moviesm4u", "files.im", "bysezoxexe"}},
        {"amazon", {"amazon", "amz"}},
        {"uqload", {"uqload"}},
        {"hexload", {"hexload"}},
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
        {"mixdrop", {"mixdrop"}},
        {"wolfstream", {"wolfstream"}},
        {"vidhide", {"ahvsh", "streamhide", "guccihide", "streamvid", "vidhide", "kinoger", "smoothpre", "dhtpre", "peytonepre",
                     "earnvids", "ryderjet", "hgcloud", "hglink", "minochinos", "movearnpre", "dintezuvio", "luluvdo"}},
        {"vidguard", {"vembed", "guard", "listeamed", "bembed", "vgfplay", "listeam"}},
    };
    return list;
}

using KeyOrder = std::vector<const char*>;
const KeyOrder ORDER_NINJA = {"saidochesto", "filemoon", "doodstream", "streamtape", "mixdrop", "uqload", "wolfstream", "mp4upload", "vidhide", "streamwish"};
const KeyOrder ORDER_FLIX = {"voe", "okru", "filemoon", "amazon", "uqload", "mp4upload", "streamwish", "doodstream", "streamlare",
                             "yourupload", "burstcloud", "fastream", "upstream", "streamsilk", "streamtape", "vidhide", "vidguard"};
const KeyOrder ORDER_SOLO = {"streamwish", "uqload", "vidguard", "doodstream", "voe", "filemoon", "vidhide"};
const KeyOrder ORDER_VERPELIS = {"uqload", "hexload", "streamwish", "streamtape", "filemoon", "vidhide"};
const KeyOrder ORDER_ALL = {"voe", "okru", "filemoon", "amazon", "uqload", "hexload", "mp4upload", "mixdrop", "wolfstream", "streamwish",
                            "doodstream", "streamlare", "fastream", "upstream", "streamtape", "vidhide", "vidguard", "burstcloud", "yourupload"};

std::string matchKey(const std::string& url, const KeyOrder& order, const std::string& name = "") {
    std::string u = lower(url), n = lower(name);
    for (auto* key : order)
        for (auto& c : conventions()) {
            if (std::strcmp(c.key, key) != 0) continue;
            for (auto* nm : c.names) {
                std::string x = lower(nm);
                if (contains(u, x) || (!n.empty() && contains(n, x))) return key;
            }
        }
    return "";
}

/** hgcloud.to / hglink.to reindirizzano verso domini alternativi (VerPelisTop.redirectHgCloudHgLink). */
std::string redirectHg(std::string url) {
    static const char* redirected[] = {"hanerix.com", "vibuxer.com", "audinifer.com", "masukestin.com"};
    for (const char* d : {"hgcloud.to", "hglink.to"}) {
        if (!contains(url, d)) continue;
        static thread_local std::mt19937 rng((unsigned)std::random_device{}());
        url = replaceAll(url, d, redirected[rng() % 4]);
        break;
    }
    return replaceAll(url, "dintezuvio", "callistanise");
}

/** Estrae i video dall'hoster indicato dalla chiave; p e' il prefisso del titolo (es. "[LAT] "). */
std::vector<Video> hosterByKey(const std::string& key, const std::string& url, const std::string& p, const std::string& site) {
    http::Headers siteHeaders = {{"Referer", site + "/"}};
    if (key == "voe") return voe(url, p);
    if (key == "okru") return okru(url, p);
    if (key == "filemoon") return moon(url, site, p + "Filemoon - ");
    if (key == "amazon") return amazon(url, p);
    if (key == "uqload") return uqload(url, p);
    if (key == "hexload") return hexload(url, p + "HexLoad", siteHeaders);
    if (key == "mp4upload") return mp4upload(url, p);
    if (key == "streamwish") return streamWish(url, p, siteHeaders);
    if (key == "doodstream") return dood(replaceAll(url, "https://doodstream.com/e/", "https://d0000d.com/e/"), p);
    if (key == "streamlare") return streamlare(url, p);
    if (key == "yourupload") return yourUpload(url, p);
    if (key == "burstcloud") return burstCloud(url, p);
    if (key == "fastream") return fastream(url, p);
    if (key == "upstream") return upstream(url, p);
    if (key == "streamtape") return streamtape(url, p);
    if (key == "mixdrop") return mixDrop(url, p);
    if (key == "wolfstream") return wolfstream(url, p);
    if (key == "vidhide") return vidHide(redirectHg(url), p, siteHeaders);
    // streamsilk (deoffuscatore JS "hunter"), vidguard (motore JS Rhino): non supportati
    return {};
}

std::string titlePrefix(const std::string& prefix) {
    std::string t = trim(prefix);
    return t.empty() ? "" : t + " ";
}

std::vector<Video> resolveHoster(const std::string& url, const std::string& prefix, const std::string& site, const KeyOrder& order,
                                 const std::string& name = "") {
    std::string key = matchKey(url, order, name);
    if (key.empty()) return {};
    return hosterByKey(key, url, titlePrefix(prefix), site);
}

/**
 * Sostituto dell'UniversalExtractor (che usa una WebView): prova gli hoster conosciuti, altrimenti cerca
 * direttamente URL .m3u8/.mp4 nella pagina (anche negli script "packed").
 */
std::vector<Video> universal(const std::string& url, const std::string& prefix, const std::string& site) {
    if (!startsWith(url, "http")) return {};
    if (contains(url, "blogger.com")) return blogger(url, site);
    auto known = resolveHoster(url, prefix, site, ORDER_ALL);
    if (!known.empty()) return known;
    http::Response r = http::request("GET", url, {{"Referer", site + "/"}});
    if (!ok(r)) return {};
    std::string text = r.body;
    if (contains(text, "eval(function(p,a,c,k,e,d)")) {
        html::Document d(r.body, url);
        for (auto& s : d.select("script")) {
            std::string data = s.data();
            if (!contains(data, "eval(function(p,a,c,k,e,d)")) continue;
            std::string un = unpacker::unpackAndCombine(data);
            if (!un.empty()) text += "\n" + un;
        }
    }
    std::string pre = trim(prefix).empty() ? http::hostOf(url) + ": " : trim(prefix) + ": ";
    std::vector<Video> out;
    int n = 0;
    for (auto& u : quotedUrls(text, ".m3u8")) {
        if (n++ >= 2) break;
        appendAll(out, hlsVideos(u, url, pre));
    }
    if (out.empty()) {
        n = 0;
        for (auto& u : quotedUrls(text, ".mp4")) {
            if (n++ >= 2) break;
            out.push_back(simpleVideo(u, pre + "MP4", url));
        }
    }
    return out;
}

// =============================================================================================== dataLink (embed69)

/** Rimuove i wrapper JSON.parse / decodeURIComponent / atob attorno all'espressione dataLink. */
std::string resolveDataLink(const std::string& raw) {
    std::string expr = trim(raw);
    while (!expr.empty() && expr.back() == ';') expr.pop_back();
    expr = trim(expr);
    auto removeOuterCall = [](const std::string& s, const std::string& prefix, std::string& inner) {
        if (lower(s).rfind(lower(prefix), 0) != 0 || s.empty() || s.back() != ')') return false;
        auto start = s.find('(');
        auto end = s.rfind(')');
        if (start == std::string::npos || end == std::string::npos || end <= start) return false;
        inner = trim(s.substr(start + 1, end - start - 1));
        return true;
    };
    auto trimQuotes = [](const std::string& s) {
        if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'')))
            return s.substr(1, s.size() - 2);
        return s;
    };
    for (int guard = 0; guard < 16; guard++) {
        std::string inner;
        if (removeOuterCall(expr, "JSON.parse", inner) || removeOuterCall(expr, "window.JSON.parse", inner)) {
            expr = inner;
        } else if (removeOuterCall(expr, "decodeURIComponent", inner) || removeOuterCall(expr, "window.decodeURIComponent", inner)) {
            expr = urlDecode(trimQuotes(inner));
        } else if (removeOuterCall(expr, "atob", inner) || removeOuterCall(expr, "window.atob", inner)) {
            expr = base64Decode(trimQuotes(inner));
        } else {
            break;
        }
    }
    return trim(trimQuotes(trim(expr)));
}

/** Regex("dataLink\\s*=\\s*([^;]+);") senza std::regex. */
std::string dataLinkExpression(const std::string& text) {
    size_t pos = 0;
    while ((pos = text.find("dataLink", pos)) != std::string::npos) {
        size_t p = pos + 8;
        pos = p;
        while (p < text.size() && std::isspace((unsigned char)text[p])) p++;
        if (p >= text.size() || text[p] != '=') continue;
        p++;
        while (p < text.size() && std::isspace((unsigned char)text[p])) p++;
        auto e = text.find(';', p);
        if (e == std::string::npos || e == p) continue;
        return text.substr(p, e - p);
    }
    return "";
}

const char* EMBED_AES_KEY = "Ak7qrvvH4WKYxV2OgaeHAEg2a5eh16vE";

std::string decryptEmbedLink(const std::string& rawLink) {
    std::string link = trim(rawLink);
    if (link.empty()) return "";
    if (startsWith(lower(link), "http")) return link;
    auto looksLikeUrl = [](const std::string& s) { return contains(s, "://") && s.size() < 4096; };
    // CryptoAES.decryptCbcIV: IV nei primi 16 byte, chiave UTF-8
    try {
        std::string data = base64Decode(link);
        if (data.size() > 16) {
            std::string plain = crypto::aesCbcDecrypt(data.substr(16), EMBED_AES_KEY, data.substr(0, 16));
            if (looksLikeUrl(plain)) return trim(plain);
        }
    } catch (const std::exception&) {
    }
    // CryptoAES.decrypt (formato "Salted__")
    try {
        std::string plain = crypto::cryptoJsDecrypt(link, EMBED_AES_KEY);
        if (looksLikeUrl(plain)) return trim(plain);
    } catch (const std::exception&) {
    }
    // JWT: payload.link oppure payload.data.link
    auto segments = splitStr(link, ".");
    if (segments.size() >= 2) {
        json j = json::parse(base64Decode(segments[1]), nullptr, false);
        if (j.is_object()) {
            std::string l = jsonString(j, "link");
            if (l.empty() && j.contains("data")) l = jsonString(j["data"], "link");
            if (!l.empty()) return l;
        }
    }
    return "";
}

/** extractNewExtractorLinks di FlixLatam/SoloLatino: coppie (url, lingua). */
std::vector<std::pair<std::string, std::string>> dataLinkLinks(const std::string& htmlText) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string raw;
    {
        html::Document doc(htmlText);
        for (auto& s : doc.select("script")) {
            std::string d = s.data();
            if (!contains(d, "dataLink")) continue;
            raw = dataLinkExpression(d);
            break;
        }
    }
    if (raw.empty()) raw = dataLinkExpression(htmlText);
    std::string payload = resolveDataLink(raw);
    if (payload.empty()) return out;
    json items = json::parse(payload, nullptr, false);
    if (!items.is_array()) return out;
    for (auto& item : items) {
        if (!item.is_object()) continue;
        std::string l = upper(jsonString(item, "video_language"));
        std::string lang = l == "LAT" ? "[LAT]" : l == "ESP" ? "[CAST]" : l == "SUB" ? "[SUB]" : "unknown";
        if (!item.contains("sortedEmbeds") || !item["sortedEmbeds"].is_array()) continue;
        for (auto& e : item["sortedEmbeds"]) {
            if (lower(jsonString(e, "type")) != "video") continue;
            std::string link = decryptEmbedLink(jsonString(e, "link"));
            if (!link.empty()) out.push_back({link, lang});
        }
    }
    return out;
}

// =============================================================================================== tema

enum class Site {
    AnimePlay,
    AnimePlayer,
    AnimeQ,
    AnimesDrive,
    Q1N,
    AnimesOnlineCC,
    AnimesOnlineCloud,
    AnimesRoll,
    BetterAnimeIo,
    PiFansubs,
    Animenix,
    AnimeOnlineNinja,
    JetAnime,
    VoirCartoon,
    Kinoking,
    Cineplus123,
    DeTodoPeliculas,
    FlixLatam,
    SoloLatino,
    VerPelisTop,
    Hds,
};

struct SiteInfo {
    Site site;
    const char* id;
    const char* name;
    const char* url;
    const char* lang;
    bool nsfw;
};

// Selettori speciali per "pagina successiva" non esprimibili col motore di selettori
const char* NEXT_LAST_CHILD = "@lastchild";      // div.pagination > *:last-child:not(span):not(.current)
const char* NEXT_CURRENT_PLUS_A = "@current+a";  // div.pagination > span.current + a

class DooPlay : public Source {
  public:
    explicit DooPlay(SiteInfo info) : info(info) {}

    std::string id() const override { return info.id; }
    std::string name() const override { return info.name; }
    std::string defaultBaseUrl() const override { return info.url; }
    std::string lang() const override { return info.lang; }
    bool nsfw() const override { return info.nsfw; }
    bool supportsLatest() const override { return info.site != Site::VoirCartoon && info.site != Site::Hds; }

    http::Headers imageHeaders() const override { return {{"Referer", baseUrl() + "/"}}; }

    // ------------------------------------------------------------------------------------------ elenchi

    Page popular(int page) override {
        return withFallback([&] { return popularPrimary(page); }, page == 1 ? baseUrl() + "/" : "");
    }

    Page latest(int page) override {
        if (!supportsLatest()) return {};
        return withFallback([&] { return latestPrimary(page); }, "");
    }

  private:
    /** Selettori generici del tema, usati se quelli del sito non trovano nulla. */
    static constexpr const char* GENERIC_SEL =
        "article.item div.poster, article.w_item_a > a, article.w_item_b > a, div.result-item div.image a, "
        "div.result-item article div.thumbnail > a, div.items article div.poster, article.item";

    /** Esegue l'elenco del sito; se fallisce o e' vuoto riprova con i selettori generici sulla pagina indicata. */
    template <typename F>
    Page withFallback(F primary, const std::string& fallbackUrl) {
        std::string error;
        try {
            Page p = primary();
            if (!p.animes.empty()) return p;
        } catch (const std::exception& e) {
            error = e.what();
        }
        if (!fallbackUrl.empty()) {
            try {
                Page p = list(fallbackUrl, GENERIC_SEL, "");
                if (!p.animes.empty()) return p;
            } catch (const std::exception&) {
            }
        }
        if (!error.empty()) throw http::Error(error);
        return {};
    }

    Page popularPrimary(int page) {
        std::string b = baseUrl(), p = std::to_string(page);
        switch (info.site) {
            case Site::AnimePlay:
            case Site::AnimesDrive:
            case Site::AnimesOnlineCloud: return list(b + "/anime", "article.w_item_a > a", "");
            case Site::AnimeQ: return list(b + "/anime", "article.w_item_a > a, article.w_item_b > a", "");
            case Site::AnimePlayer: return list(b + "/animes/", "div#archive-content article div.poster", "");
            case Site::Q1N:
            case Site::AnimesRoll: return list(b + "/animes/", "div.items.featured article div.poster", "");
            case Site::AnimesOnlineCC: return list(b, "article.w_item_b > a", "");
            case Site::BetterAnimeIo: return list(b + "/animes", "div#featured-titles article.item div.poster", "");
            case Site::PiFansubs:
            case Site::Kinoking: return list(b, "div#featured-titles div.poster", "");
            case Site::Animenix: return list(b + "/ratings/" + p, LATEST_SEL, NEXT_LAST_CHILD);
            case Site::AnimeOnlineNinja:
                return list(page == 1 ? b + "/tendencias/" : b + "/tendencias/page/" + p + "/", LATEST_SEL, NEXT_LAST_CHILD);
            case Site::JetAnime: return list(b, "aside#dtw_content_views-2 div.dtw_content > article", "");
            case Site::VoirCartoon: return list(b + "/tendance/page/" + p + "/", LATEST_SEL, "div.pagination a.arrow_pag > i#nextpagination");
            case Site::Hds: return list(b + "/tendance/page/" + p + "/", LATEST_SEL, "#nextpagination");
            case Site::Cineplus123: return list(b + "/tendencias/" + p, LATEST_SEL, latestNext());
            case Site::DeTodoPeliculas: return list(b + "/novedades/page/" + p, LATEST_SEL, latestNext());
            case Site::FlixLatam: return list(b + "/pelicula/page/" + p, LATEST_SEL, latestNext());
            case Site::SoloLatino: return list(b + "/tendencias/page/" + p, "article.item", "div.pagMovidy a");
            case Site::VerPelisTop: return list(b, "#featured-titles article > div.poster", "");
        }
        return {};
    }

    Page latestPrimary(int page) {
        std::string b = baseUrl(), p = std::to_string(page);
        std::string path;
        switch (info.site) {
            case Site::AnimePlayer: path = b + "/episodios/page/" + p; break;
            case Site::AnimesRoll: path = b + "/episodios/page/" + p; break;
            case Site::BetterAnimeIo: return list(b + "/animes/page/" + p, "div#archive-content article.item div.poster", latestNext());
            case Site::PiFansubs: path = b + "/episodios/page/" + p; break;
            case Site::Animenix: path = b + "/ver/page/" + p; break;
            case Site::AnimeOnlineNinja: path = b + "/genero/en-emision-1/page/" + p; break;
            case Site::Cineplus123: path = b + "/ano/2024/page/" + p; break;
            case Site::DeTodoPeliculas: path = b + "/peliculas-de-estreno/page/" + p; break;
            case Site::FlixLatam: path = b + "/lanzamiento/2024/page/" + p; break;
            case Site::SoloLatino: return list(b + "/pelicula/estrenos/page/" + p, "article.item", "div.pagMovidy a");
            case Site::VerPelisTop: return list(b + "/online/page/" + p, "#archive-content article > div.poster", "#nextpagination");
            default: path = b + "/" + (ptBR() ? "episodio" : "episodes") + "/page/" + p; break;
        }
        return list(path, LATEST_SEL, latestNext(), info.site == Site::JetAnime);
    }

  public:
    Page search(const std::string& rawQuery, int page) override {
        std::string query = trim(rawQuery);
        if (startsWith(query, "https://") || startsWith(query, "http://")) {
            if (http::hostOf(query) != http::hostOf(baseUrl())) throw http::Error("URL non supportato");
            auto segs = splitStr(substringBefore(substringBefore(http::pathOf(query), "?"), "#"), "/");
            std::vector<std::string> parts;
            for (auto& s : segs)
                if (!s.empty()) parts.push_back(s);
            if (parts.size() < 2) throw http::Error("URL non supportato");
            std::string path = "/" + parts[0] + "/" + parts[1];
            Details d = details(path);
            Page res;
            res.animes.push_back({path, d.title, d.thumbnail});
            return res;
        }
        if (query.empty()) return popular(page);
        std::string b = baseUrl(), p = std::to_string(page), q = http::urlEncode(query);
        std::string url;
        std::string sel = "div.result-item div.image a";
        std::string next = latestNext();
        switch (info.site) {
            case Site::AnimePlay:
            case Site::AnimesDrive:
            case Site::AnimesOnlineCloud:
                url = b + (page > 1 ? "/page/" + p : "") + "/?s=" + q + "&orderby=date&order=desc";
                break;
            case Site::AnimeQ: url = b + (page > 1 ? "/page/" + p : "") + "/?s=" + q; break;
            case Site::Q1N:
            case Site::AnimesRoll: sel = "div.result-item article div.thumbnail > a"; break;
            case Site::AnimesOnlineCC: sel = "div#animation-2 > article > div.poster > a"; break;
            case Site::Animenix: url = b + "/page/" + p + "/?s=" + q + "&tipo="; break;
            case Site::AnimeOnlineNinja: url = b + "/page/" + p + "/?s=" + q + "&tipo=todos"; break;
            case Site::JetAnime: sel = "div.search-page > div.result-item div.image a"; break;
            case Site::VoirCartoon: next = "div.pagination a.arrow_pag > i#nextpagination"; break;
            case Site::Hds: next = "#nextpagination"; break;
            case Site::SoloLatino:
                sel = "article.item";
                next = "div.pagMovidy a";
                break;
            case Site::VerPelisTop: url = b + "/?s=" + q; break;
            default: break;
        }
        if (url.empty()) url = b + "/page/" + p + "/?s=" + q;
        return list(url, sel, next);
    }

    // ------------------------------------------------------------------------------------------ dettagli

    Details details(const std::string& animeUrl) override {
        DocPtr doc = realAnimeDoc(fetch(abs(animeUrl)));
        Details d;
        html::Node sheader = doc->selectFirst("div.sheader");
        if (!sheader) throw http::Error("Pagina dell'anime non valida");

        html::Node content = info.site == Site::AnimePlayer ? doc->selectFirst("div#contenedor > div.data") : html::Node();
        html::Node img = sheader.selectFirst(info.site == Site::Q1N ? "div.poster img" : "div.poster > img");
        d.thumbnail = imageUrl(*doc, img);
        d.title = trim(img.attr("alt"));
        if (d.title.empty()) d.title = sheader.selectFirst("div.data > h1").text();
        if (d.title.empty() && content) d.title = content.selectFirst("h1").text();
        if (d.title.empty()) d.title = doc->selectFirst("h1").text();

        std::vector<std::string> genres;
        auto genreNodes = info.site == Site::AnimePlayer ? content.select("div.sgeneros > a") : sheader.select("div.data div.sgeneros > a");
        for (auto& a : genreNodes) {
            std::string g = trim(a.text());
            if (!g.empty() && std::find(genres.begin(), genres.end(), g) == genres.end()) genres.push_back(g);
        }
        for (auto& g : genres) d.genre += (d.genre.empty() ? "" : ", ") + g;

        d.description = trim(description(*doc));

        if (info.site == Site::VoirCartoon) {
            for (auto& p : doc->select("div.mvic-info p")) {
                if (!contains(p.text(), "Status:")) continue;
                std::string st = trim(p.selectFirst("a[rel]").text());
                if (st == "Ongoing") d.status = "In corso";
                else if (st == "Completed") d.status = "Completato";
                break;
            }
        } else if (info.site == Site::Hds) {
            auto crumbs = doc->select(".dt-breadcrumb li");
            if (crumbs.size() >= 2 && trim(crumbs[1].text()) == "Films") d.status = "Completato";
        }

        d.episodes = episodes(*doc);
        return d;
    }

    // ------------------------------------------------------------------------------------------ video

    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::string pageUrl = abs(episodeUrl);
        std::vector<Video> out;
        switch (info.site) {
            case Site::AnimePlay:
            case Site::AnimesDrive:
            case Site::AnimesOnlineCloud:
            case Site::AnimeQ: out = animePlayVideos(pageUrl); break;
            case Site::AnimePlayer: out = animePlayerVideos(pageUrl); break;
            case Site::Q1N: out = q1nVideos(pageUrl); break;
            case Site::AnimesOnlineCC: out = animesOnlineCCVideos(pageUrl); break;
            case Site::AnimesRoll: out = animesRollVideos(pageUrl); break;
            case Site::BetterAnimeIo: out = betterAnimeVideos(pageUrl); break;
            case Site::PiFansubs: out = piFansubsVideos(pageUrl); break;
            case Site::Animenix: out = animenixVideos(pageUrl); break;
            case Site::AnimeOnlineNinja: out = ninjaVideos(pageUrl); break;
            case Site::JetAnime: out = jetAnimeVideos(pageUrl); break;
            case Site::VoirCartoon: out = voirCartoonVideos(pageUrl); break;
            case Site::Hds: out = hdsVideos(pageUrl); break;
            case Site::Kinoking: out = kinokingVideos(pageUrl); break;
            case Site::Cineplus123: out = cineplusVideos(pageUrl); break;
            case Site::DeTodoPeliculas: out = deTodoVideos(pageUrl); break;
            case Site::FlixLatam: out = flixLatamVideos(pageUrl); break;
            case Site::SoloLatino: out = soloLatinoVideos(pageUrl); break;
            case Site::VerPelisTop: out = verPelisVideos(pageUrl); break;
        }
        // rimuove eventuali duplicati
        std::set<std::string> seen;
        std::vector<Video> unique;
        for (auto& v : out)
            if (!v.url.empty() && seen.insert(v.url).second) unique.push_back(v);
        if (unique.empty()) throw http::Error("Nessun video trovato");
        sortVideos(unique);
        return unique;
    }

  private:
    SiteInfo info;
    static constexpr const char* LATEST_SEL = "div.content article > div.poster";

    bool ptBR() const { return std::string(info.lang) == "pt"; }

    http::Headers hdr() const {
        http::Headers h = {{"Referer", baseUrl()}};
        if (info.site == Site::PiFansubs) h.push_back({"Accept-Language", "pt-BR,pt;q=0.9,en-US;q=0.8,en;q=0.7"});
        return h;
    }

    std::string abs(const std::string& url) const {
        if (startsWith(url, "http")) return url;
        if (startsWith(url, "//")) return "https:" + url;
        return baseUrl() + (startsWith(url, "/") ? url : "/" + url);
    }

    /** URL relativo al sito (come setUrlWithoutDomain), assoluto se su un altro dominio. */
    std::string rel(const std::string& url) const {
        if (!startsWith(url, "http")) return url;
        if (http::hostOf(url) == http::hostOf(baseUrl())) return http::pathOf(url);
        return url;
    }

    DocPtr fetch(const std::string& url, const http::Headers& extra = {}) const {
        http::Response r = http::request("GET", url, mergeHeaders(hdr(), extra));
        if (!ok(r)) throw http::Error("Errore HTTP " + std::to_string(r.status));
        return std::make_unique<html::Document>(r.body, r.finalUrl.empty() ? url : r.finalUrl);
    }

    std::string latestNext() const {
        switch (info.site) {
            case Site::AnimePlay:
            case Site::AnimesDrive:
            case Site::AnimesOnlineCloud:
            case Site::AnimeQ: return "div.pagination > a.arrow_pag > i.fa-caret-right";
            case Site::AnimesOnlineCC: return "div.pagination > a.arrow_pag > i.icon-caret-right";
            case Site::AnimePlayer: return "a > i#nextpagination";
            case Site::Animenix:
            case Site::AnimeOnlineNinja: return NEXT_LAST_CHILD;
            case Site::JetAnime: return NEXT_CURRENT_PLUS_A;
            case Site::Kinoking:
            case Site::VerPelisTop: return "#nextpagination";
            case Site::SoloLatino: return "div.pagMovidy a";
            default: return "div.resppages > a > span.fa-chevron-right";
        }
    }

    static bool hasNext(const html::Document& doc, const std::string& sel) {
        if (sel.empty()) return false;
        if (sel == NEXT_LAST_CHILD) {
            for (auto& pag : doc.select("div.pagination")) {
                auto ch = pag.children();
                if (ch.empty()) continue;
                const html::Node& last = ch.back();
                std::string cls = " " + last.attr("class") + " ";
                if (last.tag() != "span" && !contains(cls, " current ")) return true;
            }
            return false;
        }
        if (sel == NEXT_CURRENT_PLUS_A) {
            for (auto& pag : doc.select("div.pagination")) {
                auto ch = pag.children();
                for (size_t i = 0; i + 1 < ch.size(); i++) {
                    std::string cls = " " + ch[i].attr("class") + " ";
                    if (ch[i].tag() == "span" && contains(cls, " current ") && ch[i + 1].tag() == "a") return true;
                }
            }
            return false;
        }
        return doc.selectFirst(sel).valid();
    }

    /** Element.getImageUrl(): data-src, data-lazy-src, srcset, src (scartando i segnaposto data:). */
    std::string imageUrl(const html::Document& doc, const html::Node& img) const {
        if (!img) return "";
        std::string u;
        for (const char* a : {"data-src", "data-lazy-src", "srcset", "src"}) {
            std::string v = trim(img.attr(a));
            if (v.empty() || startsWith(v, "data:")) continue;
            if (std::string(a) == "srcset") v = substringBefore(v, " ");
            u = http::resolve(doc.url().empty() ? baseUrl() + "/" : doc.url(), v);
            break;
        }
        if (info.site == Site::AnimePlay || info.site == Site::AnimesDrive || info.site == Site::AnimesOnlineCloud ||
            info.site == Site::AnimeQ)
            u = stripSizeSuffix(u);
        return u;
    }

    /** Toglie il suffisso "-<larghezza>x<altezza>" prima dell'estensione (".../file-200x300.jpg"). */
    static std::string stripSizeSuffix(const std::string& url) {
        auto dot = url.rfind('.');
        if (dot == std::string::npos) return url;
        for (size_t i = dot + 1; i < url.size(); i++)
            if (!std::isalnum((unsigned char)url[i])) return url;
        size_t p = dot;
        size_t e = p;
        while (p > 0 && std::isdigit((unsigned char)url[p - 1])) p--;
        if (p == e || p == 0 || url[p - 1] != 'x') return url;
        size_t e2 = p - 1, p2 = e2;
        while (p2 > 0 && std::isdigit((unsigned char)url[p2 - 1])) p2--;
        if (p2 == e2 || p2 == 0 || url[p2 - 1] != '-') return url;
        return url.substr(0, p2 - 1) + url.substr(dot);
    }

    /** popularAnimeFromElement / searchAnimeFromElement. */
    bool fromElement(const html::Document& doc, const html::Node& el, Anime& an, bool jetLatest) const {
        html::Node img = el.tag() == "img" ? el : el.selectFirst("img");
        if (!img) return false;
        html::Node link = el.tag() == "a" ? el : el.selectFirst("a");
        std::string href = link ? doc.absUrl(link, "href") : doc.absUrl(el, "href");
        if (href.empty()) return false;
        if (jetLatest) {
            std::string slug = substringAfter(href, "/episodes/");
            auto cut = slug.rfind("-episode");
            if (cut != std::string::npos) slug = slug.substr(0, cut);
            cut = slug.rfind("-saison");
            if (cut != std::string::npos) slug = slug.substr(0, cut);
            an.url = "/serie/" + slug;
        } else {
            an.url = rel(href);
        }
        an.title = trim(img.attr("alt"));
        if (info.site == Site::SoloLatino) an.thumbnail = img.attr("data-srcset");
        else an.thumbnail = imageUrl(doc, img);
        return !an.title.empty();
    }

    Page list(const std::string& url, const std::string& selector, const std::string& next, bool jetLatest = false) {
        DocPtr doc = fetch(url);
        Page p;
        std::set<std::string> seen;
        for (auto& el : doc->select(selector)) {
            Anime an;
            if (!fromElement(*doc, el, an, jetLatest)) continue;
            if (seen.insert(an.url).second) p.animes.push_back(an);
        }
        if (p.animes.empty() && selector != GENERIC_SEL) {
            for (auto& el : doc->select(GENERIC_SEL)) {
                Anime an;
                if (!fromElement(*doc, el, an, jetLatest)) continue;
                if (seen.insert(an.url).second) p.animes.push_back(an);
            }
        }
        p.hasNextPage = !p.animes.empty() && hasNext(*doc, next);
        return p;
    }

    /** getRealAnimeDoc: dalla pagina di un episodio passa a quella dell'anime. */
    DocPtr realAnimeDoc(DocPtr doc) const {
        std::string href;
        if (info.site == Site::Q1N) {
            if (!contains(doc->url(), "/e/")) return doc;
            for (auto& a : doc->select("div.pag_episodes div.item > a"))
                if (a.selectFirst("i.fa-th")) {
                    href = doc->absUrl(a, "href");
                    break;
                }
        } else {
            bool iconBars = info.site == Site::AnimePlayer || info.site == Site::AnimesOnlineCC || info.site == Site::AnimeOnlineNinja;
            html::Node icon = doc->selectFirst(iconBars ? "div.pag_episodes div.item a[href] i.icon-bars"
                                                        : "div.pag_episodes div.item a[href] i.fa-bars");
            if (icon) {
                html::Node a = icon.parent();
                while (a && a.tag() != "a") a = a.parent();
                if (a) href = doc->absUrl(a, "href");
            }
        }
        if (href.empty()) return doc;
        return fetch(href);
    }

    // ------------------------------------------------------------------------------------------ descrizione

    std::vector<std::string> infoItems() const {
        if (info.site == Site::Animenix || info.site == Site::AnimeOnlineNinja) return {"Título", "Temporadas", "Episodios", "Duración media"};
        if (ptBR()) return {"Título", "Ano", "Temporadas", "Episódios"};
        return {"Original", "First", "Last", "Seasons", "Episodes"};
    }

    /** Element.getInfo: "div.custom_fields:contains(x)" -> "\nchiave: valore". */
    std::string infoText(const html::Node& infoNode) const {
        std::string out;
        if (!infoNode) return out;
        auto fields = infoNode.select("div.custom_fields");
        for (auto& item : infoItems())
            for (auto& f : fields) {
                if (!contains(f.text(), item)) continue;
                out += "\n" + f.selectFirst("b").text() + ": " + f.selectFirst("span").text();
                break;
            }
        return out;
    }

    static std::string joinTexts(const std::vector<html::Node>& nodes, const std::string& sep) {
        std::string out;
        for (size_t i = 0; i < nodes.size(); i++) out += (i ? sep : "") + nodes[i].text();
        return out;
    }

    std::string description(const html::Document& doc) const {
        html::Node info_ = doc.selectFirst("div#info");
        switch (info.site) {
            case Site::AnimePlayer: return "";
            case Site::AnimePlay:
            case Site::AnimesDrive:
            case Site::AnimesOnlineCloud:
            case Site::AnimeQ: {
                if (!info_) return "";
                std::string desc, alt;
                for (auto& p : doc.select("div.wp-content p")) {
                    std::string t = p.text();
                    if (contains(t, "Título Alternativo")) {
                        if (alt.empty()) alt = t + "\n";
                    } else if (desc.empty()) {
                        desc = (info.site == Site::AnimeQ ? substringAfter(t, "Sinopse: ") : t) + "\n";
                    }
                }
                return desc + (info.site == Site::AnimeQ ? "\n" : "") + alt + infoText(info_);
            }
            case Site::Q1N:
                if (!info_) return "";
                return doc.selectFirst("div.wp-content p").text() + "\n" + infoText(info_);
            case Site::AnimesOnlineCC: {
                html::Node p = doc.selectFirst("div.wp-content p");
                return p ? p.text() + "\n" : "";
            }
            case Site::AnimesRoll:
                if (!info_) return "";
                return joinTexts(doc.select("div.wp-content p"), "\n") + infoText(info_);
            case Site::PiFansubs:
                if (!info_) return "";
                return joinTexts(doc.select("div#info p"), "\n\n") + "\n" + infoText(info_);
            case Site::Animenix:
            case Site::AnimeOnlineNinja:
                if (!info_) return "";
                return joinTexts(doc.select("div#info div.wp-content p"), "\n") + infoText(info_);
            case Site::SoloLatino: {
                html::Node wp = doc.selectFirst("#single > div.content > div.wp-content");
                if (!wp) return "";
                html::Node p = wp.selectFirst("p");
                return (p ? p.text() + "\n" : "") + infoText(wp);
            }
            case Site::VerPelisTop: return joinTexts(doc.select("div#info p"), " ");
            default: {
                if (!info_) return "";
                html::Node p = doc.selectFirst("div#info p");
                return (p ? p.text() + "\n" : "") + infoText(info_);
            }
        }
    }

    // ------------------------------------------------------------------------------------------ episodi

    std::string movieText() const {
        switch (info.site) {
            case Site::Animenix:
            case Site::AnimeOnlineNinja:
            case Site::Cineplus123:
            case Site::DeTodoPeliculas:
            case Site::FlixLatam:
            case Site::SoloLatino:
            case Site::VerPelisTop: return "Película";
            default: return ptBR() ? "Filme" : "Movie";
        }
    }

    std::string seasonPrefix() const {
        switch (info.site) {
            case Site::Cineplus123:
            case Site::DeTodoPeliculas:
            case Site::FlixLatam:
            case Site::VerPelisTop: return "Temporada";
            default: return ptBR() ? "Temporada" : "Season";
        }
    }

    /** episodeFromElement(element, seasonName) predefinito del tema. */
    bool defaultEpisode(const html::Document& doc, const html::Node& el, const std::string& seasonName, Episode& ep) const {
        html::Node num = el.selectFirst("div.numerando");
        html::Node href = el.selectFirst("a[href]");
        if (!num || !href) return false;
        std::string epNum = trailingNumber(num.text(), info.site == Site::AnimeOnlineNinja);
        ep.number = parseNumber(epNum, 0);
        ep.name = seasonPrefix() + " " + seasonName + " x " + epNum + " - " + ownText(href);
        ep.url = rel(doc.absUrl(href, "href"));
        if (info.site == Site::Kinoking) {
            // "Season 1 x 5 - Titolo" -> "Staffel 1 Folge 5 : Titolo"
            std::string sub = substringBefore(ep.name, " -");
            std::string repl = replaceAll(replaceAll(sub, "Season", "Staffel"), "x", "Folge");
            ep.name = replaceAll(ep.name, sub + " -", repl + " :");
        }
        return true;
    }

    std::vector<Episode> seasonEpisodes(const html::Document& doc, const html::Node& season) const {
        std::vector<Episode> out;
        Episode ep;
        switch (info.site) {
            case Site::AnimePlayer: {
                std::string seasonName = season.selectFirst("span.title").text();
                for (auto& el : season.select("ul.episodios > li")) {
                    html::Node p = el.selectFirst("div.episodiotitle p");
                    html::Node href = el.selectFirst("a[href]");
                    if (!p || !href) continue;
                    std::string epNum = trailingNumber(p.text(), false);
                    ep.number = parseNumber(epNum, 0);
                    ep.name = seasonName + " x Episódio " + epNum;
                    ep.url = rel(doc.absUrl(href, "href"));
                    out.push_back(ep);
                }
                return out;
            }
            case Site::AnimeQ: {
                std::string seasonName = season.selectFirst("span.se-t").text();
                for (auto& el : season.select("div.episodios-grid > div.episode-card")) {
                    html::Node href = el.selectFirst("a[href]");
                    if (!href) continue;
                    std::string epNum = trim(el.attr("data-episode-number"));
                    ep.number = parseNumber(epNum, 0);
                    ep.name = seasonPrefix() + " " + seasonName + " x " + epNum + " - " + trim(el.attr("data-episode-title"));
                    ep.url = rel(doc.absUrl(href, "href"));
                    out.push_back(ep);
                }
                return out;
            }
            case Site::Q1N: {
                // Q1N.episodeFromElement(element): elemento = link dell'episodio
                for (auto& a : season.select("ul.episodios > li > div.episodiotitle > a")) {
                    ep.name = a.text();
                    ep.number = parseNumber(substringAfter(ep.name, " "), 0);
                    ep.url = rel(doc.absUrl(a, "href"));
                    out.push_back(ep);
                }
                return out;
            }
            case Site::BetterAnimeIo: {
                std::string seasonName = season.selectFirst("span.se-t").text();
                if (seasonName.empty()) seasonName = "1";
                for (auto& el : season.select("ul.episodios > li")) {
                    html::Node link = el.selectFirst(".episodiotitle a");
                    if (!link) continue;
                    std::string epNum = trim(substringBefore(link.text(), " -"));
                    ep.number = parseNumber(epNum, 0);
                    ep.name = seasonPrefix() + " " + seasonName + " x " + epNum;
                    ep.url = rel(doc.absUrl(link, "href"));
                    out.push_back(ep);
                }
                return out;
            }
            case Site::SoloLatino: {
                std::string seasonName = season.attr("data-season");
                for (auto& el : season.select("ul.episodios li")) {
                    html::Node href = el.selectFirst("a[href]");
                    if (!href) continue;
                    std::string epNum = el.selectFirst("div.numerando") ? trailingNumber(el.selectFirst("div.numerando").text(), false) : "0";
                    html::Node epst = el.selectFirst("div.epst");
                    ep.number = parseNumber(epNum, 0);
                    ep.name = "T" + seasonName + " - Episodio " + epNum + ": " + (epst ? epst.text() : std::string("Sin título"));
                    ep.url = rel(doc.absUrl(href, "href"));
                    out.push_back(ep);
                }
                return out;
            }
            default: {
                html::Node se = season.selectFirst("span.se-t");
                std::string seasonName = se.text();
                // AnimesROLL: senza nome della stagione episodeFromElement(element) non e' implementato
                if (info.site == Site::AnimesRoll && trim(seasonName).empty()) return out;
                for (auto& el : season.select("ul.episodios > li"))
                    if (defaultEpisode(doc, el, seasonName, ep)) out.push_back(ep);
                return out;
            }
        }
    }

    std::vector<Episode> episodes(const html::Document& doc) const {
        std::vector<Episode> out;
        auto movie = [&]() {
            Episode ep;
            ep.url = rel(doc.url());
            ep.number = 1;
            ep.name = movieText();
            return std::vector<Episode>{ep};
        };
        if (info.site == Site::VoirCartoon) {
            for (auto& el : doc.select("ul.episodios > li")) {
                html::Node num = el.selectFirst("div.numerando");
                html::Node href = el.selectFirst("a[href]");
                if (!num || !href) continue;
                Episode ep;
                ep.number = parseNumber(trailingNumber(num.text(), false), 0);
                ep.name = "Saison" + afterLast(ownText(href), "Saison");
                ep.url = rel(doc.absUrl(href, "href"));
                out.push_back(ep);
            }
            if (out.empty()) return movie();
            std::reverse(out.begin(), out.end());
            return out;
        }
        auto seasons = doc.select(info.site == Site::SoloLatino ? "div#seasons div.se-c" : "div#seasons > div");
        if (seasons.empty()) return movie();
        if (info.site == Site::AnimeOnlineNinja) {
            // stagioni dalla piu' recente, episodi di ogni stagione dal piu' recente
            for (auto it = seasons.rbegin(); it != seasons.rend(); ++it) {
                auto eps = seasonEpisodes(doc, *it);
                out.insert(out.end(), eps.rbegin(), eps.rend());
            }
            return out;
        }
        for (auto& s : seasons) {
            auto eps = seasonEpisodes(doc, s);
            out.insert(out.end(), eps.begin(), eps.end());
        }
        std::reverse(out.begin(), out.end());
        // AnimePlayer: l'estensione inverte di nuovo la lista (il sito elenca gia' dal piu' recente)
        if (info.site == Site::AnimePlayer) std::reverse(out.begin(), out.end());
        return out;
    }

    // ------------------------------------------------------------------------------------------ player DooPlay

    enum class Api { Ajax, V1, V2 };

    std::string playerAjax(const html::Node& li, const std::string& referer = "", const std::string& ajaxUrl = "") const {
        http::Headers h = mergeHeaders(hdr(), {{"Content-Type", "application/x-www-form-urlencoded; charset=UTF-8"},
                                               {"X-Requested-With", "XMLHttpRequest"}});
        if (!referer.empty()) h = mergeHeaders(h, {{"Referer", referer}});
        std::string type = li.attr("data-type");
        if (info.site == Site::DeTodoPeliculas && trim(type).empty()) type = "movie";
        http::Response r = http::request("POST", ajaxUrl.empty() ? baseUrl() + "/wp-admin/admin-ajax.php" : ajaxUrl, h,
                                         formEncode({{"action", "doo_player_ajax"},
                                                     {"post", li.attr("data-post")},
                                                     {"nume", li.attr("data-nume")},
                                                     {"type", type}}));
        if (!ok(r)) throw http::Error("Errore del player");
        return r.body;
    }

    std::string playerV2(const html::Node& li, const std::string& apiBase = "") const {
        std::string base = apiBase.empty() ? baseUrl() + "/wp-json/dooplayer/v2/" : apiBase;
        if (!endsWith(base, "/")) base += "/";
        return http::getText(base + li.attr("data-post") + "/" + li.attr("data-type") + "/" + li.attr("data-nume"), hdr());
    }

    std::string playerV1(const html::Node& li, const std::string& origin = "") const {
        return http::getText((origin.empty() ? baseUrl() : origin) + "/wp-json/dooplayer/v1/post/" + li.attr("data-post") +
                                 "?type=" + li.attr("data-type") + "&source=" + li.attr("data-nume"),
                             hdr());
    }

    /** Configurazione del player letta dalla pagina (dtAjax = {"url":..., "player_api":..., "play_method":...}). */
    struct PlayerCfg {
        std::string origin, ajaxUrl, apiV2;
        bool ajaxFirst = false;
        bool known = false;
    };

    PlayerCfg playerCfg(const html::Document& doc) const {
        PlayerCfg c;
        c.origin = doc.url().empty() ? baseUrl() : http::originOf(doc.url());
        std::string script = scriptWith(doc, {"dtAjax"});
        if (!script.empty()) {
            std::string obj = substringAfter(script.substr(script.find("dtAjax")), "=");
            auto end = obj.find("};");
            if (end != std::string::npos && end < 8192) {
                json j = json::parse(trim(obj.substr(0, end + 1)), nullptr, false);
                if (j.is_object()) {
                    c.known = true;
                    std::string u = jsonString(j, "url");
                    if (!u.empty()) c.ajaxUrl = http::resolve(c.origin + "/", u);
                    c.apiV2 = jsonString(j, "player_api");
                    c.ajaxFirst = jsonString(j, "play_method") == "admin_ajax";
                }
            }
        }
        if (c.ajaxUrl.empty()) c.ajaxUrl = c.origin + "/wp-admin/admin-ajax.php";
        if (c.apiV2.empty()) c.apiV2 = c.origin + "/wp-json/dooplayer/v2/";
        return c;
    }

    /**
     * URL dell'embed di un'opzione del player. Usa il metodo indicato dalla pagina (dtAjax), poi quello
     * dell'estensione Kotlin e infine gli altri: i siti cambiano spesso tra admin-ajax e le API REST v1/v2.
     */
    std::string playerEmbed(const html::Document& doc, const html::Node& li, Api pref) const {
        if (li.attr("data-nume") == "trailer") return "";
        PlayerCfg c = playerCfg(doc);
        std::vector<Api> order;
        if (c.known) order = c.ajaxFirst ? std::vector<Api>{Api::Ajax, Api::V2} : std::vector<Api>{Api::V2, Api::Ajax};
        for (Api a : {pref, Api::Ajax, Api::V2, Api::V1})
            if (std::find(order.begin(), order.end(), a) == order.end()) order.push_back(a);
        for (Api a : order) {
            try {
                std::string body = a == Api::Ajax ? playerAjax(li, doc.url(), c.ajaxUrl)
                                   : a == Api::V2 ? playerV2(li, c.apiV2)
                                                  : playerV1(li, c.origin);
                std::string url = embedUrlOf(body);
                if (url.empty() && contains(body, "src='")) url = replaceAll(substringBefore(substringAfter(body, "src='"), "'"), "\\", "");
                if (url.empty()) url = iframeSrc(body);
                url = fixUrl(url, c.origin + "/");
                if (startsWith(url, "http")) return url;
            } catch (const std::exception&) {
            }
        }
        return "";
    }

    template <typename F>
    static void each(const std::vector<html::Node>& nodes, std::vector<Video>& out, F fn) {
        for (auto& n : nodes) {
            try {
                appendAll(out, fn(n));
            } catch (const std::exception&) {
                // server non disponibile: si passa al successivo
            }
        }
    }

    static std::string ptQualityName(const std::string& t) {
        std::string u = upper(trim(t));
        if (u == "SD") return "360p";
        if (u == "HD" || u == "SD/HD" || u == "SD / HD") return "720p";
        if (u == "FHD" || u == "FULLHD" || u == "FULLHD / HLS") return "1080p";
        return t;
    }

    // ---- AnimePlay / AnimesDrive / AnimesOnlineCloud / AnimeQ
    std::vector<Video> animePlayVideos(const std::string& pageUrl) {
        DocPtr doc = fetch(pageUrl);
        std::vector<Video> out;
        each(doc->select("ul#playeroptionsul li"), out, [&](const html::Node& li) -> std::vector<Video> {
            std::string title = li.selectFirst("span.title").text();
            std::string name = title.empty() ? "Player" : ptQualityName(title);
            std::string url = playerEmbed(*doc, li, info.site == Site::AnimePlay ? Api::Ajax : Api::V2);
            if (!startsWith(url, "http")) return {};
            if (contains(url, "blogger.com")) {
                auto v = blogger(url, baseUrl());
                if (!v.empty()) return v;
            } else if (contains(url, "jwplayer?source=")) {
                std::string src = urlDecode(http::queryParam(url, "source"));
                if (src.empty()) return {};
                std::string origin = "https://" + http::hostOf(url);
                return {simpleVideo(src, "JWPlayer - " + name, origin + "/", {{"Origin", origin}, {"Accept", "*/*"}})};
            }
            return universal(url, name, baseUrl());
        });
        return out;
    }

    // ---- AnimePlayer
    std::vector<Video> animePlayerVideos(const std::string& pageUrl) {
        DocPtr doc = fetch(pageUrl);
        html::Node iframe = doc->selectFirst("div.playex iframe");
        if (!iframe) return {};
        std::string playerUrl = doc->absUrl(iframe, "src");
        if (!startsWith(playerUrl, "http")) return {};
        html::Node q = doc->selectFirst("span.qualityx");
        std::string quality = q ? afterLast(q.text(), " ") : "Default";
        std::string link = urlDecode(http::queryParam(playerUrl, "link"));
        std::string url = link.empty() ? playerUrl : link;
        try {
            if (contains(url, "cdn.animeson.com.br")) return {simpleVideo(url, quality, baseUrl())};
            if (contains(url, "blogger.com")) return blogger(url, baseUrl());
        } catch (const std::exception&) {
        }
        return {};
    }

    // ---- Q1N (animesgratis)
    std::vector<Video> q1nVideos(const std::string& pageUrl) {
        DocPtr doc = fetch(pageUrl);
        std::vector<Video> out;
        each(doc->select("ul#playeroptionsul li"), out, [&](const html::Node& li) -> std::vector<Video> {
            std::string name = lower(li.selectFirst("span.title").text());
            html::Node iframe = doc->selectFirst("div#source-player-" + li.attr("data-nume") + " iframe");
            if (!iframe) return {};
            std::string url = iframe.hasAttr("data-litespeed-src") ? iframe.attr("data-litespeed-src") : iframe.attr("src");
            url = trim(url);
            if (url.empty()) return {};
            if (contains(url, "/aviso/")) url = urlDecode(http::queryParam(url, "url"));
            url = fixUrl(url, pageUrl);
            if (url.empty()) return {};
            if (contains(name, "ruplay")) return ruplay(url);
            if (contains(name, "streamwish")) return streamWish(url, "", {{"Referer", baseUrl() + "/"}});
            if (contains(name, "filemoon")) return moon(url, baseUrl(), "Filemoon - ");
            if (contains(name, "mixdrop")) return mixDrop(url, "");
            if (contains(name, "streamtape")) return streamtape(url, "");
            if (contains(name, "noa")) return noa(url, baseUrl());
            if (contains(name, "mdplayer") || contains(url, "/antivirus3/")) return noa(url, baseUrl(), name);
            if (contains(url, "/player/") || contains(url, "blogger.com")) return blogger(url, baseUrl());
            return universal(url, name, baseUrl());
        });
        return out;
    }

    // ---- Animes Online CC
    std::vector<Video> animesOnlineCCVideos(const std::string& pageUrl) {
        DocPtr doc = fetch(pageUrl);
        std::vector<Video> out;
        each(doc->select("#playex iframe"), out, [&](const html::Node& iframe) -> std::vector<Video> {
            std::string url = iframe.attr("src");
            std::string id = iframe.parent().attr("id");
            std::string language;
            for (auto& a : doc->select("a.options")) {
                if (a.attr("href") != "#" + id) continue;
                std::string t = trim(a.text());
                if (lower(t) == "legendado" || lower(t) == "dublado") language = t;
                break;
            }
            if (contains(url, "blogger.com")) return blogger(url, baseUrl(), language);
            return {};
        });
        return out;
    }

    // ---- Animes ROLL
    std::vector<Video> animesRollVideos(const std::string& pageUrl) {
        DocPtr doc = fetch(pageUrl);
        std::vector<Video> out;
        each(doc->select("ul#playeroptionsul li"), out, [&](const html::Node& li) -> std::vector<Video> {
            std::string fullName = li.selectFirst("span.title").text();
            std::string realName = substringBefore(substringAfter(fullName, "("), ")");
            std::string url = playerEmbed(*doc, li, Api::Ajax);
            std::vector<Video> v;
            if (contains(url, "anroll.online")) v = anrollOnline(url, realName);
            if (v.empty()) v = universal(url, realName, baseUrl());
            return v;
        });
        return out;
    }

    // ---- BetterAnimeIo
    std::vector<Video> betterAnimeVideos(const std::string& pageUrl) {
        DocPtr doc = fetch(pageUrl);
        std::vector<Video> out;
        each(doc->select("ul#playeroptionsul li"), out, [&](const html::Node& li) -> std::vector<Video> {
            std::string url = playerEmbed(*doc, li, Api::V2);
            if (!contains(url, "jwplayer?source=") && !contains(url, "jwplayer/?source=")) return {};
            std::string src = urlDecode(http::queryParam(url, "source"));
            if (src.empty()) return {};
            return betterAnimeApi(src);
        });
        return out;
    }

    // ---- Pi Fansubs
    std::vector<Video> piFansubsVideos(const std::string& pageUrl) {
        DocPtr doc = fetch(pageUrl);
        std::vector<Video> out;
        each(doc->select("div.source-box:not(#source-player-trailer) iframe"), out, [&](const html::Node& f) -> std::vector<Video> {
            std::string url = f.attr("data-src");
            if (url.empty()) url = f.attr("src");
            if (!startsWith(url, "http")) url = "https:" + url;
            if (contains(url, "https://vidhide")) return vidHide(url, "", hdr());
            if (contains(url, "https://blembed")) return blembed(url, hdr(), baseUrl());
            return {};
        });
        return out;
    }

    // ---- Animenix
    std::vector<Video> animenixVideos(const std::string& pageUrl) {
        DocPtr doc = fetch(pageUrl);
        std::vector<Video> out;
        each(doc->select("li.dooplay_player_option"), out, [&](const html::Node& li) -> std::vector<Video> {
            std::string link = playerEmbed(*doc, li, Api::Ajax);
            if (link.empty()) return {};
            std::string server = li.selectFirst("span.title").text();
            if (contains(link, "filemoon")) return moon(link, baseUrl(), "Filemoon - ");
            if (contains(link, "swdyu") || contains(link, "wishembed") || contains(link, "cdnwish") || contains(link, "flaswish") ||
                contains(link, "sfastwish") || contains(link, "streamwish") || contains(link, "asnwish"))
                return streamWish(link, "", {{"Referer", baseUrl() + "/"}});
            return universal(link, server, baseUrl());
        });
        return out;
    }

    // ---- AnimeOnline.Ninja
    std::vector<Video> ninjaExtract(const std::string& url, const std::string& lang, int depth = 0) {
        std::string key = matchKey(url, ORDER_NINJA, lang);
        if (key.empty()) return {};
        if (key != "saidochesto") return hosterByKey(key, url, titlePrefix(lang), baseUrl());
        if (depth > 0) return {};
        // multiserver: tutte le lingue (SUB, ES, LAT) come video separati
        DocPtr doc = fetch(url);
        std::vector<Video> out;
        for (auto& li : doc->select("div.ODDIV div > li")) {
            std::string hosterUrl = substringBefore(substringAfter(li.attr("onclick"), "('"), "')");
            std::string cls = li.parent().attr("class");
            std::string l = substringBefore(after(cls, "OD_", ""), " ");
            try {
                appendAll(out, ninjaExtract(hosterUrl, l, depth + 1));
            } catch (const std::exception&) {
            }
        }
        return out;
    }

    std::vector<Video> ninjaVideos(const std::string& pageUrl) {
        DocPtr doc = fetch(pageUrl);
        std::vector<Video> out;
        each(doc->select("ul#playeroptionsul li"), out, [&](const html::Node& li) -> std::vector<Video> {
            std::string name = li.selectFirst("span.title").text();
            std::string url = playerEmbed(*doc, li, Api::V1);
            if (url.empty()) return {};
            return ninjaExtract(url, name);
        });
        return out;
    }

    // ---- JetAnime
    std::vector<Video> jetAnimeVideos(const std::string& pageUrl) {
        DocPtr doc = fetch(pageUrl);
        std::vector<Video> out;
        each(doc->select("ul#playeroptionsul li"), out, [&](const html::Node& li) -> std::vector<Video> {
            if (li.attr("data-nume") == "trailer") return {};
            std::string url = playerEmbed(*doc, li, Api::V1);
            if (url.empty()) return {};
            std::string redirected = url;
            try {
                http::Response r = http::request("GET", url, {}, "", 20, false);
                std::string loc = r.header("location");
                if (!loc.empty()) redirected = http::resolve(url, loc);
            } catch (const std::exception&) {
            }
            std::string name = trim(li.text());
            if (contains(redirected, "https://sentinel")) return jetPlayer(redirected, "Sentinel", name);
            if (contains(redirected, "https://hdsplay")) return jetPlayer(redirected, "Hdsplay", name);
            return universal(redirected, name, http::originOf(doc->url()));
        });
        return out;
    }

    // ---- VoirCartoon
    std::vector<Video> voirCartoonVideos(const std::string& pageUrl) {
        DocPtr doc = fetch(pageUrl);
        html::Node idNode = doc->selectFirst("input[name=idpost]");
        if (!idNode) return {};
        std::string id = idNode.attr("value");
        std::vector<std::string> urls;
        for (auto& opt : doc->select("nav.player select > option")) {
            if (contains(opt.text(), "Hydrax")) continue;
            try {
                std::string u = trim(http::getText(baseUrl() + "/ajax-get-link-stream/?server=" + http::urlEncode(opt.attr("value")) +
                                                       "&filmId=" + http::urlEncode(id),
                                                   hdr()));
                if (!u.empty() && std::find(urls.begin(), urls.end(), u) == urls.end()) urls.push_back(u);
            } catch (const std::exception&) {
            }
        }
        std::vector<Video> out;
        for (auto& u : urls) {
            try {
                if (contains(u, "comedy")) appendAll(out, comedyShow(u));
            } catch (const std::exception&) {
            }
        }
        return out;
    }

    // ---- HDS
    std::vector<Video> hdsVideos(const std::string& pageUrl) {
        DocPtr doc = fetch(pageUrl);
        std::vector<Video> out;
        each(doc->select("#playeroptions li:not(#player-option-trailer)"), out, [&](const html::Node& li) -> std::vector<Video> {
            std::string secured = playerEmbed(*doc, li, Api::V1);
            if (!startsWith(secured, "http")) return {};
            http::Response r = http::request("GET", secured, hdr());
            std::string playerUrl = r.finalUrl.empty() ? secured : r.finalUrl;
            if (contains(playerUrl, "sentinel")) return moon(playerUrl, baseUrl(), "Filemoon - ");
            if (contains(playerUrl, "hdsplay")) return vidHide(playerUrl, "", hdr());
            return {};
        });
        return out;
    }

    // ---- Kinoking
    std::vector<Video> kinokingVideos(const std::string& pageUrl) {
        DocPtr doc = fetch(pageUrl);
        std::vector<Video> out;
        each(doc->select("li.dooplay_player_option"), out, [&](const html::Node& li) -> std::vector<Video> {
            std::string link = playerEmbed(*doc, li, Api::Ajax);
            if (contains(link, "https://dood")) return dood(link, "");
            if (contains(link, "https://voe.sx")) return voe(link, "");
            if (contains(link, "filehosted")) return {simpleVideo(link, "Filehosted")};
            return {};
        });
        return out;
    }

    // ---- Cineplus123
    std::vector<Video> cineplusVideos(const std::string& pageUrl) {
        DocPtr doc = fetch(pageUrl);
        std::vector<Video> out;
        each(doc->select("ul#playeroptionsul li"), out, [&](const html::Node& li) -> std::vector<Video> {
            std::string lang = li.selectFirst("span.title").text();
            std::string url = playerEmbed(*doc, li, Api::Ajax);
            if (url.empty()) return {};
            if (contains(url, "uqload")) return uqload(url, lang + " -");
            if (contains(url, "strwish")) return streamWish(url, titlePrefix(lang), {{"Referer", baseUrl() + "/"}});
            return universal(url, lang, baseUrl());
        });
        return out;
    }

    // ---- DeTodoPeliculas
    std::string normalizeUrl(const std::string& url) const {
        std::string t = trim(url);
        if (t.empty()) return "";
        if (startsWith(t, "//")) return "https:" + t;
        if (startsWith(t, "/")) return baseUrl() + t;
        return t;
    }

    std::string deTodoPlayerUrl(const html::Node& li, const std::string& referer, const std::string& ajaxUrl) const {
        for (const char* a : {"data-option", "data-player", "data-src", "data-url", "data-video"}) {
            std::string v = trim(li.attr(a));
            if (!v.empty()) return normalizeUrl(v);
        }
        std::string href = trim(li.selectFirst("a[href]").attr("href"));
        if (!href.empty()) return normalizeUrl(href);
        if (trim(li.attr("data-post")).empty() || trim(li.attr("data-nume")).empty()) return "";
        std::string body = playerAjax(li, referer, ajaxUrl);
        if (trim(body).empty()) return "";
        std::string embed = normalizeUrl(embedUrlOf(body));
        if (!embed.empty()) return embed;
        std::string iframe = normalizeUrl(iframeSrc(body));
        if (!iframe.empty()) return iframe;
        html::Document d(body);
        return normalizeUrl(d.selectFirst("source[src]").attr("src"));
    }

    std::vector<Video> deTodoExtract(const std::string& url, const std::string& lang, const std::string& referer, int depth = 0) {
        if (depth >= 3) return {};
        std::string normalized = normalizeUrl(url);
        if (normalized.empty()) return {};
        if (startsWith(normalized, baseUrl() + "/player")) {
            std::string id = urlDecode(http::queryParam(normalized, "id"));
            if (!id.empty()) {
                std::string decoded = base64Decode(id);
                if (!startsWith(decoded, "http") && !startsWith(decoded, "/")) decoded = id;
                decoded = normalizeUrl(decoded);
                if (!decoded.empty() && decoded != normalized) return deTodoExtract(decoded, lang, normalized, depth + 1);
            }
        }
        if (contains(normalized, "trembed")) {
            DocPtr embed = fetch(normalized, {{"Referer", referer}, {"Origin", baseUrl()}});
            html::Node f = embed->selectFirst("iframe[src], iframe[data-src], iframe[data-lazy-src]");
            std::string src;
            for (const char* a : {"src", "data-src", "data-lazy-src"}) {
                src = trim(f.attr(a));
                if (!src.empty()) break;
            }
            src = normalizeUrl(src);
            if (src.empty()) return {};
            return deTodoExtract(src, lang, referer, depth + 1);
        }
        std::string p = lang + " - ";
        for (const char* d : {"vidhide", "vidhidepro", "luluvdo", "vidhideplus"})
            if (containsCI(normalized, d)) return vidHide(normalized, p + upper(d) + " - ", hdr());
        if (contains(normalized, "uqload")) return uqload(normalized, p);
        if (contains(normalized, "streamwish") || contains(normalized, "strwish") || contains(normalized, "wishembed"))
            return streamWish(normalized, p, {{"Referer", baseUrl() + "/"}});
        // vidguard/listeamed: richiede un motore JS, non supportato
        if (contains(normalized, "voe")) return voe(normalized, p);
        return {};
    }

    std::vector<Video> deTodoVideos(const std::string& pageUrl) {
        DocPtr doc = fetch(pageUrl);
        std::vector<Video> out;
        std::string ajaxUrl = playerCfg(*doc).ajaxUrl;
        each(doc->select("ul#playeroptionsul li"), out, [&](const html::Node& li) -> std::vector<Video> {
            html::Node flag = li.selectFirst("span.flag img");
            std::string flagSrc = flag.attr("data-lazy-src");
            if (trim(flagSrc).empty()) flagSrc = flag.attr("src");
            std::string f = lower(flagSrc);
            std::string lang = contains(f, "sub") ? "[SUB]" : contains(f, "cas") ? "[CAST]" : contains(f, "lat") ? "[LAT]" : "UNKNOWN";
            std::string url = deTodoPlayerUrl(li, pageUrl, ajaxUrl);
            if (url.empty()) return {};
            return deTodoExtract(url, lang, pageUrl);
        });
        return out;
    }

    // ---- FlixLatam
    std::vector<Video> flixLatamVideos(const std::string& pageUrl) {
        DocPtr doc = fetch(pageUrl);
        std::vector<Video> out;
        each(doc->select("ul#playeroptionsul li"), out, [&](const html::Node& li) -> std::vector<Video> {
            std::string url = playerEmbed(*doc, li, Api::Ajax);
            if (!contains(url, "embed69")) return {};
            http::Response r = http::request("GET", url, mergeHeaders(hdr(), {{"Referer", pageUrl}}));
            if (!ok(r) || trim(r.body).empty()) return {};
            std::vector<Video> res;
            for (auto& link : dataLinkLinks(r.body)) {
                try {
                    appendAll(res, resolveHoster(link.first, link.second, baseUrl(), ORDER_FLIX));
                } catch (const std::exception&) {
                }
            }
            return res;
        });
        return out;
    }

    // ---- SoloLatino
    std::vector<std::pair<std::string, std::string>> soloParseLinks(const std::string& htmlText) const {
        auto links = dataLinkLinks(htmlText);
        html::Document doc(htmlText);
        for (auto& li : doc.select("li")) {
            std::string onclick = li.attr("onclick");
            if (onclick.empty()) continue;
            auto p = onclick.find(".php?link=");
            if (p != std::string::npos) {
                auto e = onclick.find("&servidor=", p);
                if (e != std::string::npos) {
                    std::string decoded = base64Decode(onclick.substr(p + 10, e - p - 10));
                    if (!trim(decoded).empty()) links.push_back({decoded, "unknown"});
                }
            }
            for (const char* pat : {"go_to_playerVast('", "go_to_player('"}) {
                auto s = onclick.find(pat);
                if (s == std::string::npos) continue;
                s += std::strlen(pat);
                auto e = onclick.find('\'', s);
                if (e != std::string::npos && e > s) links.push_back({onclick.substr(s, e - s), "unknown"});
            }
        }
        return links;
    }

    std::vector<Video> soloLatinoVideos(const std::string& pageUrl) {
        DocPtr doc = fetch(pageUrl);
        std::vector<std::pair<std::string, std::string>> links;
        for (auto& el : doc->select("[data-post][data-nume]")) {
            if (el.attr("data-type").empty()) continue;
            try {
                std::string iframe = playerEmbed(*doc, el, Api::Ajax);
                if (!startsWith(iframe, "http")) continue;
                http::Response r = http::request("GET", iframe, mergeHeaders(hdr(), {{"Referer", pageUrl}}));
                if (!ok(r)) continue;
                auto l = soloParseLinks(r.body);
                links.insert(links.end(), l.begin(), l.end());
            } catch (const std::exception&) {
            }
        }
        if (links.empty()) {
            html::Node f = doc->selectFirst("[class$=pframe] > iframe[class]");
            std::string web = f.attr("src");
            if (startsWith(web, "http")) {
                try {
                    http::Response r = http::request("GET", web, mergeHeaders(hdr(), {{"Referer", pageUrl}}));
                    if (ok(r)) links = soloParseLinks(r.body);
                } catch (const std::exception&) {
                }
                if (links.empty() && contains(web, "xyz")) links.push_back({web, "unknown"});
            }
        }
        std::vector<Video> out;
        for (auto& link : links) {
            if (trim(link.first).empty()) continue;
            std::string prefix = link.second == "unknown" ? "[UNK]" : link.second;
            try {
                appendAll(out, resolveHoster(link.first, prefix, baseUrl(), ORDER_SOLO));
            } catch (const std::exception&) {
            }
        }
        return out;
    }

    // ---- VerPelisTop
    std::vector<Video> verPelisVideos(const std::string& pageUrl) {
        DocPtr doc = fetch(pageUrl);
        std::vector<Video> out;
        each(doc->select("ul#playeroptionsul li"), out, [&](const html::Node& li) -> std::vector<Video> {
            std::string iframe = playerEmbed(*doc, li, Api::Ajax);
            if (!startsWith(iframe, "http")) return {};
            DocPtr frame = fetch(iframe);
            std::vector<Video> res;
            for (auto& h : frame->select(".OD li[onclick]")) {
                std::string server = textOf(h.select("span"));
                std::string lang = trim(substringBefore(textOf(h.select("p")), "-"));
                std::string url = substringBefore(substringAfter(h.attr("onclick"), "('"), "')");
                try {
                    std::string key = matchKey(url, ORDER_VERPELIS);
                    if (key == "streamtape") appendAll(res, streamtape(url, lang + " - "));
                    else if (!key.empty()) appendAll(res, hosterByKey(key, url, lang + " - ", baseUrl()));
                    else appendAll(res, universal(url, lang + " " + server, baseUrl()));
                } catch (const std::exception&) {
                }
            }
            return res;
        });
        return out;
    }

    // ------------------------------------------------------------------------------------------ ordinamento

    /** sortVideos con le preferenze predefinite di ogni estensione (lingua, server, qualita'). */
    void sortVideos(std::vector<Video>& list) const {
        std::string langPref, serverPref, quality = "1080p";
        switch (info.site) {
            case Site::AnimeOnlineNinja: langPref = "SUB"; serverPref = "Uqload"; break;
            case Site::Animenix: langPref = "SUB"; break;
            case Site::Cineplus123: langPref = "LATINO"; serverPref = "Uqload"; break;
            case Site::DeTodoPeliculas:
            case Site::FlixLatam: langPref = "[LAT]"; serverPref = "Uqload"; break;
            case Site::SoloLatino: langPref = "[LAT]"; serverPref = "StreamWish"; quality = "1080"; break;
            case Site::VerPelisTop: langPref = "Latino"; serverPref = "VidHide"; break;
            case Site::AnimesOnlineCC: langPref = "Legendado"; break;
            default: break;
        }
        auto key = [&](const Video& v) {
            int l = !langPref.empty() && containsCI(v.title, langPref) ? 1 : 0;
            int s = !serverPref.empty() && containsCI(v.title, serverPref) ? 1 : 0;
            int q = containsCI(v.title, quality) ? 1 : 0;
            int h = v.quality > 0 ? v.quality : qualityOf(v.title);
            return std::make_tuple(l, s, q, h);
        };
        std::stable_sort(list.begin(), list.end(), [&](const Video& a, const Video& b) { return key(a) > key(b); });
    }
};

}  // namespace

std::vector<std::shared_ptr<Source>> makeDooplaySources() {
    static const SiteInfo sites[] = {
        // anime (pt)
        {Site::AnimePlay, "pt.animeplay", "Anime Play", "https://animeplay.cloud", "pt", true},
        {Site::AnimePlayer, "pt.animeplayer", "AnimePlayer", "https://animeplayer.com.br", "pt", true},
        {Site::AnimeQ, "pt.animeq", "AnimeQ", "https://animeq.net", "pt", true},
        {Site::AnimesDrive, "pt.animesdrive", "Animes Drive", "https://animesdrive.online", "pt", true},
        {Site::Q1N, "pt.animesgratis", "Q1N", "https://q1n.net", "pt", false},
        {Site::AnimesOnlineCC, "pt.animesonlinecc", "Animes Online CC", "https://animesonlinecc.to", "pt", true},
        {Site::AnimesOnlineCloud, "pt.animesonlinecloud", "Animes Online Cloud", "https://animesonline.cloud", "pt", true},
        {Site::AnimesRoll, "pt.animesroll", "Animes ROLL", "https://anroll.tv", "pt", false},
        {Site::BetterAnimeIo, "pt.betteranimeio", "BetterAnimeIo", "https://betteranime.io", "pt", false},
        {Site::PiFansubs, "pt.pifansubs", "Pi Fansubs", "https://pifansubs.club", "pt", true},
        // anime (es, fr)
        {Site::Animenix, "es.animenix", "Animenix", "https://animenix.com", "es", false},
        {Site::AnimeOnlineNinja, "es.animeonlineninja", "AnimeOnline.Ninja", "https://ww3.animeonline.ninja", "es", false},
        {Site::JetAnime, "fr.jetanime", "JetAnime", "https://ssl.jetanimes.com", "fr", false},
        {Site::VoirCartoon, "fr.voircartoon", "VoirCartoon", "https://voircartoon.com", "fr", true},
        // film / serie
        {Site::Kinoking, "de.kinoking", "Kinoking", "https://kinoking.cc", "de", false},
        {Site::Cineplus123, "es.cineplus123", "Cineplus123", "https://cineplus123.org", "es", false},
        {Site::DeTodoPeliculas, "es.detodopeliculas", "DeTodo Peliculas", "https://detodopeliculas.nu", "es", false},
        {Site::FlixLatam, "es.flixlatam", "FlixLatam", "https://flixlatam.com", "es", false},
        {Site::SoloLatino, "es.sololatino", "SoloLatino", "https://sololatino.net", "es", false},
        {Site::VerPelisTop, "es.verpelistop", "VerPelisTop", "https://www1.verpelis.top", "es", false},
        {Site::Hds, "fr.hds", "HDS", "https://on1.hds.quest", "fr", false},
    };
    std::vector<std::shared_ptr<Source>> out;
    for (auto& s : sites) out.push_back(std::make_shared<DooPlay>(s));
    return out;
}

}  // namespace src
