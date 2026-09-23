#include "app/updater.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

#include "net/http.hpp"
#include "util/i18n.hpp"

#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif
#ifndef UPDATE_REPO
#define UPDATE_REPO "DiGiTaLAnGeL92/AnikkuNX"
#endif

namespace updater {

namespace {
std::string nroPath = "sdmc:/switch/AnikkuNX/AnikkuNX.nro";

std::vector<int> parts(std::string v) {
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v = v.substr(1);
    std::vector<int> out;
    std::stringstream ss(v);
    std::string item;
    while (std::getline(ss, item, '.')) out.push_back(std::atoi(item.c_str()));
    while (out.size() < 3) out.push_back(0);
    return out;
}
}  // namespace

std::string currentVersion() { return APP_VERSION; }

bool isNewer(const std::string& a, const std::string& b) { return parts(a) > parts(b); }

void setAppPath(const std::string& argv0) {
    // hbloader passa il percorso completo, es. "sdmc:/switch/AnikkuNX/AnikkuNX.nro"
    if (argv0.rfind("sdmc:/", 0) == 0 && argv0.size() > 4 && argv0.substr(argv0.size() - 4) == ".nro")
        nroPath = argv0;
}

std::string appPath() { return nroPath; }

Release latest() {
    http::Headers h = {{"Accept", "application/vnd.github+json"}, {"User-Agent", "AnikkuNX-updater"}};
    http::Response r = http::request("GET", "https://api.github.com/repos/" UPDATE_REPO "/releases/latest", h, "", 20);
    if (r.status == 404) throw http::Error(tr("Nessuna release pubblicata"));
    if (r.status < 200 || r.status >= 300) throw http::Error("GitHub: HTTP " + std::to_string(r.status));
    auto j = nlohmann::json::parse(r.body);
    Release rel;
    rel.tag = j.value("tag_name", "");
    rel.version = rel.tag.empty() || (rel.tag[0] != 'v' && rel.tag[0] != 'V') ? rel.tag : rel.tag.substr(1);
    rel.notes = j["body"].is_string() ? j["body"].get<std::string>() : "";
    for (auto& a : j.value("assets", nlohmann::json::array())) {
        std::string name = a.value("name", "");
        if (name.size() > 4 && name.substr(name.size() - 4) == ".nro") {
            rel.nroUrl = a.value("browser_download_url", "");
            rel.size = a.value("size", 0LL);
            break;
        }
    }
    return rel;
}

void install(const Release& r, std::function<bool(float)> progress) {
    if (r.nroUrl.empty()) throw http::Error(tr("La release non contiene il file .nro"));
    std::string tmp = nroPath + ".download";
    http::downloadToFile(r.nroUrl, tmp, {{"Accept", "application/octet-stream"}},
                         [&](long long now, long long total) {
                             if (total <= 0) total = r.size;
                             return progress ? progress(total > 0 ? (float)now / (float)total : 0.f) : true;
                         });

    // controlli: dimensione e firma "NRO0" all'offset 0x10
    FILE* f = fopen(tmp.c_str(), "rb");
    if (!f) throw http::Error(tr("File scaricato non leggibile"));
    char magic[4] = {0};
    fseek(f, 0x10, SEEK_SET);
    size_t n = fread(magic, 1, 4, f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    if (n != 4 || std::memcmp(magic, "NRO0", 4) != 0 || (r.size > 0 && size != r.size)) {
        std::remove(tmp.c_str());
        throw http::Error(tr("Il file scaricato non e' valido, riprova"));
    }

    // sostituzione: il .nro in esecuzione e' gia' in memoria, il file si puo' rimpiazzare
    std::string backup = nroPath + ".old";
    std::remove(backup.c_str());
    std::rename(nroPath.c_str(), backup.c_str());
    if (std::rename(tmp.c_str(), nroPath.c_str()) != 0) {
        std::rename(backup.c_str(), nroPath.c_str());  // ripristina la versione precedente
        std::remove(tmp.c_str());
        throw http::Error(tr("Impossibile sostituire {}", nroPath));
    }
    std::remove(backup.c_str());
}

}  // namespace updater
