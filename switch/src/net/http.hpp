#pragma once

#include <map>
#include <stdexcept>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace http {

using Headers = std::vector<std::pair<std::string, std::string>>;

struct Response {
    long status = 0;
    std::string body;
    std::string finalUrl;
    /** intestazioni con nome in minuscolo (Set-Cookie puo' comparire piu' volte) */
    std::multimap<std::string, std::string> headers;

    std::string header(const std::string& lowerName) const {
        auto it = headers.find(lowerName);
        return it == headers.end() ? "" : it->second;
    }
};

class Error : public std::runtime_error {
  public:
    explicit Error(const std::string& msg) : std::runtime_error(msg) {}
};

extern const char* DEFAULT_UA;

/** Solo per lo strumento di prova: se impostato, riceve ogni richiesta completata (metodo, url, corpo inviato, risposta). */
extern std::function<void(const std::string&, const std::string&, const std::string&, const Response&)> debugHook;

void globalInit(const std::string& caBundlePath);
void globalCleanup();

/** Richiesta HTTP sincrona. I cookie sono condivisi tra tutte le richieste (come un browser). */
Response request(const std::string& method, const std::string& url, const Headers& headers = {},
                 const std::string& body = "", long timeoutSeconds = 30, bool followRedirects = true);

inline Response get(const std::string& url, const Headers& headers = {}, long timeout = 30) {
    return request("GET", url, headers, "", timeout);
}

/** Come get() ma lancia un errore se lo stato non e' 2xx. */
std::string getText(const std::string& url, const Headers& headers = {}, long timeout = 30);

/**
 * Scarica un file direttamente su disco (senza tenerlo in memoria), seguendo i redirect.
 * progress(scaricati, totale) viene chiamato dal thread di lavoro; se ritorna false il download si interrompe.
 */
void downloadToFile(const std::string& url, const std::string& path, const Headers& headers = {},
                    std::function<bool(long long, long long)> progress = nullptr);

std::string urlEncode(const std::string& s);
/** Risolve un URL relativo rispetto a una base (come "abs:href" di Jsoup). */
std::string resolve(const std::string& base, const std::string& rel);
/** "https://host/percorso?x" -> "/percorso?x" */
std::string pathOf(const std::string& url);
/** "https://host/percorso" -> "https://host" */
std::string originOf(const std::string& url);
std::string hostOf(const std::string& url);
std::string queryParam(const std::string& url, const std::string& name);

}  // namespace http
