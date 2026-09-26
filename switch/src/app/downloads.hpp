#pragma once

#include <nlohmann/json.hpp>

#include <string>

/**
 * Download degli episodi per la visione offline.
 *
 * Una coda (salvata in <dati>/downloads.json) scaricata un episodio alla volta da un thread dedicato.
 * Per ogni episodio si provano i video della fonte uno dopo l'altro: un video vale solo se il suo
 * contenuto e' davvero video (primi byte / primo segmento controllati), altrimenti si passa al successivo.
 * Formati: file diretti (MP4/MKV/WebM/TS) e HLS (anche cifrato AES-128, fMP4 e segmenti camuffati),
 * salvati in <dati>/downloads/<anime>/. I sottotitoli esterni vengono salvati accanto al video.
 */
namespace downloads {

using json = nlohmann::json;

/** Da chiamare all'avvio (dopo api::init): carica la coda e avvia il thread. */
void init(const std::string& dataDir);

struct EpisodeInfo {
    std::string sourceId;
    std::string animeUrl;
    std::string animeTitle;
    std::string thumbnail;
    std::string episodeUrl;
    std::string episodeName;
    double number = -1;
};

/** Aggiunge alla coda. Ritorna false se l'episodio e' gia' in coda o scaricato. */
bool enqueue(const EpisodeInfo& ep);

/** Stato di un episodio: "" (mai scaricato), "queued", "downloading", "done", "failed". */
std::string status(const std::string& sourceId, const std::string& episodeUrl, double* progress = nullptr);

/** File locale di un episodio scaricato (con i sottotitoli), oppure null se non c'e'. */
json localFile(const std::string& sourceId, const std::string& episodeUrl);

/** Tutte le voci (copia): [{id, sourceId, animeUrl, animeTitle, thumbnail, cover, episodeUrl, episodeName,
 *  number, status, progress, bytes, error, file, subtitles, speed}] */
json items();

/** Anime con almeno un episodio scaricato: [{sourceId, animeUrl, title, cover, count}] */
json animes();

/** Episodi scaricati di un anime, ordinati per numero. */
json episodes(const std::string& sourceId, const std::string& animeUrl);

void remove(const std::string& id);  // annulla se in corso e cancella i file
void retry(const std::string& id);
void moveToTop(const std::string& id);
void setPaused(bool paused);
bool paused();
/** Numero di voci non ancora completate (in coda, in corso, fallite). */
int pendingCount();

}  // namespace downloads
