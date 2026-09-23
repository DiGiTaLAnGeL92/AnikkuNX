#pragma once

#include <memory>
#include <string>
#include <vector>

#include "net/http.hpp"

/**
 * Fonti anime integrate nell'app (porting in C++ delle estensioni Aniyomi).
 * Tutti i metodi sono bloccanti e vanno chiamati fuori dal thread dell'interfaccia.
 */
namespace src {

struct Anime {
    std::string url;  // identificativo relativo al sito (come SAnime.url)
    std::string title;
    std::string thumbnail;
};

struct Page {
    std::vector<Anime> animes;
    bool hasNextPage = false;
};

struct Episode {
    std::string url;
    std::string name;
    double number = -1;
};

struct Details {
    std::string title;
    std::string thumbnail;
    std::string description;
    std::string genre;
    std::string author;
    std::string status;
    std::vector<Episode> episodes;  // dal piu' recente al piu' vecchio
};

struct Video {
    std::string title;  // server / qualita'
    std::string url;
    std::string referer;
    std::string userAgent;
    std::string cookie;
    int quality = 0;  // altezza in pixel se nota (per ordinare)
    http::Headers headers;  // intestazioni HTTP aggiuntive per il player (es. Origin)

    struct Track {
        std::string url;
        std::string lang;  // etichetta mostrata (es. "English")
    };
    std::vector<Track> subtitles;  // sottotitoli esterni (VTT/SRT/ASS)
    std::vector<Track> audio;      // tracce audio esterne
};

class Source {
  public:
    virtual ~Source() = default;

    virtual std::string id() const = 0;  // stabile, usato per libreria e progressi
    virtual std::string name() const = 0;
    virtual std::string defaultBaseUrl() const = 0;
    virtual bool supportsLatest() const { return true; }
    /** Lingua dei contenuti: "it", "en", "es", ... oppure "all" (multilingua). */
    virtual std::string lang() const { return "it"; }
    /** Fonte per adulti (18+): nascosta finche' l'utente non la attiva. */
    virtual bool nsfw() const { return false; }

    /** Dominio in uso (modificabile dalle impostazioni se il sito cambia indirizzo). */
    std::string baseUrl() const;

    virtual Page popular(int page) = 0;
    virtual Page latest(int page) = 0;
    virtual Page search(const std::string& query, int page) = 0;
    virtual Details details(const std::string& animeUrl) = 0;
    virtual std::vector<Video> videos(const std::string& episodeUrl) = 0;

    /** Intestazioni da usare per scaricare le copertine. */
    virtual http::Headers imageHeaders() const { return {{"Referer", baseUrl() + "/"}}; }
};

/** Elenco delle fonti disponibili. */
const std::vector<std::shared_ptr<Source>>& all();
std::shared_ptr<Source> byId(const std::string& id);

// ---- utilita' condivise dalle fonti
std::string trim(const std::string& s);
std::string replaceAll(std::string s, const std::string& from, const std::string& to);
std::string substringAfter(const std::string& s, const std::string& delim);
std::string substringBefore(const std::string& s, const std::string& delim);
std::string base64Decode(const std::string& in);
std::string digitsOnly(const std::string& s);
double parseNumber(const std::string& s, double def = -1);

}  // namespace src
