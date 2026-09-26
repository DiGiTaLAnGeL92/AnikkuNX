#pragma once

#include <functional>
#include <string>

/**
 * Debug interno: piccolo server HTTP sulla rete locale per inviare un nuovo AnikkuNX.nro dal PC
 * (pagina web con "scegli file" + codice PIN), senza passare da GitHub.
 * Si attiva tenendo premuti L+R mentre si sceglie "Controlla aggiornamenti".
 */
namespace uploadserver {

struct Status {
    bool running = false;
    int port = 0;
    long long received = 0;  // byte ricevuti del file in arrivo
    long long total = 0;     // dimensione annunciata (Content-Length)
    std::string state;       // "attesa", "ricezione", "ricevuto", "errore"
    std::string error;
};

/** Avvia il server (porta 8080..8090). destPath: dove salvare il file ricevuto. Ritorna la porta o 0. */
int start(const std::string& pin, const std::string& destPath);
void stop();
Status status();
/** Indirizzo IP della console sulla rete locale ("" se non connessa). */
std::string localIp();

}  // namespace uploadserver
