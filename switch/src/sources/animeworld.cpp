// Porting in C++ dell'estensione Aniyomi "ANIMEWORLD.tv" (it.animeworld, v14.59).

#include <nlohmann/json.hpp>

#include <algorithm>
#include <mutex>

#include "html/html.hpp"
#include "sources/registry.hpp"

namespace src {

class AnimeWorld : public Source {
  public:
    std::string id() const override { return "6601315231151625432"; }
    std::string name() const override { return "AnimeWorld"; }
    std::string defaultBaseUrl() const override { return "https://www.animeworld.ac"; }

    Page popular(int page) override { return listPage(baseUrl() + "/filter?sort=6&page=" + std::to_string(page)); }
    Page latest(int page) override { return listPage(baseUrl() + "/updated?page=" + std::to_string(page)); }
    Page search(const std::string& q, int page) override {
        return listPage(baseUrl() + "/filter?keyword=" + http::urlEncode(q) + "&page=" + std::to_string(page));
    }

    Details details(const std::string& url) override {
        std::string pageUrl = baseUrl() + url;
        html::Document doc(fetch(pageUrl), pageUrl);
        Details d;
        d.thumbnail = doc.selectFirst("div.thumb img").attr("src");
        d.title = doc.selectFirst("div.c1 h2.title").text();
        d.description = doc.selectFirst("div.desc").text();

        // dd:has(a[href*=...]) non e' supportato dal motore CSS: si filtra a mano
        std::vector<std::string> genres, studios;
        std::string status;
        for (auto& dd : doc.select("div.info dl dd")) {
            for (auto& a : dd.select("a")) {
                std::string href = a.attr("href");
                if (href.find("language") != std::string::npos || href.find("genre") != std::string::npos)
                    genres.push_back(a.text());
                else if (href.find("studio") != std::string::npos)
                    studios.push_back(a.text());
                else if (href.find("status") != std::string::npos)
                    status = replaceAll(a.text(), "Status: ", "");
            }
        }
        d.genre = join(genres);
        d.author = join(studios);
        d.status = status == "Finito" ? "Completato" : status;

        for (auto& a : doc.select("div.server.active ul.episodes li.episode a")) {
            Episode e;
            e.url = http::pathOf(doc.absUrl(a, "href"));
            std::string t = a.text();
            e.name = "Episodio " + t;
            std::string digits = digitsOnly(t);
            e.number = digits.empty() ? 1 : parseNumber(digits, 1);
            d.episodes.push_back(e);
        }
        std::reverse(d.episodes.begin(), d.episodes.end());
        return d;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::string pageUrl = baseUrl() + episodeUrl;
        html::Document doc(fetch(pageUrl), pageUrl);

        // compare quando l'episodio e' stato rimosso per copyright
        for (auto& alert : doc.select("div.alert.alert-primary")) {
            std::string t = alert.text();
            if (t.find("Copyright") != std::string::npos) throw http::Error(t);
        }

        std::string epId = doc.selectFirst("div#player[data-episode-id]").attr("data-episode-id");
        std::vector<Video> out;
        for (auto& tab : doc.select("div.servers > div.widget-title span.server-tab")) {
            std::string serverName = tab.text();
            std::string dataName = tab.attr("data-name");
            html::Node link = doc.selectFirst("div.server[data-name=\"" + dataName +
                                              "\"] li.episode a[data-episode-id=\"" + epId + "\"]");
            std::string dataId = link.attr("data-id");
            if (dataId.empty()) continue;
            try {
                http::Headers h = {
                    {"Accept", "application/json, text/javascript, */*; q=0.01"},
                    {"Referer", pageUrl},
                    {"X-Requested-With", "XMLHttpRequest"},
                };
                auto j = nlohmann::json::parse(fetch(baseUrl() + "/api/episode/info?id=" + dataId + "&alt=0", h));
                std::string target = j.value("grabber", "");
                if (target.empty()) continue;
                if (serverName.find("AnimeWorld Server") != std::string::npos) {
                    Video v;
                    v.title = "AnimeWorld Server";
                    v.url = target;
                    v.referer = baseUrl() + "/";
                    out.insert(out.begin(), v);  // server principale per primo
                } else if (target.find("streamtape") != std::string::npos) {
                    Video v = streamtape(replaceAll(target, "/v/", "/e/"));
                    if (!v.url.empty()) out.push_back(v);
                }
            } catch (const std::exception&) {
                // server non disponibile: si prova il successivo
            }
        }
        if (out.empty()) throw http::Error("Nessun server riproducibile per questo episodio");
        return out;
    }

  private:
    std::mutex cookieMutex;
    std::string awCookie;  // cookie della protezione anti-bot (risposta 202)

    static std::string join(const std::vector<std::string>& v) {
        std::string out;
        for (auto& s : v) {
            if (s.empty()) continue;
            if (!out.empty()) out += ", ";
            out += s;
        }
        return out;
    }

    /** GET con gestione della "ShittyRedirection": HTTP 202 + document.cookie="..." */
    std::string fetch(const std::string& url, http::Headers headers = {}) {
        std::string cookie;
        {
            std::lock_guard<std::mutex> lock(cookieMutex);
            cookie = awCookie;
        }
        auto withCookie = headers;
        if (!cookie.empty()) withCookie.push_back({"Cookie", cookie});
        http::Response r = http::request("GET", url, withCookie);
        if (r.status == 202) {
            std::string marker = "document.cookie=\"";
            auto p = r.body.find(marker);
            if (p != std::string::npos) {
                std::string c = r.body.substr(p + marker.size());
                c = substringBefore(substringBefore(c, "\""), ";");
                {
                    std::lock_guard<std::mutex> lock(cookieMutex);
                    awCookie = c;
                }
                withCookie = headers;
                withCookie.push_back({"Cookie", c});
                r = http::request("GET", url, withCookie);
            }
        }
        if (r.status < 200 || r.status >= 300)
            throw http::Error("AnimeWorld ha risposto HTTP " + std::to_string(r.status));
        return std::move(r.body);
    }

    Page listPage(const std::string& url) {
        html::Document doc(fetch(url), url);
        Page p;
        for (auto& a : doc.select("div.film-list div.item div.inner a.poster")) {
            Anime an;
            an.url = http::pathOf(doc.absUrl(a, "href"));
            html::Node img = a.selectFirst("img");
            an.thumbnail = img.attr("src");
            an.title = img.attr("alt");
            if (!an.url.empty()) p.animes.push_back(an);
        }
        p.hasNextPage = doc.selectFirst("div.paging-wrapper a#go-next-page").valid();
        return p;
    }

    Video streamtape(const std::string& url) {
        const std::string base = "https://streamtape.com/e/";
        std::string newUrl = url;
        if (url.rfind(base, 0) != 0) {
            // ["https:", "", "<dominio>", "<?>", "<id>", ...]
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
        for (auto& s : doc.select("script")) {
            std::string data = s.data();
            auto p = data.find(target);
            if (p == std::string::npos) continue;
            std::string script = substringAfter(data.substr(p), target + ".innerHTML = '");
            std::string first = substringBefore(script, "'");
            std::string second = substringBefore(substringAfter(script, "+ ('xcd"), "'");
            Video v;
            v.title = "StreamTape";
            v.url = "https:" + first + second;
            v.referer = "https://streamtape.com/";
            return v;
        }
        return {};
    }
};

std::shared_ptr<Source> makeAnimeWorld() { return std::make_shared<AnimeWorld>(); }

}  // namespace src
