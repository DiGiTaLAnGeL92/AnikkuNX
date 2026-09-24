#include "view/mpv_view.hpp"
#include "util/i18n.hpp"

#include <clocale>
#include <cstring>
#include <vector>

#ifdef BOREALIS_USE_OPENGL
#include <glad/glad.h>
#ifdef __SDL2__
#include <SDL2/SDL.h>
#else
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#endif
#endif

#include "config.hpp"

#include <sys/stat.h>
#include <cstdio>
#ifdef __SWITCH__
#include <switch.h>
#endif

/**
 * libass per Switch e' compilato senza font provider di sistema (niente fontconfig): senza un font
 * esplicito i sottotitoli vengono caricati ma non disegnati. Estraiamo i font di sistema della console
 * (servizio pl, gia' decifrati in memoria) in <configDir>/mpv e li passiamo a mpv:
 *  - subfont.ttf  = font di ripiego usato da libass quando il font richiesto non esiste
 *  - fonts/       = tutti i font (latino, cinese, coreano, simboli) per sub-fonts-dir
 */
static std::string prepareSubtitleFonts() {
    std::string dir = Config::instance().configDir() + "/mpv";
    mkdir(dir.c_str(), 0777);
    mkdir((dir + "/fonts").c_str(), 0777);
#ifdef __SWITCH__
    // Solo i font che servono: latino/europeo sempre, piu' quello CJK della lingua della console
    // (come fa Switchfin). I font "Ext" di Nintendo non servono ai sottotitoli e sono esclusi.
    struct Item {
        PlSharedFontType type;
        const char* name;
    };
    std::vector<Item> items = {{PlSharedFontType_Standard, "standard.ttf"}};
    std::string loc = brls::Application::getPlatform()->getLocale();
    if (loc == brls::LOCALE_ZH_HANS) items.push_back({PlSharedFontType_ChineseSimplified, "zh-hans.ttf"});
    else if (loc == brls::LOCALE_ZH_HANT) items.push_back({PlSharedFontType_ChineseTraditional, "zh-hant.ttf"});
    else if (loc == brls::LOCALE_Ko) items.push_back({PlSharedFontType_KO, "ko.ttf"});
    auto writeFile = [](const std::string& path, const void* data, size_t size) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && (size_t)st.st_size == size) return;
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) return;
        fwrite(data, 1, size, f);
        fclose(f);
    };
    // font di versioni precedenti (troppi e pesanti): vengono tolti dalla cartella caricata da libass
    for (const char* old : {"zh-hans.ttf", "zh-hans-ext.ttf", "zh-hant.ttf", "ko.ttf", "nintendo-ext.ttf"}) {
        bool wanted = false;
        for (auto& it : items)
            if (strcmp(it.name, old) == 0) wanted = true;
        if (!wanted) remove((dir + "/fonts/" + old).c_str());
    }
    for (auto& it : items) {
        PlFontData font;
        if (R_FAILED(plGetSharedFontByType(&font, it.type)) || !font.address || !font.size) continue;
        writeFile(dir + "/fonts/" + it.name, font.address, font.size);
        if (it.type == PlSharedFontType_Standard) writeFile(dir + "/subfont.ttf", font.address, font.size);
    }
#endif
    return dir;
}

static void* getProcAddress(void*, const char* name) {
#ifdef __SDL2__
    return SDL_GL_GetProcAddress(name);
#else
    return (void*)glfwGetProcAddress(name);
#endif
}

enum ObserveId : uint64_t {
    OBS_TIME = 1,
    OBS_DURATION,
    OBS_PAUSE,
    OBS_CACHE,
    OBS_EOF,
};

MpvView::MpvView() {
    this->setFocusable(false);
    setlocale(LC_NUMERIC, "C");

    mpv = mpv_create();
    if (!mpv) {
        brls::Logger::error("mpv_create fallita");
        return;
    }

    mpv_set_option_string(mpv, "vo", "libmpv");
    mpv_set_option_string(mpv, "ytdl", "no");
    mpv_set_option_string(mpv, "idle", "yes");
    mpv_set_option_string(mpv, "keep-open", "yes");
    mpv_set_option_string(mpv, "hr-seek", "yes");
    mpv_set_option_string(mpv, "osd-level", "0");
    mpv_set_option_string(mpv, "audio-channels", "stereo");
    mpv_set_option_string(mpv, "video-timing-offset", "0");
    mpv_set_option_string(mpv, "sub-auto", "no");
    mpv_set_option_string(mpv, "slang", "it,ita,Italian,Italiano,en,eng");
    mpv_set_option_string(mpv, "alang", "ja,jpn,it,ita");
    mpv_set_option_string(mpv, "sub-font-size", "46");
    if (Config::instance().subtitleFonts) {
        std::string fontDir = prepareSubtitleFonts();
        mpv_set_option_string(mpv, "config", "yes");  // serve perche' mpv cerchi ~~/subfont.ttf
        mpv_set_option_string(mpv, "config-dir", fontDir.c_str());
        mpv_set_option_string(mpv, "sub-fonts-dir", (fontDir + "/fonts").c_str());
        mpv_set_option_string(mpv, "sub-font-provider", "none");
        std::string loc = brls::Application::getPlatform()->getLocale();
        const char* family = "nintendo_udsg-r_std_003";
        if (loc == brls::LOCALE_ZH_HANS) family = "nintendo_udsg-r_org_zh-cn_003";
        else if (loc == brls::LOCALE_ZH_HANT) family = "nintendo_udjxh-db_zh-tw_003";
        else if (loc == brls::LOCALE_Ko) family = "nintendo_udsg-r_ko_003";
        mpv_set_option_string(mpv, "sub-font", family);
        mpv_set_option_string(mpv, "sub-border-size", "3");
        mpv_set_option_string(mpv, "sub-ass-force-margins", "yes");
    }
    mpv_set_option_string(mpv, "demuxer-max-bytes", "64MiB");
    mpv_set_option_string(mpv, "demuxer-max-back-bytes", "16MiB");
    mpv_set_option_string(mpv, "cache", "yes");
    mpv_set_option_string(mpv, "network-timeout", "30");
    mpv_set_option_string(mpv, "tls-verify", "no");
    // molti CDN camuffano i segmenti HLS da immagini (.jpg/.png): ffmpeg >= 6.1 li rifiuta senza questa opzione
    mpv_set_option_string(mpv, "demuxer-lavf-o", "extension_picky=0");
    mpv_set_option_string(mpv, "demuxer-lavf-analyzeduration", "0.4");
    mpv_set_option_string(mpv, "demuxer-lavf-probescore", "24");
    mpv_set_option_string(mpv, "hwdec", Config::instance().hardwareDecoding ? "auto" : "no");
#ifdef __SWITCH__
    mpv_set_option_string(mpv, "vd-lavc-dr", "no");
    mpv_set_option_string(mpv, "vd-lavc-threads", "4");
    mpv_set_option_string(mpv, "opengl-glfinish", "yes");
#endif

    if (mpv_initialize(mpv) < 0) {
        brls::Logger::error("mpv_initialize fallita");
        mpv_terminate_destroy(mpv);
        mpv = nullptr;
        return;
    }

    mpv_observe_property(mpv, OBS_TIME, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, OBS_DURATION, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, OBS_PAUSE, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, OBS_CACHE, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, OBS_EOF, "eof-reached", MPV_FORMAT_FLAG);

#ifdef BOREALIS_USE_OPENGL
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &defaultFbo);
    mpv_opengl_init_params glInit{getProcAddress, nullptr};
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    if (mpv_render_context_create(&render, mpv, params) < 0) {
        brls::Logger::error("mpv_render_context_create fallita");
        render = nullptr;
    }
#endif

    char* ver = mpv_get_property_string(mpv, "mpv-version");
    if (ver) {
        brls::Logger::info("{} (hwdec={})", ver, Config::instance().hardwareDecoding ? "auto" : "no");
        mpv_free(ver);
    }
}

MpvView::~MpvView() {
    *alive = false;
    if (render) mpv_render_context_free(render);
    if (mpv) mpv_terminate_destroy(mpv);
}

void MpvView::draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
                   brls::FrameContext* ctx) {
    // Gli eventi vengono letti a ogni frame: e' il modo piu' semplice e sicuro
    // per non dipendere dalla durata della vista nelle callback di mpv.
    handleEvents();

#ifdef BOREALIS_USE_OPENGL
    if (!render) return;
    int w = (int)brls::Application::windowWidth;
    int h = (int)brls::Application::windowHeight;
    mpv_opengl_fbo fbo{defaultFbo, w, h, 0};
    int flipY = 1;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flipY},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    mpv_render_context_render(render, params);
    glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);
    glViewport(0, 0, w, h);
    mpv_render_context_report_swap(render);
#endif
}

/** Le callback possono chiudere la schermata: le eseguiamo fuori dal draw(). */
void MpvView::defer(std::function<void()> cb) {
    if (!cb) return;
    std::weak_ptr<bool> weak = alive;
    brls::sync([weak, cb] {
        auto a = weak.lock();
        if (a && *a) cb();
    });
}

void MpvView::handleEvents() {
    if (!mpv) return;
    while (true) {
        mpv_event* ev = mpv_wait_event(mpv, 0);
        if (!ev || ev->event_id == MPV_EVENT_NONE) break;
        switch (ev->event_id) {
            case MPV_EVENT_PROPERTY_CHANGE: {
                auto* p = (mpv_event_property*)ev->data;
                if (!p->data) break;
                switch (ev->reply_userdata) {
                    case OBS_TIME: position = *(double*)p->data; break;
                    case OBS_DURATION: duration = *(double*)p->data; break;
                    case OBS_PAUSE: paused = *(int*)p->data; break;
                    case OBS_CACHE: buffering = *(int*)p->data; break;
                    case OBS_EOF:
                        if (*(int*)p->data && loaded && !ended) {
                            ended = true;
                            defer(onEnd);
                        }
                        break;
                    default: break;
                }
                break;
            }
            case MPV_EVENT_FILE_LOADED:
                loaded = true;
                ended = false;
                defer(onFileLoaded);
                break;
            case MPV_EVENT_END_FILE: {
                auto* e = (mpv_event_end_file*)ev->data;
                if (e && e->reason == MPV_END_FILE_REASON_ERROR) {
                    std::string msg = tr("Impossibile riprodurre: {}", mpv_error_string(e->error));
                    brls::Logger::error("{}", msg);
                    auto cb = onError;
                    defer([cb, msg] {
                        if (cb) cb(msg);
                    });
                }
                break;
            }
            case MPV_EVENT_LOG_MESSAGE: {
                auto* m = (mpv_event_log_message*)ev->data;
                brls::Logger::debug("[mpv/{}] {}", m->prefix, m->text);
                break;
            }
            default: break;
        }
        if (!*alive) break;
    }
}

void MpvView::command(std::vector<std::string> args) {
    if (!mpv) return;
    std::vector<const char*> argv;
    for (auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    mpv_command_async(mpv, 0, argv.data());
}

void MpvView::load(const std::string& url, double startSeconds,
                   const std::vector<std::pair<std::string, std::string>>& opts) {
    if (!mpv) {
        if (onError) onError(tr("Player non inizializzato"));
        return;
    }
    for (auto& kv : opts) mpv_set_option_string(mpv, kv.first.c_str(), kv.second.c_str());
    loaded = false;
    ended = false;
    position = 0;
    duration = 0;
    std::string fileOpts;
    if (startSeconds > 5) fileOpts = "start=" + std::to_string((long)startSeconds);
    if (fileOpts.empty())
        command({"loadfile", url, "replace"});
    else
        command({"loadfile", url, "replace", fileOpts});
    setPause(false);
}

void MpvView::addSubtitle(const std::string& url, const std::string& lang, bool select) {
    command({"sub-add", url, select ? "select" : "auto", lang, lang});
}

void MpvView::addAudio(const std::string& url, const std::string& lang) { command({"audio-add", url, "auto", lang, lang}); }

void MpvView::togglePause() { command({"cycle", "pause"}); }

void MpvView::setPause(bool p) { command({"set", "pause", p ? "yes" : "no"}); }

void MpvView::seekRelative(double seconds) {
    lastSeek = std::chrono::steady_clock::now();
    command({"seek", std::to_string((long)seconds), "relative"}); }

void MpvView::seekAbsolute(double seconds) {
    lastSeek = std::chrono::steady_clock::now();
    command({"seek", std::to_string((long)seconds), "absolute"}); }

void MpvView::stop() { command({"stop"}); }

std::string MpvView::getString(const char* prop) {
    if (!mpv) return "";
    char* v = mpv_get_property_string(mpv, prop);
    if (!v) return "";
    std::string s(v);
    mpv_free(v);
    return s;
}
