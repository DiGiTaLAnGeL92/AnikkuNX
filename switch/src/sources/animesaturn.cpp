// Porting in C++ dell'estensione Aniyomi "Anime Saturn" (it.animesaturn, v14.13).

#include <nlohmann/json.hpp>

#include <algorithm>

#include "html/html.hpp"
#include "sources/registry.hpp"

namespace src {

class AnimeSaturn : public Source {
  public:
    std::string id() const override { return "1863212142777957799"; }
    std::string name() const override { return "AnimeSaturn"; }
    // altri domini noti: animesaturn.cx, animesaturn.cc, animesaturn.com, animemars.org
    std::string defaultBaseUrl() const override { return "https://animesaturn.net"; }

    Page popular(int page) override { return listPage(baseUrl() + "/ongoing/" + std::to_string(page)); }
    Page latest(int page) override { return listPage(baseUrl() + "/newest/" + std::to_string(page)); }
    Page search(const std::string& q, int page) override {
        return listPage(baseUrl() + "/filter/" + std::to_string(page) + "?key=" + http::urlEncode(q));
    }

    Details details(const std::string& url) override {
        std::string pageUrl = baseUrl() + url;
        html::Document doc(http::getText(pageUrl), pageUrl);
        Details d;
        d.title = formatTitle(doc.selectFirst("h1").text());
        d.author = doc.selectFirst("div a[href*=studios]").text();
        std::string status = doc.selectFirst("div a[href*=states]").text();
        d.status = status.find("In corso") != std::string::npos ? "In corso"
                   : status.find("Finito") != std::string::npos ? "Completato"
                                                                 : status;
        std::string genres;
        for (auto& a : doc.select("div a[href*=categories]")) {
            if (!genres.empty()) genres += ", ";
            genres += a.text();
        }
        d.genre = genres;
        d.thumbnail = doc.selectFirst("img[src*=locandine]").attr("src");

        // "section:has(h2) div": primo div di una section che contiene un h2
        for (auto& section : doc.select("section")) {
            if (!section.selectFirst("h2")) continue;
            html::Node div = section.selectFirst("div");
            if (div) {
                d.description = trim(div.text());
                break;
            }
        }
        std::string alter = trim(replaceAll(formatTitle(doc.selectFirst("p.mt-1").text()), "(ITA)", ""));
        if (!alter.empty() && !containsIgnoreCase(d.title, alter))
            d.description += "\n\nTitolo alternativo: " + alter;

        for (auto& a : doc.select("a.ep-tile")) {
            Episode e;
            std::string href = replaceAll(replaceAll(a.attr("href"), "episode/", "anime/"), "watch/", "stream/");
            e.url = http::pathOf(http::resolve(pageUrl, href));
            e.name = a.attr("title");
            e.number = parseNumber(substringAfter(e.name, "Episodio "), 0);
            d.episodes.push_back(e);
        }
        std::reverse(d.episodes.begin(), d.episodes.end());
        return d;
    }

    std::vector<Video> videos(const std::string& episodeUrl) override {
        std::string pageUrl = baseUrl() + episodeUrl;
        html::Document page(http::getText(pageUrl), pageUrl);
        std::string iframe = page.absUrl(page.selectFirst("iframe"), "src");
        if (iframe.empty()) throw http::Error("AnimeSaturn: player non trovato");

        std::string playlistUrl = replaceAll(iframe, "?", "/playlist?");
        http::Response r = http::request("GET", playlistUrl, {{"Referer", iframe}});
        if (r.status != 200) throw http::Error("AnimeSaturn: video non disponibile (HTTP " + std::to_string(r.status) + ")");

        std::string token = http::queryParam(r.finalUrl.empty() ? playlistUrl : r.finalUrl, "token");
        if (token.empty()) token = http::queryParam(playlistUrl, "token");
        if (token.empty()) throw http::Error("AnimeSaturn: token mancante");

        auto j = nlohmann::json::parse(r.body);
        std::string encoded = j.value("d", "");
        std::string base = base64Decode(encoded);
        std::string videoUrl;
        videoUrl.reserve(base.size());
        for (size_t i = 0; i < base.size(); i++) videoUrl += (char)(base[i] ^ token[i % token.size()]);

        Video v;
        v.url = videoUrl;
        v.referer = iframe;
        v.title = videoUrl.find(".mp4") != std::string::npos ? "Qualita' predefinita" : "HLS (automatica)";
        return {v};
    }

    http::Headers imageHeaders() const override { return {}; }

  private:
    static std::string formatTitle(const std::string& t) { return replaceAll(t, "(ITA)", "Dub ITA"); }

    static bool containsIgnoreCase(const std::string& hay, const std::string& needle) {
        auto lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
            return s;
        };
        return lower(hay).find(lower(needle)) != std::string::npos;
    }

    Page listPage(const std::string& url) {
        html::Document doc(http::getText(url), url);
        Page p;
        for (auto& a : doc.select("a.group[href]:not(.flex)")) {
            Anime an;
            an.url = http::pathOf(doc.absUrl(a, "href"));
            an.title = formatTitle(a.selectFirst("h3").text());
            an.thumbnail = a.selectFirst("img").attr("src");
            if (!an.url.empty() && !an.title.empty()) p.animes.push_back(an);
        }
        p.hasNextPage = doc.selectFirst("a[rel=\"next\"]").valid();
        return p;
    }
};

std::shared_ptr<Source> makeAnimeSaturn() { return std::make_shared<AnimeSaturn>(); }

}  // namespace src
