#include "app/api.hpp"
#include "util/i18n.hpp"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>

#include "config.hpp"
#include "net/http.hpp"
#include "sources/source.hpp"

namespace api {

namespace {

std::string dataDir;
std::mutex storeMutex;
json libraryData = json::array();
json progressData = json::object();
json hiddenHistory = json::object();  // animeKey -> istante (ms) in cui e' stato tolto da "Continua a guardare"

std::mutex cacheMutex;
std::map<std::string, src::Details> detailsCache;       // sourceId|url
std::map<std::string, src::Anime> animeCache;           // sourceId|url -> titolo e copertina
std::map<std::string, std::pair<std::string, src::Video>> videoTokens;  // token -> (sourceId, video)
std::atomic<unsigned long> tokenCounter{1};

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string key(const std::string& sid, const std::string& url) { return sid + "|" + url; }

json readJson(const std::string& path, json def) {
    std::ifstream in(path);
    if (!in) return def;
    try {
        json j;
        in >> j;
        return j;
    } catch (...) {
        return def;
    }
}

void writeJson(const std::string& path, const json& j) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp);
        out << j.dump();
    }
    std::remove(path.c_str());
    std::rename(tmp.c_str(), path.c_str());
}

std::shared_ptr<src::Source> source(const std::string& id) {
    auto s = src::byId(id);
    if (!s) throw http::Error(tr("Fonte non trovata"));
    return s;
}

bool inLibrary(const std::string& sid, const std::string& url) {
    std::lock_guard<std::mutex> lock(storeMutex);
    for (auto& e : libraryData)
        if (e.value("sourceId", "") == sid && e.value("url", "") == url) return true;
    return false;
}

json animeJson(const std::string& sid, const src::Anime& a) {
    return {{"sourceId", sid}, {"url", a.url}, {"title", a.title}, {"thumbnail", a.thumbnail},
            {"inLibrary", inLibrary(sid, a.url)}};
}

bool watched(const json& p) {
    double d = p.value("duration", 0.0), pos = p.value("position", 0.0);
    return d > 0 && pos >= d * 0.9;
}

/** Aggiorna il numero di episodi noti di un anime in libreria e azzera le novita' (pagina aperta). */
void markSeen(const std::string& sid, const std::string& url, int count) {
    if (count <= 0) return;
    std::lock_guard<std::mutex> lock(storeMutex);
    bool changed = false;
    for (auto& e : libraryData) {
        if (e.value("sourceId", "") != sid || e.value("url", "") != url) continue;
        if (e.value("knownEpisodes", -1) != count || e.value("newEpisodes", 0) != 0) {
            e["knownEpisodes"] = count;
            e["newEpisodes"] = 0;
            changed = true;
        }
    }
    if (changed) writeJson(dataDir + "/library.json", libraryData);
}

}  // namespace

void init(const std::string& dir) {
    dataDir = dir;
    std::lock_guard<std::mutex> lock(storeMutex);
    libraryData = readJson(dataDir + "/library.json", json::array());
    progressData = readJson(dataDir + "/progress.json", json::object());
    hiddenHistory = readJson(dataDir + "/history_hidden.json", json::object());
    if (!hiddenHistory.is_object()) hiddenHistory = json::object();
    if (!libraryData.is_array()) libraryData = json::array();
    if (!progressData.is_object()) progressData = json::object();
}

json sources(bool includeDisabled) {
    json arr = json::array();
    for (auto& s : src::all()) {
        bool enabled = Config::instance().isSourceEnabled(s->id());
        if (!enabled && !includeDisabled) continue;
        arr.push_back({{"id", s->id()},
                       {"name", s->name()},
                       {"lang", s->lang()},
                       {"nsfw", s->nsfw()},
                       {"enabled", enabled},
                       {"supportsLatest", s->supportsLatest()},
                       {"baseUrl", s->baseUrl()}});
    }
    return arr;
}

json browse(const std::string& sid, const std::string& mode, int page, const std::string& query) {
    auto s = source(sid);
    src::Page p = mode == "popular" ? s->popular(page) : mode == "latest" ? s->latest(page) : s->search(query, page);
    json animes = json::array();
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        for (auto& a : p.animes) animeCache[key(sid, a.url)] = a;
    }
    for (auto& a : p.animes) animes.push_back(animeJson(sid, a));
    return {{"hasNextPage", p.hasNextPage}, {"animes", animes}};
}

json anime(const std::string& sid, const std::string& url, const std::string& title, bool cached) {
    auto s = source(sid);
    src::Details d;
    bool have = false;
    if (cached) {
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = detailsCache.find(key(sid, url));
        if (it != detailsCache.end()) {
            d = it->second;
            have = true;
        }
    }
    if (!have) {
        d = s->details(url);
        std::lock_guard<std::mutex> lock(cacheMutex);
        detailsCache[key(sid, url)] = d;
        auto& a = animeCache[key(sid, url)];
        a.url = url;
        if (!d.title.empty()) a.title = d.title;
        if (!d.thumbnail.empty()) a.thumbnail = d.thumbnail;
    }
    if (d.title.empty()) d.title = title;
    markSeen(sid, url, (int)d.episodes.size());

    json eps = json::array();
    {
        std::lock_guard<std::mutex> lock(storeMutex);
        for (auto& e : d.episodes) {
            json p = progressData.value(key(sid, e.url), json::object());
            eps.push_back({{"url", e.url},
                           {"name", e.name},
                           {"number", e.number},
                           {"position", p.value("position", 0.0)},
                           {"duration", p.value("duration", 0.0)},
                           {"watched", watched(p)}});
        }
    }
    return {{"sourceId", sid},
            {"sourceName", s->name()},
            {"url", url},
            {"title", d.title},
            {"thumbnail", d.thumbnail},
            {"description", d.description},
            {"genre", d.genre},
            {"author", d.author},
            {"status", d.status},
            {"fetchType", "episodes"},
            {"inLibrary", inLibrary(sid, url)},
            {"episodes", eps},
            {"seasons", json::array()}};
}

json hosters(const std::string& sid, const std::string& episodeUrl, const std::string&) {
    auto s = source(sid);
    auto videos = s->videos(episodeUrl);
    json list = json::array();
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (videoTokens.size() > 500) videoTokens.clear();
    for (auto& v : videos) {
        std::string token = std::to_string(tokenCounter++);
        videoTokens[token] = {sid, v};
        list.push_back({{"token", token}, {"title", v.title}, {"resolution", v.quality}, {"preferred", false}});
    }
    return {{"hosters", json::array({{{"index", 0}, {"name", "Video"}, {"lazy", false}, {"videos", list}}})}};
}

json hosterVideos(const std::string& sid, const std::string& episodeUrl, int) {
    return hosters(sid, episodeUrl, "")["hosters"][0];
}

json play(const std::string& token) {
    std::pair<std::string, src::Video> entry;
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = videoTokens.find(token);
        if (it == videoTokens.end()) throw http::Error(tr("Video scaduto, riapri l'episodio"));
        entry = it->second;
    }
    const src::Video& v = entry.second;
    json args = json::array();
    args.push_back({"user-agent", v.userAgent.empty() ? http::DEFAULT_UA : v.userAgent});
    args.push_back({"referrer", v.referer});
    // mpv separa le intestazioni con la virgola: quelle che ne contengono una vengono scartate
    std::string fields;
    auto addField = [&fields](const std::string& name, const std::string& value) {
        if (value.empty() || value.find(',') != std::string::npos) return;
        if (!fields.empty()) fields += ",";
        fields += name + ": " + value;
    };
    addField("Cookie", v.cookie);
    for (auto& h : v.headers) {
        std::string lower = h.first;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        if (lower == "referer" || lower == "user-agent") continue;  // gia' passati a parte
        addField(h.first, h.second);
    }
    args.push_back({"http-header-fields", fields});
    json subs = json::array(), audio = json::array();
    for (auto& t : v.subtitles) subs.push_back({{"url", t.url}, {"lang", t.lang}});
    for (auto& t : v.audio) audio.push_back({{"url", t.url}, {"lang", t.lang}});
    return {{"title", v.title},
            {"url", v.url},
            {"subtitles", subs},
            {"audio", audio},
            {"timestamps", json::array()},
            {"mpvArgs", args}};
}

// ------------------------------------------------------------------------------ libreria

json library() {
    json out = json::array();
    std::lock_guard<std::mutex> lock(storeMutex);
    json sorted = libraryData;
    // prima gli anime con episodi nuovi, poi i piu' recenti in libreria
    std::sort(sorted.begin(), sorted.end(), [](const json& a, const json& b) {
        bool na = a.value("newEpisodes", 0) > 0, nb = b.value("newEpisodes", 0) > 0;
        if (na != nb) return na;
        return a.value("addedAt", 0LL) > b.value("addedAt", 0LL);
    });
    for (auto& e : sorted) {
        auto s = src::byId(e.value("sourceId", ""));
        json item = e;
        item["sourceName"] = s ? s->name() : "";
        out.push_back(item);
    }
    return out;
}

void addLibrary(const std::string& sid, const std::string& url, const std::string& title) {
    src::Anime a;
    int known = -1;
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = animeCache.find(key(sid, url));
        if (it != animeCache.end()) a = it->second;
        auto dit = detailsCache.find(key(sid, url));
        if (dit != detailsCache.end()) known = (int)dit->second.episodes.size();
    }
    std::lock_guard<std::mutex> lock(storeMutex);
    json filtered = json::array();
    for (auto& e : libraryData)
        if (!(e.value("sourceId", "") == sid && e.value("url", "") == url)) filtered.push_back(e);
    filtered.push_back({{"sourceId", sid},
                        {"url", url},
                        {"title", a.title.empty() ? title : a.title},
                        {"thumbnail", a.thumbnail},
                        {"knownEpisodes", known},
                        {"newEpisodes", 0},
                        {"addedAt", nowMs()}});
    libraryData = filtered;
    writeJson(dataDir + "/library.json", libraryData);
}

json refreshLibrary(const std::function<bool()>& keepGoing) {
    json entries;
    {
        std::lock_guard<std::mutex> lock(storeMutex);
        entries = libraryData;
    }
    json found = json::array();  // anime con episodi nuovi trovati in questo controllo
    for (auto& e : entries) {
        if (keepGoing && !keepGoing()) break;
        std::string sid = e.value("sourceId", ""), url = e.value("url", "");
        auto s = src::byId(sid);
        if (!s) continue;
        int count;
        try {
            src::Details d = s->details(url);
            count = (int)d.episodes.size();
            std::lock_guard<std::mutex> lock(cacheMutex);
            detailsCache[key(sid, url)] = d;
        } catch (const std::exception&) {
            continue;  // sito non raggiungibile: si riprova al prossimo avvio
        }
        if (count <= 0) continue;
        std::lock_guard<std::mutex> lock(storeMutex);
        for (auto& le : libraryData) {
            if (le.value("sourceId", "") != sid || le.value("url", "") != url) continue;
            int known = le.value("knownEpisodes", -1);
            if (known < 0 || count < known) {
                le["knownEpisodes"] = count;  // primo controllo (o episodi rimossi dal sito): nessuna novita'
            } else if (count > known) {
                int added = count - known;
                le["newEpisodes"] = le.value("newEpisodes", 0) + added;
                le["knownEpisodes"] = count;
                found.push_back({{"sourceId", sid}, {"url", url}, {"title", le.value("title", "")}, {"added", added}});
            }
            le["checkedAt"] = nowMs();
        }
        writeJson(dataDir + "/library.json", libraryData);
    }
    return found;
}

void removeLibrary(const std::string& sid, const std::string& url) {
    std::lock_guard<std::mutex> lock(storeMutex);
    json filtered = json::array();
    for (auto& e : libraryData)
        if (!(e.value("sourceId", "") == sid && e.value("url", "") == url)) filtered.push_back(e);
    libraryData = filtered;
    writeJson(dataDir + "/library.json", libraryData);
}

json history() {
    std::vector<json> items;
    {
        std::lock_guard<std::mutex> lock(storeMutex);
        for (auto& kv : progressData.items()) {
            const json& p = kv.value();
            // nascosto dall'utente, finche' non guarda di nuovo qualcosa di quell'anime
            std::string k = key(p.value("sourceId", ""), p.value("animeUrl", ""));
            if (hiddenHistory.contains(k) && hiddenHistory[k].get<int64_t>() >= p.value("updatedAt", (int64_t)0)) continue;
            items.push_back(p);
        }
    }
    std::sort(items.begin(), items.end(),
              [](const json& a, const json& b) { return a.value("updatedAt", 0LL) > b.value("updatedAt", 0LL); });
    json out = json::array();
    std::vector<std::string> seen;
    for (auto& p : items) {
        std::string k = key(p.value("sourceId", ""), p.value("animeUrl", ""));
        if (std::find(seen.begin(), seen.end(), k) != seen.end()) continue;
        seen.push_back(k);
        json item = p;
        item["title"] = p.value("animeTitle", "");
        item["watched"] = watched(p);
        out.push_back(item);
        if (out.size() >= 30) break;
    }
    return out;
}

void removeFromHistory(const std::string& sid, const std::string& animeUrl) {
    std::lock_guard<std::mutex> lock(storeMutex);
    hiddenHistory[key(sid, animeUrl)] = nowMs();
    writeJson(dataDir + "/history_hidden.json", hiddenHistory);
}

void saveProgress(const json& p) {
    std::string sid = p.value("sourceId", ""), animeUrl = p.value("animeUrl", "");
    json entry = p;
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = animeCache.find(key(sid, animeUrl));
        if (it != animeCache.end()) {
            if (entry.value("animeTitle", "").empty()) entry["animeTitle"] = it->second.title;
            entry["thumbnail"] = it->second.thumbnail;
        }
    }
    entry["updatedAt"] = nowMs();
    std::lock_guard<std::mutex> lock(storeMutex);
    progressData[key(sid, p.value("episodeUrl", ""))] = entry;
    writeJson(dataDir + "/progress.json", progressData);
}

std::string download(const std::string& url) {
    http::Headers h;
    std::string host = http::hostOf(url);
    for (auto& s : src::all()) {
        std::string n = s->name();
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return std::tolower(c); });
        if (host.find(n) != std::string::npos || host.find(http::hostOf(s->baseUrl())) != std::string::npos) {
            h = s->imageHeaders();
            break;
        }
    }
    http::Response r = http::request("GET", url, h, "", 30);
    if (r.status >= 400) throw http::Error("HTTP " + std::to_string(r.status));
    return std::move(r.body);
}

}  // namespace api
