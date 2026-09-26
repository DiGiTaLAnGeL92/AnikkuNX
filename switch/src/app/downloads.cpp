#include "app/downloads.hpp"

#ifndef ANX_NO_UI
#include <borealis.hpp>
#endif

#include <sys/stat.h>
#include <pthread.h>
#include <unistd.h>

#include <ctime>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#include "app/api.hpp"
#include "net/hls_proxy.hpp"
#include "net/http.hpp"
#include "sources/registry.hpp"
#include "sources/source.hpp"
#include "util/crypto.hpp"
#include "util/i18n.hpp"
#include "util/platform.hpp"

namespace downloads {

namespace {

// senza interfaccia (strumento di prova sul PC) i messaggi vanno sul terminale
void logWarning(const std::string& msg) {
#ifndef ANX_NO_UI
    brls::Logger::warning("{}", msg);
#else
    fprintf(stderr, "%s\n", msg.c_str());
#endif
}

void notifyUi(const std::string& msg) {
#ifndef ANX_NO_UI
    brls::sync([msg] { brls::Application::notify(msg); });
#else
    fprintf(stderr, "%s\n", msg.c_str());
#endif
}

std::mutex mtx;
std::condition_variable cv;
std::vector<json> list;  // la coda, nell'ordine di download
std::string baseDir;     // <dati>
bool pausedFlag = false;
std::string currentId;
std::atomic<bool> cancelCurrent{false};
int idCounter = 0;

struct Cancelled {};

std::string storePath() { return baseDir + "/downloads.json"; }

void saveLocked() {
    json j = {{"paused", pausedFlag}, {"items", list}};
    std::string tmp = storePath() + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) return;
        out << j.dump();
    }
    std::remove(storePath().c_str());
    std::rename(tmp.c_str(), storePath().c_str());
}

json* findLocked(const std::string& id) {
    for (auto& it : list)
        if (it.value("id", "") == id) return &it;
    return nullptr;
}

bool fileExists(const std::string& p) {
    struct stat st;
    return !p.empty() && stat(p.c_str(), &st) == 0;
}

std::string hexHash(const std::string& s) {
    char buf[20];
    snprintf(buf, sizeof(buf), "%08x", (unsigned)(std::hash<std::string>{}(s) & 0xffffffffu));
    return buf;
}

/** Nome di cartella sicuro per la scheda SD (FAT32): solo lettere, cifre, '-' e '_'. */
std::string safeName(const std::string& s, size_t maxLen) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-')
            out += (char)c;
        else if ((c == ' ' || c == '_' || c == '.') && !out.empty() && out.back() != '_')
            out += '_';
        if (out.size() >= maxLen) break;
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? "anime" : out;
}

void mkdirs(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); i++) {
        cur += path[i];
        if ((path[i] == '/' && i > 0 && path[i - 1] != ':') || i + 1 == path.size()) mkdir(cur.c_str(), 0777);
    }
}

http::Headers headersFor(const src::Video& v) {
    http::Headers hh;
    hh.push_back({"User-Agent", v.userAgent.empty() ? http::DEFAULT_UA : v.userAgent});
    if (!v.referer.empty()) hh.push_back({"Referer", v.referer});
    if (!v.cookie.empty()) hh.push_back({"Cookie", v.cookie});
    for (auto& h : v.headers) {
        std::string lower = h.first;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        if (lower == "referer" || lower == "user-agent") continue;
        hh.push_back(h);
    }
    return hh;
}

bool at(const std::string& b, size_t off, const char* m, size_t n) { return b.size() >= off + n && b.compare(off, n, m, n) == 0; }

/** Estensione del file se i primi byte sono davvero video, altrimenti "". */
std::string videoKind(const std::string& b) {
    if (b.size() > 376 && (unsigned char)b[0] == 0x47 && (unsigned char)b[188] == 0x47) return "ts";
    if (at(b, 4, "ftyp", 4) || at(b, 4, "moov", 4) || at(b, 4, "moof", 4) || at(b, 4, "styp", 4) ||
        at(b, 4, "sidx", 4) || at(b, 4, "free", 4) || at(b, 4, "mdat", 4))
        return "mp4";
    if (at(b, 0, "\x1A\x45\xDF\xA3", 4)) return "mkv";
    if (at(b, 0, "ID3", 3)) {  // TS con tag ID3 davanti (comune negli HLS)
        for (size_t i = 0; i + 376 < b.size() && i < 4096; i++)
            if ((unsigned char)b[i] == 0x47 && (unsigned char)b[i + 188] == 0x47) return "ts";
    }
    return "";
}

void setProgress(const std::string& id, double p, long long bytes) {
    std::lock_guard<std::mutex> lock(mtx);
    if (json* it = findLocked(id)) {
        (*it)["progress"] = p;
        (*it)["bytes"] = bytes;
    }
}

http::Response getWithRetry(const std::string& url, const http::Headers& h, long timeout = 60) {
    std::string lastErr;
    for (int attempt = 0; attempt < 4; attempt++) {
        if (cancelCurrent) throw Cancelled{};
        try {
            http::Response r = http::request("GET", url, h, "", timeout);
            if (r.status >= 200 && r.status < 300) return r;
            lastErr = "HTTP " + std::to_string(r.status);
            if (r.status == 403 || r.status == 404 || r.status == 410) break;  // inutile riprovare
        } catch (const std::exception& e) {
            lastErr = e.what();
        }
        std::this_thread::sleep_for(std::chrono::seconds(1 + attempt * 2));
    }
    throw http::Error(lastErr);
}

std::string attr(const std::string& line, const std::string& name) {
    size_t p = 0;
    while ((p = line.find(name + "=", p)) != std::string::npos) {
        if (p == 0 || line[p - 1] == ':' || line[p - 1] == ',') break;
        p += name.size();
    }
    if (p == std::string::npos) return "";
    p += name.size() + 1;
    if (p < line.size() && line[p] == '"') {
        size_t e = line.find('"', p + 1);
        return line.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1);
    }
    size_t e = line.find(',', p);
    return line.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

std::vector<std::string> lines(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string l;
    while (std::getline(in, l)) {
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        out.push_back(l);
    }
    return out;
}

std::string hexToBytes(std::string h) {
    if (h.rfind("0x", 0) == 0 || h.rfind("0X", 0) == 0) h = h.substr(2);
    std::string out;
    for (size_t i = 0; i + 1 < h.size(); i += 2) out += (char)std::stoi(h.substr(i, 2), nullptr, 16);
    while (out.size() < 16) out.insert(out.begin(), '\0');
    return out;
}

/** Scarica uno stream HLS in un unico file (.ts, o .mp4 se fMP4). Ritorna l'estensione. */
std::string downloadHls(const std::string& url, const std::string& body0, const std::string& finalUrl0,
                        const http::Headers& h, const std::string& partPath, const std::string& id) {
    std::string body = body0, base = finalUrl0.empty() ? url : finalUrl0;
    auto ls = lines(body);

    // playlist principale: scegli la qualita' migliore fino a 1080p
    if (body.find("#EXT-X-STREAM-INF") != std::string::npos) {
        std::string bestUri, bestAudio;
        long bestH = -1, bestBw = -1;
        for (size_t i = 0; i + 1 < ls.size(); i++) {
            if (ls[i].rfind("#EXT-X-STREAM-INF", 0) != 0) continue;
            std::string uri;
            for (size_t j = i + 1; j < ls.size(); j++)
                if (!ls[j].empty() && ls[j][0] != '#') {
                    uri = ls[j];
                    break;
                }
            if (uri.empty()) continue;
            long bw = atol(attr(ls[i], "BANDWIDTH").c_str());
            std::string res = attr(ls[i], "RESOLUTION");
            long hgt = res.find('x') != std::string::npos ? atol(res.substr(res.find('x') + 1).c_str()) : 0;
            long score = hgt > 1080 ? 1 : hgt;  // oltre 1080p: troppo pesante per la Switch
            if (score > bestH || (score == bestH && bw > bestBw)) {
                bestH = score;
                bestBw = bw;
                bestUri = uri;
                bestAudio = attr(ls[i], "AUDIO");
            }
        }
        if (bestUri.empty()) throw http::Error(tr("Playlist senza video"));
        if (!bestAudio.empty()) {
            for (auto& l : ls)
                if (l.rfind("#EXT-X-MEDIA", 0) == 0 && attr(l, "TYPE") == "AUDIO" && attr(l, "GROUP-ID") == bestAudio &&
                    !attr(l, "URI").empty())
                    throw http::Error(tr("audio separato dal video (non supportato)"));
        }
        std::string vurl = http::resolve(base, bestUri);
        http::Response vr = getWithRetry(vurl, h, 30);
        body = vr.body;
        base = vr.finalUrl.empty() ? vurl : vr.finalUrl;
        ls = lines(body);
    }
    if (body.find("#EXTM3U") == std::string::npos) throw http::Error(tr("Playlist non valida"));

    struct Seg {
        std::string url, keyUri, iv, range;
        long seq;
    };
    std::vector<Seg> segs;
    std::string mapUri, keyUri, keyIv, pendingRange;
    long seq = 0;
    for (auto& l : ls) {
        if (l.rfind("#EXT-X-MEDIA-SEQUENCE:", 0) == 0) {
            seq = atol(l.substr(22).c_str());
        } else if (l.rfind("#EXT-X-KEY:", 0) == 0) {
            std::string method = attr(l, "METHOD");
            if (method == "NONE") {
                keyUri.clear();
                keyIv.clear();
            } else if (method == "AES-128") {
                keyUri = http::resolve(base, attr(l, "URI"));
                keyIv = attr(l, "IV");
            } else {
                throw http::Error(tr("video protetto ({})", method));
            }
        } else if (l.rfind("#EXT-X-MAP:", 0) == 0) {
            mapUri = http::resolve(base, attr(l, "URI"));
        } else if (l.rfind("#EXT-X-BYTERANGE:", 0) == 0) {
            pendingRange = l.substr(17);
        } else if (!l.empty() && l[0] != '#') {
            segs.push_back({http::resolve(base, l), keyUri, keyIv, pendingRange, seq});
            pendingRange.clear();
            seq++;
        }
    }
    if (segs.empty()) throw http::Error(tr("Playlist senza segmenti"));

    FILE* f = fopen(partPath.c_str(), "wb");
    if (!f) throw http::Error(tr("Impossibile scrivere sulla scheda SD"));
    std::map<std::string, std::string> keys;
    long long written = 0;
    std::string kind;
    long rangeNext = 0;
    try {
        if (!mapUri.empty()) {
            std::string init = getWithRetry(mapUri, h).body;
            if (videoKind(init) != "mp4") throw http::Error(tr("segmento iniziale non valido"));
            fwrite(init.data(), 1, init.size(), f);
            written += (long long)init.size();
            kind = "mp4";
        }
        for (size_t i = 0; i < segs.size(); i++) {
            if (cancelCurrent) throw Cancelled{};
            const Seg& s = segs[i];
            http::Headers sh = h;
            if (!s.range.empty()) {
                long len = atol(s.range.c_str());
                size_t atPos = s.range.find('@');
                long off = atPos != std::string::npos ? atol(s.range.substr(atPos + 1).c_str()) : rangeNext;
                sh.push_back({"Range", "bytes=" + std::to_string(off) + "-" + std::to_string(off + len - 1)});
                rangeNext = off + len;
            }
            std::string data = getWithRetry(s.url, sh).body;
            if (!s.keyUri.empty()) {
                if (!keys.count(s.keyUri)) {
                    std::string k = getWithRetry(s.keyUri, h, 30).body;
                    if (k.size() != 16) throw http::Error(tr("chiave di cifratura non valida"));
                    keys[s.keyUri] = k;
                }
                std::string iv;
                if (!s.iv.empty()) {
                    iv = hexToBytes(s.iv);
                } else {
                    iv.assign(16, '\0');
                    for (int b = 0; b < 8; b++) iv[15 - b] = (char)((s.seq >> (8 * b)) & 0xff);
                }
                try {
                    data = crypto::aesCbcDecrypt(data, keys[s.keyUri], iv, true);
                } catch (...) {
                    data = crypto::aesCbcDecrypt(data, keys[s.keyUri], iv, false);
                }
            }
            data = hlsproxy::stripFakeHeader(data);
            if (i == 0) {
                // il video e' davvero partito? (non una pagina di errore o un'immagine)
                std::string k = videoKind(data);
                if (k.empty()) throw http::Error(tr("il primo segmento non e' video"));
                if (kind.empty()) kind = k == "mp4" ? "mp4" : "ts";
            }
            if (fwrite(data.data(), 1, data.size(), f) != data.size())
                throw http::Error(tr("Scheda SD piena o non scrivibile"));
            written += (long long)data.size();
            setProgress(id, (double)(i + 1) / segs.size(), written);
        }
    } catch (...) {
        fclose(f);
        std::remove(partPath.c_str());
        throw;
    }
    fclose(f);
    return kind.empty() ? "ts" : kind;
}

/** Scarica un file video diretto; il contenuto viene controllato appena arrivano i primi byte. */
std::string downloadDirect(const std::string& url, const http::Headers& h, const std::string& partPath,
                           const std::string& id, bool& isPlaylist) {
    std::string kind;
    isPlaylist = false;
    try {
        http::downloadToFile(
            url, partPath, h,
            [id](long long now, long long total) {
                if (cancelCurrent) return false;
                setProgress(id, total > 0 ? (double)now / total : 0.0, now);
                return true;
            },
            [&kind, &isPlaylist](const std::string& head) {
                // una playlist HLS con un URL "normale": ci pensa il chiamante
                if (head.find("#EXTM3U") != std::string::npos && head.find("#EXTM3U") < 16) {
                    isPlaylist = true;
                    return false;
                }
                kind = videoKind(head);
                return !kind.empty();
            });
    } catch (const std::exception&) {
        if (isPlaylist) return "";
        throw;
    }
    if (cancelCurrent) throw Cancelled{};
    struct stat st;
    if (stat(partPath.c_str(), &st) != 0 || st.st_size < 512 * 1024) {
        std::remove(partPath.c_str());
        throw http::Error(tr("file troppo piccolo"));
    }
    return kind;
}

std::string cleanSubtitle(const std::string& b) {
    std::string out;
    out.reserve(b.size());
    for (size_t i = 0; i < b.size(); i++) {
        if (b[i] == '\\' && i + 1 < b.size() && (b[i + 1] == '"' || b[i + 1] == '\'' || b[i + 1] == '/')) {
            out += b[++i];
            continue;
        }
        out += b[i];
    }
    return out;
}

/** Scarica un episodio provando i video della fonte in ordine. */
void process(json item) {
    std::string id = item.value("id", "");
    auto s = src::byId(item.value("sourceId", ""));
    if (!s) throw http::Error(tr("Fonte non trovata"));
    std::vector<src::Video> videos = s->videos(item.value("episodeUrl", ""));
    if (videos.empty()) throw http::Error(tr("Nessun video trovato"));

    std::string folder = baseDir + "/downloads/" + safeName(item.value("animeTitle", ""), 40) + "_" +
                         hexHash(item.value("sourceId", "") + "|" + item.value("animeUrl", ""));
    mkdirs(folder);
    double num = item.value("number", -1.0);
    std::string epName;
    if (num >= 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "ep%g", num);
        epName = buf;
        std::replace(epName.begin(), epName.end(), '.', '_');
    } else {
        epName = "ep_" + hexHash(item.value("episodeUrl", ""));
    }
    std::string base = folder + "/" + epName;
    std::string part = base + ".part";

    std::string lastErr;
    for (auto& v : videos) {
        if (cancelCurrent) throw Cancelled{};
        http::Headers h = headersFor(v);
        std::string kind;
        try {
            {
                std::lock_guard<std::mutex> lock(mtx);
                if (json* it = findLocked(id)) {
                    (*it)["video"] = v.title;
                    (*it)["progress"] = 0.0;
                    (*it)["bytes"] = 0;
                }
            }
            std::string lower = v.url;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
            if (lower.find(".mpd") != std::string::npos) throw http::Error("DASH");
            bool isPlaylist = lower.find("m3u8") != std::string::npos;
            // file diretto (MP4...): il contenuto si controlla sui primi byte mentre arriva
            if (!isPlaylist) kind = downloadDirect(v.url, h, part, id, isPlaylist);
            if (isPlaylist) {
                http::Response r = getWithRetry(v.url, h, 30);
                if (r.body.find("#EXTM3U") == std::string::npos) throw http::Error(tr("Il link non contiene un video"));
                kind = downloadHls(v.url, r.body, r.finalUrl, h, part, id);
            }
        } catch (const Cancelled&) {
            std::remove(part.c_str());
            throw;
        } catch (const std::exception& e) {
            std::remove(part.c_str());
            lastErr = v.title + ": " + e.what();
            logWarning("download " + id + ": " + lastErr);
            continue;
        }

        std::string file = base + "." + kind;
        std::remove(file.c_str());
        if (std::rename(part.c_str(), file.c_str()) != 0) throw http::Error(tr("Impossibile salvare il file"));

        // sottotitoli esterni
        json subs = json::array();
        int n = 0;
        for (auto& t : v.subtitles) {
            try {
                http::Response r = getWithRetry(t.url, h, 20);
                std::string body = cleanSubtitle(r.body);
                std::string ext = body.find("[Script Info]") != std::string::npos ? "ass"
                                  : body.find("WEBVTT") != std::string::npos      ? "vtt"
                                                                                  : "srt";
                std::string p = base + ".sub" + std::to_string(n++) + "." + ext;
                std::ofstream(p, std::ios::binary) << body;
                subs.push_back({{"url", p}, {"lang", t.lang}});
            } catch (...) {
            }
        }
        // copertina, per vederla anche offline
        std::string cover = folder + "/cover.img";
        if (!fileExists(cover) && !item.value("thumbnail", "").empty()) {
            try {
                std::string img = api::download(item.value("thumbnail", ""));
                if (!img.empty()) std::ofstream(cover, std::ios::binary) << img;
            } catch (...) {
            }
        }
        struct stat st;
        long long size = stat(file.c_str(), &st) == 0 ? (long long)st.st_size : 0;
        std::lock_guard<std::mutex> lock(mtx);
        if (json* it = findLocked(id)) {
            (*it)["status"] = "done";
            (*it)["file"] = file;
            (*it)["subtitles"] = subs;
            (*it)["cover"] = fileExists(cover) ? cover : "";
            (*it)["progress"] = 1.0;
            (*it)["bytes"] = size;
            (*it)["error"] = "";
            (*it)["video"] = v.title;
        }
        saveLocked();
        return;
    }
    throw http::Error(lastErr.empty() ? tr("Nessun video scaricabile") : lastErr);
}

void* workerMain(void*) {
    while (true) {
        json item;
        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [] {
                if (pausedFlag) return false;
                for (auto& it : list)
                    if (it.value("status", "") == "queued") return true;
                return false;
            });
            for (auto& it : list)
                if (it.value("status", "") == "queued") {
                    it["status"] = "downloading";
                    it["error"] = "";
                    item = it;
                    break;
                }
            currentId = item.value("id", "");
            cancelCurrent = false;
            saveLocked();
        }
        platform::setAwake(platform::AWAKE_DOWNLOAD, true);
        std::string err;
        bool cancelled = false;
        try {
            process(item);
        } catch (const Cancelled&) {
            cancelled = true;
        } catch (const std::exception& e) {
            err = e.what();
        } catch (...) {
            err = tr("errore sconosciuto");
        }
        platform::setAwake(platform::AWAKE_DOWNLOAD, false);

        std::string title = item.value("animeTitle", "") + " - " + item.value("episodeName", "");
        {
            std::lock_guard<std::mutex> lock(mtx);
            json* it = findLocked(currentId);
            if (it) {
                if (cancelled)
                    (*it)["status"] = pausedFlag ? "queued" : (*it).value("status", "queued");  // messo in pausa
                else if (!err.empty()) {
                    (*it)["status"] = "failed";
                    (*it)["error"] = err;
                }
                if ((*it).value("status", "") == "downloading") (*it)["status"] = "queued";
            }
            currentId.clear();
            saveLocked();
        }
        if (!cancelled) {
            std::string msg = err.empty() ? tr("Scaricato: {}", title) : tr("Download non riuscito: {}", title);
            notifyUi(msg);
        }
    }
    return nullptr;
}

void deleteFiles(const json& it) {
    std::string f = it.value("file", "");
    if (!f.empty()) std::remove(f.c_str());
    for (auto& s : it.value("subtitles", json::array())) std::remove(s.value("url", "").c_str());
}

}  // namespace

#ifdef ANX_NO_UI
std::string testDownload(const std::string& url, const http::Headers& h, const std::string& base) {
    std::string part = base + ".part";
    std::string lower = url;
    bool isPlaylist = lower.find("m3u8") != std::string::npos;
    std::string kind;
    if (!isPlaylist) kind = downloadDirect(url, h, part, "test", isPlaylist);
    if (isPlaylist) {
        http::Response r = getWithRetry(url, h, 30);
        if (r.body.find("#EXTM3U") == std::string::npos) throw http::Error("Il link non contiene un video");
        kind = downloadHls(url, r.body, r.finalUrl, h, part, "test");
    }
    std::string file = base + "." + kind;
    std::rename(part.c_str(), file.c_str());
    return file;
}
#endif

void init(const std::string& dataDir) {
    baseDir = dataDir;
    {
        std::lock_guard<std::mutex> lock(mtx);
        std::ifstream in(storePath());
        if (in) {
            try {
                json j;
                in >> j;
                pausedFlag = j.value("paused", false);
                for (auto& it : j.value("items", json::array())) {
                    if (!it.is_object()) continue;
                    // interrotti alla chiusura dell'app: si ricomincia
                    if (it.value("status", "") == "downloading") it["status"] = "queued";
                    // file cancellati a mano dalla scheda SD
                    if (it.value("status", "") == "done" && !fileExists(it.value("file", ""))) continue;
                    list.push_back(it);
                }
            } catch (...) {
            }
        }
        for (auto& it : list) idCounter = std::max(idCounter, atoi(it.value("id", "0").c_str()));
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    pthread_create(&th, &attr, workerMain, nullptr);
    pthread_attr_destroy(&attr);
}

bool enqueue(const EpisodeInfo& ep) {
    std::lock_guard<std::mutex> lock(mtx);
    for (auto& it : list)
        if (it.value("sourceId", "") == ep.sourceId && it.value("episodeUrl", "") == ep.episodeUrl) {
            if (it.value("status", "") != "failed") return false;
            it["status"] = "queued";  // gia' fallito: riprova
            it["error"] = "";
            saveLocked();
            cv.notify_all();
            return true;
        }
    list.push_back({{"id", std::to_string(++idCounter)},
                    {"sourceId", ep.sourceId},
                    {"animeUrl", ep.animeUrl},
                    {"animeTitle", ep.animeTitle},
                    {"thumbnail", ep.thumbnail},
                    {"episodeUrl", ep.episodeUrl},
                    {"episodeName", ep.episodeName},
                    {"number", ep.number},
                    {"status", "queued"},
                    {"progress", 0.0},
                    {"bytes", 0},
                    {"error", ""},
                    {"added", (long long)std::time(nullptr)}});
    saveLocked();
    cv.notify_all();
    return true;
}

std::string status(const std::string& sourceId, const std::string& episodeUrl, double* progress) {
    std::lock_guard<std::mutex> lock(mtx);
    for (auto& it : list)
        if (it.value("episodeUrl", "") == episodeUrl && it.value("sourceId", "") == sourceId) {
            if (progress) *progress = it.value("progress", 0.0);
            return it.value("status", "");
        }
    return "";
}

json localFile(const std::string& sourceId, const std::string& episodeUrl) {
    std::lock_guard<std::mutex> lock(mtx);
    for (auto& it : list)
        if (it.value("episodeUrl", "") == episodeUrl && it.value("sourceId", "") == sourceId &&
            it.value("status", "") == "done" && fileExists(it.value("file", "")))
            return {{"file", it.value("file", "")}, {"subtitles", it.value("subtitles", json::array())}};
    return nullptr;
}

json items() {
    std::lock_guard<std::mutex> lock(mtx);
    return list;
}

json animes() {
    std::lock_guard<std::mutex> lock(mtx);
    json out = json::array();
    std::map<std::string, size_t> index;
    for (auto& it : list) {
        if (it.value("status", "") != "done") continue;
        std::string key = it.value("sourceId", "") + "|" + it.value("animeUrl", "");
        if (!index.count(key)) {
            index[key] = out.size();
            out.push_back({{"sourceId", it.value("sourceId", "")},
                           {"animeUrl", it.value("animeUrl", "")},
                           {"title", it.value("animeTitle", "")},
                           {"thumbnail", it.value("thumbnail", "")},
                           {"cover", it.value("cover", "")},
                           {"count", 0}});
        }
        auto& a = out[index[key]];
        a["count"] = a.value("count", 0) + 1;
        if (a.value("cover", "").empty()) a["cover"] = it.value("cover", "");
    }
    return out;
}

json episodes(const std::string& sourceId, const std::string& animeUrl) {
    std::lock_guard<std::mutex> lock(mtx);
    json out = json::array();
    for (auto& it : list)
        if (it.value("status", "") == "done" && it.value("sourceId", "") == sourceId &&
            it.value("animeUrl", "") == animeUrl)
            out.push_back(it);
    std::sort(out.begin(), out.end(),
              [](const json& a, const json& b) { return a.value("number", -1.0) < b.value("number", -1.0); });
    return out;
}

void remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(mtx);
    for (size_t i = 0; i < list.size(); i++) {
        if (list[i].value("id", "") != id) continue;
        if (id == currentId) cancelCurrent = true;  // il thread cancella il file parziale
        json removed = list[i];
        list.erase(list.begin() + i);
        deleteFiles(removed);
        // ultima puntata di quell'anime: via anche copertina e cartella
        bool others = false;
        for (auto& it : list)
            if (it.value("sourceId", "") == removed.value("sourceId", "") &&
                it.value("animeUrl", "") == removed.value("animeUrl", ""))
                others = true;
        std::string cover = removed.value("cover", "");
        if (!others && !cover.empty()) {
            std::remove(cover.c_str());
            std::string dir = cover.substr(0, cover.find_last_of('/'));
            rmdir(dir.c_str());
        }
        saveLocked();
        return;
    }
}

void retry(const std::string& id) {
    std::lock_guard<std::mutex> lock(mtx);
    if (json* it = findLocked(id)) {
        if (it->value("status", "") == "failed") {
            (*it)["status"] = "queued";
            (*it)["error"] = "";
            saveLocked();
            cv.notify_all();
        }
    }
}

void moveToTop(const std::string& id) {
    std::lock_guard<std::mutex> lock(mtx);
    for (size_t i = 0; i < list.size(); i++)
        if (list[i].value("id", "") == id) {
            json it = list[i];
            list.erase(list.begin() + i);
            list.insert(list.begin(), it);
            saveLocked();
            return;
        }
}

void setPaused(bool p) {
    std::lock_guard<std::mutex> lock(mtx);
    pausedFlag = p;
    if (p && !currentId.empty()) cancelCurrent = true;  // si ferma subito, ripartira' da capo
    saveLocked();
    cv.notify_all();
}

bool paused() {
    std::lock_guard<std::mutex> lock(mtx);
    return pausedFlag;
}

int pendingCount() {
    std::lock_guard<std::mutex> lock(mtx);
    int n = 0;
    for (auto& it : list)
        if (it.value("status", "") != "done") n++;
    return n;
}

}  // namespace downloads
