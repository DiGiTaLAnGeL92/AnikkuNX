#include "net/hls_proxy.hpp"

#ifdef _WIN32
// solo per lo strumento di prova su Windows (curl ha gia' inizializzato Winsock)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#define close closesocket
#define SHUT_RDWR SD_BOTH
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <pthread.h>
#include <cstdint>
#include <cstdio>

namespace hlsproxy {

namespace {

std::mutex mtx;
int serverPort = 0;
bool serverFailed = false;
int nextSession = 1;
std::map<int, http::Headers> sessions;  // id -> intestazioni della fonte
std::atomic<int> activeClients{0};
std::string logPath;
std::mutex logMutex;

/** Diario del proxy (sdmc:/switch/AnikkuNX/proxy.log): utile se qualcosa va storto sulla console. */
void proxyLog(const std::string& msg) {
    std::lock_guard<std::mutex> lock(logMutex);
    if (logPath.empty()) return;
    FILE* f = fopen(logPath.c_str(), "a");
    if (!f) return;
    fprintf(f, "%s\n", msg.c_str());
    fclose(f);
}

// ---------------------------------------------------------------- base64url (senza '=')
const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string b64enc(const std::string& in) {
    std::string out;
    unsigned int val = 0;
    int bits = -6;
    for (unsigned char c : in) {
        val = ((val << 8) + c) & 0xFFFFFF;
        bits += 8;
        while (bits >= 0) {
            out.push_back(B64[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) out.push_back(B64[((val << 8) >> (bits + 8)) & 0x3F]);
    return out;
}

std::string b64dec(const std::string& in) {
    int T[256];
    for (int& t : T) t = -1;
    for (int i = 0; i < 64; i++) T[(unsigned char)B64[i]] = i;
    std::string out;
    unsigned int val = 0;
    int bits = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) break;
        val = ((val << 6) + (unsigned)T[c]) & 0xFFFFFF;
        bits += 6;
        if (bits >= 0) {
            out.push_back(char((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

bool startsWith(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }

bool looksLikeImage(const std::string& b) {
    auto at = [&](size_t off, const char* m) {
        size_t n = strlen(m);
        return b.size() >= off + n && b.compare(off, n, m) == 0;
    };
    // attenzione: niente firme che iniziano con \0 (strlen le renderebbe vuote e combacerebbero con tutto)
    if (b.size() >= 4 && (unsigned char)b[0] == 0x47) return false;  // e' gia' MPEG-TS
    return at(0, "\x89PNG") || at(0, "\xFF\xD8\xFF") || at(0, "GIF8") || at(0, "BM") || at(0, "RIFF");
}

std::string proxyUrl(int sid, const std::string& target, bool playlist) {
    return "http://127.0.0.1:" + std::to_string(serverPort) + (playlist ? "/p/" : "/s/") + std::to_string(sid) +
           "/" + b64enc(target) + (playlist ? ".m3u8" : ".ts");
}

/** Riscrive tutti gli URI della playlist verso il proxy. */
std::string rewrite(const std::string& body, const std::string& base, int sid) {
    std::istringstream in(body);
    std::string line, out;
    bool nextIsPlaylist = false;
    auto rewriteAttr = [&](std::string l, bool playlist) {
        size_t p = l.find("URI=\"");
        if (p == std::string::npos) return l;
        size_t s = p + 5, e = l.find('"', s);
        if (e == std::string::npos) return l;
        std::string abs = http::resolve(base, l.substr(s, e - s));
        return l.substr(0, s) + proxyUrl(sid, abs, playlist) + l.substr(e);
    };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) {
            out += "\n";
            continue;
        }
        if (line[0] == '#') {
            if (startsWith(line, "#EXT-X-STREAM-INF")) nextIsPlaylist = true;
            if (startsWith(line, "#EXT-X-MEDIA:") || startsWith(line, "#EXT-X-I-FRAME-STREAM-INF"))
                line = rewriteAttr(line, true);
            else if (startsWith(line, "#EXT-X-KEY") || startsWith(line, "#EXT-X-MAP") ||
                     startsWith(line, "#EXT-X-SESSION-KEY"))
                line = rewriteAttr(line, false);
            out += line + "\n";
            continue;
        }
        std::string abs = http::resolve(base, line);
        out += proxyUrl(sid, abs, nextIsPlaylist) + "\n";
        nextIsPlaylist = false;
    }
    return out;
}

void sendAll(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        long n = send(fd, data.data() + off, (int)std::min<size_t>(data.size() - off, 64 * 1024), 0);
        if (n <= 0) return;
        off += (size_t)n;
    }
}

void reply(int fd, int status, const std::string& type, const std::string& body, bool headOnly) {
    std::string head = "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : " Error") +
                       "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) +
                       "\r\nConnection: close\r\n\r\n";
    sendAll(fd, head);
    if (!headOnly) sendAll(fd, body);
}

void handleClientImpl(int fd);

void handleClient(int fd) {
    activeClients++;
    try {
        handleClientImpl(fd);
    } catch (...) {
    }
    shutdown(fd, SHUT_RDWR);
    close(fd);
    activeClients--;
}

void handleClientImpl(int fd) {
    std::string req;
    char buf[4096];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
        long n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        req.append(buf, (size_t)n);
    }
    bool headOnly = startsWith(req, "HEAD ");
    size_t sp1 = req.find(' '), sp2 = sp1 == std::string::npos ? sp1 : req.find(' ', sp1 + 1);
    std::string path = (sp1 != std::string::npos && sp2 != std::string::npos) ? req.substr(sp1 + 1, sp2 - sp1 - 1) : "";

    // /p/<sid>/<b64>.m3u8  oppure  /s/<sid>/<b64>.ts
    bool ok = false;
    if (path.size() > 4 && path[0] == '/' && (path[1] == 'p' || path[1] == 's') && path[2] == '/') {
        size_t slash = path.find('/', 3);
        if (slash != std::string::npos) {
            int sid = atoi(path.substr(3, slash - 3).c_str());
            std::string enc = path.substr(slash + 1);
            size_t dot = enc.find('.');
            if (dot != std::string::npos) enc = enc.substr(0, dot);
            std::string target = b64dec(enc);
            proxyLog(std::string(path[1] == 'p' ? "playlist " : "segmento ") + target.substr(0, 120));
            http::Headers headers;
            {
                std::lock_guard<std::mutex> lock(mtx);
                auto it = sessions.find(sid);
                if (it != sessions.end()) headers = it->second;
            }
            try {
                http::Response r;
                for (int attempt = 0; attempt < 3; attempt++) {
                    r = http::request("GET", target, headers, "", 60);
                    if (r.status < 500 && r.status != 0) break;
                }
                proxyLog("  HTTP " + std::to_string(r.status) + ", " + std::to_string(r.body.size()) + " byte");
                if (r.status >= 200 && r.status < 300) {
                    std::string& body = r.body;
                    std::string base = r.finalUrl.empty() ? target : r.finalUrl;
                    size_t skip = 0;
                    while (skip < body.size() && (unsigned char)body[skip] <= ' ') skip++;
                    if (body.compare(skip, 7, "#EXTM3U") == 0 || body.compare(skip, 10, "\xEF\xBB\xBF#EXTM3U") == 0) {
                        reply(fd, 200, "application/vnd.apple.mpegurl", rewrite(body, base, sid), headOnly);
                    } else {
                        std::string clean = stripFakeHeader(body);
                        proxyLog("  inviati " + std::to_string(clean.size()) + " byte (tolti " +
                                 std::to_string(body.size() - clean.size()) + ")");
                        reply(fd, 200, "video/mp2t", clean, headOnly);
                    }
                } else {
                    reply(fd, r.status ? (int)r.status : 502, "text/plain", "upstream error", headOnly);
                }
                ok = true;
            } catch (const std::exception& e) {
                proxyLog(std::string("  errore: ") + e.what());
                reply(fd, 502, "text/plain", e.what(), headOnly);
                ok = true;
            }
        }
    }
    if (!ok) reply(fd, 404, "text/plain", "not found", headOnly);
}

/**
 * Pochi thread fissi, creati una volta sola, che fanno accept() e servono la richiesta:
 * niente thread creati e distrutti per ogni segmento (su Switch le risorse dei thread sono limitate).
 */
void* workerLoop(void* arg) {
    int sock = (int)(intptr_t)arg;
    int failures = 0;
    while (true) {
        sockaddr_in cli{};
        socklen_t len = sizeof(cli);
        int fd = accept(sock, (sockaddr*)&cli, &len);
        if (fd < 0) {
            if (++failures > 50) break;  // socket chiuso (uscita dall'app)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        failures = 0;
        proxyLog("richiesta");
        handleClient(fd);
    }
    return nullptr;
}

bool startWorkers(int sock) {
    int started = 0;
    for (int i = 0; i < 3; i++) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 512 * 1024);  // curl + TLS: stack generoso
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_t th;
        if (pthread_create(&th, &attr, workerLoop, (void*)(intptr_t)sock) == 0) started++;
        pthread_attr_destroy(&attr);
    }
    proxyLog("thread avviati: " + std::to_string(started));
    return started > 0;
}

bool ensureServer() {
    if (serverPort) return true;
    if (serverFailed) return false;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        serverFailed = true;
        return false;
    }
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // porta fissa in un intervallo alto: su Switch getsockname con porta 0 non e' sempre affidabile
    for (int port = 48620; port < 48660; port++) {
        addr.sin_port = htons(port);
        if (bind(sock, (sockaddr*)&addr, sizeof(addr)) == 0 && listen(sock, 8) == 0) {
            serverPort = port;
            break;
        }
    }
    if (!serverPort) {
        close(sock);
        serverFailed = true;
        return false;
    }
    if (!startWorkers(sock)) {
        close(sock);
        serverPort = 0;
        serverFailed = true;
        return false;
    }
    return true;
}

}  // namespace

void setLogFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(logMutex);
    if (logPath == path) return;
    logPath = path;
    FILE* f = fopen(path.c_str(), "w");
    if (f) fclose(f);
}

std::string stripFakeHeader(const std::string& b) {
    if (!looksLikeImage(b)) return b;
    size_t limit = std::min<size_t>(b.size(), 256 * 1024);
    for (size_t i = 1; i + 376 < limit; i++)
        if ((unsigned char)b[i] == 0x47 && (unsigned char)b[i + 188] == 0x47 && (unsigned char)b[i + 376] == 0x47)
            return b.substr(i);
    // fMP4 camuffato
    for (size_t i = 1; i + 8 < limit; i++)
        if (b.compare(i + 4, 4, "ftyp") == 0 || b.compare(i + 4, 4, "styp") == 0 || b.compare(i + 4, 4, "moof") == 0)
            return b.substr(i);
    return b;
}

bool needsProxy(const std::string& url, const http::Headers& headers) {
    try {
        http::Response pl = http::get(url, headers, 20);
        if (pl.status < 200 || pl.status >= 300 || pl.body.find("#EXTM3U") == std::string::npos) return false;
        std::string base = pl.finalUrl.empty() ? url : pl.finalUrl;
        auto firstUri = [](const std::string& body, bool afterStreamInf) {
            std::istringstream in(body);
            std::string line;
            bool want = !afterStreamInf;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                if (line[0] == '#') {
                    if (startsWith(line, "#EXT-X-STREAM-INF")) want = true;
                    continue;
                }
                if (want) return line;
            }
            return std::string();
        };
        std::string body = pl.body;
        if (body.find("#EXT-X-STREAM-INF") != std::string::npos) {  // master: prendi la prima variante
            std::string v = firstUri(body, true);
            if (v.empty()) return false;
            std::string vurl = http::resolve(base, v);
            http::Response vr = http::get(vurl, headers, 20);
            if (vr.status < 200 || vr.status >= 300) return false;
            body = vr.body;
            base = vr.finalUrl.empty() ? vurl : vr.finalUrl;
        }
        if (body.find("#EXT-X-KEY:METHOD=AES") != std::string::npos) return false;  // cifrati: non toccare
        std::string seg = firstUri(body, false);
        if (seg.empty()) return false;
        http::Headers h = headers;
        h.push_back({"Range", "bytes=0-2047"});
        http::Response sr = http::get(http::resolve(base, seg), h, 20);
        if (sr.status < 200 || sr.status >= 300) return false;
        return looksLikeImage(sr.body);
    } catch (...) {
        return false;
    }
}

std::string wrap(const std::string& url, const http::Headers& headers) {
    std::lock_guard<std::mutex> lock(mtx);
    if (!ensureServer()) {
        proxyLog("server non avviato");
        return "";
    }
    proxyLog("nuovo stream: " + url.substr(0, 120));
    int sid = nextSession++;
    sessions[sid] = headers;
    while (sessions.size() > 32) sessions.erase(sessions.begin());
    return proxyUrl(sid, url, true);
}

}  // namespace hlsproxy
