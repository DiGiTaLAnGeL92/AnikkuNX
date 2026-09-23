#pragma once

#include <functional>
#include <string>

/**
 * Aggiornamento dell'app dalle release di GitHub.
 * La versione corrente (APP_VERSION) e il repository (UPDATE_REPO) arrivano da CMakeLists.txt.
 */
namespace updater {

struct Release {
    std::string version;  // es. "0.3.2"
    std::string tag;      // es. "v0.3.2"
    std::string notes;    // testo della release
    std::string nroUrl;   // link diretto a AnikkuNX.nro
    long long size = 0;
};

/** Versione in esecuzione, es. "0.3.1". */
std::string currentVersion();
/** true se la versione a e' piu' recente di b (confronto numerico x.y.z). */
bool isNewer(const std::string& a, const std::string& b);
/** Percorso del .nro in esecuzione (da argv[0]); va impostato all'avvio. */
void setAppPath(const std::string& argv0);
std::string appPath();

/** Legge l'ultima release (bloccante, lancia http::Error). */
Release latest();
/** Scarica e sostituisce il .nro installato (bloccante). progress(0..1); se ritorna false annulla. */
void install(const Release& r, std::function<bool(float)> progress);

}  // namespace updater
