#pragma once

#include <borealis.hpp>
#include <nlohmann/json.hpp>

#include <chrono>

#include "util/async.hpp"
#include "view/mpv_view.hpp"

struct EpisodeRef {
    std::string url;
    std::string name;
    double number = -1;
};

struct PlayRequest {
    std::string sourceId;
    std::string animeUrl;
    std::string animeTitle;
    std::string thumbnail;
    std::vector<EpisodeRef> episodes;
    int index = 0;
    bool oldestFirst = false;  // ordine della lista episodi
    std::string forcedToken;  // video scelto a mano (opzionale)
    double startAt = 0;
};

/** Zone toccabili dell'overlay. */
enum class OverlayHit { NONE, BACK, REWIND, PLAY_PAUSE, FORWARD, BAR, SUBS, AUDIO, SKIP, PREV, NEXT, HINT };

struct HitRect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(const brls::Point& p, float pad = 0) const {
        return w > 0 && p.x >= x - pad && p.x <= x + w + pad && p.y >= y - pad && p.y <= y + h + pad;
    }
};

/** Barra informativa e comandi touch disegnati sopra al video con nanovg. */
class PlayerOverlay : public brls::View {
  public:
    explicit PlayerOverlay(MpvView* mpv);
    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
              brls::FrameContext* ctx) override;
    void poke(double seconds = 4.0);
    /** Ultima interazione dell'utente (tasti o tocchi), per lo standby. */
    std::chrono::steady_clock::time_point lastInteraction = std::chrono::steady_clock::now();
    void hide();
    bool controlsVisible() const;
    OverlayHit hitTest(const brls::Point& p) const;
    /** Posizione (0..1) sulla barra di avanzamento per una coordinata x dello schermo. */
    float barFraction(float screenX) const;
    void flash(const std::string& text, bool leftSide);
    /** Indicatore verticale di volume/luminosita' (0..1) sul lato indicato. */
    void showGauge(const std::string& label, float value, bool leftSide);

    float dim = 0;  // oscuramento software (0 = nessuno) quando la luminosita' di sistema non e' disponibile

    int seekStep = 10;
    float scrubFrac = -1;  // >= 0 mentre si trascina la barra

    std::string title;
    std::string subtitle;
    std::string message;  // caricamento / errori (sempre visibile se non vuoto)
    std::string hint;     // es. "Y: salta sigla"
    std::function<void()> onTick;  // chiamata a ogni frame

  private:
    void drawButton(NVGcontext* vg, HitRect& r, float cx, float cy, float radius, unsigned iconCode, bool dark);
    MpvView* mpv;
    std::chrono::steady_clock::time_point visibleUntil;
    std::string flashText;
    bool flashLeft = false;
    std::chrono::steady_clock::time_point flashUntil;
    std::string gaugeLabel;
    float gaugeValue = 0;
    bool gaugeLeft = false;
    std::chrono::steady_clock::time_point gaugeUntil;
    HitRect backRect, rewindRect, playRect, forwardRect, barRect, subsRect, audioRect, skipRect, prevRect, nextRect,
        hintRect;
};

class PlayerActivity : public brls::Activity {
  public:
    explicit PlayerActivity(PlayRequest req);
    ~PlayerActivity() override;

    brls::View* createContentView() override;
    void onContentAvailable() override;

  private:
    void startEpisode();
    void playResolved(const nlohmann::json& info);
    void saveProgress(bool force);
    void goToEpisode(int newIndex);
    int nextIndex() const;
    void skipSegment();
    void exitPlayer();
    void tick();
    void seekBy(double seconds);
    void togglePause();
    void cycleSubs();
    void cycleAudio();
    void playNext();
    void playPrevious();
    int previousIndex() const;
    void setVolume(float v);       // 0..1
    void readSystemVolume();
    bool systemVolume = false;
    int volumeTarget = 1, volumeMin = 0, volumeMax = 15;
    void setBrightness(float v);   // 0..1

    // scorrimento verticale: 0 = nessuno, 1 = volume (sinistra), 2 = luminosita' (destra)
    int swipeMode = 0;
    float swipeStartValue = 0;
    float volume = 1;
    float brightness = 1;
    float originalBrightness = -1;  // valore di sistema da ripristinare all'uscita
    bool systemBrightness = false;

    // risparmio energetico: in pausa da 30 s abbassa la luminosita', dopo altri 30 s mette in standby
    void updatePowerState();
    void applyIdleDim(bool on);
    int idleStage = 0;  // 0 attivo, 1 luminosita' ridotta, 2 standby richiesto
    bool mediaPlaying = false;
    std::chrono::steady_clock::time_point pausedSince;
    bool wasPaused = false;
    void onTap(const brls::Point& p);

    std::chrono::steady_clock::time_point lastTapTime;
    int lastTapSide = 0;  // -1 sinistra, 1 destra
    bool scrubbing = false;

    PlayRequest req;
    MpvView* mpv = nullptr;
    PlayerOverlay* overlay = nullptr;
    AliveToken alive = makeAlive();
    nlohmann::json current;  // info del video in riproduzione
    std::chrono::steady_clock::time_point lastSave;
    bool autoSkipped = false;
    std::shared_ptr<std::function<void(size_t)>> retryHolder;
};
