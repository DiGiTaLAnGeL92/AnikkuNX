#include "net/http.hpp"
#include "util/i18n.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <mutex>
#include <set>

namespace http {

const char* DEFAULT_UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 "
    "Safari/537.36";

static CURLSH* share = nullptr;
static std::mutex shareLocks[CURL_LOCK_DATA_LAST];
static std::string caBundle;

static void lockCb(CURL*, curl_lock_data data, curl_lock_access, void*) { shareLocks[data].lock(); }
static void unlockCb(CURL*, curl_lock_data data, void*) { shareLocks[data].unlock(); }

void globalInit(const std::string& caBundlePath) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    caBundle = caBundlePath;
    share = curl_share_init();
    curl_share_setopt(share, CURLSHOPT_LOCKFUNC, lockCb);
    curl_share_setopt(share, CURLSHOPT_UNLOCKFUNC, unlockCb);
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
}

void globalCleanup() {
    if (share) curl_share_cleanup(share);
    share = nullptr;
    curl_global_cleanup();
}

static size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

static size_t headerCb(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* res = static_cast<Response*>(userdata);
    std::string line(buffer, size * nitems);
    // una nuova risposta (redirect) azzera le intestazioni precedenti
    if (line.rfind("HTTP/", 0) == 0) {
        res->headers.clear();
        return size * nitems;
    }
    auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string name = line.substr(0, colon);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
        std::string value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' '))
            value.pop_back();
        res->headers.emplace(name, value);
    }
    return size * nitems;
}

// Siti il cui certificato non viene accettato dalla libreria TLS della Switch (mbedTLS, piu' vecchia di
// quella dei PC): dopo il primo errore di verifica si ripete la richiesta senza verifica del certificato.
static std::mutex insecureMutex;
static std::set<std::string> insecureHosts;

static bool isInsecureHost(const std::string& host) {
    std::lock_guard<std::mutex> lock(insecureMutex);
    return insecureHosts.count(host) > 0;
}

static CURLcode perform(const std::string& method, const std::string& url, const Headers& headers,
                        const std::string& body, long timeoutSeconds, bool followRedirects, bool insecure,
                        Response& res) {
    CURL* curl = curl_easy_init();
    if (!curl) throw Error(tr("curl non disponibile"));

    res = Response();
    struct curl_slist* list = nullptr;
    bool hasUA = false;
    for (auto& h : headers) {
        std::string lower = h.first;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        if (lower == "user-agent") hasUA = true;
        list = curl_slist_append(list, (h.first + ": " + h.second).c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    if (list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    if (!hasUA) curl_easy_setopt(curl, CURLOPT_USERAGENT, DEFAULT_UA);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &res);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, followRedirects ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");  // abilita il motore dei cookie
    if (share) curl_easy_setopt(curl, CURLOPT_SHARE, share);
    if (!caBundle.empty()) curl_easy_setopt(curl, CURLOPT_CAINFO, caBundle.c_str());
    if (insecure) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    } else if (method == "HEAD") {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    } else if (method != "GET") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res.status);
        char* eff = nullptr;
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff);
        res.finalUrl = eff ? eff : url;
    }
    curl_slist_free_all(list);
    curl_easy_cleanup(curl);
    return rc;
}

std::function<void(const std::string&, const std::string&, const std::string&, const Response&)> debugHook;

Response request(const std::string& method, const std::string& url, const Headers& headers, const std::string& body,
                 long timeoutSeconds, bool followRedirects) {
    Response res;
    std::string host = hostOf(url);
    bool insecure = isInsecureHost(host);
    CURLcode rc = perform(method, url, headers, body, timeoutSeconds, followRedirects, insecure, res);
    if (!insecure && (rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CACERT_BADFILE ||
                      rc == CURLE_SSL_ISSUER_ERROR)) {
        {
            std::lock_guard<std::mutex> lock(insecureMutex);
            insecureHosts.insert(host);
        }
        rc = perform(method, url, headers, body, timeoutSeconds, followRedirects, true, res);
    }

    if (rc != CURLE_OK) {
        if (rc == CURLE_COULDNT_RESOLVE_HOST) throw Error(tr("Sito non raggiungibile ({}): e' cambiato dominio?", host));
        if (rc == CURLE_OPERATION_TIMEDOUT) throw Error(tr("Il sito {} non risponde (timeout)", host));
        if (rc == CURLE_COULDNT_CONNECT) throw Error(tr("Connessione a {} rifiutata", host));
        if (rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CONNECT_ERROR)
            throw Error(tr("Errore di connessione sicura con {} ({})", host, curl_easy_strerror(rc)));
        throw Error(tr("Errore di rete: {}", curl_easy_strerror(rc)));
    }
    if (debugHook) debugHook(method, url, body, res);
    return res;
}

namespace {
struct DownloadCtx {
    FILE* file = nullptr;
    std::function<bool(long long, long long)> progress;
};

size_t fileWriteCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<DownloadCtx*>(userdata);
    return fwrite(ptr, size, nmemb, ctx->file) * size;
}

int progressCb(void* userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<DownloadCtx*>(userdata);
    if (ctx->progress && !ctx->progress((long long)dlnow, (long long)dltotal)) return 1;  // annullato
    return 0;
}
}  // namespace

void downloadToFile(const std::string& url, const std::string& path, const Headers& headers,
                    std::function<bool(long long, long long)> progress) {
    DownloadCtx ctx;
    ctx.progress = std::move(progress);
    ctx.file = fopen(path.c_str(), "wb");
    if (!ctx.file) throw Error(tr("Impossibile scrivere {}", path));

    auto attempt = [&](bool insecure) {
        fseek(ctx.file, 0, SEEK_SET);
        CURL* curl = curl_easy_init();
        struct curl_slist* list = nullptr;
        for (auto& h : headers) list = curl_slist_append(list, (h.first + ": " + h.second).c_str());
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        if (list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, DEFAULT_UA);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 512L);  // interrompe se fermo per 60 s
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fileWriteCb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
        if (!caBundle.empty()) curl_easy_setopt(curl, CURLOPT_CAINFO, caBundle.c_str());
        if (insecure) {
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        }
        CURLcode rc = curl_easy_perform(curl);
        curl_slist_free_all(list);
        curl_easy_cleanup(curl);
        return rc;
    };
    CURLcode rc = attempt(isInsecureHost(hostOf(url)));
    if (rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_ISSUER_ERROR) rc = attempt(true);
    fclose(ctx.file);
    if (rc != CURLE_OK) {
        std::remove(path.c_str());
        if (rc == CURLE_ABORTED_BY_CALLBACK) throw Error(tr("Download annullato"));
        throw Error(tr("Download non riuscito: {}", curl_easy_strerror(rc)));
    }
}

std::string getText(const std::string& url, const Headers& headers, long timeout) {
    Response r = request("GET", url, headers, "", timeout);
    if (r.status < 200 || r.status >= 300)
        throw Error(tr("HTTP {} da {}", std::to_string(r.status), hostOf(url)));
    return std::move(r.body);
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string originOf(const std::string& url) {
    auto p = url.find("://");
    if (p == std::string::npos) return "";
    auto slash = url.find('/', p + 3);
    return slash == std::string::npos ? url : url.substr(0, slash);
}

std::string hostOf(const std::string& url) {
    std::string o = originOf(url);
    auto p = o.find("://");
    std::string h = p == std::string::npos ? o : o.substr(p + 3);
    auto at = h.find('@');
    if (at != std::string::npos) h = h.substr(at + 1);
    auto colon = h.find(':');
    if (colon != std::string::npos) h = h.substr(0, colon);
    return h;
}

std::string pathOf(const std::string& url) {
    auto p = url.find("://");
    if (p == std::string::npos) return url;
    auto slash = url.find('/', p + 3);
    return slash == std::string::npos ? "/" : url.substr(slash);
}

std::string resolve(const std::string& base, const std::string& rel) {
    if (rel.empty()) return base;
    if (rel.rfind("http://", 0) == 0 || rel.rfind("https://", 0) == 0) return rel;
    if (rel.rfind("//", 0) == 0) {
        auto p = base.find("://");
        return (p == std::string::npos ? "https:" : base.substr(0, p + 1)) + rel;
    }
    std::string origin = originOf(base);
    if (rel[0] == '/') return origin + rel;
    if (rel[0] == '?') {
        auto q = base.find('?');
        return (q == std::string::npos ? base : base.substr(0, q)) + rel;
    }
    // relativo alla cartella corrente
    std::string path = pathOf(base);
    auto q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);
    auto lastSlash = path.rfind('/');
    std::string dir = lastSlash == std::string::npos ? "/" : path.substr(0, lastSlash + 1);
    return origin + dir + rel;
}

std::string queryParam(const std::string& url, const std::string& name) {
    auto q = url.find('?');
    if (q == std::string::npos) return "";
    std::string qs = url.substr(q + 1);
    auto hash = qs.find('#');
    if (hash != std::string::npos) qs = qs.substr(0, hash);
    size_t pos = 0;
    while (pos <= qs.size()) {
        auto amp = qs.find('&', pos);
        std::string part = qs.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        auto eq = part.find('=');
        if (part.substr(0, eq) == name) return eq == std::string::npos ? "" : part.substr(eq + 1);
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return "";
}

}  // namespace http
