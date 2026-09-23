// Porting in C++ dell'estensione Aniyomi "AnimeUnity" (it.animeunity, v14.9).

#include <nlohmann/json.hpp>

#include <algorithm>

#include "html/html.hpp"
#include "sources/registry.hpp"

using json = nlohmann::json;

namespace src {

static std::string jstr(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return "";
    if (it->is_string()) return it->get<std::string>();
    return it->dump();
}

class AnimeUnity : public Source {
  public:
    std::string id() const override { return "7762808921754601430"; }
    std::string name() const override { return "AnimeUnity"; }
    std::string defaultBaseUrl() const override { return "https://www.animeunity.so"; }

    Page popular(int page) override {
        std::string body = http::getText(baseUrl() + "/top-anime?popular=true&page=" + std::to_string(page));
        return parseTopAnime(body);
    }

    Page latest(int page) override {
        std::string url = baseUrl() + "/?anime=" + std::to_string(page);
        html::Document doc(http::getText(url), url);
        Page p;
        for (auto& el : doc.select("div.home-wrapper-body > div.row > div.latest-anime-container")) {
            Anime a;
            a.title = textOfAll(el.select("a > strong"));
            std::string href = el.selectFirst("a").attr("href");
            a.url = substringBefore(substringAfter(href, "/anime/"), "/");
            a.thumbnail = el.selectFirst("img").attr("src");
            if (!a.url.empty()) p.animes.push_back(a);
        }
        p.hasNextPage = false;
        auto items = doc.select("ul.pagination > li");
        bool afterActive = false;
        for (auto& li : items) {
            std::string cls = " " + li.attr("class") + " ";
            if (afterActive) {
                p.hasNextPage = true;
                break;
            }
            if (cls.find(" active ") != std::string::npos) afterActive = true;
        }
        return p;
    }

    Page search(const std::string& query, int page) override {
        std::string archivio = baseUrl() + "/archivio";
        http::Response ar = http::request("GET", archivio);
        if (ar.status >= 400) throw http::Error("AnimeUnity: HTTP " + std::to_string(ar.status));
        html::Document doc(ar.body, ar.finalUrl);
        std::string csrf = doc.selectFirst("meta[name=csrf-token]").attr("content");

        http::Headers h = sessionHeaders(ar);
        h.push_back({"X-CSRF-TOKEN", csrf});
        h.push_back({"Accept-Language", "en-US,en;q=0.5"});
        h.push_back({"Accept", "application/json, text/plain, */*"});
        h.push_back({"Content-Type", "application/json;charset=utf-8"});
        h.push_back({"Origin", baseUrl()});
        h.push_back({"Referer", ar.finalUrl.empty() ? archivio : ar.finalUrl});
        h.push_back({"X-Requested-With", "XMLHttpRequest"});

        json body = {
            {"title", query.empty() ? json(false) : json(query)},
            {"type", false},
            {"year", false},
            {"order", false},
            {"status", false},
            {"genres", false},
            {"offset", (page - 1) * 30},
            {"dubbed", false},
            {"season", false},
        };
        http::Response r = http::request("POST", baseUrl() + "/archivio/get-animes", h, body.dump());
        if (r.status >= 400) throw http::Error("AnimeUnity: ricerca non riuscita (HTTP " + std::to_string(r.status) + ")");
        json data = json::parse(r.body);
        Page p;
        for (auto& rec : data.value("records", json::array())) {
            Anime a;
            a.title = displayTitle(rec);
            a.thumbnail = jstr(rec, "imageurl");
            a.url = jstr(rec, "id") + "-" + jstr(rec, "slug");
            p.animes.push_back(a);
        }
        int tot = data.value("tot", 0);
        p.hasNextPage = tot - page * 30 >= 30 && tot > 30;
        return p;
    }

    Details details(const std::string& url) override {
        std::string pageUrl = baseUrl() + "/anime/" + url;
        http::Response resp = http::request("GET", pageUrl);
        if (resp.status >= 400) throw http::Error("AnimeUnity: HTTP " + std::to_string(resp.status));
        html::Document doc(resp.body, resp.finalUrl.empty() ? pageUrl : resp.finalUrl);
        html::Node player = doc.selectFirst("video-player[episodes_count]");
        if (!player) throw http::Error("AnimeUnity: pagina dell'anime non riconosciuta");

        Details d;
        json info = json::parse(player.attr("anime"));
        d.title = displayTitle(info);
        d.thumbnail = jstr(info, "imageurl");
        std::string status = jstr(info, "status");
        d.status = status == "In Corso" ? "In corso" : status == "Terminato" ? "Completato" : status;
        d.author = jstr(info, "studio");
        std::string genres;
        for (auto& g : info.value("genres", json::array())) {
            if (!genres.empty()) genres += ", ";
            genres += jstr(g, "name");
        }
        d.genre = genres;
        d.description = jstr(info, "plot") + "\n\nTipo: " + jstr(info, "type") + "\nStagione: " + jstr(info, "season") +
                        " " + jstr(info, "date");
        std::string score = jstr(info, "score");
        if (!score.empty()) d.description += "\nValutazione: " + score;

        std::string animeId = substringBefore(url, "-");
        std::string episodeBase = "/anime/" + substringBefore(url, "/");
        auto addEpisodes = [&](const json& arr) {
            for (auto& e : arr) {
                if (!e.contains("id") || e["id"].is_null()) continue;
                Episode ep;
                std::string num = jstr(e, "number");
                ep.name = "Episodio " + num;
                ep.number = parseNumber(substringBefore(num, "-"), 0);
                ep.url = episodeBase + "/" + jstr(e, "id");
                d.episodes.push_back(ep);
            }
        };
        addEpisodes(json::parse(player.attr("episodes")));

        int count = std::atoi(player.attr("episodes_count").c_str());
        if (count > 120) {
            http::Headers h = sessionHeaders(resp);
            h.push_back({"X-CSRF-TOKEN", doc.selectFirst("meta[name=csrf-token]").attr("content")});
            h.push_back({"Content-Type", "application/json"});
            h.push_back({"Referer", pageUrl});
            h.push_back({"Accept", "application/json, text/plain, */*"});
            h.push_back({"X-Requested-With", "XMLHttpRequest"});
            for (int start = 121; start <= count; start += 120) {
                int end = std::min(start + 119, count);
                std::string api = baseUrl() + "/info_api/" + animeId + "/1?start_range=" + std::to_string(start) +
                                  "&end_range=" + std::to_string(end);
                try {
                    json j = json::parse(http::getText(api, h));
                    addEpisodes(j.value("episodes", json::array()));
                } catch (const std::exception&) {
                    break;
                }
            }
        }
        std::stable_sort(d.episodes.begin(), d.episodes.end(),
                         [](const Episode& a, const Episode& b) { return a.number > b.number; });
        return d;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::string pageUrl = baseUrl() + episodeUrl;
        html::Document doc(http::getText(pageUrl), pageUrl);
        html::Node player = doc.selectFirst("video-player[embed_url]");
        if (!player) throw http::Error("AnimeUnity: player non trovato");
        std::string iframeUrl = doc.absUrl(player, "embed_url");

        http::Headers ih = {
            {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8"},
            {"Referer", baseUrl() + "/"},
        };
        html::Document iframe(http::getText(iframeUrl, ih), iframeUrl);
        std::string script;
        for (auto& s : iframe.select("script")) {
            std::string data = s.data();
            if (data.find("masterPlaylist") != std::string::npos) {
                script = data;
                break;
            }
        }
        if (script.empty()) throw http::Error("AnimeUnity: playlist non trovata");

        std::string playlistUrl = quotedAfter(script, "url:");
        std::string expires = quotedAfter(script, "'expires':");
        std::string token = quotedAfter(script, "'token':");
        if (playlistUrl.empty() || token.empty()) throw http::Error("AnimeUnity: dati del video incompleti");

        std::string master = playlistUrl + (playlistUrl.find('?') != std::string::npos ? "&" : "?") +
                             "h=1&token=" + token + "&expires=" + expires;

        std::vector<Video> out;
        // la playlist principale lascia a mpv la scelta automatica della qualita'
        Video autoV;
        autoV.title = "Automatica";
        autoV.url = master;
        autoV.referer = iframeUrl;
        autoV.quality = 0;

        try {
            std::string pl = http::getText(master);
            size_t pos = 0;
            while (pos < pl.size()) {
                auto nl = pl.find('\n', pos);
                std::string line = trim(pl.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
                pos = nl == std::string::npos ? pl.size() : nl + 1;
                if (line.find("vixcloud") == std::string::npos || line.find("type=video") == std::string::npos) continue;
                std::string rendition = substringBefore(substringAfter(line, "rendition="), "&");
                // come l'estensione: aggiunge .m3u8 al nome della playlist se manca
                std::string beforeQ = substringBefore(line, "?");
                if (beforeQ.size() < 5 || beforeQ.compare(beforeQ.size() - 5, 5, ".m3u8") != 0)
                    line = beforeQ + ".m3u8" + line.substr(beforeQ.size());
                Video v;
                v.url = line;
                v.title = rendition;
                v.quality = std::atoi(rendition.c_str());
                v.referer = iframeUrl;
                out.push_back(v);
            }
        } catch (const std::exception&) {
        }
        std::sort(out.begin(), out.end(), [](const Video& a, const Video& b) { return a.quality > b.quality; });
        out.push_back(autoV);  // riserva: se le singole qualita' non si caricano
        return out;
    }

  private:
    static std::string displayTitle(const json& j) {
        std::string t = jstr(j, "title_eng");
        if (t.empty()) t = jstr(j, "title");
        return t;
    }

    static std::string textOfAll(const std::vector<html::Node>& nodes) { return html::textOf(nodes); }

    /** Valore tra apici dopo una chiave: url: 'xxx' */
    static std::string quotedAfter(const std::string& s, const std::string& key) {
        auto p = s.find(key);
        if (p == std::string::npos) return "";
        p += key.size();
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n')) p++;
        if (p >= s.size() || (s[p] != '\'' && s[p] != '"')) return "";
        char q = s[p++];
        auto end = s.find(q, p);
        return end == std::string::npos ? "" : s.substr(p, end - p);
    }

    Page parseTopAnime(const std::string& body) {
        // l'elenco e' un JSON nell'attributo animes="..." del tag <top-anime>
        std::string marker = "top-anime animes=\"";
        auto p = body.find(marker);
        if (p == std::string::npos) throw http::Error("AnimeUnity: elenco non trovato");
        std::string raw = body.substr(p + marker.size());
        raw = substringBefore(raw, "\"></top-anime>");
        raw = replaceAll(raw, "&quot;", "\"");
        raw = replaceAll(raw, "&amp;", "&");
        json parsed = json::parse(raw);
        Page page;
        for (auto& a : parsed.value("data", json::array())) {
            Anime an;
            an.title = displayTitle(a);
            an.url = jstr(a, "id") + "-" + jstr(a, "slug");
            an.thumbnail = jstr(a, "imageurl");
            page.animes.push_back(an);
        }
        page.hasNextPage = parsed.value("current_page", 0) < parsed.value("last_page", 0);
        return page;
    }

    /** Header X-XSRF-TOKEN e cookie di sessione presi dalla risposta, come fa l'estensione. */
    static http::Headers sessionHeaders(const http::Response& r) {
        http::Headers h;
        std::string cookies;
        auto range = r.headers.equal_range("set-cookie");
        for (auto it = range.first; it != range.second; ++it) {
            const std::string& c = it->second;
            if (c.rfind("XSRF-TOKEN", 0) == 0)
                h.push_back({"X-XSRF-TOKEN", replaceAll(substringBefore(substringAfter(c, "="), ";"), "%3D", "=")});
            if (c.rfind("animeunity_session", 0) == 0) {
                if (!cookies.empty()) cookies += "; ";
                cookies += replaceAll(substringBefore(c, ";"), "%3D", "=");
            }
        }
        if (!cookies.empty()) h.push_back({"Cookie", cookies});
        return h;
    }
};

std::shared_ptr<Source> makeAnimeUnity() { return std::make_shared<AnimeUnity>(); }

}  // namespace src
