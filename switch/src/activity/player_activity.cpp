#include "activity/player_activity.hpp"
#include "util/i18n.hpp"

#include <cmath>

#include "config.hpp"

#ifdef __SWITCH__
#include <switch.h>
#endif
#include "app/api.hpp"
#include "net/http.hpp"

using json = nlohmann::json;

static std::string formatTime(double s) {
    if (s < 0 || std::isnan(s)) s = 0;
    long t = (long)s;
    long h = t / 3600, m = (t % 3600) / 60, sec = t % 60;
    char buf[32];
    if (h > 0)
        snprintf(buf, sizeof(buf), "%ld:%02ld:%02ld", h, m, sec);
    else
        snprintf(buf, sizeof(buf), "%02ld:%02ld", m, sec);
    return buf;
}

// ----------------------------------------------------------------------------- overlay

PlayerOverlay::PlayerOverlay(MpvView* m) : mpv(m) {
    this->setFocusable(false);
    poke(5);
}

void PlayerOverlay::poke(double seconds) {
    lastInteraction = std::chrono::steady_clock::now();
    visibleUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds((long)(seconds * 1000));
}

void PlayerOverlay::hide() { visibleUntil = std::chrono::steady_clock::now(); }

bool PlayerOverlay::controlsVisible() const {
    return std::chrono::steady_clock::now() < visibleUntil || mpv->paused || scrubFrac >= 0;
}

void PlayerOverlay::flash(const std::string& text, bool leftSide) {
    flashText = text;
    flashLeft = leftSide;
    flashUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
}

void PlayerOverlay::showGauge(const std::string& label, float value, bool leftSide) {
    gaugeLabel = label;
    gaugeValue = std::min(1.0f, std::max(0.0f, value));
    gaugeLeft = leftSide;
    gaugeUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(900);
}

OverlayHit PlayerOverlay::hitTest(const brls::Point& p) const {
    if (!hint.empty() && hintRect.contains(p, 6)) return OverlayHit::HINT;
    if (!controlsVisible()) return OverlayHit::NONE;
    if (backRect.contains(p, 12)) return OverlayHit::BACK;
    if (rewindRect.contains(p, 10)) return OverlayHit::REWIND;
    if (playRect.contains(p, 10)) return OverlayHit::PLAY_PAUSE;
    if (forwardRect.contains(p, 10)) return OverlayHit::FORWARD;
    if (subsRect.contains(p, 6)) return OverlayHit::SUBS;
    if (audioRect.contains(p, 6)) return OverlayHit::AUDIO;
    if (skipRect.contains(p, 6)) return OverlayHit::SKIP;
    if (prevRect.contains(p, 6)) return OverlayHit::PREV;
    if (nextRect.contains(p, 6)) return OverlayHit::NEXT;
    if (barRect.contains(p, 4)) return OverlayHit::BAR;
    return OverlayHit::NONE;
}

float PlayerOverlay::barFraction(float screenX) const {
    if (barRect.w <= 0) return 0;
    return std::min(1.0f, std::max(0.0f, (screenX - barRect.x) / barRect.w));
}

// icone Material (font "material" caricato da borealis)
namespace icon {
const unsigned ARROW_BACK = 0xe5c4, PLAY = 0xe037, PAUSE = 0xe034, REPLAY = 0xe042, FORWARD = 0xe01f,
               REPLAY_5 = 0xe05b, REPLAY_10 = 0xe059, REPLAY_30 = 0xe05a, FORWARD_5 = 0xe058, FORWARD_10 = 0xe056,
               FORWARD_30 = 0xe057, SKIP_PREV = 0xe045, SKIP_NEXT = 0xe044, SUBTITLES = 0xe048,
               AUDIOTRACK = 0xe3a1, FAST_FORWARD = 0xe01f, VOLUME = 0xe050, BRIGHTNESS = 0xe1ac;
}

static std::string utf8(unsigned cp) {
    std::string out;
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
    return out;
}

static void drawIcon(NVGcontext* vg, float cx, float cy, float size, unsigned cp, NVGcolor color) {
    int f = brls::Application::getFont(brls::FONT_MATERIAL_ICONS);
    if (f < 0) return;
    nvgFontFaceId(vg, f);
    nvgFontSize(vg, size);
    nvgFillColor(vg, color);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    std::string s = utf8(cp);
    nvgText(vg, cx, cy, s.c_str(), nullptr);
    nvgFontFaceId(vg, brls::Application::getDefaultFont());
}

/** Pulsante tondo con icona; r riceve l'area toccabile. */
void PlayerOverlay::drawButton(NVGcontext* vg, HitRect& r, float cx, float cy, float radius, unsigned cp,
                               bool dark) {
    r = {cx - radius, cy - radius, radius * 2, radius * 2};
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, radius);
    nvgFillColor(vg, dark ? nvgRGBA(0, 0, 0, 130) : nvgRGBA(255, 255, 255, 38));
    nvgFill(vg);
    drawIcon(vg, cx, cy, radius * 1.15f, cp, nvgRGB(255, 255, 255));
}

void PlayerOverlay::draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
                         brls::FrameContext* ctx) {
    if (onTick) onTick();

    int font = brls::Application::getDefaultFont();
    nvgFontFaceId(vg, font);

    auto now = std::chrono::steady_clock::now();
    bool visible = controlsVisible() || !message.empty();

    if (dim > 0.01f) {
        nvgBeginPath(vg);
        nvgRect(vg, x, y, width, height);
        nvgFillColor(vg, nvgRGBA(0, 0, 0, (unsigned char)(dim * 255)));
        nvgFill(vg);
    }

    // indicatore di volume / luminosita' durante lo scorrimento verticale
    if (now < gaugeUntil) {
        float gw = 56, gh = 260;
        float gx = gaugeLeft ? x + 70 : x + width - 70 - gw, gy = y + (height - gh) / 2;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, gx, gy, gw, gh, 16);
        nvgFillColor(vg, nvgRGBA(0, 0, 0, 170));
        nvgFill(vg);
        float inner = gh - 70;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, gx + 22, gy + 16, 12, inner, 6);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, 60));
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, gx + 22, gy + 16 + inner * (1 - gaugeValue), 12, inner * gaugeValue, 6);
        nvgFillColor(vg, nvgRGB(214, 51, 108));
        nvgFill(vg);
        nvgFontSize(vg, 18);
        nvgFillColor(vg, nvgRGB(255, 255, 255));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        std::string pct = std::to_string((int)std::lround(gaugeValue * 100)) + "%";
        nvgText(vg, gx + gw / 2, gy + gh - 36, pct.c_str(), nullptr);
        drawIcon(vg, gx + gw / 2, gy + gh + 24, 30, gaugeLabel == "volume" ? icon::VOLUME : icon::BRIGHTNESS,
                 nvgRGB(255, 255, 255));
    }

    // riscontro del doppio tocco (-10 s / +10 s) sul lato toccato
    if (now < flashUntil && !flashText.empty()) {
        float cx = flashLeft ? x + width * 0.18f : x + width * 0.82f;
        nvgBeginPath(vg);
        nvgCircle(vg, cx, y + height / 2, 70);
        nvgFillColor(vg, nvgRGBA(0, 0, 0, 120));
        nvgFill(vg);
        nvgFontSize(vg, 30);
        nvgFillColor(vg, nvgRGB(255, 255, 255));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(vg, cx, y + height / 2, flashText.c_str(), nullptr);
    }

    // messaggio centrale (caricamento, buffering, errori)
    std::string center = message;
    if (center.empty() && mpv->buffering) center = tr("Buffering...");
    if (!center.empty()) {
        nvgFontSize(vg, 26);
        float bounds[4];
        nvgTextBounds(vg, 0, 0, center.c_str(), nullptr, bounds);
        float tw = bounds[2] - bounds[0];
        float bw = std::min(width - 80, tw + 60);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x + (width - bw) / 2, y + height / 2 - 35, bw, 70, 12);
        nvgFillColor(vg, nvgRGBA(0, 0, 0, 180));
        nvgFill(vg);
        nvgFillColor(vg, nvgRGB(255, 255, 255));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgTextBox(vg, x + (width - bw) / 2 + 10, y + height / 2, bw - 20, center.c_str(), nullptr);
    }

    if (!hint.empty()) {
        nvgFontSize(vg, 20);
        nvgBeginPath(vg);
        hintRect = {x + width - 330, y + height - 250, 300, 50};
        nvgRoundedRect(vg, hintRect.x, hintRect.y, hintRect.w, hintRect.h, 10);
        nvgFillColor(vg, nvgRGBA(0, 0, 0, 170));
        nvgFill(vg);
        nvgFillColor(vg, nvgRGB(255, 255, 255));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(vg, hintRect.x + hintRect.w / 2, hintRect.y + hintRect.h / 2, hint.c_str(), nullptr);
    } else {
        hintRect = {};
    }

    if (!visible) {
        backRect = rewindRect = playRect = forwardRect = barRect = subsRect = audioRect = skipRect = prevRect = nextRect = {};
        return;
    }

    // fascia superiore
    NVGpaint top = nvgLinearGradient(vg, x, y, x, y + 140, nvgRGBA(0, 0, 0, 200), nvgRGBA(0, 0, 0, 0));
    nvgBeginPath(vg);
    nvgRect(vg, x, y, width, 140);
    nvgFillPaint(vg, top);
    nvgFill(vg);

    // pulsante indietro (touch)
    drawButton(vg, backRect, x + 50, y + 52, 28, icon::ARROW_BACK, false);

    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFillColor(vg, nvgRGB(255, 255, 255));
    nvgFontSize(vg, 30);
    nvgText(vg, x + 100, y + 28, title.c_str(), nullptr);
    nvgFontSize(vg, 22);
    nvgFillColor(vg, nvgRGB(200, 200, 205));
    nvgText(vg, x + 100, y + 68, subtitle.c_str(), nullptr);

    // comandi centrali: indietro, pausa/play, avanti (nascosti durante i messaggi)
    if (center.empty()) {
        float cx = x + width / 2, cy = y + height / 2;
        unsigned rw = seekStep == 5 ? icon::REPLAY_5 : seekStep == 10 ? icon::REPLAY_10 : seekStep == 30 ? icon::REPLAY_30 : icon::REPLAY;
        unsigned fw = seekStep == 5 ? icon::FORWARD_5 : seekStep == 10 ? icon::FORWARD_10 : seekStep == 30 ? icon::FORWARD_30 : icon::FORWARD;
        drawButton(vg, playRect, cx, cy, 50, mpv->paused ? icon::PLAY : icon::PAUSE, true);
        drawButton(vg, rewindRect, cx - 180, cy, 38, rw, true);
        drawButton(vg, forwardRect, cx + 180, cy, 38, fw, true);
    } else {
        rewindRect = playRect = forwardRect = {};
    }

    // fascia inferiore
    NVGpaint bottom =
        nvgLinearGradient(vg, x, y + height - 200, x, y + height, nvgRGBA(0, 0, 0, 0), nvgRGBA(0, 0, 0, 220));
    nvgBeginPath(vg);
    nvgRect(vg, x, y + height - 200, width, 200);
    nvgFillPaint(vg, bottom);
    nvgFill(vg);

    float barX = x + 40, barW = width - 80, barY = y + height - 88;
    barRect = {barX, barY - 22, barW, 50};
    double dur = mpv->duration, pos = mpv->position;
    if (scrubFrac >= 0 && dur > 0) pos = scrubFrac * dur;
    float frac = dur > 0 ? (float)std::min(1.0, std::max(0.0, pos / dur)) : 0;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, barX, barY, barW, 6, 3);
    nvgFillColor(vg, nvgRGBA(255, 255, 255, 70));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, barX, barY, barW * frac, 6, 3);
    nvgFillColor(vg, nvgRGB(214, 51, 108));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgCircle(vg, barX + barW * frac, barY + 3, scrubFrac >= 0 ? 14 : 9);
    nvgFillColor(vg, nvgRGB(255, 255, 255));
    nvgFill(vg);

    nvgFontSize(vg, 20);
    nvgFillColor(vg, nvgRGB(255, 255, 255));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    std::string times = formatTime(pos) + " / " + formatTime(dur) + (mpv->paused ? "   " + tr("(in pausa)") : "");
    nvgText(vg, barX, barY + 18, times.c_str(), nullptr);

    nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
    nvgFillColor(vg, nvgRGB(190, 190, 195));
    std::string help = tr("A pausa   Sx/Dx 10s   L/R 85s   X sottotitoli   ZL audio   - precedente   + prossimo   B esci");
    nvgText(vg, barX + barW, barY + 18, help.c_str(), nullptr);

    // pulsanti touch sopra la barra, allineati a destra
    float by = barY - 46, step = 64, bx = barX + barW - 24;
    drawButton(vg, nextRect, bx, by, 24, icon::SKIP_NEXT, false);
    drawButton(vg, skipRect, bx - step, by, 24, icon::FAST_FORWARD, false);
    nvgFontSize(vg, 13);
    nvgFillColor(vg, nvgRGB(255, 255, 255));
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
    nvgText(vg, bx - step, by + 26, "85s", nullptr);
    drawButton(vg, audioRect, bx - step * 2, by, 24, icon::AUDIOTRACK, false);
    drawButton(vg, subsRect, bx - step * 3, by, 24, icon::SUBTITLES, false);
    drawButton(vg, prevRect, bx - step * 4, by, 24, icon::SKIP_PREV, false);
}

// ----------------------------------------------------------------------------- activity

PlayerActivity::PlayerActivity(PlayRequest r) : req(std::move(r)) {
#ifdef __SWITCH__
    // volume di sistema (lo stesso dei tasti della console) tramite il servizio audctl
    if (R_SUCCEEDED(audctlInitialize())) {
        systemVolume = true;
        readSystemVolume();
    }
    // luminosita' dello schermo: servizio lbl (se l'accesso viene negato si usa l'oscuramento software)
    if (R_SUCCEEDED(lblInitialize())) {
        float cur = 1;
        if (R_SUCCEEDED(lblGetCurrentBrightnessSetting(&cur))) {
            systemBrightness = true;
            originalBrightness = cur;
            brightness = cur;
        } else {
            lblExit();
        }
    }
#endif
}

PlayerActivity::~PlayerActivity() {
    *alive = false;
    brls::Application::setActiveEvent(false);
#ifdef __SWITCH__
    appletSetMediaPlaybackState(false);
    if (systemVolume) audctlExit();
    if (systemBrightness) {
        if (originalBrightness >= 0) lblSetCurrentBrightnessSetting(originalBrightness);
        lblExit();
    }
#endif
}

void PlayerActivity::applyIdleDim(bool on) {
#ifdef __SWITCH__
    if (systemBrightness) {
        lblSetCurrentBrightnessSetting(on ? std::max(0.02f, brightness * 0.25f) : std::max(0.02f, brightness));
        return;
    }
#endif
    overlay->dim = on ? 0.75f : (1 - brightness) * 0.85f;
}

void PlayerActivity::updatePowerState() {
    auto now = std::chrono::steady_clock::now();
    // "in riproduzione" = video caricato e non in pausa: niente oscuramento ne' standby automatico
    bool playing = mpv->loaded && !mpv->paused && !mpv->ended;
    if (playing != mediaPlaying) {
        mediaPlaying = playing;
#ifdef __SWITCH__
        appletSetMediaPlaybackState(playing);
#endif
    }
    bool paused = !playing;
    if (paused && !wasPaused) pausedSince = now;
    wasPaused = paused;

    auto since = std::max(pausedSince, overlay->lastInteraction);
    if (!paused || now - since < std::chrono::seconds(30)) {
        if (idleStage != 0) {
            idleStage = 0;
            applyIdleDim(false);  // ripristina appena l'utente torna
        }
        return;
    }
    if (idleStage == 0) {
        idleStage = 1;
        applyIdleDim(true);
    } else if (idleStage == 1 && now - since >= std::chrono::seconds(60)) {
        idleStage = 2;
        saveProgress(true);
#ifdef __SWITCH__
        if (R_FAILED(appletRequestToSleep())) appletSetMediaPlaybackState(false);  // lascia fare al sistema
#endif
    }
}

void PlayerActivity::readSystemVolume() {
#ifdef __SWITCH__
    if (!systemVolume) return;
    AudioTarget target = AudioTarget_Speaker;
    if (R_FAILED(audctlGetActiveOutputTarget(&target)) || target == AudioTarget_Invalid)
        audctlGetDefaultTarget(&target);
    volumeTarget = (int)target;
    s32 mn = 0, mx = 15, cur = 0;
    audctlGetTargetVolumeMin(&mn);
    audctlGetTargetVolumeMax(&mx);
    if (mx <= mn) mx = mn + 15;
    volumeMin = mn;
    volumeMax = mx;
    if (R_SUCCEEDED(audctlGetTargetVolume(&cur, target))) volume = (float)(cur - mn) / (float)(mx - mn);
#endif
}

void PlayerActivity::setVolume(float v) {
    volume = v;
#ifdef __SWITCH__
    if (systemVolume) {
        s32 level = (s32)std::lround(volumeMin + v * (volumeMax - volumeMin));
        if (R_SUCCEEDED(audctlSetTargetVolume((AudioTarget)volumeTarget, level))) {
            if (level > volumeMin) audctlSetTargetMute((AudioTarget)volumeTarget, false);
            return;
        }
    }
#endif
    mpv->command({"set", "volume", std::to_string((int)std::lround(v * 100))});  // riserva: volume del player
}

void PlayerActivity::setBrightness(float v) {
    brightness = v;
#ifdef __SWITCH__
    if (systemBrightness) {
        lblSetCurrentBrightnessSetting(std::max(0.02f, v));
        return;
    }
#endif
    overlay->dim = (1 - v) * 0.85f;  // senza accesso al sistema: oscura il video
}

brls::View* PlayerActivity::createContentView() {
    auto* root = new brls::Box();
    root->setDimensions(brls::View::AUTO, brls::View::AUTO);
    root->setWidthPercentage(100);
    root->setHeightPercentage(100);
    // Niente sfondo: nanovg disegna DOPO mpv e coprirebbe il video.

    mpv = new MpvView();
    mpv->setPositionType(brls::PositionType::ABSOLUTE);
    mpv->setPositionTop(0);
    mpv->setPositionLeft(0);
    mpv->setWidthPercentage(100);
    mpv->setHeightPercentage(100);
    mpv->setFocusable(true);
    mpv->setHideHighlight(true);
    root->addView(mpv);

    overlay = new PlayerOverlay(mpv);
    overlay->setPositionType(brls::PositionType::ABSOLUTE);
    overlay->setPositionTop(0);
    overlay->setPositionLeft(0);
    overlay->setWidthPercentage(100);
    overlay->setHeightPercentage(100);
    root->addView(overlay);
    overlay->onTick = [this] { tick(); };

    auto seek = [this](double sec) {
        return [this, sec](brls::View*) {
            seekBy(sec);
            return true;
        };
    };
    double step = Config::instance().seekSeconds;
    overlay->seekStep = (int)step;
    mpv->registerAction("", brls::BUTTON_LEFT, seek(-step), true, true);
    mpv->registerAction("", brls::BUTTON_RIGHT, seek(step), true, true);
    mpv->registerAction("", brls::BUTTON_LB, seek(-85), true, true);
    mpv->registerAction("", brls::BUTTON_RB, seek(85), true, true);
    mpv->registerAction("", brls::BUTTON_A, [this](brls::View*) {
        togglePause();
        return true;
    }, true);
    mpv->registerAction("", brls::BUTTON_UP, [this](brls::View*) {
        overlay->poke();
        return true;
    }, true);
    mpv->registerAction("", brls::BUTTON_DOWN, [this](brls::View*) {
        overlay->poke();
        return true;
    }, true);
    mpv->registerAction("", brls::BUTTON_X, [this](brls::View*) {
        cycleSubs();
        return true;
    }, true);
    mpv->registerAction("", brls::BUTTON_LT, [this](brls::View*) {
        cycleAudio();
        return true;
    }, true);
    mpv->registerAction("", brls::BUTTON_Y, [this](brls::View*) {
        skipSegment();
        return true;
    }, true);
    mpv->registerAction("", brls::BUTTON_START, [this](brls::View*) {
        playNext();
        return true;
    }, true);
    mpv->registerAction("", brls::BUTTON_BACK, [this](brls::View*) {
        playPrevious();
        return true;
    }, true);
    mpv->registerAction("", brls::BUTTON_B, [this](brls::View*) {
        exitPlayer();
        return true;
    }, true);

    // ---- touch: tocco singolo/doppio e trascinamento della barra
    mpv->addGestureRecognizer(new brls::TapGestureRecognizer([this](brls::TapGestureStatus status, brls::Sound*) {
        if (status.state == brls::GestureState::END) onTap(status.position);
    }));
    mpv->addGestureRecognizer(new brls::PanGestureRecognizer(
        [this](brls::PanGestureStatus status, brls::Sound*) {
            if (status.state == brls::GestureState::START) {
                scrubbing = overlay->controlsVisible() &&
                            overlay->hitTest(status.startPosition) == OverlayHit::BAR && mpv->duration > 0;
            }
            if (!scrubbing) return;
            overlay->poke();
            if (status.state == brls::GestureState::END) {
                float f = overlay->barFraction(status.position.x);
                mpv->seekAbsolute(f * mpv->duration);
                overlay->scrubFrac = -1;
                scrubbing = false;
            } else if (status.state == brls::GestureState::INTERRUPTED ||
                       status.state == brls::GestureState::FAILED) {
                overlay->scrubFrac = -1;
                scrubbing = false;
            } else {
                overlay->scrubFrac = overlay->barFraction(status.position.x);
            }
        },
        brls::PanAxis::HORIZONTAL));

    // scorrimento verticale: meta' sinistra = volume, meta' destra = luminosita'
    mpv->addGestureRecognizer(new brls::PanGestureRecognizer(
        [this](brls::PanGestureStatus status, brls::Sound*) {
            float w = overlay->getWidth(), h = overlay->getHeight();
            if (status.state == brls::GestureState::START) {
                swipeMode = 0;
                // escluse le fasce con i comandi (in alto e in basso)
                if (status.startPosition.y < 110 || status.startPosition.y > h - 180) return;
                swipeMode = status.startPosition.x < w / 2 ? 1 : 2;
                if (swipeMode == 1) readSystemVolume();  // cuffie/TV possono essere cambiate
                swipeStartValue = swipeMode == 1 ? volume : brightness;
            }
            if (swipeMode == 0) return;
            overlay->lastInteraction = std::chrono::steady_clock::now();
            float v = swipeStartValue + (status.startPosition.y - status.position.y) / (h * 0.6f);
            v = std::min(1.0f, std::max(0.0f, v));
            if (swipeMode == 1) {
                setVolume(v);
                overlay->showGauge("volume", v, true);
            } else {
                setBrightness(v);
                overlay->showGauge("brightness", v, false);
            }
            if (status.state == brls::GestureState::END || status.state == brls::GestureState::INTERRUPTED ||
                status.state == brls::GestureState::FAILED)
                swipeMode = 0;
        },
        brls::PanAxis::VERTICAL));

    mpv->onFileLoaded = [this] {
        overlay->message.clear();
        for (auto& s : current.value("subtitles", json::array()))
            mpv->addSubtitle(s.value("url", ""), s.value("lang", ""));
        for (auto& a : current.value("audio", json::array()))
            mpv->addAudio(a.value("url", ""), a.value("lang", ""));
        overlay->poke(4);
    };
    mpv->onEnd = [this] {
        saveProgress(true);
        int n = nextIndex();
        if (n >= 0) {
            brls::Application::notify(tr("Episodio successivo..."));
            goToEpisode(n);
        } else {
            overlay->message = tr("Fine della serie");
        }
    };
    mpv->onError = [this](const std::string& msg) { overlay->message = tr("{}\nPremi B per tornare indietro", msg); };

    return root;
}

void PlayerActivity::onContentAvailable() {
    brls::Application::setActiveEvent(true);  // ridisegno continuo durante la riproduzione
    startEpisode();
}

void PlayerActivity::tick() {
    updatePowerState();
    auto now = std::chrono::steady_clock::now();
    if (now - lastSave > std::chrono::seconds(15)) saveProgress(false);

    // suggerimento per saltare sigla/riassunto quando la sorgente fornisce i timestamp
    overlay->hint.clear();
    for (auto& t : current.value("timestamps", json::array())) {
        double s = t.value("start", 0.0), e = t.value("end", 0.0);
        if (mpv->position >= s && mpv->position < e - 1) {
            std::string type = t.value("type", "Other");
            if (Config::instance().autoSkipOpening && type == "Opening" && !autoSkipped) {
                autoSkipped = true;
                mpv->seekAbsolute(e);
            } else {
                std::string segName = t.value("name", std::string());
                overlay->hint = tr("Y: salta {}", segName.empty() ? tr("sigla") : segName);
            }
        }
    }
}

void PlayerActivity::seekBy(double seconds) {
    mpv->seekRelative(seconds);
    overlay->poke();
}

void PlayerActivity::togglePause() {
    mpv->togglePause();
    overlay->poke();
    saveProgress(true);
}

void PlayerActivity::cycleSubs() {
    mpv->command({"cycle", "sub"});
    overlay->poke();
    brls::delay(300, [this, a = std::weak_ptr<bool>(alive)] {
        auto l = a.lock();
        if (!l || !*l) return;
        std::string sid = mpv->getString("sid");
        std::string lang = mpv->getString("current-tracks/sub/lang");
        brls::Application::notify(sid == "no" || sid.empty() ? tr("Sottotitoli disattivati")
                                                              : tr("Sottotitoli: {}", lang.empty() ? sid : lang));
    });
}

void PlayerActivity::cycleAudio() {
    mpv->command({"cycle", "audio"});
    overlay->poke();
    brls::delay(300, [this, a = std::weak_ptr<bool>(alive)] {
        auto l = a.lock();
        if (!l || !*l) return;
        std::string lang = mpv->getString("current-tracks/audio/lang");
        brls::Application::notify(tr("Audio: {}", lang.empty() ? mpv->getString("aid") : lang));
    });
}

void PlayerActivity::playNext() {
    int n = nextIndex();
    if (n < 0)
        brls::Application::notify(tr("Questo e' l'ultimo episodio"));
    else
        goToEpisode(n);
}

int PlayerActivity::previousIndex() const {
    if (req.episodes.empty()) return -1;
    const auto& cur = req.episodes[req.index];
    if (cur.number >= 0) {
        int best = -1;
        for (int i = 0; i < (int)req.episodes.size(); i++) {
            double n = req.episodes[i].number;
            if (n >= 0 && n < cur.number && (best < 0 || n > req.episodes[best].number)) best = i;
        }
        if (best >= 0) return best;
    }
    int p = req.oldestFirst ? req.index - 1 : req.index + 1;
    return p >= 0 && p < (int)req.episodes.size() ? p : -1;
}

void PlayerActivity::playPrevious() {
    int p = previousIndex();
    if (p < 0)
        brls::Application::notify(tr("Questo e' il primo episodio"));
    else
        goToEpisode(p);
}

void PlayerActivity::onTap(const brls::Point& p) {
    switch (overlay->hitTest(p)) {
        case OverlayHit::BACK: exitPlayer(); return;
        case OverlayHit::PLAY_PAUSE: togglePause(); return;
        case OverlayHit::REWIND: seekBy(-overlay->seekStep); return;
        case OverlayHit::FORWARD: seekBy(overlay->seekStep); return;
        case OverlayHit::SUBS: cycleSubs(); return;
        case OverlayHit::AUDIO: cycleAudio(); return;
        case OverlayHit::SKIP: seekBy(85); return;
        case OverlayHit::NEXT: playNext(); return;
        case OverlayHit::PREV: playPrevious(); return;
        case OverlayHit::HINT: skipSegment(); return;
        case OverlayHit::BAR:
            if (mpv->duration > 0) mpv->seekAbsolute(overlay->barFraction(p.x) * mpv->duration);
            overlay->poke();
            return;
        case OverlayHit::NONE: break;
    }

    // doppio tocco sui lati: indietro/avanti di N secondi
    float w = overlay->getWidth();
    int side = p.x < w / 3 ? -1 : (p.x > w * 2 / 3 ? 1 : 0);
    auto now = std::chrono::steady_clock::now();
    bool doubleTap = side != 0 && side == lastTapSide && now - lastTapTime < std::chrono::milliseconds(400);
    lastTapTime = now;
    lastTapSide = side;
    if (doubleTap) {
        int st = overlay->seekStep;
        mpv->seekRelative(side * st);
        overlay->flash((side < 0 ? "-" : "+") + std::to_string(st) + " s", side < 0);
        return;  // altri tocchi rapidi sullo stesso lato continuano a spostare
    }
    // tocco singolo: mostra/nasconde i comandi
    if (overlay->controlsVisible() && !mpv->paused)
        overlay->hide();
    else
        overlay->poke();
}

void PlayerActivity::skipSegment() {
    for (auto& t : current.value("timestamps", json::array())) {
        double s = t.value("start", 0.0), e = t.value("end", 0.0);
        if (mpv->position >= s && mpv->position < e) {
            mpv->seekAbsolute(e);
            return;
        }
    }
    mpv->seekRelative(85);  // nessun timestamp: salto standard di una sigla
    overlay->poke();
}

int PlayerActivity::nextIndex() const {
    if (req.episodes.empty()) return -1;
    const auto& cur = req.episodes[req.index];
    if (cur.number >= 0) {
        int best = -1;
        for (int i = 0; i < (int)req.episodes.size(); i++) {
            double n = req.episodes[i].number;
            if (n > cur.number && (best < 0 || n < req.episodes[best].number)) best = i;
        }
        if (best >= 0) return best;
    }
    // senza numerazione: le sorgenti elencano di solito dal piu' recente al piu' vecchio
    int n = req.oldestFirst ? req.index + 1 : req.index - 1;
    return n >= 0 && n < (int)req.episodes.size() ? n : -1;
}

void PlayerActivity::goToEpisode(int newIndex) {
    saveProgress(true);
    req.index = newIndex;
    req.forcedToken.clear();
    req.startAt = 0;
    autoSkipped = false;
    mpv->stop();
    startEpisode();
}

void PlayerActivity::startEpisode() {
    const auto& ep = req.episodes[req.index];
    overlay->title = req.animeTitle;
    overlay->subtitle = ep.name;
    overlay->message = tr("Cerco i video disponibili...");
    overlay->poke(6);
    current = json::object();

    struct Candidate {
        std::string token;
        std::string label;
    };
    auto sid = req.sourceId;
    auto epUrl = ep.url;
    auto epName = ep.name;
    auto forced = req.forcedToken;

    runAsync<json>(
        alive,
        [sid, epUrl, epName, forced]() -> json {
            if (!forced.empty()) return json{{"forced", forced}};
            return api::hosters(sid, epUrl, epName);
        },
        [this, sid, epUrl](json hs) {
            // Costruisce la lista di candidati: prima i video "preferiti", poi gli altri,
            // infine gli hoster da caricare su richiesta.
            auto cands = std::make_shared<std::vector<Candidate>>();
            auto lazy = std::make_shared<std::vector<std::pair<int, std::string>>>();
            if (hs.contains("forced")) {
                cands->push_back({hs["forced"].get<std::string>(), ""});
            } else {
                std::vector<Candidate> normal;
                for (auto& h : hs.value("hosters", json::array())) {
                    std::string hname = h.value("name", "");
                    if (h["videos"].is_array()) {
                        for (auto& v : h["videos"]) {
                            Candidate c{v.value("token", ""), hname + " - " + v.value("title", "")};
                            if (v.value("preferred", false))
                                cands->push_back(c);
                            else
                                normal.push_back(c);
                        }
                    } else {
                        lazy->push_back({h.value("index", 0), hname});
                    }
                }
                cands->insert(cands->end(), normal.begin(), normal.end());
            }

            auto tryNext = std::make_shared<std::function<void(size_t)>>();
            std::weak_ptr<std::function<void(size_t)>> weakTry = tryNext;
            *tryNext = [this, cands, lazy, sid, epUrl, weakTry](size_t i) {
                auto self = weakTry.lock();
                if (!self) return;
                if (i >= cands->size()) {
                    if (lazy->empty()) {
                        overlay->message = tr("Nessun video riproducibile trovato.\nPremi B per tornare indietro");
                        return;
                    }
                    auto h = lazy->front();
                    lazy->erase(lazy->begin());
                    overlay->message = tr("Carico {}...", h.second);
                    runAsync<json>(
                        alive, [sid, epUrl, h] { return api::hosterVideos(sid, epUrl, h.first); },
                        [cands, self, i, h](json r) {
                            for (auto& v : r.value("videos", json::array()))
                                cands->push_back({v.value("token", ""), h.second + " - " + v.value("title", "")});
                            (*self)(i);
                        },
                        [self, i](const std::string&) { (*self)(i); });
                    return;
                }
                auto c = (*cands)[i];
                overlay->message = c.label.empty() ? tr("Avvio del video...") : tr("Avvio: {}", c.label);
                runAsync<json>(
                    alive, [c] { return api::play(c.token); },
                    [this, self, i, c](json info) {
                        // se mpv fallisce, prova il candidato successivo
                        mpv->onError = [this, self, i](const std::string& msg) {
                            brls::Logger::warning("Video fallito ({}), provo il successivo", msg);
                            (*self)(i + 1);
                        };
                        info["label"] = c.label;
                        playResolved(info);
                    },
                    [self, i](const std::string& err) {
                        brls::Logger::warning("play fallito: {}", err);
                        (*self)(i + 1);
                    });
            };
            // la lambda deve restare viva finche' l'activity esiste
            retryHolder = tryNext;
            (*tryNext)(0);
        },
        [this](const std::string& err) { overlay->message = tr("{}\nPremi B per tornare indietro", err); });
}

void PlayerActivity::playResolved(const json& info) {
    current = info;
    overlay->message = tr("Caricamento...");
    std::vector<std::pair<std::string, std::string>> opts;
    for (auto& kv : info.value("mpvArgs", json::array()))
        if (kv.is_array() && kv.size() == 2) opts.push_back({kv[0].get<std::string>(), kv[1].get<std::string>()});
    double start = req.startAt;
    req.startAt = 0;
    mpv->load(info.value("url", ""), start, opts);
    lastSave = std::chrono::steady_clock::now();
}

void PlayerActivity::saveProgress(bool force) {
    lastSave = std::chrono::steady_clock::now();
    if (!mpv || !mpv->loaded || mpv->duration <= 0) return;
    if (!force && mpv->paused) return;
    const auto& ep = req.episodes[req.index];
    json p = {
        {"sourceId", req.sourceId},     {"animeUrl", req.animeUrl}, {"animeTitle", req.animeTitle},
        {"episodeUrl", ep.url},         {"episodeName", ep.name},   {"position", mpv->position},
        {"duration", mpv->duration},
    };
    ThreadPool::instance().run([p] {
        try {
            api::saveProgress(p);
        } catch (const std::exception& e) {
            brls::Logger::warning("Salvataggio progressi fallito: {}", e.what());
        }
    });
}

void PlayerActivity::exitPlayer() {
    saveProgress(true);
    mpv->stop();
    brls::Application::popActivity();
}
