#pragma once

#include <borealis.hpp>
#include <chrono>
#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "util/async.hpp"

/**
 * Vista che incapsula libmpv e disegna il video a schermo intero
 * direttamente sul framebuffer (prima che nanovg disegni l'interfaccia sopra).
 */
class MpvView : public brls::View {
  public:
    MpvView();
    ~MpvView() override;

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
              brls::FrameContext* ctx) override;

    void load(const std::string& url, double startSeconds, const std::vector<std::pair<std::string, std::string>>& opts);
    void addSubtitle(const std::string& url, const std::string& lang, bool select = false);
    void addAudio(const std::string& url, const std::string& lang);
    void togglePause();
    void setPause(bool pause);
    void seekRelative(double seconds);
    void seekAbsolute(double seconds);
    void stop();
    void command(std::vector<std::string> args);
    std::string getString(const char* prop);

    double position = 0;
    double duration = 0;
    bool paused = false;
    bool buffering = false;
    bool loaded = false;
    bool ended = false;
    /** ultimo seek (anche dell'utente): serve a distinguere una fine vera da uno stream rotto */
    std::chrono::steady_clock::time_point lastSeek{};

    std::function<void()> onFileLoaded;
    std::function<void()> onEnd;
    std::function<void(const std::string&)> onError;

  private:
    void handleEvents();
    void defer(std::function<void()> cb);

    mpv_handle* mpv = nullptr;
    mpv_render_context* render = nullptr;
    AliveToken alive = makeAlive();
    int defaultFbo = 0;
};
