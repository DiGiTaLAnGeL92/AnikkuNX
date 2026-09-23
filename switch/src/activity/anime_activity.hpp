#pragma once

#include <borealis.hpp>
#include <nlohmann/json.hpp>

#include "util/async.hpp"
#include "view/cover_image.hpp"

class EpisodeDataSource;

/** Pagina di un anime: copertina, trama, libreria e lista episodi (o stagioni). */
class AnimeActivity : public brls::Activity {
  public:
    AnimeActivity(std::string sourceId, std::string url, std::string title, std::string thumbnail);
    ~AnimeActivity() override;

    brls::View* createContentView() override;
    void onContentAvailable() override;
    void willAppear(bool resetState) override;

    void playEpisode(int index, const std::string& forcedToken = "", double startAt = -1);
    void chooseVideo(int index);
    void openSeason(int index);
    void toggleOrder();

  private:
    void load(bool cached = false);
    void render();
    void updateLibraryButton();
    int continueIndex() const;

    std::string sourceId, url, title, thumbnail;
    nlohmann::json data;
    bool inLibrary = false;
    bool loadedOnce = false;
    bool oldestFirst = false;  // ordine episodi: dal primo invece che dall'ultimo
    AliveToken alive = makeAlive();

    brls::Label* titleLabel = nullptr;
    brls::Label* metaLabel = nullptr;
    brls::Label* descLabel = nullptr;
    brls::Label* statusLabel = nullptr;
    CoverImage* cover = nullptr;
    brls::Button* libraryButton = nullptr;
    brls::Button* resumeButton = nullptr;
    brls::Button* orderButton = nullptr;
    brls::RecyclerFrame* recycler = nullptr;
    EpisodeDataSource* dataSource = nullptr;
};
