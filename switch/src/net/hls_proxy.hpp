#pragma once

#include <string>

#include "net/http.hpp"

/**
 * Piccolo proxy HTTP locale (127.0.0.1) per gli stream HLS "difficili".
 *
 * Alcuni CDN (es. i server HD-1 di Anichi/Anikoto) camuffano i segmenti MPEG-TS da immagini: ogni segmento
 * inizia con un'intestazione PNG/JPEG finta. ffmpeg li riconosce come immagini, "riproduce" un fotogramma
 * e arriva subito alla fine. Il proxy scarica playlist e segmenti con le intestazioni della fonte,
 * riscrive le playlist in modo che puntino al proxy e toglie l'intestazione finta dai segmenti.
 */
namespace hlsproxy {

/** true se lo stream HLS ha segmenti camuffati (controlla la playlist e l'inizio del primo segmento). */
bool needsProxy(const std::string& url, const http::Headers& headers);

/** Restituisce l'URL locale da passare a mpv (avvia il server se serve). Se il server non parte, "" */
std::string wrap(const std::string& url, const http::Headers& headers);

/** Toglie l'intestazione immagine finta davanti a un segmento TS/fMP4 (usata anche dai test). */
std::string stripFakeHeader(const std::string& data);

}  // namespace hlsproxy
