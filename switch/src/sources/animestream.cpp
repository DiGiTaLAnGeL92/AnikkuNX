// Porting in C++ del tema multisrc Aniyomi "AnimeStream" (lib-multisrc/animestream).
// Siti inglesi / multilingua: Animenosub, AnimeKhor, LuciferDonghua, DonghuaStream, AnimeXin, ChineseAnime, LMAnime.
// Altre lingue: desu-online (pl), AnimeYT.es e Tiodonghua (es), MyKdrama (fr), AnimeIndo e MiniOppai (id),
// AnimeBalkan (sr), AsyaAnimeleri e TRAnimeCI (tr), Anikyuu, Animeito e SmartAnimes (pt).
// Gli estrattori degli hoster (lib/*extractor e quelli specifici delle estensioni) sono inclusi qui sotto.

#include <gumbo.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <random>
#include <regex>
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

// =============================================================================================== utilita'

const char* DESKTOP_UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/134.0.0.0 Safari/537.36";

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

/** Come String.trim(vararg chars) di Kotlin. */
std::string trimChars(const std::string& s, const std::string& chars) {
    size_t b = 0, e = s.size();
    while (b < e && chars.find(s[b]) != std::string::npos) b++;
    while (e > b && chars.find(s[e - 1]) != std::string::npos) e--;
    return s.substr(b, e - b);
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

/** Valore JSON come stringa (numeri e booleani compresi). */
std::string jstr(const json& j, const char* key) {
    if (!j.is_object() || !j.contains(key)) return "";
    const json& v = j[key];
    if (v.is_string()) return v.get<std::string>();
    if (v.is_null()) return "";
    return v.dump();
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

bool hasClass(const html::Node& n, const std::string& cls) {
    for (auto& c : split(n.attr("class"), " "))
        if (c == cls) return true;
    return false;
}

/** Fratelli successivi (elementi) di un nodo. */
std::vector<html::Node> nextSiblings(const html::Node& n) {
    std::vector<html::Node> out;
    html::Node parent = n.parent();
    if (!parent) return out;
    bool found = false;
    for (auto& c : parent.children()) {
        if (found) out.push_back(c);
        else if (c.raw() == n.raw()) found = true;
    }
    return out;
}

/** Primo id Google Drive nell'URL (Regex("[\\w-]{28,}")). */
std::string driveId(const std::string& url) {
    size_t i = 0;
    while (i < url.size()) {
        size_t e = i;
        while (e < url.size() && (std::isalnum((unsigned char)url[e]) || url[e] == '_' || url[e] == '-')) e++;
        if (e - i >= 28) return url.substr(i, e - i);
        i = e + 1;
    }
    return "";
}

// =============================================================================================== hoster

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
    std::string script = scriptWith(doc, {"sources:"});
    std::string u = substringBefore(substringAfter(script, "sources: [\""), "\"");
    if (script.empty() || !startsWith(u, "http")) {
        std::string packed = scriptWith(doc, {"eval(function(p,a,c,k,e,d)"});
        if (packed.empty()) return {};
        std::string un = unpacker::unpackAndCombine(packed);
        u = substringBefore(substringAfter(un, "sources:[\""), "\"");
        if (!startsWith(u, "http")) return {};
    }
    std::string title = prefix + "Uqload";
    if (contains(u, ".m3u8")) return hlsVideos(u, fixed, title + " - ");
    return {simpleVideo(u, title, "https://uqload.is/")};
}

// ---- MixDrop (lib/mixdropextractor)
std::vector<Video> mixDrop(const std::string& url, const std::string& prefix) {
    const std::string referer = "https://mixdrop.co/";
    http::Headers h = {{"Referer", referer}, {"User-Agent", DESKTOP_UA}};
    html::Document doc(http::getText(url, h), url);
    std::string packed = scriptWith(doc, {"eval", "MDCore"});
    if (packed.empty()) return {};
    std::string un = unpacker::unpackAndCombine(packed);
    if (un.empty() || !contains(un, "Core.wurl=\"")) return {};
    std::string videoUrl = "https:" + substringBefore(substringAfter(un, "Core.wurl=\""), "\"");
    Video v = simpleVideo(videoUrl, prefix + "MixDrop", referer);
    v.userAgent = DESKTOP_UA;
    if (contains(un, "Core.remotesub=\"")) {
        std::string sub = substringBefore(substringAfter(un, "Core.remotesub=\""), "\"");
        if (!trim(sub).empty()) v.subtitles.push_back({urlDecode(sub), "sub"});
    }
    return {v};
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
    std::string cdn = j["purchase"].value("cdnUrl", "");
    if (cdn.empty()) return {};
    return {simpleVideo(cdn, prefix + "BurstCloud", base)};
}

// ---- Sendvid (lib/sendvidextractor)
std::vector<Video> sendvid(const std::string& url, const std::string& prefix) {
    html::Document doc(http::getText(url), url);
    std::string master = doc.selectFirst("source#video_source").attr("src");
    if (master.empty()) return {};
    if (contains(master, ".m3u8")) return hlsVideos(master, url, prefix + "Sendvid:");
    std::string origin = "https://" + http::hostOf(url);
    return {simpleVideo(master, prefix + "Sendvid:default", origin + "/", {{"Origin", origin}})};
}

// ---- Vudeo (lib/vudeoextractor)
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

// ---- VK (lib/vkextractor)
std::vector<Video> vk(const std::string& url, const std::string& prefix) {
    const std::string vkUrl = "https://vk.com";
    http::Headers doch = {{"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"},
                          {"Accept-Language", "en-US,en;q=0.9"}};
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

// ---- Sibnet (lib/sibnetextractor)
std::vector<Video> sibnet(const std::string& url, const std::string& prefix) {
    html::Document doc(http::getText(url), url);
    std::string script = scriptWith(doc, {"player.src"});
    if (script.empty()) return {};
    std::string slug =
        substringBefore(substringAfter(substringAfter(substringAfter(script, "player.src"), "src:"), "\""), "\"");
    if (slug.empty()) return {};
    std::string videoUrl = contains(slug, "http") ? slug : "https://" + http::hostOf(url) + slug;
    return {simpleVideo(videoUrl, prefix + "Sibnet", url)};
}

// ---- CDA (pl/desuonline/extractors/CDAExtractor)
std::vector<Video> cda(const std::string& url, const std::string& name, const std::string& referer) {
    std::string host = http::hostOf(url);
    http::Headers dh = {{"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8"},
                        {"Referer", referer}};
    html::Document doc(http::getText(url, dh), url);
    std::string playerData;
    for (auto& d : doc.select("div[player_data]"))
        if (contains(d.attr("id"), "mediaplayer")) {
            playerData = d.attr("player_data");
            break;
        }
    json pd = json::parse(playerData, nullptr, false);
    if (!pd.is_object() || !pd.contains("api") || !pd.contains("video")) return {};
    std::string ts = substringBefore(jstr(pd["api"], "ts"), "_");
    const json& video = pd["video"];
    std::string hash2 = jstr(video, "hash2");
    if (!video.contains("qualities") || !video["qualities"].is_object()) return {};
    std::string path = substringBefore(substringBefore(http::pathOf(url), "?"), "#");
    while (!path.empty() && path.back() == '/') path.pop_back();
    std::string id = substringAfterLast(path, "/");
    http::Headers ph = {{"Accept", "application/json, text/javascript, */*; q=0.01"},
                        {"Content-Type", "application/json; charset=utf-8"},
                        {"Origin", "https://" + host},
                        {"Referer", url}};
    std::vector<Video> out;
    int counter = 1;
    for (auto it = video["qualities"].begin(); it != video["qualities"].end(); ++it) {
        std::string qualityId = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
        json body = {{"id", counter++},
                     {"jsonrpc", "2.0"},
                     {"method", "videoGetLink"},
                     {"params", {id, qualityId, std::atoi(ts.c_str()), hash2}}};
        try {
            http::Response r = http::request("POST", "https://www.cda.pl/", ph, body.dump());
            json j = json::parse(r.body, nullptr, false);
            if (!j.is_object() || !j.contains("result") || !j["result"].is_object()) continue;
            std::string videoUrl = jstr(j["result"], "resp");
            if (!startsWith(videoUrl, "http")) continue;
            out.push_back(simpleVideo(videoUrl, name + " - " + it.key(), videoUrl));
        } catch (const std::exception&) {
        }
    }
    return out;
}

// ---- Google Drive (lib/googledriveextractor): link di download diretto
std::vector<Video> googleDrive(const std::string& itemId, const std::string& videoName) {
    if (itemId.empty()) return {};
    std::string url = "https://drive.usercontent.google.com/download?id=" + itemId;
    http::Headers h = {{"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8"}};
    // HEAD al posto di peekBody: evita di scaricare l'intero video se il link e' gia' diretto
    http::Response head = http::request("HEAD", url, h, "", 20);
    if (!containsCI(head.header("content-type"), "text/html")) return {simpleVideo(url, videoName)};
    html::Document doc(http::getText(url, h), url);
    std::string size;
    html::Node sz = doc.selectFirst("span.uc-name-size");
    if (sz) size = " " + trim(ownText(sz)) + " ";
    std::string query;
    std::vector<std::pair<std::string, std::string>> params = {{"id", itemId}};
    for (auto& in : doc.select("input[type=hidden]")) {
        bool replaced = false;
        for (auto& p : params)
            if (p.first == in.attr("name")) {
                p.second = in.attr("value");
                replaced = true;
            }
        if (!replaced) params.push_back({in.attr("name"), in.attr("value")});
    }
    for (auto& p : params) query += (query.empty() ? "" : "&") + http::urlEncode(p.first) + "=" + http::urlEncode(p.second);
    std::string videoUrl = "https://drive.usercontent.google.com/download?" + query;
    return {simpleVideo(videoUrl, videoName + size)};
}

// ---- GdrivePlayer (lib/gdriveplayerextractor)
std::vector<Video> gdrivePlayer(const std::string& url, const std::string& name, const http::Headers& headers) {
    std::string newUrl = replaceAll(replaceAll(url, ".us", ".to"), ".me", ".to");
    std::string body = http::getText(newUrl, headers);
    std::vector<Video::Track> subs;
    {
        html::Document doc(body, newUrl);
        for (auto& d : doc.select("div")) {
            std::string t = trim(ownText(d));
            if (contains(t, ".srt")) {
                subs.push_back({"https://gdriveplayer.to/?subtitle=" + t, "Subtitles"});
                break;
            }
        }
    }
    std::string eval = replaceAll(unpacker::unpackAndCombine(body), "\\", "");
    if (eval.empty()) return {};
    auto dp = eval.find("data=\"");
    if (dp == std::string::npos) return {};
    std::string dataJson = substringBefore(eval.substr(dp + 6), "\";");
    // null,'12a34b' -> sequenza di codici carattere
    std::string sojson;
    for (const char* pat : {"null,'", "null,\""}) {
        auto p = eval.find(pat);
        if (p == std::string::npos) continue;
        p += 6;
        size_t e = p;
        while (e < eval.size() && (std::isalnum((unsigned char)eval[e]) || eval[e] == '_')) e++;
        sojson = eval.substr(p, e - p);
        break;
    }
    std::string decoded;
    for (size_t i = 0; i < sojson.size();) {
        if (!std::isdigit((unsigned char)sojson[i])) {
            i++;
            continue;
        }
        size_t e = i;
        while (e < sojson.size() && std::isdigit((unsigned char)sojson[e])) e++;
        int c = std::atoi(sojson.substr(i, e - i).c_str());
        if (c > 0 && c < 128) decoded += (char)c;
        i = e;
    }
    std::string password = substringBefore(substringAfter(decoded, "var pass = \""), "\"");
    if (password.empty() || password == decoded) return {};
    json dj = json::parse(dataJson, nullptr, false);
    if (!dj.is_object()) return {};
    std::string ct = jstr(dj, "ct"), salt = jstr(dj, "s");
    std::string kiv = crypto::evpBytesToKey(password, crypto::fromHex(salt), 32, 16);
    std::string decrypted = crypto::aesCbcDecrypt(base64Decode(ct), kiv.substr(0, 32), kiv.substr(32, 16));
    std::string second = replaceAll(unpacker::unpackAndCombine(decrypted), "\\", "");
    std::vector<Video> out;
    std::set<std::string> seen;
    size_t pos = 0;
    while ((pos = second.find("file\":\"", pos)) != std::string::npos) {
        pos += 7;
        auto e = second.find('"', pos);
        if (e == std::string::npos) break;
        std::string file = second.substr(pos, e - pos);
        auto rp = second.find("res=", e);
        if (rp == std::string::npos) break;
        rp += 4;
        size_t re = rp;
        while (re < second.size() && std::isdigit((unsigned char)second[re])) re++;
        std::string q = second.substr(rp, re - rp);
        pos = e;
        if (q.empty() || !seen.insert(q).second) continue;
        Video v = simpleVideo("https:" + file + "&res=" + q, "GDRIVE " + q + "p - " + name);
        v.subtitles = subs;
        out.push_back(v);
    }
    return out;
}

// ---- Mail.ru (sr/animebalkan/extractors/MailRuExtractor)
std::vector<Video> mailRu(const std::string& url) {
    html::Document doc(http::getText(url), url);
    std::string script = scriptWith(doc, {"metadataUrl"});
    if (script.empty()) return {};
    std::string metaUrl = substringBefore(substringAfter(script, "metadataUrl\":\""), "\"");
    if (startsWith(metaUrl, "//")) metaUrl = "https:" + metaUrl;
    std::string origin = "https://" + http::hostOf(url);
    http::Headers mh = {{"Referer", url}, {"Origin", origin}};
    http::Response r = http::request("GET", metaUrl, mh);
    std::string videoKey;
    auto range = r.headers.equal_range("set-cookie");
    for (auto it = range.first; it != range.second; ++it)
        if (startsWith(lower(it->second), "video_key")) {
            videoKey = substringBefore(it->second, ";");
            break;
        }
    json j = json::parse(r.body, nullptr, false);
    if (!j.is_object() || !j.contains("videos") || !j["videos"].is_array()) return {};
    std::vector<Video> out;
    for (auto& v : j["videos"]) {
        std::string u = jstr(v, "url");
        if (startsWith(u, "//")) u = "https:" + u;
        u = replaceAll(u, ".mp4", ".mp4/stream.mpd");
        Video vid = simpleVideo(u, "Mail.ru " + jstr(v, "key"), url, {{"Origin", origin}});
        vid.cookie = videoKey;
        out.push_back(vid);
    }
    return out;
}

// ---- MiniOppai / paistream (id/minioppai/extractors/MiniOppaiExtractor)
std::vector<Video> miniOppai(const std::string& url, const http::Headers& headers) {
    html::Document doc(http::getText(url, headers), url);
    std::string packed = scriptWith(doc, {"eval", "p,a,c,k,e,d"});
    if (packed.empty()) return {};
    std::string data = unpacker::unpackAndCombine(packed);
    if (trim(data).empty()) return {};
    std::string base = "https://" + substringBefore(substringAfter(url, "//"), "/");
    auto extractKey = [](const std::string& s, const std::string& key) {
        return trimChars(trimChars(substringAfter(substringBefore(substringBefore(substringAfter(s, key), "}"), ","), ":"), "\""),
                         "'");
    };
    auto items = [&](const std::string& key) {
        std::vector<std::pair<std::string, std::string>> out;
        auto parts = split(substringBefore(substringAfter(data, key + ":["), "]"), "{");
        for (size_t i = 1; i < parts.size(); i++)
            out.push_back({base + extractKey(parts[i], "file"), extractKey(parts[i], "label")});
        return out;
    };
    std::vector<Video::Track> subs;
    for (auto& t : items("\"tracks\"")) subs.push_back({t.first, t.second});
    std::string referer;
    for (auto& kv : headers)
        if (lower(kv.first) == "referer") referer = kv.second;
    std::vector<Video> out;
    for (auto& s : items("sources")) {
        if (contains(s.first, "/uploads/unavailable.mp4")) continue;
        Video v = simpleVideo(s.first, "MiniOppai - " + s.second, referer);
        v.subtitles = subs;
        out.push_back(v);
    }
    return out;
}

// ---- Strmup (pt/anikyuu/extractors/StrmupExtractor)
std::vector<Video> strmup(const std::string& url, const http::Headers& headers, const std::string& name = "Strmup") {
    std::string id = substringAfterLast(url, "/");
    std::string body = http::getText("https://strmup.to/ajax/stream?filecode=" + id, headers);
    if (!contains(body, "streaming_url")) return {};
    std::string videoUrl =
        replaceAll(substringBefore(substringAfter(substringAfter(body, "streaming_url"), ":\""), "\""), "\\", "");
    if (!startsWith(videoUrl, "http")) return {};
    return hlsVideos(videoUrl, "", name + " - ");
}

// ---- AniDrive (pt/animeito/extractors/AnimeItoExtractor), senza WebView e senza m3u8server
std::string anidriveDecode(const std::string& script) {
    std::string main = substringBefore(script, "})();");
    auto start = main.find("([\"");
    auto end = main.rfind("\");");
    if (start == std::string::npos || end == std::string::npos || end <= start + 1) return "";
    std::string params = main.substr(start + 2, end + 1 - (start + 2));
    // prima occorrenza di "],<spazi>[" e di "],<spazi>\""
    auto findSep = [&](char next) -> size_t {
        size_t p = 0;
        while ((p = params.find("],", p)) != std::string::npos) {
            size_t q = p + 2;
            while (q < params.size() && std::isspace((unsigned char)params[q])) q++;
            if (q < params.size() && params[q] == next) return p;
            p += 2;
        }
        return std::string::npos;
    };
    size_t a1 = findSep('['), a2 = findSep('"');
    if (a1 == std::string::npos || a2 == std::string::npos) return "";
    std::string first = params.substr(0, a1 + 1);
    size_t open2 = params.find('[', a1 + 1);
    if (open2 == std::string::npos || open2 >= a2) return "";
    std::string second = params.substr(open2, a2 + 1 - open2);
    size_t kq = params.find('"', a2 + 1);
    if (kq == std::string::npos) return "";
    size_t kq2 = params.find('"', kq + 1);
    if (kq2 == std::string::npos) return "";
    std::string keyStr = params.substr(kq + 1, kq2 - kq - 1);
    std::vector<std::string> strings;
    for (size_t p = 0; (p = first.find('"', p)) != std::string::npos;) {
        size_t e = first.find('"', p + 1);
        if (e == std::string::npos) break;
        strings.push_back(first.substr(p + 1, e - p - 1));
        p = e + 1;
    }
    std::string joined;
    for (auto& tok : split(trimChars(second, "[]"), ",")) {
        std::string t = trim(tok);
        if (t.empty() || !std::all_of(t.begin(), t.end(), [](char c) { return std::isdigit((unsigned char)c); })) continue;
        size_t idx = (size_t)std::atol(t.c_str());
        if (idx < strings.size()) joined += strings[idx];
    }
    if (joined.empty()) return "";
    std::string data = base64Decode(joined), key = base64Decode(keyStr);
    if (data.empty() || key.empty()) return "";
    for (size_t i = 0; i < data.size(); i++) data[i] = (char)(data[i] ^ key[i % key.size()]);
    return data;
}

std::vector<Video> anidriveFromText(const std::string& text, const std::string& pageUrl, const std::string& prefix) {
    std::string origin = http::originOf(pageUrl);
    std::vector<Video> out;
    const std::string fk = "\"file\":\"", lk = "\",\"label\":\"";
    size_t pos = 0;
    while ((pos = text.find(fk, pos)) != std::string::npos) {
        pos += fk.size();
        size_t e = text.find('"', pos);
        if (e == std::string::npos) break;
        if (text.compare(e, lk.size(), lk) != 0) continue;
        std::string u = replaceAll(text.substr(pos, e - pos), "\\/", "/");
        size_t ls = e + lk.size(), le = text.find('"', ls);
        if (le == std::string::npos) break;
        std::string label = text.substr(ls, le - ls);
        pos = le;
        if (startsWith(u, "//")) u = "https:" + u;
        if (!contains(u, "videoplayback") && !contains(u, ".m3u8") && !contains(u, ".mp4")) continue;
        if (contains(u, ".m3u8")) append(out, hlsVideos(u, pageUrl, prefix + " - "));
        else out.push_back(simpleVideo(u, prefix + " - " + label, origin + "/", {{"Origin", origin}}));
    }
    return out;
}

std::vector<Video> anidriveFromScript(const std::string& script, const std::string& pageUrl, const std::string& prefix) {
    const std::string marker = "\"sources\":[";
    auto m = script.find(marker);
    if (m != std::string::npos) {
        size_t s = m + marker.size() - 1;
        int depth = 0;
        for (size_t i = s; i < script.size(); i++) {
            if (script[i] == '[') depth++;
            else if (script[i] == ']' && --depth == 0) {
                auto v = anidriveFromText(script.substr(s, i + 1 - s), pageUrl, prefix);
                if (!v.empty()) return v;
                break;
            }
        }
    }
    if (contains(script, "videoplayback")) return anidriveFromText(script, pageUrl, prefix);
    size_t pos = 0;
    const std::string fk = "\"file\":\"";
    while ((pos = script.find(fk, pos)) != std::string::npos) {
        pos += fk.size();
        size_t e = script.find('"', pos);
        if (e == std::string::npos) break;
        std::string u = replaceAll(script.substr(pos, e - pos), "\\/", "/");
        if ((startsWith(u, "http") || startsWith(u, "//")) && contains(u, ".m3u8")) {
            if (startsWith(u, "//")) u = "https:" + u;
            return hlsVideos(u, pageUrl, prefix + " - ");
        }
    }
    return {};
}

std::vector<Video> anidrive(const std::string& url, const std::string& serverName, const http::Headers& headers) {
    std::string prefix = trim(serverName).empty() ? "Animei.to" : "Animei.to " + trim(serverName);
    html::Document doc(http::getText(url, headers), url);
    for (auto& s : doc.select("script")) {
        std::string d = s.data();
        if (!contains(d, "TextDecoder")) continue;
        std::string decoded = anidriveDecode(d);
        if (decoded.empty()) continue;
        auto v = anidriveFromScript(decoded, url, prefix);
        if (!v.empty()) return v;
    }
    std::string fallback = scriptWith(doc, {"AniDrivePlayerConfig"});
    if (fallback.empty()) fallback = scriptWith(doc, {"const player"});
    if (!fallback.empty()) return anidriveFromScript(fallback, url, prefix);
    return {};  // WebView non disponibile
}

// ---- SmartAnimes / soralink (pt/smartanimes/extractors/SmartAnimesExtractor)
std::vector<Video> soralink(const std::string& url, const std::string& name, const http::Headers& headers) {
    std::string content = http::getText(url, headers);
    if (!contains(content, "var item = ") || !contains(content, "var options = ")) return {};
    json item = json::parse(substringBefore(substringAfter(content, "var item = "), ";"), nullptr, false);
    json options = json::parse(substringBefore(substringAfter(content, "var options = "), ";"), nullptr, false);
    if (!item.is_object() || !options.is_object()) return {};
    std::string ajax = jstr(options, "soralink_ajaxurl");
    if (ajax.empty()) return {};
    http::Headers ph;
    for (auto& kv : headers)
        if (lower(kv.first) != "referer") ph.push_back(kv);
    ph.push_back({"Referer", jstr(item, "post")});
    ph.push_back({"Content-Type", "application/x-www-form-urlencoded"});
    std::string form;
    auto add = [&](const std::string& k, const std::string& v) {
        form += (form.empty() ? "" : "&") + k + "=" + http::urlEncode(v);
    };
    add("token", jstr(item, "token"));
    add("id", jstr(item, "id"));
    add("time", jstr(item, "time"));
    add("post", jstr(item, "post"));
    add("redirect", jstr(item, "redirect"));
    add("cacha", jstr(item, "cacha"));
    add("new", "false");
    add("link", jstr(item, "link"));
    add("action", jstr(options, "soralink_z"));
    http::Response r = http::request("POST", ajax, ph, form, 30, false);
    std::string source = r.header("location");
    if (source.empty()) return {};
    // Google Drive: il player via WebView non e' disponibile, si usa il link di download diretto
    if (contains(source, "drive.google.com")) return googleDrive(driveId(source), name);
    return {};  // send.now richiede Cloudflare Turnstile (WebView)
}

// =============================================================================================== tema

enum class Site {
    Animenosub,
    AnimeKhor,
    LuciferDonghua,
    DonghuaStream,
    AnimeXin,
    ChineseAnime,
    LMAnime,
    DesuOnline,
    AnimeYTES,
    Tiodonghua,
    MyKdrama,
    AnimeIndo,
    MiniOppai,
    AnimeBalkan,
    AsyaAnimeleri,
    TRAnimeCI,
    Anikyuu,
    AnimeIto,
    SmartAnimes,
};

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
        std::string p = std::to_string(page);
        switch (info.site) {
            case Site::AnimeIndo: return listPage(baseUrl() + "/browse?sort=view&page=" + p, "");
            case Site::MiniOppai: return listPage(animeListUrl() + "/page/" + p + "/?order=popular", listNextSelector());
            case Site::TRAnimeCI: return trPopular();
            default: return listPage(animeListUrl() + "/?page=" + p + "&order=popular", listNextSelector());
        }
    }

    Page latest(int page) override {
        std::string p = std::to_string(page);
        switch (info.site) {
            case Site::AnimeIndo: return listPage(baseUrl() + "/browse?sort=created_at&page=" + p, "");
            case Site::MiniOppai: return listPage(animeListUrl() + "/page/" + p + "/?order=update", listNextSelector());
            case Site::TRAnimeCI: return trLatest(page);
            default: return listPage(animeListUrl() + "/?page=" + p + "&order=update", listNextSelector());
        }
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
        std::string p = std::to_string(page);
        if (info.site == Site::AnimeIndo) {
            std::string url = baseUrl() + "/browse?page=" + p;
            if (!query.empty()) url += "&title=" + http::urlEncode(query);
            return listPage(url, "");
        }
        if (info.site == Site::TRAnimeCI)
            return listPage(animeListUrl() + (query.empty() ? "" : "?name=" + http::urlEncode(query)), "");
        if (query.empty()) {
            switch (info.site) {
                case Site::MiniOppai: return listPage(animeListUrl() + "/page/" + p + "/?order=", searchNextSelector());
                case Site::MyKdrama:
                case Site::AsyaAnimeleri: return listPage(animeListUrl() + "/?page=" + p, searchNextSelector());
                default:
                    return listPage(animeListUrl() + "/?page=" + p + "&status=&type=&sub=&order=", searchNextSelector());
            }
        }
        std::string url = info.site == Site::DonghuaStream
                              ? baseUrl() + "/pagg/" + p + "/?s=" + http::urlEncode(query)
                              : baseUrl() + "/page/" + p + "/?s=" + http::urlEncode(query);
        return listPage(url, searchNextSelector());
    }

    Details details(const std::string& animeUrl) override {
        std::string pageUrl = absolute(animeUrl);
        html::Document doc(fetch(pageUrl), pageUrl);
        Details d;
        d.title = doc.selectFirst(info.site == Site::TRAnimeCI ? ".entry-title" : "h1.entry-title").text();
        if (info.site == Site::MiniOppai) d.title = trim(substringBefore(substringBefore(d.title, "Episode"), " OVA "));
        if (d.title.empty()) throw http::Error("Pagina dell'anime non valida");
        html::Node img = doc.selectFirst("div.thumb > img, div.limage > img");
        if (img) d.thumbnail = imageUrl(doc, img);

        html::Node infos = doc.selectFirst(info.site == Site::TRAnimeCI ? "div.infox" : "div.info-content, div.right ul.data");
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

        bool turkish = info.site == Site::AsyaAnimeleri || info.site == Site::TRAnimeCI;
        d.status = parseStatus(lower(trim(getInfo(infos, turkish ? "Durum" : "Status"))));
        std::string fansub = trim(getInfo(infos, "Fansub"));
        std::string studio = trim(getInfo(infos, isPortuguese() ? "Estudio" : "Studio"));
        d.author = !fansub.empty() ? fansub : studio;

        std::string desc;
        if (info.site == Site::MiniOppai) {
            std::string text;
            for (auto& p : doc.select("div.entry-content > p")) text += (text.empty() ? "" : "\n") + p.text();
            desc += text + "\n\n";
        } else {
            std::string descSelector = info.site == Site::ChineseAnime  ? ".entry-content"
                                       : info.site == Site::SmartAnimes ? ".entry-content[itemprop=description]"
                                                                        : ".entry-content[itemprop=description], .desc";
            auto descNodes = doc.select(descSelector);
            if (!descNodes.empty()) {
                std::string text = descNodes.back().text();
                if (info.site == Site::AnimeXin) text = animeXinDescription(text);
                desc += text + "\n\n";
            }
        }
        std::string alt = doc.selectFirst(".alter").text();
        if (!trim(alt).empty()) desc += (isPortuguese() ? "Nome(s) alternativo(s): " : "Alternative name(s): ") + alt + "\n";
        for (auto& sp : infos.select("div.spe > span")) desc += sp.text() + "\n";
        for (auto& li : infos.select("li"))
            if (li.selectFirst("b").valid()) desc += li.text() + "\n";
        d.description = trim(desc);

        d.episodes = episodes(doc);
        return d;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::string pageUrl = absolute(episodeUrl);
        html::Document doc(fetch(pageUrl), pageUrl);
        std::vector<Video> out;
        if (info.site == Site::TRAnimeCI) {
            out = trVideos(doc);
        } else if (info.site == Site::SmartAnimes) {
            for (auto& el : doc.select(".dlbox li:not(.head)")) {
                try {
                    std::string name = trim(el.selectFirst(".q").text()) + " - " + trim(el.selectFirst(".w").text());
                    std::string url = el.selectFirst("a").attr("href");
                    if (url.empty()) continue;
                    append(out, getVideoList(url, name));
                } catch (const std::exception&) {
                }
            }
        } else {
            static const char* allowedLangs[] = {"English", "Español", "Indonesian", "Portugués", "Türkçe", "العَرَبِيَّة", "ไทย"};
            std::string selector = info.site == Site::AnimeIto ? "ul.tabs_videos li" : "select.mirror > option[data-index], ul.mirror a[data-em]";
            for (auto& el : doc.select(selector)) {
                std::string name = el.text();
                if (info.site == Site::LMAnime) {
                    bool ok = false;
                    for (auto* l : allowedLangs)
                        if (contains(name, l)) ok = true;
                    if (!ok) continue;
                    name = substringBefore(name, " ");
                }
                try {
                    std::string encoded = (el.tag() == "option" || info.site == Site::AnimeIto) ? el.attr("value") : el.attr("data-em");
                    std::string hosterUrl = info.site == Site::AnimeBalkan && contains(name, "Server AB")
                                                ? el.attr("value")
                                                : getHosterUrl(encoded);
                    if (hosterUrl.empty()) continue;
                    append(out, getVideoList(hosterUrl, name));
                } catch (const std::exception&) {
                    // hoster non disponibile: si passa al successivo
                }
            }
            if (out.empty() && info.site == Site::MyKdrama) {
                for (auto& el : doc.select(".gov-the-embed")) {
                    try {
                        std::string page2 = substringBefore(substringAfter(el.attr("onclick"), "'"), "'");
                        if (!startsWith(page2, "http")) continue;
                        html::Document d2(fetch(page2), page2);
                        std::string url = d2.selectFirst("#pembed iframe").attr("src");
                        if (startsWith(url, "//")) url = "https:" + url;
                        if (url.empty()) continue;
                        append(out, getVideoList(url, el.text()));
                    } catch (const std::exception&) {
                    }
                }
            }
        }
        if (out.empty()) throw http::Error("Nessun video trovato");
        sortVideos(out);
        return out;
    }

    http::Headers imageHeaders() const override {
        http::Headers h = {{"Referer", baseUrl() + "/"}};
        std::string c = cookie();
        if (!c.empty()) h.push_back({"Cookie", c});
        return h;
    }

  private:
    SiteInfo info;
    mutable std::mutex cookieMutex;
    std::string protectionCookie;  // cookie della protezione "slowAES" (siti turchi)

    bool isPortuguese() const { return std::string(info.lang) == "pt"; }

    std::string cookie() const {
        std::lock_guard<std::mutex> lock(cookieMutex);
        return protectionCookie;
    }

    std::string animeListUrl() const {
        switch (info.site) {
            case Site::AnimeYTES: return baseUrl() + "/tv";
            case Site::MyKdrama: return baseUrl() + "/drama";
            case Site::AnimeIndo: return baseUrl() + "/browse";
            case Site::MiniOppai: return baseUrl() + "/advanced-search";
            case Site::AnimeBalkan: return baseUrl() + "/animesaprevodom";
            case Site::AsyaAnimeleri: return baseUrl() + "/series";
            case Site::TRAnimeCI: return baseUrl() + "/search";
            default: return baseUrl() + "/anime";
        }
    }

    /** Intestazioni aggiunte dalle estensioni (headersBuilder). */
    http::Headers siteHeaders() const {
        switch (info.site) {
            case Site::MiniOppai:
            case Site::Anikyuu: return {{"Referer", baseUrl()}};
            case Site::TRAnimeCI: return {{"Referer", baseUrl() + "/"}};
            case Site::SmartAnimes:
                return {{"Accept",
                         "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8,"
                         "application/signed-exchange;v=b3;q=0.7"},
                        {"Referer", baseUrl() + "/"},
                        {"Accept-Language", "pt-BR,pt;q=0.9,en-US;q=0.8,en;q=0.7"}};
            default: return {};
        }
    }

    /** GET di una pagina del sito; per i siti turchi gestisce la protezione "slowAES" (ShittyProtectionInterceptor). */
    std::string fetch(const std::string& url) {
        http::Headers h = siteHeaders();
        if (info.site != Site::AsyaAnimeleri && info.site != Site::TRAnimeCI) return http::getText(url, h);
        std::string c = cookie();
        http::Headers h1 = h;
        if (!c.empty()) h1.push_back({"Cookie", c});
        http::Response r = http::request("GET", url, h1);
        if (r.status == 202 || (contains(r.body, "slowAES") && contains(r.body, "toNumbers("))) {
            std::string solved = solveSlowAes(r.body);
            if (!solved.empty()) {
                {
                    std::lock_guard<std::mutex> lock(cookieMutex);
                    protectionCookie = solved;
                }
                http::Headers h2 = h;
                h2.push_back({"Cookie", solved});
                r = http::request("GET", url, h2);
            }
        }
        if (r.status < 200 || r.status >= 300) throw http::Error("Errore HTTP " + std::to_string(r.status));
        return r.body;
    }

    /** a=toNumbers(key), b=toNumbers(iv), c=toNumbers(dati); cookie = toHex(slowAES.decrypt(c, CBC, a, b)). */
    static std::string solveSlowAes(const std::string& body) {
        std::vector<std::string> nums;
        size_t pos = 0;
        while (nums.size() < 3 && (pos = body.find("toNumbers(\"", pos)) != std::string::npos) {
            pos += 11;
            auto e = body.find('"', pos);
            if (e == std::string::npos) break;
            nums.push_back(body.substr(pos, e - pos));
            pos = e;
        }
        if (nums.size() < 3) return "";
        std::string cp = body.find("document.cookie=\"") != std::string::npos ? "document.cookie=\"" : "document.cookie = \"";
        auto np = body.find(cp);
        if (np == std::string::npos) return "";
        std::string name = substringBefore(body.substr(np + cp.size()), "=");
        if (name.empty() || name.size() > 64) return "";
        try {
            std::string dec = crypto::aesCbcDecrypt(crypto::fromHex(nums[2]), crypto::fromHex(nums[0]), crypto::fromHex(nums[1]), false);
            return name + "=" + crypto::toHex(dec);
        } catch (const std::exception&) {
            return "";
        }
    }

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

    std::string itemSelector() const {
        switch (info.site) {
            case Site::AnimeIndo: return "div.animepost > div > a";
            case Site::MiniOppai: return "div.latest article a.tip";
            case Site::TRAnimeCI: return "div.advancedsearch a.tip";
            default: return "div.listupd article a.tip";
        }
    }

    std::string imageUrl(const html::Document& doc, const html::Node& img) const {
        std::string u;
        if (img.hasAttr("data-src")) u = doc.absUrl(img, "data-src");
        else if (img.hasAttr("data-lazy-src")) u = doc.absUrl(img, "data-lazy-src");
        else if (img.hasAttr("srcset")) u = http::resolve(doc.url(), substringBefore(trim(img.attr("srcset")), " "));
        else u = doc.absUrl(img, "src");
        // AsyaAnimeleri: senza "?resize" alcune immagini non si caricano
        return info.site == Site::AsyaAnimeleri ? u : substringBefore(u, "?resize");
    }

    Anime parseItem(const html::Document& doc, const html::Node& a) const {
        Anime an;
        an.url = http::pathOf(doc.absUrl(a, "href"));
        if (info.site == Site::AnimeIndo) {
            an.title = a.selectFirst("div.title").text();
        } else if (info.site == Site::MiniOppai) {
            an.title = ownText(a.selectFirst("h2.entry-title"));
        } else {
            html::Node tt = a.selectFirst("div.tt, div.ttl");
            an.title = ownText(tt);
            if (an.title.empty()) an.title = tt.selectFirst("h2").text();
            if (an.title.empty()) an.title = a.attr("title");
        }
        html::Node img = a.selectFirst("img");
        if (img) an.thumbnail = imageUrl(doc, img);
        return an;
    }

    Page listPage(const std::string& url, const std::string& nextSelector) {
        html::Document doc(fetch(url), url);
        Page p;
        for (auto& a : doc.select(itemSelector())) {
            Anime an = parseItem(doc, a);
            if (!an.url.empty() && !an.title.empty()) p.animes.push_back(an);
        }
        if (info.site == Site::AnimeIndo) {
            // "div.pagination a:has(i#nextpagination)"
            for (auto& a : doc.select("div.pagination a"))
                if (a.selectFirst("i#nextpagination").valid()) p.hasNextPage = true;
        } else if (!nextSelector.empty()) {
            p.hasNextPage = doc.selectFirst(nextSelector).valid();
        }
        return p;
    }

    /** Blocchi "div.releases" il cui testo contiene la stringa indicata. */
    static std::vector<html::Node> releases(const html::Document& doc, const std::string& text) {
        std::vector<html::Node> out;
        for (auto& r : doc.select("div.releases"))
            if (containsCI(r.text(), text)) out.push_back(r);
        return out;
    }

    /** TRAnimeCI: "div.releases:contains(Populer) + div.listupd a.tip" dalla home. */
    Page trPopular() {
        std::string url = baseUrl();
        html::Document doc(fetch(url), url);
        Page p;
        for (auto& r : releases(doc, "Populer")) {
            auto sib = nextSiblings(r);
            if (sib.empty() || sib[0].tag() != "div" || !hasClass(sib[0], "listupd")) continue;
            for (auto& a : sib[0].select("a.tip")) {
                Anime an = parseItem(doc, a);
                if (!an.url.empty() && !an.title.empty()) p.animes.push_back(an);
            }
        }
        return p;
    }

    /** TRAnimeCI: "div.releases:contains(Son Güncellenenler) ~ div.listupd a.tip" (link di episodi -> serie). */
    Page trLatest(int page) {
        std::string url = baseUrl() + "/index?page=" + std::to_string(page);
        html::Document doc(fetch(url), url);
        Page p;
        std::set<std::string> seen;
        for (auto& r : releases(doc, "Son Güncellenenler")) {
            for (auto& sib : nextSiblings(r)) {
                if (sib.tag() != "div" || !hasClass(sib, "listupd")) continue;
                for (auto& a : sib.select("a.tip")) {
                    Anime an = parseItem(doc, a);
                    std::string u = replaceAll("/series" + an.url, "/video", "");
                    an.url = substringBeforeLast(substringBefore(u, "-bolum"), "-");
                    if (an.title.empty() || !seen.insert(an.url + "|" + an.title).second) continue;
                    p.animes.push_back(an);
                }
            }
        }
        // "div.hpage > a:last-child[href]"
        for (auto& hp : doc.select("div.hpage")) {
            auto ch = hp.children();
            if (!ch.empty() && ch.back().tag() == "a" && ch.back().hasAttr("href")) p.hasNextPage = true;
        }
        return p;
    }

    /** Element.getInfo: primo span che contiene il testo -> testo del link oppure testo proprio. */
    std::string getInfo(const html::Node& infos, const std::string& text) const {
        if (info.site == Site::MiniOppai) {
            // li:has(b:contains(text)) > span.colspan
            for (auto& li : infos.select("li")) {
                bool has = false;
                for (auto& b : li.select("b"))
                    if (containsCI(b.text(), text)) has = true;
                if (has) return li.selectFirst("span.colspan").text();
            }
            return "";
        }
        for (auto& sp : infos.select("span")) {
            if (!containsCI(sp.text(), text)) continue;
            html::Node a = sp.selectFirst("a");
            return a.valid() ? a.text() : ownText(sp);
        }
        return "";
    }

    std::string parseStatus(const std::string& status) const {
        switch (info.site) {
            case Site::AnimeIndo:
                if (status == "finished airing") return "Completato";
                if (status == "currently airing") return "In corso";
                return "";
            case Site::AsyaAnimeleri:
            case Site::TRAnimeCI:
                if (status == "tamamlandı" || status == "tamamlandi") return "Completato";
                if (status == "devam ediyor") return "In corso";
                return "";
            case Site::SmartAnimes:
                if (status == "completo") return "Completato";
                if (status == "em lançamento") return "In corso";
                return "";
            default:
                if (status == "completed" || status == "completo") return "Completato";
                if (status == "ongoing" || status == "lançamento") return "In corso";
                return "";
        }
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

    std::string episodePrefix() const {
        if (isPortuguese()) return "Episódio";
        if (info.site == Site::AsyaAnimeleri) return "Bölüm";
        return "Episode";
    }

    std::vector<Episode> episodes(const html::Document& doc) {
        std::vector<Episode> out;
        if (info.site == Site::AnimeIndo) {
            // "div.listeps li:has(.epsleft)"
            for (auto& li : doc.select("div.listeps li")) {
                if (!li.selectFirst(".epsleft")) continue;
                html::Node a = li.selectFirst("a");
                if (!a) continue;
                Episode e;
                e.url = http::pathOf(doc.absUrl(a, "href"));
                std::string num = a.text();
                e.name = "Episode " + num;
                e.number = parseNumber(num, 0);
                out.push_back(e);
            }
            return out;
        }
        if (info.site == Site::MiniOppai) {
            for (auto& a : doc.select("div.epsdlist > ul > li > a")) {
                html::Node numNode = a.selectFirst(".epl-num");
                if (!numNode) continue;
                std::string t = numNode.text();
                std::string num = substringAfterLast(t, " ");
                Episode e;
                e.url = http::pathOf(doc.absUrl(a, "href"));
                e.name = (containsCI(t, "OVA") ? "OVA " : "Episode ") + num;
                e.number = parseNumber(num, 0);
                out.push_back(e);
            }
            return out;
        }
        if (info.site == Site::TRAnimeCI) {
            for (auto& a : doc.select("div.eplister > ul > li > a")) {
                std::string t = substringBefore(substringBefore(a.selectFirst(".epl-title").text(), "."), " ");
                int n = 1;
                if (!t.empty() && std::all_of(t.begin(), t.end(), [](char c) { return std::isdigit((unsigned char)c); }))
                    n = std::atoi(t.c_str());
                Episode e;
                e.url = http::pathOf(doc.absUrl(a, "href"));
                e.name = "Bölüm " + std::to_string(n);
                e.number = n;
                out.push_back(e);
            }
            std::reverse(out.begin(), out.end());
            return out;
        }

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
                ep.e.name = episodePrefix() + " " + epNum;
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
        for (auto& ep : list) {
            if (info.site == Site::DonghuaStream && containsCI(ep.e.name, "Preview")) continue;
            out.push_back(ep.e);
        }
        return out;
    }

    /** TRAnimeCI: sorgenti nello script "let video_source = [{name, url}, ...]". */
    std::vector<Video> trVideos(const html::Document& doc) {
        std::string script = scriptWith(doc, {"let video_source"});
        if (script.empty()) return {};
        std::vector<Video> out;
        auto parts = split(substringBefore(substringAfter(script, "["), "]"), "{");
        for (size_t i = 1; i < parts.size(); i++) {
            std::string quality = substringBefore(substringAfter(parts[i], "name\":\""), "\"");
            std::string url = replaceAll(substringBefore(substringAfter(parts[i], "url\":\""), "\""), "\\/", "/");
            if (!startsWith(url, "http")) continue;
            Video v = simpleVideo(url, quality, baseUrl() + "/");
            if (contains(url, ".m3u8")) {
                auto h = hlsVideos(url, baseUrl() + "/", quality + " - ");
                if (!h.empty()) {
                    append(out, h);
                    continue;
                }
            }
            out.push_back(v);
        }
        return out;
    }

    /** getHosterUrl: dato base64 con l'HTML dell'iframe, oppure URL di una pagina da scaricare. */
    std::string getHosterUrl(const std::string& encodedData) {
        std::string data = trim(encodedData);
        if (data.empty()) return "";
        bool isUrl = startsWith(data, "http://") || startsWith(data, "https://");
        std::string docUrl = isUrl ? data : "";
        std::string htmlText = isUrl ? fetch(data) : base64Decode(data);
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
            case Site::DesuOnline:
                if (contains(url, "ok.ru")) return okru(url, prefix);
                if (contains(url, "cda.pl")) return cda(url, name, site + "/");
                if (contains(url, "sibnet")) return sibnet(url, prefix);
                if (contains(url, "drive.google.com")) return googleDrive(driveId(url), name);
                break;
            case Site::AnimeYTES:
                if (name == "OK") return okru(url, prefix);
                if (name == "Stream") return streamtape(url, prefix);
                if (name == "Send") return sendvid(url, prefix);
                if (name == "Your") return yourUpload(url, prefix);
                if (name == "Alpha") return burstCloud(url, prefix);
                if (name == "Moon") return moon(url, site, prefix);
                return extraHoster(url, prefix);  // UniversalExtractor (WebView) -> riconoscimento per URL
            case Site::Tiodonghua:
                if (name == "Okru") return okru(url, prefix);
                if (name == "Voe") return voe(url, prefix);
                if (name == "YourUpload") return yourUpload(url, prefix);
                if (name == "MixDrop") return mixDrop(url, prefix);
                return {};
            case Site::MyKdrama:
                if (contains(url, "ok.ru")) return okru(url, prefix);
                if (contains(url, "uqload")) return uqload(url, prefix);
                if (contains(url, "dood")) return dood(url, prefix);
                if (contains(url, "vudeo")) return vudeo(url, prefix);
                return {};
            case Site::AnimeIndo:
                if (containsCI(name, "streamtape")) return streamtape(url, prefix);
                if (containsCI(name, "mp4")) return mp4upload(url, prefix);
                if (containsCI(name, "yourupload")) return yourUpload(url, prefix);
                if (contains(url, "ok.ru")) return okru(url, prefix);
                if (containsCI(name, "gdrive")) {
                    std::string gdriveUrl = contains(url, site) ? "https:" + http::queryParam(url, "data") : url;
                    return gdrivePlayer(gdriveUrl, "Gdrive", siteHeaders());
                }
                return {};
            case Site::MiniOppai:
                if (contains(url, "gdriveplayer")) {
                    std::string data = http::queryParam(url, "data");
                    if (data.empty()) return {};
                    return gdrivePlayer((startsWith(data, "//") ? "https:" : "") + data, name, siteHeaders());
                }
                if (contains(url, "paistream.my.id")) return miniOppai(url, siteHeaders());
                return {};
            case Site::AnimeBalkan:
                if (contains(name, "Server OK") || contains(url, "ok.ru")) return okru(url, prefix);
                if (contains(name, "Server Ru") || contains(url, "mail.ru")) return mailRu(url);
                if (contains(name, "Server GD") || contains(url, "google.com")) return googleDrive(driveId(url), "Video");
                if (contains(name, "Server AB") && contains(url, site)) {
                    html::Document doc(fetch(url), url);
                    std::string videoUrl = doc.selectFirst("source").attr("src");
                    if (videoUrl.empty()) return {};
                    return {simpleVideo(http::resolve(url, videoUrl), "Server AB - Default", site + "/")};
                }
                return {};
            case Site::AsyaAnimeleri: {
                std::string n = lower(trim(name));
                if (n == "vk") return vk(url, prefix);
                if (n == "ok.ru") return okru(url, prefix);
                if (n == "sibnet") return sibnet(url, prefix);
                if (n == "dood" || n == "doodstream") return dood(url, prefix);
                if (n == "gdrive") return gdrivePlayer("https://gdriveplayer.to/embed2.php?link=" + url, "Gdrive", {});
                return {};
            }
            case Site::Anikyuu:
                // filemoon e byse usano la stessa API "embed/playback" (MoonExtractor condiviso)
                if (contains(url, "filemoon")) return moon(url, site, "Filemoon - ");
                if (contains(url, "strmup.to")) return strmup(url, siteHeaders());
                if (contains(url, "byselapuix.com")) return moon(url, site, "Byse - ");
                return {};
            case Site::AnimeIto:
                if (contains(url, "anidrive.click")) return anidrive(url, trim(name), siteHeaders());
                return {};
            case Site::SmartAnimes:
                if (contains(url, http::hostOf(site))) return soralink(url, name, siteHeaders());
                return {};
            case Site::TRAnimeCI: return {};
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

    /** genericHoster + hoster aggiuntivi (solo per i siti non inglesi). */
    std::vector<Video> extraHoster(const std::string& url, const std::string& prefix) {
        std::string u = lower(url);
        if (contains(u, "yourupload")) return yourUpload(url, prefix);
        if (contains(u, "sendvid")) return sendvid(url, prefix);
        if (contains(u, "burstcloud")) return burstCloud(url, prefix);
        if (any_of_hosts(u, {"mixdrop", "mxdrop", "mixdrp"})) return mixDrop(url, prefix);
        if (contains(u, "uqload")) return uqload(url, prefix);
        if (contains(u, "vudeo")) return vudeo(url, prefix);
        return genericHoster(url, prefix);
    }

    static bool any_of_hosts(const std::string& u, std::initializer_list<const char*> keys) {
        for (auto* k : keys)
            if (contains(u, k)) return true;
        return false;
    }

    /** Ordinamento come sortVideos() con le preferenze predefinite. */
    void sortVideos(std::vector<Video>& list) const {
        std::vector<std::string> prefs;  // in ordine di priorita'
        switch (info.site) {
            case Site::Animenosub: prefs = {"SUB", "720p", "Moon"}; break;
            case Site::AnimeXin:
            case Site::ChineseAnime: prefs = {"720p", "All Sub"}; break;
            case Site::LMAnime: prefs = {"720p", "English"}; break;
            case Site::DesuOnline: prefs = {"CDA", "720p"}; break;
            case Site::AnimeYTES: prefs = {"Amazon", "1080"}; break;
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
        {Site::DesuOnline, "pl.desuonline", "desu-online", "https://desu-online.pl", "pl", false},
        {Site::AnimeYTES, "es.animeytes", "AnimeYT.es", "https://animeyt.es", "es", false},
        {Site::Tiodonghua, "es.tiodonghua", "Tiodonghua.com", "https://anime.tiodonghua.com", "es", false},
        {Site::MyKdrama, "fr.mykdrama", "MyKdrama", "https://mykdrama.co", "fr", false},
        {Site::AnimeIndo, "id.animeindo", "AnimeIndo", "https://animeindo.skin", "id", false},
        {Site::MiniOppai, "id.minioppai", "MiniOppai", "https://minioppai.org", "id", true},
        {Site::AnimeBalkan, "sr.animebalkan", "AnimeBalkan", "https://animebalkan.org", "sr", false},
        {Site::AsyaAnimeleri, "tr.asyaanimeleri", "AsyaAnimeleri", "https://asyaanimeleri.top", "tr", false},
        {Site::TRAnimeCI, "tr.tranimeci", "TRAnimeCI", "https://tranimaci.com", "tr", false},
        {Site::Anikyuu, "pt.anikyuu", "Anikyuu", "https://anikyuu.to", "pt", false},
        {Site::AnimeIto, "pt.animeito", "Animeito", "https://animesonline.io", "pt", false},
        {Site::SmartAnimes, "pt.smartanimes", "SmartAnimes", "https://smartanimes.net", "pt", true},
    };
    std::vector<std::shared_ptr<Source>> out;
    for (auto& s : sites) out.push_back(std::make_shared<AnimeStream>(s));
    return out;
}

}  // namespace src
