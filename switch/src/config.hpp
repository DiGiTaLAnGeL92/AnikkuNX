#pragma once

#include <map>
#include <set>
#include <string>

/** Impostazioni salvate su scheda SD (sdmc:/switch/AnikkuNX/config.json). */
class Config {
  public:
    static Config& instance() {
        static Config c;
        return c;
    }

    void load();
    void save();

    std::string configDir() const;

    /** Domini alternativi delle fonti (id fonte -> https://...). */
    std::map<std::string, std::string> domains;
    void applyDomains();

    /** Fonti attivate dall'utente (scelte al primo avvio). */
    std::set<std::string> enabledSources;
    bool sourcesChosen = false;
    bool showNsfw = false;
    bool isSourceEnabled(const std::string& id) const;
    bool hardwareDecoding = true;
    bool checkUpdates = true;       // controlla gli aggiornamenti all'avvio
    bool checkNewEpisodes = true;   // controlla i nuovi episodi della libreria all'avvio
    std::string skippedVersion;     // versione che l'utente ha scelto di saltare
    bool autoSkipOpening = false;
    int seekSeconds = 10;
};
