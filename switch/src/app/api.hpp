#pragma once

#include <nlohmann/json.hpp>

#include <string>

/**
 * Livello dati dell'app: interroga direttamente le fonti integrate e salva libreria e
 * progressi sulla scheda SD. Restituisce JSON con la stessa forma usata dall'interfaccia.
 * Le funzioni sono bloccanti: usarle con runAsync.
 */
namespace api {

using json = nlohmann::json;

/** Da chiamare all'avvio: cartella dei dati (es. sdmc:/switch/AnikkuNX). */
void init(const std::string& dataDir);

/** Fonti attive (o tutte, con il campo "enabled", se includeDisabled). */
json sources(bool includeDisabled = false);
json browse(const std::string& sourceId, const std::string& mode, int page, const std::string& query = "");
json anime(const std::string& sourceId, const std::string& url, const std::string& title = "", bool cached = false);
json hosters(const std::string& sourceId, const std::string& episodeUrl, const std::string& episodeName);
json hosterVideos(const std::string& sourceId, const std::string& episodeUrl, int index);
json play(const std::string& token);

json library();
void addLibrary(const std::string& sourceId, const std::string& url, const std::string& title);
void removeLibrary(const std::string& sourceId, const std::string& url);
json history();
/** Toglie un anime da "Continua a guardare" (i progressi degli episodi restano). */
void removeFromHistory(const std::string& sourceId, const std::string& animeUrl);
void saveProgress(const json& progress);

/** Scarica un'immagine (copertina) con le intestazioni giuste per il sito. */
std::string download(const std::string& url);

}  // namespace api
