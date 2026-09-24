// Porting in C++ delle estensioni Aniyomi portoghesi (src/pt/*) che non usano i temi animestream/dooplay:
// Anime Fire, Anitube, Goyabu, Animes Online Vip, Animes Digital, AnimeCore (animesotaku), Sushi Animes,
// Dattebayo BR, Meus Animes, Animes CX, Fun Anime TV, Donghua no Sekai, Doramogo, Animes Games, Muito Hentai.
// Estrattori privati (non presenti in extractors.hpp): Blogger (stream + RPC batchexecute), Google Drive, MediaFire.

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <map>
#include <cstdlib>
#include <ctime>
#include <memory>
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
using ojson = nlohmann::ordered_json;
using namespace ext;
using DocPtr = std::unique_ptr<html::Document>;

const char* ACCEPT_LANGUAGE = "pt-BR,pt;q=0.9,en-US;q=0.8,en;q=0.7";

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

std::string substringAfterLast(const std::string& s, const std::string& d) {
    auto p = s.rfind(d);
    return p == std::string::npos ? s : s.substr(p + d.size());
}

std::string substringBeforeLast(const std::string& s, const std::string& d) {
    auto p = s.rfind(d);
    return p == std::string::npos ? s : s.substr(0, p);
}

/** Come toFloatOrNull() di Kotlin: l'intera stringa deve essere un numero. */
double kNum(const std::string& s, double def) {
    std::string t = trim(s);
    if (t.empty()) return def;
    char* end = nullptr;
    double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || *end != '\0') return def;
    return v;
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

std::string qp(const std::string& url, const std::string& name) { return urlDecode(http::queryParam(url, name)); }

std::string asciiLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string reversed(std::string s) {
    std::reverse(s.begin(), s.end());
    return s;
}

bool hasClass(const html::Node& n, const std::string& cls) {
    std::string c = " " + n.attr("class") + " ";
    for (auto& ch : c)
        if (std::isspace((unsigned char)ch)) ch = ' ';
    return c.find(" " + cls + " ") != std::string::npos;
}

/** Primo nodo della lista il cui testo contiene "what" (per i selettori :contains). */
html::Node firstContaining(const std::vector<html::Node>& nodes, const std::string& what) {
    for (auto& n : nodes)
        if (contains(n.text(), what)) return n;
    return html::Node();
}

/** Valore JSON come stringa (anche se numerico/booleano). */
std::string jstr(const json& j, const char* key) {
    if (!j.is_object() || !j.contains(key)) return "";
    const json& v = j[key];
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number()) {
        std::string s = std::to_string(v.get<double>());
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        return s;
    }
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    return "";
}

bool jbool(const json& j, const char* key) {
    if (!j.is_object() || !j.contains(key)) return false;
    const json& v = j[key];
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return v.get<double>() != 0;
    if (v.is_string()) {
        std::string s = asciiLower(v.get<std::string>());
        return s == "true" || s == "1";
    }
    return false;
}

std::string form(const std::vector<std::pair<std::string, std::string>>& kv) {
    std::string out;
    for (auto& p : kv) {
        if (!out.empty()) out += '&';
        out += http::urlEncode(p.first) + "=" + http::urlEncode(p.second);
    }
    return out;
}

/** Legge il valore tra virgolette dopo "key" (salta spazi, ':', '=', '\\' e virgolette). */
std::string valueAfterKey(const std::string& text, const std::string& key, size_t from = 0) {
    auto p = text.find(key, from);
    if (p == std::string::npos) return "";
    p += key.size();
    while (p < text.size() && (text[p] == ' ' || text[p] == '\t' || text[p] == ':' || text[p] == '=' ||
                               text[p] == '\\' || text[p] == '"' || text[p] == '\''))
        p++;
    size_t e = p;
    while (e < text.size() && text[e] != '"' && text[e] != '\'' && text[e] != '\\') e++;
    return text.substr(p, e - p);
}

/** Sequenza di almeno 28 caratteri [\w-] (id di Google Drive). */
std::string gdriveId(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        size_t e = i;
        while (e < s.size() && (std::isalnum((unsigned char)s[e]) || s[e] == '_' || s[e] == '-')) e++;
        if (e - i >= 28) return s.substr(i, e - i);
        i = e + 1;
    }
    return "";
}

http::Headers mergeHeaders(http::Headers base, const http::Headers& extra) {
    for (auto& kv : extra) {
        std::string k = lower(kv.first);
        base.erase(std::remove_if(base.begin(), base.end(), [&](const std::pair<std::string, std::string>& p) {
                       return lower(p.first) == k;
                   }),
                   base.end());
    }
    for (auto& kv : extra) base.push_back(kv);
    return base;
}

std::string headerOf(const http::Headers& h, const std::string& name) {
    for (auto& kv : h)
        if (lower(kv.first) == lower(name)) return kv.second;
    return "";
}

/** Video con Referer/User-Agent/Origin presi dalle intestazioni indicate. */
Video makeVideo(const std::string& url, const std::string& title, const http::Headers& h = {}) {
    Video v;
    v.url = url;
    v.title = title;
    v.quality = qualityOf(title);
    for (auto& kv : h) {
        std::string k = lower(kv.first);
        if (k == "referer")
            v.referer = kv.second;
        else if (k == "user-agent")
            v.userAgent = kv.second;
        else if (k == "origin" || k == "cookie")
            v.headers.push_back(kv);
    }
    return v;
}

/** Video preferiti (titolo contenente "pref") per primi, poi per qualita' decrescente. */
void sortVideos(std::vector<Video>& v, const std::string& pref, bool byQuality = true) {
    std::stable_sort(v.begin(), v.end(), [&](const Video& a, const Video& b) {
        bool pa = !pref.empty() && contains(a.title, pref), pb = !pref.empty() && contains(b.title, pref);
        if (pa != pb) return pa;
        if (byQuality && a.quality != b.quality) return a.quality > b.quality;
        return false;
    });
}

// ============================================================================================ estrattori

std::string bloggerQuality(const std::string& format) {
    std::string f = trim(format);
    if (f == "7") return "240p";
    if (f == "18") return "360p";
    if (f == "22") return "720p";
    if (f == "37") return "1080p";
    return "Unknown";
}

std::vector<std::string> splitStr(const std::string& s, const std::string& sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        auto p = s.find(sep, start);
        out.push_back(s.substr(start, p == std::string::npos ? std::string::npos : p - start));
        if (p == std::string::npos) break;
        start = p + sep.size();
    }
    return out;
}

/** BloggerExtractor (lib/bloggerextractor): stream nella pagina video.g oppure RPC "WcwnYd". */
std::vector<Video> blogger(const std::string& url, const http::Headers& headers, const std::string& suffix = "") {
    http::Response r = http::request("GET", url, headers);
    if (r.status < 200 || r.status >= 300) return {};
    const std::string& body = r.body;
    auto title = [&](const std::string& q) {
        std::string t = "Blogger - " + q + (suffix.empty() ? "" : " " + suffix);
        return trim(t);
    };
    std::vector<Video> out;
    if (!contains(body, "errorContainer")) {
        std::string streams = after(body, "\"streams\":[", "");
        streams = substringBefore(streams, "]");
        if (!streams.empty()) {
            for (auto& part : splitStr(streams, "},")) {
                std::string u = substringBefore(substringAfter(part, "\"play_url\":\""), "\"");
                if (trim(u).empty() || !contains(part, "\"play_url\":\"")) continue;
                u = replaceAll(replaceAll(u, "\\u0026", "&"), "\\/", "/");
                std::string format = substringBefore(substringAfter(part, "\"format_id\":"), "}");
                out.push_back(makeVideo(u, title(bloggerQuality(format)), headers));
            }
        }
    }
    if (!out.empty()) return out;

    // ---- RPC
    std::string token = qp(url, "token");
    if (trim(token).empty()) return {};
    std::string sid = substringBefore(substringAfter(body, "FdrFJe\":\""), "\"");
    std::string bl = substringBefore(substringAfter(body, "cfb2h\":\""), "\"");
    std::string reqId = std::to_string((long long)(std::time(nullptr) % 86400));
    std::string rpcUrl = "https://www.blogger.com/_/BloggerVideoPlayerUi/data/batchexecute?rpcids=WcwnYd"
                         "&source-path=" + http::urlEncode("/video.g") + "&f.sid=" + http::urlEncode(sid) +
                         "&bl=" + http::urlEncode(bl) + "&hl=en-US&_reqid=" + reqId + "&rt=c";
    std::string rpcBody = "f.req=%5B%5B%5B%22WcwnYd%22%2C%22%5B%5C%22" + token +
                          "%5C%22%2C%5C%22%5C%22%2C0%5D%22%2Cnull%2C%22generic%22%5D%5D%5D&";
    http::Headers rh = {
        {"accept", "*/*"},
        {"accept-language", "en-US,en;q=0.9"},
        {"content-type", "application/x-www-form-urlencoded;charset=UTF-8"},
        {"sec-fetch-dest", "empty"},
        {"sec-fetch-mode", "cors"},
        {"sec-fetch-site", "same-origin"},
        {"x-same-domain", "1"},
        {"Referer", "https://www.blogger.com/"},
    };
    std::string ua = headerOf(headers, "User-Agent");
    if (!ua.empty()) rh.push_back({"User-Agent", ua});
    http::Response rr = http::request("POST", rpcUrl, rh, rpcBody);
    if (rr.status < 200 || rr.status >= 300) return {};
    const std::string& rpc = rr.body;
    if (!contains(rpc, "https://")) return {};
    std::string inner = after(rpc, "[[\\\"", "");
    inner = "\\\"" + substringBefore(inner, "]]]") + "]";
    for (auto& part : splitStr(inner, "],[")) {
        std::string raw = substringBefore(after(part, "\\\"", ""), "\\\"");
        if (trim(raw).empty()) continue;
        std::string videoUrl;
        try {
            std::string first = json::parse("\"" + raw + "\"").get<std::string>();
            videoUrl = json::parse("\"" + first + "\"").get<std::string>();
        } catch (const std::exception&) {
            continue;
        }
        if (videoUrl.empty()) continue;
        std::string format = substringBefore(substringAfter(part, "["), "]");
        out.push_back(makeVideo(videoUrl, title(bloggerQuality(format)), headers));
    }
    return out;
}

/** GoogleDriveExtractor: download diretto oppure modulo di conferma "virus scan". */
std::vector<Video> gdrive(const std::string& itemId, const std::string& name, const http::Headers& headers) {
    std::string url = "https://drive.usercontent.google.com/download?id=" + itemId;
    http::Headers h = mergeHeaders(
        headers, {{"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8"}});
    // Range: se e' direttamente il file non lo si scarica tutto
    http::Response r = http::request("GET", url, mergeHeaders(h, {{"Range", "bytes=0-14"}}));
    std::string ct = lower(r.header("content-type"));
    bool isHtml = contains(ct, "text/html") || startsWith(lower(r.body), "<!doctype html>");
    if (!isHtml) return {makeVideo(url, name, h)};
    if (r.status == 206) r = http::request("GET", url, h);
    html::Document doc(r.body, url);
    std::string size;
    html::Node sz = doc.selectFirst("span.uc-name-size");
    if (sz) size = " " + trim(ownText(sz)) + " ";
    std::vector<std::pair<std::string, std::string>> params = {{"id", itemId}};
    for (auto& in : doc.select("input[type=hidden]")) {
        std::string n = in.attr("name");
        if (n.empty()) continue;
        bool found = false;
        for (auto& p : params)
            if (p.first == n) {
                p.second = in.attr("value");
                found = true;
            }
        if (!found) params.push_back({n, in.attr("value")});
    }
    std::string videoUrl = "https://drive.usercontent.google.com/download?" + form(params);
    return {makeVideo(videoUrl, name + size, h)};
}

std::vector<Video> mediafire(const std::string& url, const std::string& name, const http::Headers& headers) {
    http::Response r = http::request("GET", url, headers);
    html::Document doc(r.body, url);
    std::string href = doc.selectFirst("a#downloadButton").attr("href");
    if (href.empty()) return {};
    return {makeVideo(href, name, headers)};
}

// ============================================================================================ base comune

class PtBase : public Source {
  public:
    PtBase(std::string id, std::string name, std::string url, bool nsfw = false, bool latest = true)
        : sid(std::move(id)), sname(std::move(name)), surl(std::move(url)), adult(nsfw), hasLatest(latest) {}

    std::string id() const override { return sid; }
    std::string name() const override { return sname; }
    std::string defaultBaseUrl() const override { return surl; }
    std::string lang() const override { return "pt"; }
    bool nsfw() const override { return adult; }
    bool supportsLatest() const override { return hasLatest; }

  protected:
    std::string sid, sname, surl;
    bool adult, hasLatest;

    virtual http::Headers siteHeaders() const { return {{"Referer", baseUrl()}}; }

    std::string abs(const std::string& u) const {
        if (u.empty()) return "";
        if (startsWith(u, "http://") || startsWith(u, "https://")) return u;
        if (startsWith(u, "//")) return "https:" + u;
        if (u[0] == '/') return baseUrl() + u;
        return baseUrl() + "/" + u;
    }

    /** Come setUrlWithoutDomain: percorso se lo stesso host del sito, altrimenti l'URL intero. */
    std::string rel(const std::string& u) const {
        if (!startsWith(u, "http")) return u;
        if (http::hostOf(u) == http::hostOf(baseUrl())) return http::pathOf(u);
        return u;
    }

    http::Response fetch(const std::string& method, const std::string& url, const http::Headers& extra = {},
                         const std::string& body = "") const {
        http::Response r = http::request(method, url, mergeHeaders(siteHeaders(), extra), body);
        if (r.status < 200 || r.status >= 300)
            throw http::Error(sname + " ha risposto HTTP " + std::to_string(r.status));
        return r;
    }

    std::string get(const std::string& url, const http::Headers& extra = {}) const {
        return fetch("GET", url, extra).body;
    }

    DocPtr doc(const std::string& url, const http::Headers& extra = {}) const {
        http::Response r = fetch("GET", url, extra);
        return std::make_unique<html::Document>(r.body, r.finalUrl.empty() ? url : r.finalUrl);
    }

    std::string postForm(const std::string& url, const std::string& body, const http::Headers& extra = {}) const {
        return fetch("POST", url,
                     mergeHeaders({{"Content-Type", "application/x-www-form-urlencoded"}}, extra), body)
            .body;
    }

    static std::vector<Video> nonEmpty(std::vector<Video> v) {
        if (v.empty()) throw http::Error("Nessun video trovato");
        return v;
    }
};

// ============================================================================================ Anime Fire

class AnimeFire : public PtBase {
  public:
    AnimeFire() : PtBase("pt.animefire", "Anime Fire", "https://animefire.io") {}

    Page popular(int page) override { return list(baseUrl() + "/top-animes/" + std::to_string(page)); }
    Page latest(int page) override { return list(baseUrl() + "/home/" + std::to_string(page)); }
    Page search(const std::string& q, int page) override {
        std::string fixed = asciiLower(replaceAll(trim(q), " ", "-"));
        if (fixed.empty()) return list(baseUrl() + "/genero/acao/" + std::to_string(page));
        return list(baseUrl() + "/pesquisar/" + fixed + "/" + std::to_string(page));
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details out;
        html::Node content = d->selectFirst("div.divDivAnimeInfo");
        html::Node names = content.selectFirst("div.div_anime_names");
        html::Node infos = content.selectFirst("div.divAnimePageInfo");
        out.thumbnail = content.selectFirst("div.sub_animepage_img > img").attr("data-src");
        out.title = names.selectFirst("h1").text();
        out.genre = joinText(infos.select("a.spanGeneros"));
        out.author = info(infos, "Estúdios");
        std::string st = trim(info(infos, "Status"));
        out.status = st == "Completo" ? "Completato" : st == "Em lançamento" ? "In corso" : st;

        std::string desc;
        html::Node sin = content.selectFirst("div.divSinopse > span");
        if (sin) desc += sin.text() + "\n";
        html::Node alt = names.selectFirst("h6");
        if (alt) desc += "\nNome alternativo: " + alt.text();
        const std::pair<const char*, const char*> items[] = {{"Dia de", "Dia de lançamento"}, {"Áudio", "Tipo"},
                                                             {"Ano", "Ano"},       {"Episódios", "Episódios"},
                                                             {"Temporada", "Temporada"}};
        for (auto& it : items) {
            std::string v = info(infos, it.first);
            if (!v.empty()) desc += std::string("\n") + it.second + ": " + v;
        }
        out.description = trim(desc);

        for (auto& a : d->select("div.div_video_list > a")) {
            Episode e;
            std::string href = d->absUrl(a, "href");
            e.url = rel(href);
            e.name = a.text();
            e.number = kNum(substringAfterLast(href, "/"), 0);
            out.episodes.push_back(e);
        }
        std::reverse(out.episodes.begin(), out.episodes.end());
        return out;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::vector<Video> out;
        html::Node video = d->selectFirst("video#my-video");
        if (video) {
            auto j = json::parse(get(video.attr("data-video-src")));
            for (auto& v : j.value("data", json::array())) {
                std::string u = replaceAll(jstr(v, "src"), "\\", "");
                if (!u.empty()) out.push_back(makeVideo(u, jstr(v, "label"), siteHeaders()));
            }
        } else {
            std::string iframe = d->selectFirst("div#div_video iframe").attr("src");
            if (iframe.empty()) throw http::Error("Nessun video trovato");
            std::string body = get(iframe);
            std::string u = substringBefore(substringAfter(substringAfter(body, "play_url"), ":\""), "\"");
            if (startsWith(u, "http")) out.push_back(makeVideo(u, "Default", siteHeaders()));
        }
        sortVideos(out, "720p", false);
        return nonEmpty(out);
    }

  protected:
    http::Headers siteHeaders() const override {
        return {{"Referer", baseUrl()}, {"Accept-Language", ACCEPT_LANGUAGE}};
    }

  private:
    static std::string info(const html::Node& infos, const std::string& key) {
        html::Node n = firstContaining(infos.select("div.animeInfo"), key);
        return n ? n.selectFirst("span").text() : "";
    }

    Page list(const std::string& url) {
        auto d = doc(url);
        Page p;
        for (auto& a : d->select("article.cardUltimosEps > a")) {
            Anime an;
            std::string href = d->absUrl(a, "href");
            std::string last = substringAfterLast(href, "/");
            if (last.empty() || digitsOnly(last) != last)
                an.url = rel(href);
            else
                an.url = rel(substringBeforeLast(href, "/") + "-todos-os-episodios");
            an.title = a.selectFirst("h3.animeTitle").text();
            an.thumbnail = a.selectFirst("img").attr("data-src");
            if (!an.url.empty()) p.animes.push_back(an);
        }
        p.hasNextPage = d->selectFirst("ul.pagination img.seta-right").valid();
        return p;
    }
};

// ============================================================================================ Anitube

class Anitube : public PtBase {
  public:
    Anitube() : PtBase("pt.anitube", "Anitube", "https://www.anitube.vip") {}

    Page popular(int page) override { return list(baseUrl() + "/anime/page/" + std::to_string(page), "div.lista_de_animes div.ani_loop_item_img > a"); }
    Page latest(int page) override { return list(baseUrl() + "/?page=" + std::to_string(page), "div.threeItensPerContent > div.epi_loop_item > a"); }
    Page search(const std::string& q, int page) override {
        if (trim(q).empty()) return popular(page);
        if (page > 1) return {};  // busca.php non e' paginata
        return list(baseUrl() + "/busca.php?s=" + http::urlEncode(q) + "&submit=Buscar",
                    "div.lista_de_animes div.ani_loop_item_img > a");
    }

    Details details(const std::string& url) override {
        auto d = realDoc(doc(abs(url)));
        Details out;
        html::Node content = d->selectFirst("div.anime_container_content");
        html::Node infos = content.selectFirst("div.anime_infos");
        out.title = d->selectFirst("div.anime_container_titulo").text();
        out.thumbnail = replaceAll(content.selectFirst("img").attr("src"), ".webp", ".jpg");
        out.genre = info(infos, "Gêneros");
        std::vector<std::string> auth = {info(infos, "Autor"), info(infos, "Estúdio")};
        out.author = joinStr(auth);
        std::string st = trim(info(infos, "Status"));
        out.status = st == "Completo" ? "Completato" : st == "Em Progresso" ? "In corso" : st;
        std::string desc = d->selectFirst("div.sinopse_container_content").text() + "\n";
        for (const char* item : {"Ano", "Direção", "Episódios", "Temporada", "Título Alternativo"}) {
            std::string v = info(infos, item);
            if (!v.empty()) desc += std::string("\n") + item + ": " + v;
        }
        out.description = trim(desc);

        // episodi, con paginazione
        std::set<std::string> seen;
        for (int guard = 0; guard < 100; guard++) {
            for (auto& a : d->select("div.animepag_episodios_item > a")) {
                Episode e;
                e.url = rel(d->absUrl(a, "href"));
                std::string t = a.selectFirst("div.animepag_episodios_item_views").text();
                e.name = t;
                e.number = kNum(substringAfter(t, " "), 0);
                if (seen.insert(e.url).second) out.episodes.push_back(e);
            }
            html::Node next = nextPage(*d);
            if (!next) break;
            std::string href = d->absUrl(next, "href");
            if (href.empty()) break;
            d = doc(href);
        }
        std::reverse(out.episodes.begin(), out.episodes.end());
        return out;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        auto links = d->select("div.video_container > a, div.playerContainer > a");
        const char* qualities[] = {"480p", "720p", "1080p"};
        std::vector<Video> out;
        std::string firstError;
        for (size_t i = 0; i < links.size() && i < 3; i++) {
            try {
                std::string u = d->absUrl(links[i], "href");
                auto v = videosFromUrl(u, qualities[i]);
                out.insert(out.end(), v.begin(), v.end());
            } catch (const std::exception& e) {
                if (firstError.empty()) firstError = e.what();
            }
        }
        if (out.empty() && !firstError.empty()) throw http::Error(firstError);
        sortVideos(out, "720p");
        return nonEmpty(out);
    }

  protected:
    http::Headers siteHeaders() const override {
        return {{"Referer", baseUrl() + "/"}, {"Accept-Language", ACCEPT_LANGUAGE}};
    }

  private:
    std::mutex adsMutex;
    std::map<std::string, std::string> adsCache;

    struct PlayerInfo {
        std::string playerUrl, referer, videoUrl;
    };

    /** "div.pagination > a.current:not(:nth-last-child(2)) + a" oppure, senza .current, l'ultimo link. */
    static html::Node nextPage(const html::Document& d) {
        html::Node pag = d.selectFirst("div.pagination");
        if (!pag) return {};
        auto ch = pag.children();
        int n = (int)ch.size();
        bool anyCurrent = false;
        for (int i = 0; i < n; i++) {
            if (!hasClass(ch[i], "current")) continue;
            anyCurrent = true;
            if (ch[i].tag() == "a" && i != n - 2 && i + 1 < n && ch[i + 1].tag() == "a") return ch[i + 1];
        }
        if (anyCurrent || pag.selectFirst(".current").valid()) return {};
        if (n == 3 && ch[0].tag() == "a" && ch[1].tag() == "a" && ch[2].tag() == "a") return {};
        if (n > 0 && ch[n - 1].tag() == "a") return ch[n - 1];
        return {};
    }

    static std::string info(const html::Node& infos, const std::string& key) {
        for (auto& div : infos.select("div.anime_info")) {
            bool ok = false;
            for (auto& b : div.select("b"))
                if (contains(b.text(), key)) ok = true;
            if (!ok) continue;
            auto as = div.select("a");
            return as.empty() ? trim(ownText(div)) : joinText(as);
        }
        return "";
    }

    Page list(const std::string& url, const std::string& selector) {
        auto d = doc(url);
        Page p;
        for (auto& a : d->select(selector)) {
            Anime an;
            an.url = rel(d->absUrl(a, "href"));
            html::Node img = a.selectFirst("img");
            an.title = img.attr("title");
            an.thumbnail = img.attr("src");
            if (!an.url.empty()) p.animes.push_back(an);
        }
        p.hasNextPage = nextPage(*d).valid();
        return p;
    }

    DocPtr realDoc(DocPtr d) {
        if (!contains(d->url(), "/video/")) return d;
        for (auto& a : d->select("div.controles_ep > a[href]")) {
            if (!a.selectFirst("i.spr.listaEP")) continue;
            return doc(abs(a.attr("href")));
        }
        return d;
    }

    static std::string publicidade(const std::string& r) {
        return substringBefore(substringAfter(after(r, "\"publicidade\"", ""), "\""), "\"");
    }

    PlayerInfo playerInfo(const std::string& link, http::Headers headers, int depth = 0) {
        if (depth > 5) throw http::Error("Troppi reindirizzamenti");
        std::string finalLink = startsWith(link, "//") ? "https:" + link : link;
        http::Response r = http::request("GET", finalLink, headers);
        if (r.status < 200 || r.status >= 300) throw http::Error("Anitube ha risposto HTTP " + std::to_string(r.status));
        std::string loc = r.finalUrl.empty() ? finalLink : r.finalUrl;
        html::Document d(r.body, loc);

        std::string refresh = d.selectFirst("meta[http-equiv=refresh]").attr("content");
        if (!trim(refresh).empty()) {
            std::string nl = substringAfter(refresh, "=");
            return playerInfo(nl, mergeHeaders(headers, {{"Referer", finalLink}}), depth + 1);
        }
        if (contains(r.body, "window.location.href = redirectUrl")) {
            std::string nl = substringBefore(substringAfter(r.body, "redirectUrl = `"), "`");
            nl = replaceAll(nl, "${token}", qp(finalLink, "t"));
            return playerInfo(nl, mergeHeaders(headers, {{"Referer", finalLink}}), depth + 1);
        }
        html::Node novo = firstContaining(d.select("p"), "Novo endereço");
        if (novo) {
            std::string nl = novo.selectFirst("strong").text();
            throw http::Error("Anitube ha cambiato dominio: " + nl + " (impostalo nelle opzioni della fonte)");
        }
        PlayerInfo pi;
        pi.referer = loc;
        pi.playerUrl = d.selectFirst("iframe").attr("src");
        if (pi.playerUrl.empty()) throw http::Error("Player non trovato");
        pi.playerUrl = fixUrl(pi.playerUrl, loc);
        pi.videoUrl = qp(pi.playerUrl, "url");
        if (pi.videoUrl.empty()) throw http::Error("Player non riconosciuto");
        return pi;
    }

    std::string videoToken(const PlayerInfo& pi) {
        std::string adsUrl, adblockUrl;
        try {
            std::string refHost = "https://" + http::hostOf(pi.referer) + "/";
            http::Response r = http::request("GET", pi.playerUrl, mergeHeaders(siteHeaders(), {{"Referer", refHost}}));
            const std::string& body = r.body;
            for (const char* key : {"urlToFetch", "ADS_URL"}) {
                size_t pos = 0;
                while (adsUrl.empty() && (pos = body.find(key, pos)) != std::string::npos) {
                    size_t p = pos + std::string(key).size();
                    while (p < body.size() && std::isspace((unsigned char)body[p])) p++;
                    if (p < body.size() && body[p] == '=') {
                        p++;
                        while (p < body.size() && std::isspace((unsigned char)body[p])) p++;
                        if (p < body.size() && (body[p] == '\'' || body[p] == '"')) {
                            char q = body[p];
                            auto e = body.find(q, p + 1);
                            if (e != std::string::npos) adsUrl = body.substr(p + 1, e - p - 1);
                        }
                    }
                    pos = p;
                }
                if (!adsUrl.empty()) break;
            }
            adblockUrl = substringBefore(substringAfter(after(body, "$.post", ""), "'"), "'");
            if (!startsWith(adsUrl, "http") || !startsWith(adblockUrl, "http")) throw http::Error("");
        } catch (const std::exception&) {
            adsUrl = "https://widgets.outbrain.com/outbrain.js";
            adblockUrl = "https://ads.anitube.vip/adblock2.php";
        }

        std::string ads;
        {
            std::lock_guard<std::mutex> lock(adsMutex);
            auto it = adsCache.find(adsUrl);
            if (it != adsCache.end()) ads = it->second;
        }
        if (ads.empty()) {
            ads = http::getText(adsUrl);
            std::lock_guard<std::mutex> lock(adsMutex);
            adsCache[adsUrl] = ads;
        }

        http::Headers api = mergeHeaders(siteHeaders(), {{"Referer", "https://" + http::hostOf(pi.referer) + "/"},
                                                         {"Accept", "*/*"},
                                                         {"Cache-Control", "no-cache"},
                                                         {"Pragma", "no-cache"},
                                                         {"Sec-Fetch-Dest", "empty"},
                                                         {"Sec-Fetch-Mode", "cors"},
                                                         {"Sec-Fetch-Site", "same-site"}});
        std::string body = form({{"category", "client"}, {"type", "premium"}, {"ad", ads}, {"url", pi.videoUrl}});
        http::Response r = http::request(
            "POST", adblockUrl, mergeHeaders(api, {{"Content-Type", "application/x-www-form-urlencoded"}}), body);
        if (r.status < 200 || r.status >= 300) throw http::Error("Anitube: token non ottenuto");
        std::string token = publicidade(r.body);
        if (trim(token).empty()) token = "undefined";
        try {
            http::Response r2 = http::request("GET", adblockUrl + "?token=" + token + "&url=" + pi.videoUrl, api);
            std::string vt = publicidade(r2.body);
            if (startsWith(vt, "?")) return vt;
        } catch (const std::exception&) {
        }
        return "";
    }

    std::vector<Video> videosFromUrl(const std::string& url, const std::string& quality) {
        PlayerInfo pi = playerInfo(url, siteHeaders());
        std::string token = videoToken(pi);
        if (token.empty()) return {};
        return {makeVideo(pi.videoUrl + token, "Anitube - " + quality, siteHeaders())};
    }
};

// ============================================================================================ Goyabu

class Goyabu : public PtBase {
  public:
    Goyabu() : PtBase("pt.goyabu", "Goyabu", "https://goyabu.io", true) {}

    Page popular(int page) override {
        if (page > 1) return {};
        return list(baseUrl() + "/?s=");
    }
    Page latest(int page) override {
        std::string url = page == 1 ? baseUrl() + "/lancamentos" : baseUrl() + "/lancamentos/page/" + std::to_string(page);
        auto d = doc(url);
        Page p;
        for (auto& a : d->select("article.boxEP a")) {
            Anime an;
            an.url = rel(d->absUrl(a, "href"));
            an.title = a.selectFirst("div.title").text();
            an.thumbnail = a.selectFirst("figure").attr("data-thumb");
            if (!an.url.empty()) p.animes.push_back(an);
        }
        html::Node pag = d->selectFirst("div.pagination");
        if (pag) p.hasNextPage = kNum(pag.attr("data-current-page"), 1) < kNum(pag.attr("data-total-pages"), 1);
        return p;
    }
    Page search(const std::string& q, int page) override {
        if (page > 1) return {};
        return list(baseUrl() + "/?s=" + http::urlEncode(q));
    }

    Details details(const std::string& url) override {
        auto d = realDoc(doc(abs(url)));
        Details out;
        out.title = d->selectFirst("div.streamer-info h1").text();
        out.thumbnail = d->selectFirst("div.streamer-poster img").attr("src");
        out.description = d->selectFirst(".sinopse-full").text();
        out.genre = joinText(d->select("div.filter-items a.filter-btn"));
        std::string st = asciiLower(d->selectFirst(".streamer-info-list li.status").text());
        out.status = st == "completo" ? "Completato" : st == "lançamento" ? "In corso" : "";

        std::string script = scriptWith(*d, {"const allEpisodes"});
        if (!script.empty()) {
            std::string js = substringAfter(script, "const allEpisodes =");
            auto end = js.find("];");
            js = end != std::string::npos ? js.substr(0, end + 1) : substringBefore(js, ";");
            try {
                auto arr = json::parse(trim(js));
                for (auto& ep : arr) {
                    Episode e;
                    e.url = rel(jstr(ep, "link"));
                    std::string num = jstr(ep, "episodio");
                    std::string en = jstr(ep, "episode_name");
                    e.name = "Episódio " + num + (en.empty() ? "" : " - " + en);
                    std::string audio = trim(jstr(ep, "audio"));
                    if (!audio.empty()) e.name += " [" + audio + "]";
                    e.number = kNum(num, 1);
                    out.episodes.push_back(e);
                }
            } catch (const std::exception&) {
            }
            std::reverse(out.episodes.begin(), out.episodes.end());
        }
        return out;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::vector<Video> out;
        for (auto& n : d->select("[data-blogger-url-encrypted]")) {
            try {
                std::string u = reversed(base64Decode(n.attr("data-blogger-url-encrypted")));
                if (contains(u, "blogger.com")) {
                    auto v = blogger(u, siteHeaders());
                    out.insert(out.end(), v.begin(), v.end());
                }
            } catch (const std::exception&) {
            }
        }
        sortVideos(out, "720p");
        return nonEmpty(out);
    }

  protected:
    http::Headers siteHeaders() const override { return {{"Referer", baseUrl()}, {"Origin", baseUrl()}}; }

  private:
    Page list(const std::string& url) {
        auto d = doc(url);
        Page p;
        for (auto& a : d->select("article.boxAN a")) {
            Anime an;
            an.url = rel(d->absUrl(a, "href"));
            an.title = a.selectFirst("div.title").text();
            an.thumbnail = a.selectFirst("img").attr("src");
            if (!an.url.empty()) p.animes.push_back(an);
        }
        return p;
    }

    DocPtr realDoc(DocPtr d) {
        html::Node menu = d->selectFirst(".episode-navigation span.lista");
        if (!menu) return d;
        std::string href = menu.parent().attr("href");
        if (href.empty()) return d;
        return doc(abs(href));
    }
};

// ============================================================================================ Animes Online Vip

class AnimesOnlineVip : public PtBase {
  public:
    AnimesOnlineVip() : PtBase("pt.animesonlinevip", "Animes Online Vip", "https://animesonlinefhd.vip", true) {}

    Page popular(int page) override {
        if (page > 1) return {};
        return list(baseUrl() + "/top-100", "a.top100Item", "");
    }
    Page latest(int page) override {
        return list(baseUrl() + "/page/" + std::to_string(page), "div.videos div.video div.video-thumb a",
                    "ul.paginacao li.next");
    }
    Page search(const std::string& q, int page) override {
        return list(baseUrl() + "/page/" + std::to_string(page) + "?s=" + http::urlEncode(q),
                    "div.videos div.video div.video-thumb a", "ul.paginacao li.next");
    }

    Details details(const std::string& url) override {
        auto d = realDoc(doc(abs(url)));
        Details out;
        out.title = trim(d->selectFirst("div.pagina-titulo h1").text());
        out.thumbnail = d->selectFirst("div.post-capa img").attr("src");
        out.description = d->selectFirst("ul.post-infos p").text();
        out.genre = joinText(d->select("ul.post-infos li a"));
        for (auto& a : d->select("ul.episodios li a")) {
            Episode e;
            e.url = rel(d->absUrl(a, "href"));
            e.name = trim(substringAfterLast(a.attr("title"), "–"));
            auto spans = a.select("div.listaEpInfosEp span");
            e.number = spans.empty() ? 1 : kNum(substringAfter(spans[0].text(), ":"), 1);
            if (e.name.empty()) e.name = "Episódio " + std::to_string((int)e.number);
            out.episodes.push_back(e);
        }
        std::reverse(out.episodes.begin(), out.episodes.end());
        return out;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::vector<Video> out;
        for (auto& n : d->select("#video source, div.post-video iframe")) {
            std::string src = fixUrl(n.attr("src"), d->url());
            if (src.empty()) continue;
            try {
                if (contains(src, "assistonapi.link")) {
                    auto v = blogger(src, siteHeaders());
                    out.insert(out.end(), v.begin(), v.end());
                } else {
                    out.push_back(makeVideo(src, "Default", siteHeaders()));
                }
            } catch (const std::exception&) {
            }
        }
        sortVideos(out, "720p");
        return nonEmpty(out);
    }

  protected:
    http::Headers siteHeaders() const override { return {{"Referer", baseUrl()}, {"Origin", baseUrl()}}; }

  private:
    Page list(const std::string& url, const std::string& sel, const std::string& next) {
        auto d = doc(url);
        Page p;
        for (auto& a : d->select(sel)) {
            Anime an;
            an.url = rel(d->absUrl(a, "href"));
            an.title = a.attr("title");
            an.thumbnail = a.selectFirst("img").attr("src");
            if (!an.url.empty()) p.animes.push_back(an);
        }
        p.hasNextPage = !next.empty() && d->selectFirst(next).valid();
        return p;
    }

    DocPtr realDoc(DocPtr d) {
        html::Node icon = d->selectFirst("div.post-botoes ul li a i.fa-bars");
        if (!icon) return d;
        std::string href = icon.parent().attr("href");
        if (href.empty()) return d;
        return doc(abs(href));
    }
};

// ============================================================================================ Animes Digital

/** ScriptExtractor di Animes Digital: sources:[{file/src:"...", label:"..."}] (eventualmente "packed"). */
std::vector<Video> sourcesFromScript(const std::string& data, const http::Headers& headers, const std::string& def) {
    std::string script = contains(data, "eval(function") ? unpacker::unpackAndCombine(data) : data;
    if (script.empty()) return {};
    script = replaceAll(script, "\\", "");
    auto afterKey = [](const std::string& s) {
        std::string t = substringAfter(s, ":");
        t = substringBefore(substringAfter(t, "\""), "\"");
        t = substringBefore(substringAfter(t, "'"), "'");
        return t;
    };
    std::string part = substringAfter(substringAfter(script, "sources:"), ".src(");
    part = substringBefore(substringAfter(substringBefore(part, ")"), "["), "]");
    std::vector<Video> out;
    auto items = splitStr(part, "{");
    for (size_t i = 1; i < items.size(); i++) {
        std::string q = trim(afterKey(after(items[i], "label", "")));
        if (q.empty()) q = def;
        std::string u = trim(afterKey(substringAfter(substringAfter(items[i], "file"), "src")));
        if (!startsWith(u, "http")) continue;
        out.push_back(makeVideo(u, q, headers));
    }
    return out;
}

class AnimesDigital : public PtBase {
  public:
    AnimesDigital() : PtBase("pt.animesdigital", "Animes Digital", "https://animesdigital.org") {}

    Page popular(int page) override {
        if (page > 1) return {};
        return list(baseUrl() + "/home", false);
    }
    Page latest(int page) override {
        // il link "Ver Mais" della home ora punta a /lancamentos01 (/lancamentos da' 404)
        std::string p1 = page <= 1 ? baseUrl() + "/lancamentos01" : baseUrl() + "/lancamentos01/page/" + std::to_string(page);
        try {
            return list(p1, true);
        } catch (const std::exception&) {
            return list(baseUrl() + "/lancamentos/page/" + std::to_string(page), true);
        }
    }
    Page search(const std::string& q, int page) override {
        // modulo di ricerca del sito: GET /pesquisa/?s=...
        try {
            std::string url = page <= 1 ? baseUrl() + "/pesquisa/?s=" + http::urlEncode(q)
                                        : baseUrl() + "/pesquisa/page/" + std::to_string(page) + "/?s=" + http::urlEncode(q);
            auto d = doc(url);
            Page p;
            std::set<std::string> seen;
            for (auto& a : d->select("div.itemA > a, div.itemE > a")) {
                Anime an = item(*d, a);
                if (an.title.empty()) an.title = trim(substringBefore(replaceAll(a.attr("title"), "Assistir ", ""), " Online"));
                if (an.url.empty() || !seen.insert(an.url).second) continue;
                p.animes.push_back(an);
            }
            p.hasNextPage = d->selectFirst("ul > li.next, a.next.page-numbers").valid();
            if (!p.animes.empty()) return p;
        } catch (const std::exception&) {
        }
        return ajaxSearch(q, page);
    }

    Page ajaxSearch(const std::string& q, int page) {
        std::vector<std::pair<std::string, std::string>> f = {
            {"type", "lista"}, {"limit", "30"}, {"token", searchToken()}};
        if (!q.empty()) f.push_back({"search", q});
        f.push_back({"pagina", std::to_string(page)});
        f.push_back({"filters",
                     "{\"filter_data\": \"type_url=animes&filter_audio=0&filter_letter=0&filter_order=name\", "
                     "\"filter_genre_add\": [], \"filter_genre_del\": []}"});
        Page p;
        try {
            auto j = json::parse(postForm(baseUrl() + "/func/listanime", form(f)));
            for (auto& r : j.value("results", json::array())) {
                if (!r.is_string()) continue;
                html::Document d(r.get<std::string>(), baseUrl() + "/");
                html::Node a = d.selectFirst("div.itemA > a");
                if (a) p.animes.push_back(item(d, a));
            }
            p.hasNextPage = kNum(jstr(j, "total_page"), 0) > kNum(jstr(j, "page"), 0);
        } catch (const std::exception&) {
        }
        return p;
    }

    Details details(const std::string& url) override {
        auto d = realDoc(doc(abs(url)));
        Details out;
        html::Node poster = d->selectFirst("div.poster > img");
        out.thumbnail = poster.attr("data-lazy-src");
        if (out.thumbnail.empty()) out.thumbnail = poster.attr("src");
        std::string st = d->selectFirst("div.clw > div.playon").text();
        out.status = st == "Em Lançamento" ? "In corso" : st == "Completo" ? "Completato" : "";
        html::Node dados = d->selectFirst("div.crw > div.dados");
        std::string author = info(dados, "Autor");
        if (author.empty()) author = info(dados, "Diretor");
        out.author = joinStr({author, info(dados, "Estúdio")});
        out.title = dados.selectFirst("h1").text();
        out.genre = joinText(dados.select("div.genre a"));
        out.description = dados.selectFirst("div.sinopse").text();

        auto parseEps = [&](const html::Document& doc) {
            for (auto& a : doc.select("div.item_ep > a")) {
                Episode e;
                e.url = rel(doc.absUrl(a, "href"));
                e.name = a.selectFirst("div.title_anime").text();
                e.number = kNum(substringAfterLast(e.name, " "), 1);
                out.episodes.push_back(e);
            }
        };
        parseEps(*d);
        int lastPage = 0;
        html::Node pag = d->selectFirst("ul.content-pagination");
        if (pag) {
            auto lis = pag.children();
            if (lis.size() >= 2) lastPage = (int)kNum(lis[lis.size() - 2].selectFirst("a").text(), 0);
        }
        std::string loc = d->url();
        while (!loc.empty() && loc.back() == '/') loc.pop_back();
        for (int i = 2; i <= lastPage && i <= 60; i++) {
            try {
                auto pd = doc(loc + "/page/" + std::to_string(i));
                parseEps(*pd);
            } catch (const std::exception&) {
            }
        }
        return out;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        html::Node player = d->selectFirst("div#player");
        std::vector<Video> out;
        for (auto& tab : player.select("div.tab-video")) {
            auto v = fromContainer(tab, *d, 0);
            out.insert(out.end(), v.begin(), v.end());
        }
        sortVideos(out, "720p", false);
        return nonEmpty(out);
    }

  private:
    std::mutex tokenMutex;
    std::string token;

    std::string searchToken() {
        std::lock_guard<std::mutex> lock(tokenMutex);
        for (const char* path : {"/animes-legendados-online001", "/animes-legendados-online"}) {
            if (!token.empty()) break;
            try {
                auto d = doc(baseUrl() + path);
                token = d->selectFirst("div.menu_filter_box").attr("data-secury");
            } catch (const std::exception&) {
            }
        }
        return token;
    }

    Anime item(const html::Document& d, const html::Node& a) {
        Anime an;
        an.url = rel(d.absUrl(a, "href"));
        html::Node img = a.selectFirst("img");
        an.thumbnail = img.attr("data-lazy-src");
        if (an.thumbnail.empty()) an.thumbnail = img.attr("src");
        an.title = a.selectFirst("span.title_anime").text();
        return an;
    }

    Page list(const std::string& url, bool paged) {
        auto d = doc(url);
        Page p;
        for (auto& a : d->select("div.b_flex > div.itemE > a")) {
            Anime an = item(*d, a);
            if (!an.url.empty()) p.animes.push_back(an);
        }
        p.hasNextPage = paged && d->selectFirst("ul > li.next").valid();
        return p;
    }

    DocPtr realDoc(DocPtr d) {
        html::Node link = firstContaining(d->select("div.subitem > a"), "menu");
        if (!link) return d;
        return doc(abs(link.attr("href")));
    }

    static std::string info(const html::Node& dados, const std::string& key) {
        for (auto& div : dados.select("div.info")) {
            bool ok = false;
            for (auto& s : div.select("span"))
                if (contains(ownText(s), key)) ok = true;
            if (!ok) continue;
            std::string t = trim(ownText(div));
            return t == "?" ? "" : t;
        }
        return "";
    }

    std::vector<Video> fromContainer(const html::Node& root, const html::Document& d, int depth) {
        std::vector<Video> out;
        for (auto& iframe : root.select("iframe")) {
            try {
                std::string u = d.absUrl(iframe, "data-lazy-src");
                if (u.empty()) u = d.absUrl(iframe, "src");
                if (u.empty()) continue;
                if (contains(u, "blogger.com")) {
                    auto v = blogger(u, siteHeaders());
                    out.insert(out.end(), v.begin(), v.end());
                } else if (depth < 2) {
                    auto inner = doc(u);
                    auto v = fromContainer(inner->root(), *inner, depth + 1);
                    out.insert(out.end(), v.begin(), v.end());
                }
            } catch (const std::exception&) {
            }
        }
        for (auto& s : root.select("script")) {
            std::string data = s.data();
            if (contains(data, "/bg.mp4")) continue;
            if (!contains(data, "eval") && !contains(data, "player.src") && !contains(data, "this.src") &&
                !contains(data, "sources:"))
                continue;
            try {
                auto v = sourcesFromScript(data, siteHeaders(), "Animes Digital");
                out.insert(out.end(), v.begin(), v.end());
            } catch (const std::exception&) {
            }
        }
        return out;
    }
};

// ============================================================================================ AnimeCore

class AnimeCore : public PtBase {
  public:
    AnimeCore() : PtBase("pt.animesotaku", "AnimeCore", "https://animecore.net") {}

    Page popular(int page) override { return ajaxList("", "popular", page); }
    Page latest(int page) override { return ajaxList("", "updated", page); }
    Page search(const std::string& q, int page) override { return ajaxList(q, "", page); }

    Details details(const std::string& url) override {
        auto d = realDoc(doc(abs(url)));
        Details out;
        out.thumbnail = d->absUrl(d->selectFirst("img.wp-post-image"), "src");
        out.title = cleanTitle(d->selectFirst("title").text());
        std::vector<std::string> genres;
        for (auto& a : d->select("div.flex a"))
            if (hasClass(a, "hover:text-white")) genres.push_back(a.text());
        out.genre = joinStr(genres);
        out.description = d->selectFirst("section p").text();

        std::string animeId = d->selectFirst("#seasonContent").attr("data-season");
        if (animeId.empty()) return out;
        int maxPage = 1;
        for (int page = 1; page <= maxPage && page <= 50; page++) {
            auto j = json::parse(get(baseUrl() + "/wp-admin/admin-ajax.php?action=get_episodes&anime_id=" + animeId +
                                     "&page=" + std::to_string(page) + "&order=desc"));
            const json& data = j.contains("data") ? j["data"] : json::object();
            maxPage = (int)kNum(jstr(data, "max_episodes_page"), 1);
            if (!data.contains("episodes") || !data["episodes"].is_array() || data["episodes"].empty()) break;
            for (auto& ep : data["episodes"]) {
                Episode e;
                e.name = jstr(ep, "number");
                if (!jstr(ep, "title").empty() && !contains(e.name, jstr(ep, "title")))
                    e.name += " - " + jstr(ep, "title");
                e.number = kNum(jstr(ep, "meta_number"), 1);
                e.url = "/watch/" + substringAfter(jstr(ep, "url"), "/watch/");
                out.episodes.push_back(e);
            }
        }
        return out;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::vector<Video> out;
        for (auto& f : d->select("div.episode-player-box iframe")) {
            std::string u = d->absUrl(f, "src");
            try {
                if (contains(u, "blogger.com")) {
                    auto v = blogger(u, siteHeaders());
                    out.insert(out.end(), v.begin(), v.end());
                } else if (contains(u, "proxycdn.cc")) {
                    out.push_back(makeVideo(u, "Proxy CDN"));
                }
            } catch (const std::exception&) {
            }
        }
        sortVideos(out, "");
        return nonEmpty(out);
    }

  protected:
    http::Headers siteHeaders() const override { return {{"Referer", baseUrl() + "/"}}; }

  private:
    static std::string cleanTitle(const std::string& t) {
        if (t.size() > 1500) return trim(t);
        static const std::regex re(R"(- Anime Core *$|\(? *(?:Todos Episodios )?Assistir(?: Online)? *\)? *$)");
        return trim(std::regex_replace(t, re, ""));
    }

    Page ajaxList(const std::string& q, const std::string& orderby, int page) {
        std::vector<std::pair<std::string, std::string>> f = {{"s_keyword", q}};
        if (!orderby.empty()) {
            f.push_back({"orderby", orderby});
            f.push_back({"order", "DESC"});
        }
        f.push_back({"action", "advanced_search"});
        f.push_back({"page", std::to_string(page)});
        auto j = json::parse(postForm(baseUrl() + "/wp-admin/admin-ajax.php", form(f)));
        Page p;
        const json& data = j.contains("data") ? j["data"] : json::object();
        std::string htmlStr = jstr(data, "html");
        html::Document d(htmlStr, baseUrl() + "/");
        for (auto& card : d.select("article.anime-card")) {
            html::Node a = card.selectFirst("h3 > a.stretched-link");
            if (!a) continue;
            std::string epUrl = d.absUrl(a, "href");
            if (trim(epUrl).empty()) continue;
            std::string animeUrl = epUrl;
            auto w = epUrl.find("/watch/");
            if (w != std::string::npos) {
                std::string rest = epUrl.substr(w + 7);
                auto e = rest.rfind("-episodio-");
                if (e != std::string::npos) {
                    std::string tail = rest.substr(e + 10);
                    while (!tail.empty() && tail.back() == '/') tail.pop_back();
                    if (!tail.empty() && digitsOnly(tail) == tail && rest.substr(0, e).find('/') == std::string::npos)
                        animeUrl = baseUrl() + "/anime/" + rest.substr(0, e);
                }
            }
            Anime an;
            an.thumbnail = d.absUrl(card.selectFirst("img"), "src");
            an.title = cleanTitle(a.attr("title"));
            if (an.title.empty()) an.title = a.text();
            an.url = rel(animeUrl);
            p.animes.push_back(an);
        }
        p.hasNextPage = kNum(jstr(data, "current_page"), 0) < kNum(jstr(data, "max_pages"), 0);
        return p;
    }

    DocPtr realDoc(DocPtr d) {
        html::Node menu = d->selectFirst("div.anime-information h4 a");
        if (!menu) return d;
        std::string u = d->absUrl(menu, "href");
        if (trim(u).empty()) return d;
        return doc(u);
    }
};

// ============================================================================================ Sushi Animes

/** Escapa le virgolette non escapate dentro i valori "name": "..." (JSON-LD malformato del sito). */
std::string sanitizeLdNames(const std::string& in) {
    std::string out;
    size_t pos = 0;
    while (true) {
        auto p = in.find("\"name\"", pos);
        if (p == std::string::npos) break;
        size_t q = p + 6;
        while (q < in.size() && std::isspace((unsigned char)in[q])) q++;
        if (q >= in.size() || in[q] != ':') {
            out += in.substr(pos, q - pos);
            pos = q;
            continue;
        }
        q++;
        while (q < in.size() && std::isspace((unsigned char)in[q])) q++;
        if (q >= in.size() || in[q] != '"') {
            out += in.substr(pos, q - pos);
            pos = q;
            continue;
        }
        auto end = in.find("\",", q + 1);
        if (end == std::string::npos) break;
        std::string value = in.substr(q + 1, end - q - 1);
        std::string esc;
        for (size_t i = 0; i < value.size(); i++) {
            if (value[i] == '"' && (i == 0 || value[i - 1] != '\\')) esc += "\\\"";
            else esc += value[i];
        }
        out += in.substr(pos, p - pos) + "\"name\": \"" + esc + "\",";
        pos = end + 2;
    }
    out += in.substr(std::min(pos, in.size()));
    return out;
}

class SushiAnimes : public PtBase {
  public:
    SushiAnimes() : PtBase("pt.sushianimes", "Sushi Animes", "https://sushianimes.com.br", true) {}

    Page popular(int page) override {
        if (page > 1) return {};
        auto d = doc(baseUrl() + "/trends");
        Page p;
        for (auto& a : d->select("a.list-trend")) {
            Anime an;
            an.url = rel(d->absUrl(a, "href"));
            an.title = a.selectFirst(".list-title").text();
            an.thumbnail = a.selectFirst(".media-cover").attr("data-src");
            if (!an.url.empty()) p.animes.push_back(an);
        }
        return p;
    }
    Page latest(int page) override {
        auto d = doc(baseUrl() + "/episodios?page=" + std::to_string(page));
        Page p;
        for (auto& a : d->select(".episode-grid a.list-movie")) {
            if (a.selectFirst(".hentai-list-media")) continue;
            Anime an;
            an.url = rel(d->absUrl(a, "href"));
            an.title = a.selectFirst(".list-caption").text();
            an.thumbnail = a.selectFirst(".media-episode").attr("data-src");
            if (!an.url.empty()) p.animes.push_back(an);
        }
        p.hasNextPage = d->selectFirst("a.btn.btn-theme.ml-2").valid();
        return p;
    }
    Page search(const std::string& q, int page) override {
        if (page > 1) return {};
        auto d = doc(baseUrl() + "/search/" + http::urlEncode(q));
        Page p;
        for (auto& el : d->select("div.list-movie")) {
            Anime an;
            an.url = rel(d->absUrl(el.selectFirst("a"), "href"));
            an.title = el.selectFirst(".list-title").text();
            an.thumbnail = el.selectFirst(".media-cover").attr("data-src");
            if (!an.url.empty()) p.animes.push_back(an);
        }
        return p;
    }

    Details details(const std::string& url) override {
        auto d = realDoc(doc(abs(url)));
        Details out;
        out.title = d->selectFirst("#title").text();
        out.thumbnail = d->selectFirst(".media-cover img").attr("src");
        for (auto& attr : d->select(".detail-attr")) {
            std::string t = attr.text();
            if (contains(t, "Sinopse") || contains(t, "Descrição")) {
                out.description = attr.selectFirst(".text").text();
                if (!out.description.empty()) break;
            }
        }
        out.genre = joinText(d->select(".category-list a, .categories a"));

        html::Node ld = d->selectFirst("script[type=\"application/ld+json\"]");
        if (!ld) return out;
        html::Node movie = firstContaining(d->select("a.btn"), "Assistir");
        if (movie) {
            Episode e;
            e.url = rel(d->absUrl(movie, "href"));
            e.name = "Filme";
            e.number = 1;
            out.episodes.push_back(e);
            return out;
        }
        try {
            auto j = json::parse(sanitizeLdNames(trim(ld.data())));
            for (auto& season : j.value("containsSeason", json::array())) {
                std::string sn = jstr(season, "seasonNumber");
                for (auto& ep : season.value("episode", json::array())) {
                    Episode e;
                    e.url = rel(jstr(ep, "url"));
                    std::string num = jstr(ep, "episodeNumber");
                    e.name = "Temporada " + sn + " x " + num + " - " + jstr(ep, "name");
                    e.number = kNum(num, 0);
                    out.episodes.push_back(e);
                }
            }
        } catch (const std::exception&) {
        }
        std::reverse(out.episodes.begin(), out.episodes.end());
        return out;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        auto embeds = d->select("[data-embed]");
        if (embeds.empty()) throw http::Error("Nessun video trovato");
        std::string csrf = d->selectFirst("meta[name=\"csrf-token\"]").attr("content");
        if (trim(csrf).empty()) {
            std::string s = scriptWith(*d, {"_TOKEN"});
            csrf = valueAfterKey(s, "_TOKEN");
        }
        http::Headers extra;
        if (!trim(csrf).empty()) extra.push_back({"X-CSRF-TOKEN", csrf});
        std::vector<Video> out;
        for (auto& em : embeds) {
            try {
                std::vector<std::pair<std::string, std::string>> f = {{"id", em.attr("data-embed")}};
                if (!trim(csrf).empty()) f.push_back({"_TOKEN", csrf});
                http::Response r = http::request(
                    "POST", baseUrl() + "/ajax/embed",
                    mergeHeaders(siteHeaders(),
                                 mergeHeaders(extra, {{"Content-Type", "application/x-www-form-urlencoded"}})),
                    form(f));
                if (r.status < 200 || r.status >= 300) continue;
                auto v = parseEmbed(r.body);
                out.insert(out.end(), v.begin(), v.end());
            } catch (const std::exception&) {
            }
        }
        sortVideos(out, "1080p");
        return nonEmpty(out);
    }

  protected:
    http::Headers siteHeaders() const override { return {{"Referer", baseUrl() + "/"}, {"Origin", baseUrl()}}; }

  private:
    DocPtr realDoc(DocPtr d) {
        html::Node menu = d->selectFirst(".episode-nav .home-list a");
        if (!menu) return d;
        try {
            return doc(abs(menu.attr("href")));
        } catch (const std::exception&) {
            return d;
        }
    }

    static std::string qualityFromLabel(const std::string& label) {
        std::string l = lower(label);
        if (contains(l, "fullhd") || contains(l, "fhd") || contains(l, "1080")) return "1080p";
        if (contains(l, "720") || contains(l, "hd")) return "720p";
        if (contains(l, "480") || contains(l, "sd")) return "480p";
        if (contains(l, "360")) return "360p";
        if (contains(l, "240")) return "240p";
        return label;
    }

    std::vector<Video> parseEmbed(const std::string& body) {
        html::Document d(body, baseUrl() + "/");
        auto iframes = d.select("iframe[src]");
        std::vector<Video> out;
        if (!iframes.empty()) {
            for (auto& f : iframes) {
                std::string raw = d.absUrl(f, "src");
                if (raw.empty()) raw = f.attr("src");
                std::string u = raw;
                if (contains(raw, "proxy.php")) {
                    std::string s = qp(raw, "src");
                    if (!s.empty()) u = s;
                }
                if (!startsWith(u, "http")) continue;
                if (contains(u, "blogger.com")) {
                    auto v = blogger(u, siteHeaders());
                    out.insert(out.end(), v.begin(), v.end());
                }
            }
            return out;
        }
        std::string raw;
        auto p = body.find("var playerEmbed");
        if (p != std::string::npos) raw = valueAfterKey(body.substr(p, 2048), "var playerEmbed");
        // valueAfterKey si ferma al primo '\': ricostruisce gli URL con "\/"
        if (p != std::string::npos && raw.size() < 12) {
            std::string seg = body.substr(p, 2048);
            auto q1 = seg.find_first_of("\"'");
            if (q1 != std::string::npos) {
                auto q2 = seg.find(seg[q1], q1 + 1);
                if (q2 != std::string::npos) raw = seg.substr(q1 + 1, q2 - q1 - 1);
            }
        }
        if (raw.empty()) return out;
        std::string direct = replaceAll(raw, "\\/", "/");
        std::string label;
        auto pn = body.find("var playerName");
        if (pn != std::string::npos) label = qualityFromLabel(valueAfterKey(body.substr(pn, 512), "var playerName"));
        out.push_back(makeVideo(direct, trim(label).empty() ? "Sushi Animes" : "Sushi Animes - " + label));
        return out;
    }
};

// ============================================================================================ Dattebayo BR

class DattebayoBR : public PtBase {
  public:
    DattebayoBR() : PtBase("pt.dattebayobr", "Dattebayo BR", "https://www.dattebayo-br.com") {}

    Page popular(int page) override {
        if (page > 1) return {};
        return list(baseUrl(), false);
    }
    Page latest(int page) override { return popular(page); }
    Page search(const std::string& q, int page) override {
        return list(baseUrl() + "/busca?busca=" + http::urlEncode(q) + "&page=" + std::to_string(page), true);
    }

    Details details(const std::string& url) override {
        std::string pageUrl = abs(url);
        auto d = doc(pageUrl);
        Details out;
        out.title = trim(d->selectFirst(".tituloPage h1").text());
        out.thumbnail = d->absUrl(d->selectFirst(".aniInfosSingleCapa img"), "src");
        out.description = trim(d->selectFirst(".aniInfosSingleSinopse p").text());
        out.genre = joinText(d->select(".aniInfosSingleGeneros span"));
        std::string st = asciiLower(d->selectFirst(".anime_status span").text());
        out.status = st == "completo" ? "Completato" : "";

        std::string base = substringBefore(d->url(), "/page/");
        while (!base.empty() && base.back() == '/') base.pop_back();
        std::set<std::string> seen;
        std::vector<Episode> eps;
        for (int page = 1; page <= 100; page++) {
            DocPtr pd;
            if (page == 1)
                pd = std::move(d);
            else
                pd = doc(base + "/page/" + std::to_string(page));
            auto items = pd->select("div.ultimosEpisodiosHomeItem");
            if (items.empty()) break;
            bool added = false;
            for (auto& el : items) {
                html::Node a = el.selectFirst("a");
                if (!a) continue;
                html::Node numNode = el.selectFirst(".ultimosEpisodiosHomeItemInfosNum");
                if (!numNode) continue;
                std::string numText = trim(replaceAll(numNode.text(), "Episódio", ""));
                Episode e;
                e.url = rel(pd->absUrl(a, "href"));
                e.number = kNum(replaceAll(numText, ",", "."), 0);
                html::Node nm = el.selectFirst(".ultimosEpisodiosHomeItemInfosNome");
                e.name = nm ? trim(nm.text()) : "Episódio " + numText;
                if (seen.insert(e.url).second) {
                    eps.push_back(e);
                    added = true;
                }
            }
            if (!added) break;
        }
        std::stable_sort(eps.begin(), eps.end(), [](const Episode& a, const Episode& b) {
            double x = a.number > 0 ? a.number : 1e18, y = b.number > 0 ? b.number : 1e18;
            return x > y;
        });
        for (size_t i = 0; i < eps.size(); i++)
            if (eps[i].number <= 0) eps[i].number = (double)(eps.size() - i);
        out.episodes = eps;
        return out;
    }

    std::vector<Video> videos(const std::string& url) override {
        std::string epUrl = abs(url);
        auto d = doc(epUrl);
        std::vector<Video> out;
        for (auto& aba : d->select("div.AbasBox div.Aba")) {
            try {
                std::string type = aba.attr("aba-type");
                std::string qname = aba.text();
                if (type.empty()) continue;
                html::Node container = d->selectFirst("[id=\"" + type + "\"]");
                if (!container) continue;
                std::string script = container.selectFirst("script").data();
                auto p = script.find("var vid");
                if (p == std::string::npos) continue;
                std::string vid = valueAfterKey(script.substr(p, 4096), "var vid");
                if (vid.empty()) continue;
                http::Headers adH = mergeHeaders(siteHeaders(), {{"Referer", epUrl}, {"Origin", "https://www.dattebayo-br.com"}});
                http::Response r = http::request("GET", "https://ads.animeyabu.net/?url=" + http::urlEncode(vid), adH);
                if (r.status < 200 || r.status >= 300 || !contains(r.body, "publicidade")) continue;
                auto arr = json::parse(r.body);
                if (!arr.is_array() || arr.empty()) continue;
                std::string sig = jstr(arr[0], "publicidade");
                if (trim(sig).empty()) continue;
                out.push_back(makeVideo(vid + sig, qname, adH));
            } catch (const std::exception&) {
            }
        }
        std::stable_sort(out.begin(), out.end(), [](const Video& a, const Video& b) { return a.title > b.title; });
        return nonEmpty(out);
    }

  protected:
    http::Headers siteHeaders() const override {
        return {{"User-Agent",
                 "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 "
                 "Safari/537.36"},
                {"Accept", "*/*"},
                {"Accept-Language", ACCEPT_LANGUAGE},
                {"X-Requested-With", "XMLHttpRequest"}};
    }

  private:
    Page list(const std::string& url, bool paged) {
        auto d = doc(url);
        Page p;
        for (auto& el : d->select("div.ultimosAnimesHomeItem")) {
            html::Node a = el.selectFirst("a");
            if (!a) continue;
            Anime an;
            an.url = rel(d->absUrl(a, "href"));
            an.title = trim(el.selectFirst(".ultimosAnimesHomeItemInfosNome").text());
            if (an.title.empty()) an.title = "Sem título";
            an.thumbnail = d->absUrl(el.selectFirst(".ultimosAnimesHomeItemImg img"), "src");
            if (!an.url.empty()) p.animes.push_back(an);
        }
        if (paged) {
            html::Node next = firstContaining(d->select("div.letterBox a"), "»");
            p.hasNextPage = next && contains(next.attr("href"), "page=");
        }
        return p;
    }
};

// ============================================================================================ Meus Animes

class MeusAnimes : public PtBase {
  public:
    MeusAnimes() : PtBase("pt.meusanimes", "Meus Animes", "https://meusanimes.vip", true, false) {}

    Page popular(int page) override {
        auto d = doc(baseUrl() + "/populares?page=" + std::to_string(page));
        Page p;
        for (auto& a : d->select("div.grid > div > a[href^=\"/anime/\"]")) {
            Anime an;
            an.title = joinText(a.select("h3.text-white"), " ");
            an.url = a.attr("href");
            an.thumbnail = d->absUrl(a.selectFirst("img"), "src");
            p.animes.push_back(an);
        }
        p.hasNextPage = !p.animes.empty();
        return p;
    }
    Page latest(int page) override { return popular(page); }
    Page search(const std::string& q, int page) override {
        if (page > 1) return {};
        auto j = json::parse(get(baseUrl() + "/api/animes?search=" + http::urlEncode(q)));
        Page p;
        if (!j.contains("data") || !j["data"].is_array()) return p;
        for (auto& o : j["data"]) {
            Anime an;
            an.title = jstr(o, "name");
            an.url = "/anime/" + jstr(o, "slug");
            std::string poster = jstr(o, "poster");
            if (!trim(poster).empty()) an.thumbnail = "https://image.tmdb.org/t/p/w500" + poster;
            p.animes.push_back(an);
        }
        return p;
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details out;
        json data;
        bool ok = animeData(*d, data);
        if (!ok) {
            out.title = d->selectFirst("meta[property=og:title]").attr("content");
            out.description = d->selectFirst("meta[name=description]").attr("content");
            out.thumbnail = d->selectFirst("meta[property=og:image]").attr("content");
            return out;
        }
        out.title = jstr(data, "name");
        std::string alt = trim(jstr(data, "nameOriginal"));
        std::string sin = trim(jstr(data, "sinopse"));
        out.description = sin.empty() ? "Sinopse não disponível." : sin;
        if (!alt.empty()) out.description += "\n\nTítulo original: " + alt;
        if (!trim(jstr(data, "diaLancamento")).empty())
            out.status = "In corso";
        else if (kNum(jstr(data, "episodios"), 0) > 0)
            out.status = "Completato";
        std::string poster = jstr(data, "poster");
        out.thumbnail = !trim(poster).empty() ? "https://image.tmdb.org/t/p/w500" + poster
                                              : d->selectFirst("meta[property=og:image]").attr("content");
        if (data.contains("Episode") && data["Episode"].is_array()) {
            for (auto& o : data["Episode"]) {
                Episode e;
                e.name = jstr(o, "name");
                e.number = kNum(jstr(o, "episodeNumber"), 0);
                e.url = "/episodio/" + jstr(o, "slug");
                if (e.name.empty()) e.name = "Episódio " + jstr(o, "episodeNumber");
                out.episodes.push_back(e);
            }
            std::stable_sort(out.episodes.begin(), out.episodes.end(),
                             [](const Episode& a, const Episode& b) { return a.number > b.number; });
        }
        return out;
    }

    std::vector<Video> videos(const std::string& url) override {
        std::string htmlStr = get(abs(url));
        std::string clean = replaceAll(replaceAll(htmlStr, "\\/", "/"), "\\u0026", "&");
        http::Headers gh = {{"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:146.0) Gecko/20100101 Firefox/146.0"},
                            {"Referer", "https://youtube.googleapis.com/"}};
        std::vector<Video> out;
        auto add = [&](const std::string& u, const std::string& prefix) {
            if (u.empty() || !contains(u, "blogger.com")) return;
            try {
                for (auto& v : blogger(u, gh)) {
                    v.title = prefix + ": " + v.title;
                    out.push_back(v);
                }
            } catch (const std::exception&) {
            }
        };
        add(valueAfterKey(clean, "player_leg"), "Legendado");
        add(valueAfterKey(clean, "player_dub"), "Dublado");
        if (out.empty()) {
            std::vector<std::string> found;
            size_t pos = 0;
            const std::string needle = "://www.blogger.com/video.g?token=";
            while (found.size() < 2 && (pos = clean.find(needle, pos)) != std::string::npos) {
                size_t start = pos >= 5 && clean.compare(pos - 5, 5, "https") == 0 ? pos - 5
                               : pos >= 4 && clean.compare(pos - 4, 4, "http") == 0 ? pos - 4
                                                                                   : std::string::npos;
                size_t e = pos + needle.size();
                while (e < clean.size() && (std::isalnum((unsigned char)clean[e]) || clean[e] == '_' || clean[e] == '-')) e++;
                if (start != std::string::npos && e > pos + needle.size()) {
                    std::string u = clean.substr(start, e - start);
                    if (std::find(found.begin(), found.end(), u) == found.end()) found.push_back(u);
                }
                pos = e;
            }
            for (auto& u : found) add(u, "Player");
        }
        sortVideos(out, "Legendado", false);
        return nonEmpty(out);
    }

  protected:
    http::Headers siteHeaders() const override {
        return {{"User-Agent",
                 "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 "
                 "Safari/537.36"},
                {"Referer", baseUrl()}};
    }

  private:
    static bool animeData(const html::Document& d, json& out) {
        std::string script;
        for (auto& s : d.select("script")) {
            std::string data = s.data();
            if (contains(data, "animeData")) {
                script = data;
                break;
            }
        }
        if (script.empty()) return false;
        const std::string start = "\\\"animeData\\\":{";
        auto idx = script.find(start);
        if (idx == std::string::npos) return false;
        size_t js = idx + start.size() - 1;
        auto end = script.find("]}", js);
        if (end == std::string::npos) return false;
        std::string frag = script.substr(js, end + 2 - js);
        frag = replaceAll(replaceAll(frag, "\\\"", "\""), "\\\\", "\\");
        try {
            out = json::parse(frag);
            return out.is_object();
        } catch (const std::exception&) {
            return false;
        }
    }
};

// ============================================================================================ Animes CX

class AnimesCX : public PtBase {
  public:
    AnimesCX() : PtBase("pt.animescx", "Animes CX", "https://animescx.com.br", true) {}

    Page popular(int page) override { return list(baseUrl() + "/doramas-legendados/page/" + std::to_string(page)); }
    Page latest(int page) override { return list(baseUrl() + "/doramas-em-lancamento/page/" + std::to_string(page)); }
    Page search(const std::string& q, int page) override {
        auto d = doc(baseUrl() + "/page/" + std::to_string(page) + "/?s=" + http::urlEncode(q));
        Page p;
        for (auto& art : d->select("article.rl_episodios")) {
            if (!art.selectFirst(".rl_AnimeIndexImg")) continue;
            html::Node a = art.selectFirst("a");
            Anime an;
            an.url = rel(d->absUrl(a, "href"));
            an.title = a.text();
            an.thumbnail = d->absUrl(art.selectFirst("img"), "src");
            if (!an.url.empty()) p.animes.push_back(an);
        }
        p.hasNextPage = d->selectFirst("a.next.page-numbers").valid();
        return p;
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details out;
        html::Node infos = d->selectFirst("div.rl_anime_metadados");
        out.thumbnail = d->absUrl(infos.selectFirst("img"), "src");
        out.title = infos.selectFirst(".rl_nome_anime").text();
        out.genre = replaceAll(info(infos, "Gêneros"), ";", ",");
        std::string st = info(infos, "Status");
        out.status = st == "Completo" ? "Completato" : (st == "Lançando" || st == "Sendo Legendado!") ? "In corso" : st;
        out.description = info(infos, "Sinopse");

        for (int guard = 0; guard < 100; guard++) {
            for (auto& el : d->select(".rl_anime_episodios > article.rl_episodios")) out.episodes.push_back(episode(*d, el));
            if (!hasNext(*d)) break;
            html::Node next = firstContaining(d->select("a.rl_anime_pagination"), "›");
            std::string href = d->absUrl(next, "href");
            if (href.empty()) break;
            d = doc(href);
        }
        std::reverse(out.episodes.begin(), out.episodes.end());
        return out;
    }

    std::vector<Video> videos(const std::string& url) override {
        json data = json::parse(url);
        std::vector<Video> out;
        for (auto it = data.begin(); it != data.end(); ++it) {
            std::string quality = it.key();
            for (auto& host : it.value()) {
                std::string name = jstr(host, "name"), u = jstr(host, "url");
                try {
                    if (name == "MediaFire") {
                        auto v = mediafire(u, "Mediafire - " + quality, siteHeaders());
                        out.insert(out.end(), v.begin(), v.end());
                    } else if (name == "Google Drive") {
                        std::string id = gdriveId(u);
                        if (!id.empty()) {
                            auto v = gdrive(id, "GDrive - " + quality, siteHeaders());
                            out.insert(out.end(), v.begin(), v.end());
                        }
                    }
                } catch (const std::exception&) {
                }
            }
        }
        sortVideos(out, "FULL HD", false);
        return nonEmpty(out);
    }

  protected:
    http::Headers siteHeaders() const override { return {}; }

  private:
    static std::string info(const html::Node& infos, const std::string& key) {
        html::Node n = firstContaining(infos.select(".rl_anime_meta"), key);
        return n ? trim(ownText(n)) : "";
    }

    static std::string pageOf(const std::string& u) { return substringBefore(substringAfterLast(u, "/page/"), "/"); }

    static bool hasNext(const html::Document& d) {
        auto links = d.select("a.rl_anime_pagination");
        if (links.empty()) return false;
        html::Node last = links.back();
        auto sib = last.parent().children();
        if (sib.empty() || sib.back().raw() != last.raw()) return false;
        return pageOf(last.attr("href")) != pageOf(d.url());
    }

    Page list(const std::string& url) {
        auto d = doc(url);
        Page p;
        for (auto& a : d->select("div.listaAnimes_Riverlab_Container > a")) {
            Anime an;
            an.url = rel(d->absUrl(a, "href"));
            an.title = a.selectFirst("div.infolistaAnimes_RiverLab").text();
            an.thumbnail = d->absUrl(a.selectFirst("img"), "src");
            if (!an.url.empty()) p.animes.push_back(an);
        }
        p.hasNextPage = hasNext(*d);
        return p;
    }

    static Episode episode(const html::Document& d, const html::Node& el) {
        (void)d;
        Episode e;
        std::string num = substringAfterLast(el.selectFirst("header").text(), " ");
        e.number = kNum(num, 0);
        e.name = "Episódio " + num;
        html::Node fansub = firstContaining(el.select("div.rl_episodios_info"), "Fansub");
        if (fansub) {
            std::string f = trim(ownText(fansub));
            if (!f.empty()) e.name += " [" + f + "]";
        }
        ojson data = ojson::object();
        for (auto& opt : el.select("div.rl_episodios_opcnome[onclick]")) {
            std::string onclick = opt.attr("onclick");
            std::string itemId = substringBefore(substringAfterLast(onclick, "rlToggle('"), "'");
            ojson arr = ojson::array();
            for (auto& a : el.select("[id=\"" + itemId + "\"] a.rl_episodios_link")) {
                if (a.text() == "Mega") continue;
                std::string urlId = substringAfter(a.attr("href"), "id=");
                urlId = replaceAll(replaceAll(replaceAll(urlId, "%3D", "="), "%2B", "+"), "%2F", "/");
                std::string u = reversed(base64Decode(urlId));
                arr.push_back({{"name", a.text()}, {"url", u}});
            }
            data[opt.text()] = arr;
        }
        e.url = data.dump();
        return e;
    }
};

// ============================================================================================ Fun Anime TV

class FunAnimeTV : public PtBase {
  public:
    FunAnimeTV() : PtBase("pt.funanimetv", "Fun Anime TV", "https://betterclass.click") {}

    Page popular(int page) override {
        if (page > 1) return {};
        json j = call("get_home_videos");
        Page p;
        for (auto& it : j.value("most_viewed", json::array())) {
            Anime an;
            an.url = "/?cid=" + http::urlEncode(jstr(it, "cid"));
            if (jbool(it, "is_temporada")) an.url += "&tid=" + http::urlEncode(jstr(it, "tid"));
            an.title = jstr(it, "category_name");
            an.thumbnail = jstr(it, "category_image");
            p.animes.push_back(an);
        }
        return p;
    }
    Page latest(int page) override {
        if (page > 1) return {};
        json j = call("get_home_videos");
        json cats = j.value("all_video_cat", json::array());
        Page p;
        auto addAll = [&](const char* key, const char* suffix) {
            for (auto& vid : j.value(key, json::array())) {
                Anime an;
                an.title = jstr(vid, "category_name") + " | " + suffix;
                an.thumbnail = jstr(vid, "video_thumbnail_b");
                an.url = "/?id=" + http::urlEncode(jstr(vid, "id"));
                for (auto& c : cats) {
                    if (jstr(c, "category_name") != jstr(vid, "category_name")) continue;
                    an.thumbnail = jstr(c, "category_image");
                    an.url += "&cid=" + http::urlEncode(jstr(c, "cid")) + "&tid=" + http::urlEncode(jstr(c, "tid"));
                    break;
                }
                p.animes.push_back(an);
            }
        };
        addAll("latest_video", "Legendado");
        addAll("latest_video_dub", "Dublado");
        return p;
    }
    Page search(const std::string& q, int page) override {
        if (page > 1) return {};
        ojson extra = {{"search_text", q}, {"title_type", nullptr}, {"content_type", nullptr}, {"category", nullptr}};
        json j = call("get_search_video", extra);
        Page p;
        if (!j.is_array()) return p;
        for (auto& it : j) {
            Anime an;
            bool temp = jbool(it, "is_temporada");
            an.url = "/?cid=" + http::urlEncode(jstr(it, "cid"));
            if (temp) an.url += "&tid=" + http::urlEncode(jstr(it, "tid"));
            an.title = jstr(it, "category_name") + " | " + (temp ? jstr(it, "temp_name") : jstr(it, "audio_type"));
            an.thumbnail = jstr(it, "category_image");
            p.animes.push_back(an);
        }
        return p;
    }

    Details details(const std::string& url) override {
        std::string cid = qp(url, "cid"), tid = qp(url, "tid"), id = qp(url, "id");
        Details out;
        if (cid.empty() && !id.empty()) {
            json arr = call("get_single_video", {{"video_id", id}});
            if (!arr.is_array() || arr.empty()) throw http::Error("Anime non trovato");
            const json& data = arr[0];
            cid = jstr(data, "cat_id");
            tid = jstr(data, "temp_id");
            out.title = jstr(data, "category_name") + " | " + jstr(data, "temp_name");
            out.thumbnail = jstr(data, "temp_image");
        }
        ojson params = {{"cat_id", cid}, {"page", 1}};
        if (!tid.empty()) params["tid"] = tid;
        json eps = call("get_video_by_cat_id", params);
        if (eps.is_array()) {
            for (auto& it : eps) {
                Episode e;
                e.url = "/?id=" + http::urlEncode(jstr(it, "id"));
                std::string epLabel = jstr(it, "video_ep");
                e.name = jstr(it, "video_title");
                if (!epLabel.empty()) e.name += " (" + epLabel + ")";
                auto parts = splitStr(epLabel, " ");
                e.number = parts.size() > 1 ? kNum(parts[1], 1) : 1;
                out.episodes.push_back(e);
            }
        }
        return out;
    }

    std::vector<Video> videos(const std::string& url) override {
        json arr = call("get_single_video", {{"video_id", qp(url, "id")}});
        if (!arr.is_array() || arr.empty()) throw http::Error("Nessun video trovato");
        const json& v = arr[0];
        std::vector<Video> out;
        std::string sd = jstr(v, "video_url_sd"), hd = jstr(v, "video_url"), fhd = jstr(v, "video_url_fhd");
        if (startsWith(fhd, "http")) out.push_back(makeVideo(fhd, "1080p", siteHeaders()));
        if (startsWith(hd, "http")) out.push_back(makeVideo(hd, "720p", siteHeaders()));
        if (startsWith(sd, "http")) out.push_back(makeVideo(sd, "360p", siteHeaders()));
        sortVideos(out, "1080p");
        return nonEmpty(out);
    }

  protected:
    http::Headers siteHeaders() const override {
        return {{"User-Agent", "Dalvik/2.1.0 (Linux; U; Android 16; M2007J20CG Build/BP3A.250905.014)"}};
    }

  private:
    std::mutex constMutex;
    std::string signSalt, arrayKey;

    static std::string requestBody(const std::string& method, const ojson& extra, const std::string& salt0) {
        static thread_local std::mt19937 rng((unsigned)std::random_device{}() ^ (unsigned)std::time(nullptr));
        std::string salt = std::to_string(std::uniform_int_distribution<int>(1, 900)(rng));
        ojson o = ojson::object();
        o["salt"] = salt;
        o["sign"] = crypto::toHex(crypto::md5(salt0 + salt));
        o["method_name"] = method;
        if (extra.is_object())
            for (auto it = extra.begin(); it != extra.end(); ++it) o[it.key()] = it.value();
        return form({{"data", crypto::base64Encode(o.dump(), false, false)}});
    }

    void ensureConstants() {
        std::lock_guard<std::mutex> lock(constMutex);
        if (!signSalt.empty()) return;
        std::string body = requestBody("get_app_details", ojson::object(), "JbWIGaSQOVoJLYCF0RU");
        auto j = json::parse(postForm(baseUrl() + "/valid_g.php", body));
        const json& arr = j.at("FUN_ANIME_01");
        if (!arr.is_array() || arr.empty()) throw http::Error("Fun Anime TV: configurazione non trovata");
        signSalt = jstr(arr[0], "SINGSALT");
        arrayKey = jstr(arr[0], "ARRAYPADRAO");
        if (signSalt.empty() || arrayKey.empty()) throw http::Error("Fun Anime TV: configurazione non valida");
    }

    json call(const std::string& method, const ojson& extra = ojson::object()) {
        ensureConstants();
        std::string salt, key;
        {
            std::lock_guard<std::mutex> lock(constMutex);
            salt = signSalt;
            key = arrayKey;
        }
        auto j = json::parse(postForm(baseUrl() + "/api.php", requestBody(method, extra, salt)));
        if (!j.contains(key)) throw http::Error("Fun Anime TV: risposta non valida");
        return j[key];
    }
};

// ============================================================================================ Donghua no Sekai

class DonghuaNoSekai : public PtBase {
  public:
    DonghuaNoSekai() : PtBase("pt.donghuanosekai", "Donghua no Sekai", "https://donghuanosekai.com") {}

    Page popular(int page) override {
        if (page > 1) return {};
        auto d = doc(baseUrl());
        Page p;
        // barra laterale "Novos Donghuas" (il vecchio menu navItensTop non esiste piu')
        for (auto& a : d->select("div.sidebarContent ul.postsNew li > a, div.sidebarContent div.navItensTop li > a")) {
            Anime an = genericItem(*d, a);
            if (!an.url.empty()) p.animes.push_back(an);
        }
        return p;
    }
    Page latest(int page) override {
        auto d = doc(baseUrl() + "/lancamentos?pagina=" + std::to_string(page));
        Page p;
        for (auto& a : d->select("div.boxContent div.itemE > a")) p.animes.push_back(item(*d, a));
        p.hasNextPage = d->selectFirst("ul.content-pagination > li.next").valid();
        return p;
    }
    Page search(const std::string& q, int page) override {
        // ricerca WordPress (il POST admin-ajax "getListFilter" ora risponde "no_verify_nonce")
        std::string url = page <= 1 ? baseUrl() + "/?s=" + http::urlEncode(q)
                                    : baseUrl() + "/page/" + std::to_string(page) + "/?s=" + http::urlEncode(q);
        Page p;
        http::Response r = http::request("GET", url, siteHeaders());
        if (r.status >= 200 && r.status < 300) {
            html::Document d(r.body, r.finalUrl.empty() ? url : r.finalUrl);
            std::set<std::string> seen;
            for (auto& a : d.select("div.itemE > a, div.itemA > a, div.boxContent article a, div.result-item a")) {
                Anime an = genericItem(d, a);
                if (an.url.empty() || an.title.empty() || !seen.insert(an.url).second) continue;
                p.animes.push_back(an);
            }
            p.hasNextPage = d.selectFirst("ul.content-pagination > li.next, a.next.page-numbers").valid();
        }
        if (!p.animes.empty()) return p;
        return ajaxSearch(q, page);
    }

    Page ajaxSearch(const std::string& q, int page) {
        std::vector<std::pair<std::string, std::string>> f = {
            {"type", "lista"},
            {"action", "getListFilter"},
            {"limit", "30"},
            {"token", searchToken()},
            {"search", trim(q).empty() ? "0" : q},
            {"pagina", std::to_string(page)},
            {"filters",
             "{\"filter_data\": \"filter_animation=all&filter_audio=undefined&filter_letter=0&filter_order=name&"
             "filter_status=all&type_url=ONA\", \"filter_genre_add\": [], \"filter_genre_del\": []}"}};
        Page p;
        try {
            auto j = json::parse(postForm(baseUrl() + "/wp-admin/admin-ajax.php", form(f)));
            for (auto& r : j.value("results", json::array())) {
                if (!r.is_string()) continue;
                html::Document d(r.get<std::string>(), baseUrl() + "/");
                html::Node a = d.selectFirst("div.itemE > a");
                if (a) p.animes.push_back(item(d, a));
            }
            double total = j.contains("total_page") ? kNum(jstr(j, "total_page"), 1) : 1;
            p.hasNextPage = total > kNum(jstr(j, "page"), 0);
        } catch (const std::exception&) {
        }
        return p;
    }

    Details details(const std::string& url) override {
        auto d = realDoc(doc(abs(url)));
        Details out;
        out.thumbnail = d->selectFirst("div.poster > img").attr("src");
        if (out.thumbnail.empty()) out.thumbnail = d->selectFirst("meta[property=og:image]").attr("content");
        html::Node infos = d->selectFirst("div.dados");
        out.title = infos.selectFirst("h1").text();
        if (out.title.empty()) out.title = d->selectFirst("h1").text();
        out.genre = joinText(infos.select("div.genresL > a"));
        auto lis = infos.select("ul > li");
        auto li = [&](const std::string& key) {
            html::Node n = firstContaining(lis, key);
            return n ? trim(ownText(n)) : std::string();
        };
        out.author = joinStr({li("Estúdio"), li("Fansub")});
        std::string st = asciiLower(li("Status"));
        out.status = st == "completo" ? "Completato" : st == "em lançamento" ? "In corso" : st == "em pausa" ? "In pausa" : "";

        std::vector<std::string> paras;
        for (auto& art : d->select("div.articleContent")) {
            bool ok = false;
            for (auto& div : art.select("div"))
                if (contains(div.text(), "Sinopse")) ok = true;
            if (!ok) continue;
            for (auto& p : art.select("div.context > p")) {
                if (p.parent().parent().raw() == art.raw()) paras.push_back(p.text());
            }
        }
        std::string desc = joinStr(paras, "\n\n") + "\n";
        for (auto& l : infos.select("ul.b_flex > li")) desc += "\n" + l.text();
        out.description = trim(desc);

        for (auto& a : d->select("div.episode_list > div.item > a")) {
            Episode e;
            e.url = rel(d->absUrl(a, "href"));
            e.name = a.selectFirst("span.episode").text();
            e.number = kNum(substringAfterLast(e.name, " "), 0);
            out.episodes.push_back(e);
        }
        return out;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::vector<Video> out;
        for (auto& slide : d->select("div.slideItem[data-video-url]")) {
            try {
                auto pd = doc(slide.attr("data-video-url"));
                auto v = fromPlayer(*pd);
                out.insert(out.end(), v.begin(), v.end());
            } catch (const std::exception&) {
            }
        }
        sortVideos(out, "720p", false);
        return nonEmpty(out);
    }

  protected:
    http::Headers siteHeaders() const override { return {{"Referer", baseUrl()}, {"Origin", baseUrl()}}; }

  private:
    std::mutex tokenMutex;
    std::string token;

    std::string searchToken() {
        std::lock_guard<std::mutex> lock(tokenMutex);
        if (token.empty()) token = doc(baseUrl() + "/donghuas")->selectFirst("div.menu_filter_box").attr("data-secury");
        return token;
    }

    Anime item(const html::Document& d, const html::Node& a) {
        Anime an;
        an.url = rel(d.absUrl(a, "href"));
        an.title = a.selectFirst("div.title h3").text();
        an.thumbnail = a.selectFirst("div.thumb img").attr("src");
        return an;
    }

    Anime genericItem(const html::Document& d, const html::Node& a) {
        Anime an;
        an.url = rel(d.absUrl(a, "href"));
        for (const char* sel : {"div.title h3", "h4.title", "h3", "h2", "span.title_anime", ".title"}) {
            an.title = trim(a.selectFirst(sel).text());
            if (!an.title.empty()) break;
        }
        if (an.title.empty()) an.title = trim(a.attr("title"));
        if (an.title.empty()) an.title = trim(a.attr("alt"));
        if (startsWith(an.title, "Assistir ")) an.title = an.title.substr(9);
        html::Node img = a.selectFirst("img");
        an.thumbnail = img.attr("src");
        if (an.thumbnail.empty() || startsWith(an.thumbnail, "data:")) an.thumbnail = img.attr("data-src");
        return an;
    }

    DocPtr realDoc(DocPtr d) {
        html::Node link = d->selectFirst("div.controles li.list-ep > a");
        if (!link) return d;
        return doc(abs(link.attr("href")));
    }

    std::vector<Video> fromPlayer(const html::Document& d) {
        std::string type = http::queryParam(d.url(), "type");
        int id = type.empty() ? 1 : (int)kNum(type, 0) + 1;
        std::string player = "Player " + std::to_string(id);
        html::Node iframe = d.selectFirst("iframe");
        if (!iframe) {
            html::Node source = d.selectFirst("video > source");
            if (!source) return {};
            std::string u = source.attr("src");
            return {makeVideo(u, player + " - " + source.attr("size") + "p", siteHeaders())};
        }
        std::string iu = iframe.attr("src");
        if (contains(iu, "nativov2.php") || contains(iu, "/embed2/")) {
            std::string u = qp(iu, "id");
            if (u.empty()) u = qp(iu, "v");
            if (u.empty()) return {};
            std::string q = substringBefore(substringAfter(u, "_"), "_");
            return {makeVideo(u, player + " - " + q, siteHeaders())};
        }
        if (contains(iu, "playerB.php")) {
            std::string body = get(fixUrl(iu, d.url()));
            std::string part = substringBefore(substringAfter(body, "sources:"), "]");
            auto items = splitStr(part, "{");
            std::vector<Video> out;
            for (size_t i = 1; i < items.size(); i++) {
                std::string u = substringBefore(substringAfter(items[i], "file: \""), "\"");
                std::string q = substringBefore(substringAfter(items[i], "label: \""), "\"");
                if (q == "SD") q = "480p";
                else if (q == "HD") q = "720p";
                else if (q == "FHD" || q == "FULLHD") q = "1080p";
                if (startsWith(u, "http")) out.push_back(makeVideo(u, player + " - " + q, siteHeaders()));
            }
            return out;
        }
        return {};
    }
};

// ============================================================================================ Doramogo

class Doramogo : public PtBase {
  public:
    Doramogo() : PtBase("pt.doramogo", "Doramogo", "https://doramogo.com") {}

    Page popular(int page) override {
        if (page > 1) return {};
        return list(baseUrl() + "/doramas/?filter_order=popular");
    }
    Page latest(int page) override {
        if (page > 1) return {};
        return list(baseUrl() + "/doramas/?filter_orderby=date");
    }
    Page search(const std::string& q, int page) override {
        if (page > 1) return {};
        return list(baseUrl() + "/search/" + http::urlEncode(q));
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details out;
        out.title = d->selectFirst("div.dados h1").text();
        std::string style = d->selectFirst("div.image--cover").attr("style");
        out.thumbnail = substringBefore(substringAfter(style, "background-image: url('"), "')");
        out.description = trim(ownText(d->selectFirst("p.readMor")));
        for (auto& li : d->select("li.episode--content")) {
            Episode e;
            e.url = rel(d->absUrl(li.selectFirst("a"), "href"));
            std::string t = ownText(li.selectFirst("div.title-episode a"));
            e.name = substringAfter(t, ". ");
            e.number = kNum(substringBefore(t, ". "), 1);
            out.episodes.push_back(e);
        }
        std::reverse(out.episodes.begin(), out.episodes.end());
        return out;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::vector<Video> out;
        for (auto& f : d->select("div.source-box iframe[src]")) {
            try {
                auto v = fromUrl(fixUrl(f.attr("src"), d->url()));
                out.insert(out.end(), v.begin(), v.end());
            } catch (const std::exception&) {
            }
        }
        sortVideos(out, "");
        return nonEmpty(out);
    }

  protected:
    http::Headers siteHeaders() const override { return {{"Referer", baseUrl()}, {"Origin", baseUrl()}}; }

  private:
    Page list(const std::string& url) {
        auto d = doc(url);
        Page p;
        for (auto& el : d->select("div.item-drm")) {
            Anime an;
            an.url = rel(d->absUrl(el.selectFirst("a"), "href"));
            an.title = el.selectFirst("div.title h3").text();
            an.thumbnail = el.selectFirst("div.cover > img").attr("src");
            if (!an.url.empty()) p.animes.push_back(an);
        }
        return p;
    }

    std::vector<Video> fromUrl(const std::string& url) {
        if (contains(url, "dailymotion")) return dailymotion(url, "Dailymotion - ");
        if (contains(url, "ok.ru")) return okru(url, "Okru - ");
        if (contains(url, "drive.google.com")) {
            std::string id = gdriveId(url);
            return id.empty() ? std::vector<Video>{} : gdrive(id, "GDrive", siteHeaders());
        }
        if (contains(url, "embedrise.com")) {
            html::Document pd(http::getText(url), url);
            std::string m3u8 = pd.selectFirst("video source").attr("src");
            if (m3u8.empty()) return {};
            return hlsVideos(fixUrl(m3u8, url), url, "Embedrise - ");
        }
        if (contains(url, "streamable.com")) {
            html::Document pd(http::getText(url), url);
            std::string mp4 = pd.selectFirst("video").attr("src");
            if (mp4.empty()) return {};
            if (startsWith(mp4, "//")) mp4 = "https:" + mp4;
            return {makeVideo(mp4, "Streamable", siteHeaders())};
        }
        if (contains(url, "/player/")) {
            auto pd = doc(url);
            std::string script;
            for (auto& s : pd->select("script")) {
                std::string data = s.data();
                if (contains(data, "eval") && contains(data, "p,a,c,k,e,d")) {
                    script = data;
                    break;
                }
            }
            if (script.empty()) return {};
            // lettere accentate (UTF-8 C3 A0..C3 BC) -> '-' come nel Kotlin (bug di JsUnpacker)
            std::string fixed;
            for (size_t i = 0; i < script.size(); i++) {
                unsigned char c = script[i];
                if (c == 0xC3 && i + 1 < script.size() && (unsigned char)script[i + 1] >= 0xA0 &&
                    (unsigned char)script[i + 1] <= 0xBC) {
                    fixed += '-';
                    i++;
                } else {
                    fixed += (char)c;
                }
            }
            std::string unpacked = unpacker::unpackAndCombine(fixed);
            if (unpacked.empty()) return {};
            std::string part = substringBefore(substringAfter(unpacked, "sources:"), "]");
            auto items = splitStr(part, "{");
            std::vector<Video> out;
            for (size_t i = 1; i < items.size(); i++) {
                std::string u = substringBefore(substringAfter(items[i], "file:\\'"), "\\'");
                std::string q = substringBefore(substringAfter(items[i], "label:\\'"), "\\'");
                if (!startsWith(u, "http")) {
                    // variante senza backslash
                    u = substringBefore(substringAfter(items[i], "file:'"), "'");
                    q = substringBefore(substringAfter(items[i], "label:'"), "'");
                }
                if (startsWith(u, "http")) out.push_back(makeVideo(u, "Doramogo - " + q, siteHeaders()));
            }
            return out;
        }
        return {};
    }
};

// ============================================================================================ Animes Games

class AnimesGames : public PtBase {
  public:
    AnimesGames() : PtBase("pt.animesgames", "Animes Games", "https://animesgames.cc") {}

    Page popular(int page) override {
        if (page > 1) return {};
        auto d = doc(baseUrl());
        Page p;
        for (auto& a : d->select("ul.top10 > li > a")) {
            Anime an;
            an.url = rel(d->absUrl(a, "href"));
            an.title = a.text();
            if (!an.url.empty()) p.animes.push_back(an);
        }
        return p;
    }
    Page latest(int page) override {
        auto d = doc(baseUrl() + "/lancamentos/page/" + std::to_string(page));
        Page p;
        for (auto& a : d->select("div.conteudo section.episodioItem > a")) {
            Anime an;
            an.url = rel(d->absUrl(a, "href"));
            an.title = a.selectFirst("div.tituloEP").text();
            an.thumbnail = imageUrl(*d, a.selectFirst("img"));
            if (!an.url.empty()) p.animes.push_back(an);
        }
        p.hasNextPage = firstContaining(d->select("ol.pagination > a"), ">").valid();
        return p;
    }
    Page search(const std::string& q, int page) override {
        std::vector<std::pair<std::string, std::string>> f = {
            {"pagina", std::to_string(page)},
            {"type", "lista"},
            {"type_url", "anime"},
            {"limit", "30"},
            {"token", searchToken()},
            {"search", trim(q).empty() ? "0" : q},
            {"filters",
             "{\"filter_data\": \"filter_audio=0&filter_letter=0&filter_order=name&filter_sort=abc\", "
             "\"filter_genre_add\": [], \"filter_genre_del\": []}"}};
        auto j = json::parse(postForm(baseUrl() + "/func/listanime", form(f)));
        Page p;
        for (auto& r : j.value("results", json::array())) {
            if (!r.is_string()) continue;
            html::Document d(r.get<std::string>(), baseUrl() + "/");
            html::Node a = d.selectFirst("section.animeItem > a");
            if (!a) continue;
            Anime an;
            an.url = rel(d.absUrl(a, "href"));
            an.title = a.selectFirst("div.tituloAnime").text();
            an.thumbnail = imageUrl(d, a.selectFirst("img"));
            p.animes.push_back(an);
        }
        double total = j.contains("total_page") ? kNum(jstr(j, "total_page"), 1) : 1;
        p.hasNextPage = total > kNum(jstr(j, "page"), 0);
        return p;
    }

    Details details(const std::string& url) override {
        auto d = realDoc(doc(abs(url)));
        Details out;
        html::Node content = d->selectFirst("section.conteudoPost");
        std::string title = content.selectFirst("section > h1").text();
        if (startsWith(title, "Assistir ")) title = title.substr(9);
        if (endsWith(title, "Temporada Online")) title = title.substr(0, title.size() - 16);
        out.title = trim(title);
        out.thumbnail = imageUrl(*d, content.selectFirst("img"));
        out.description = joinText(content.select("section.sinopseEp p"), "\n");
        html::Node infos = content.selectFirst("div.info > ol");
        std::string author = info(infos, "Autor");
        if (author.empty()) author = info(infos, "Diretor");
        out.author = joinStr({author, info(infos, "Estúdio")});
        std::string st = info(infos, "Status");
        out.status = st == "Completo" ? "Completato" : st == "Lançamento" ? "In corso" : st;

        for (auto& a : d->select("div.listaEp > section.episodioItem > a")) {
            Episode e;
            e.url = rel(d->absUrl(a, "href"));
            e.name = a.selectFirst("div.tituloEP").text();
            e.number = kNum(substringAfterLast(e.name, " "), 1);
            out.episodes.push_back(e);
        }
        std::reverse(out.episodes.begin(), out.episodes.end());
        return out;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::string link = d->selectFirst("div.Link > a").attr("href");
        if (link.empty()) throw http::Error("Nessun video trovato");
        auto pd = doc(fixUrl(link, d->url()));
        std::vector<Video> out;
        html::Node iframe = pd->selectFirst("iframe");
        if (iframe) {
            out = blogger(fixUrl(iframe.attr("src"), pd->url()), siteHeaders());
        } else {
            std::string script = scriptWith(*pd, {"jw = {"});
            if (!script.empty()) {
                std::string pl = replaceAll(substringBefore(substringAfter(script, "file\":\""), "\""), "\\", "");
                if (endsWith(pl, "m3u8"))
                    out = hlsVideos(pl, baseUrl(), "");
                else if (startsWith(pl, "http"))
                    out.push_back(makeVideo(pl, "Default", siteHeaders()));
            }
        }
        sortVideos(out, "");
        return nonEmpty(out);
    }

  protected:
    http::Headers siteHeaders() const override { return {{"Referer", baseUrl()}, {"Origin", baseUrl()}}; }

  private:
    std::mutex tokenMutex;
    std::string token;

    std::string searchToken() {
        std::lock_guard<std::mutex> lock(tokenMutex);
        if (token.empty())
            token = doc(baseUrl() + "/lista-de-animes")->selectFirst("div.menu_filter_box").attr("data-secury");
        return token;
    }

    static std::string imageUrl(const html::Document& d, const html::Node& img) {
        if (!img) return "";
        std::string u;
        if (img.hasAttr("data-src"))
            u = d.absUrl(img, "data-src");
        else if (img.hasAttr("data-lazy-src"))
            u = d.absUrl(img, "data-lazy-src");
        else if (img.hasAttr("srcset"))
            u = substringBefore(d.absUrl(img, "srcset"), " ");
        else
            u = d.absUrl(img, "src");
        return substringBefore(u, "?resize");
    }

    static std::string info(const html::Node& infos, const std::string& key) {
        for (auto& li : infos.select("li")) {
            bool ok = false;
            for (auto& s : li.select("span"))
                if (contains(s.text(), key)) ok = true;
            if (!ok) continue;
            html::Node data = li.selectFirst("span[data]");
            return data ? data.text() : trim(ownText(li));
        }
        return "";
    }

    DocPtr realDoc(DocPtr d) {
        if (!contains(d->url(), "/video/")) return d;
        for (auto& a : d->select("div.linksEP > a")) {
            if (!a.selectFirst("li.episodio")) continue;
            return doc(abs(a.attr("href")));
        }
        return d;
    }
};

// ============================================================================================ Muito Hentai

class MuitoHentai : public PtBase {
  public:
    MuitoHentai() : PtBase("pt.muitohentai", "Muito Hentai", "https://www.muitohentai.com", true) {}

    Page popular(int page) override {
        auto d = doc(baseUrl() + "/ranking-hentais/?paginacao=" + std::to_string(page));
        Page p;
        for (auto& li : d->select("ul.ul_sidebar > li")) {
            auto bs = li.select("div.lefthentais > div > b");
            html::Node link;
            for (size_t i = 1; i < bs.size() && !link; i++) link = bs[i].selectFirst("a.series");
            if (!link) continue;
            Anime an;
            an.thumbnail = li.selectFirst("div.zeroleft > a > img").attr("src");
            an.url = rel(d->absUrl(link, "href"));
            an.title = link.text();
            p.animes.push_back(an);
        }
        p.hasNextPage = firstContaining(d->select("div.paginacao > a"), "»").valid();
        return p;
    }
    Page latest(int page) override {
        if (page > 1) return {};
        auto d = doc(baseUrl() + "/");
        Page p;
        for (auto& art : d->select("div.animation-2 > article")) {
            if (!contains(art.text(), "Episódio")) continue;
            std::string slug =
                substringBefore(substringAfter(art.selectFirst("a").attr("href"), "/episodios/"), "-episodio");
            Anime an;
            an.url = "/info/" + slug;
            html::Node img = art.selectFirst("img");
            an.title = img.attr("alt");
            an.thumbnail = img.attr("src");
            p.animes.push_back(an);
        }
        return p;
    }
    Page search(const std::string& q, int page) override {
        if (page > 1) return {};
        auto d = doc(baseUrl() + "/buscar/" + http::urlEncode(q));
        Page p;
        for (auto& el : d->select("div#archive-content > article > div.poster")) {
            Anime an;
            an.url = rel(d->absUrl(el.selectFirst("a"), "href"));
            html::Node img = el.selectFirst("img");
            an.title = img.attr("alt");
            an.thumbnail = img.attr("src");
            if (!an.url.empty()) p.animes.push_back(an);
        }
        return p;
    }

    Details details(const std::string& url) override {
        auto d = doc(abs(url));
        Details out;
        html::Node data = d->selectFirst("div.sheader > div.data");
        out.title = data.selectFirst("h1").text();
        std::vector<std::string> genres;
        for (auto& c : data.selectFirst("div.sgeneros").children())
            if (!contains(c.text(), out.title)) genres.push_back(c.text());
        out.genre = joinStr(genres);
        out.description = data.selectFirst("div#info1 > div.wp-content > p").text();
        out.thumbnail = d->selectFirst("div.sheader > div.poster > img").attr("src");
        for (auto& art : d->select("article.item")) {
            Episode e;
            e.url = rel(d->absUrl(art.selectFirst("div.poster > div.season_m > a"), "href"));
            e.name = trim(art.selectFirst("div.data h3").text());
            e.number = firstNumber(e.name);
            if (!e.url.empty()) out.episodes.push_back(e);
        }
        std::reverse(out.episodes.begin(), out.episodes.end());
        return out;
    }

    std::vector<Video> videos(const std::string& url) override {
        auto d = doc(abs(url));
        std::string src = d->selectFirst("div.playex > div#option-0 > iframe").attr("src");
        if (src.empty()) throw http::Error("Nessun video trovato");
        std::string idplay = substringAfter(src, "?idplay=");
        html::Document pd(http::getText("https://www.hentaitube.online/players_sites/mt/index.php?idplay=" + idplay),
                          "https://www.hentaitube.online/");
        std::vector<Video> out;
        for (auto& s : pd.select("source")) {
            std::string u = s.attr("src");
            if (!u.empty()) out.push_back(makeVideo(fixUrl(u, pd.url()), s.attr("label")));
        }
        sortVideos(out, "");
        return nonEmpty(out);
    }

  protected:
    http::Headers siteHeaders() const override { return {}; }

  private:
    static double firstNumber(const std::string& s) {
        for (size_t i = 0; i < s.size(); i++) {
            if (!std::isdigit((unsigned char)s[i])) continue;
            size_t e = i;
            while (e < s.size() && (std::isdigit((unsigned char)s[e]) || s[e] == '.')) e++;
            return std::atof(s.substr(i, e - i).c_str());
        }
        return -1;
    }
};

}  // namespace

std::vector<std::shared_ptr<Source>> makePortugueseSources() {
    return {
        std::make_shared<AnimeFire>(),     std::make_shared<Anitube>(),        std::make_shared<Goyabu>(),
        std::make_shared<AnimesOnlineVip>(), std::make_shared<AnimesDigital>(), std::make_shared<AnimeCore>(),
        std::make_shared<SushiAnimes>(),   std::make_shared<DattebayoBR>(),    std::make_shared<MeusAnimes>(),
        std::make_shared<AnimesCX>(),      std::make_shared<FunAnimeTV>(),     std::make_shared<DonghuaNoSekai>(),
        std::make_shared<Doramogo>(),      std::make_shared<AnimesGames>(),    std::make_shared<MuitoHentai>(),
    };
}

}  // namespace src
