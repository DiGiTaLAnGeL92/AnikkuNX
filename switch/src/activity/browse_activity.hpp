#pragma once

#include <borealis.hpp>

#include "util/async.hpp"
#include "view/anime_grid.hpp"

/** Catalogo di una sorgente: popolari, recenti e ricerca. */
class BrowseActivity : public brls::Activity {
  public:
    BrowseActivity(std::string sourceId, std::string sourceName, bool supportsLatest, std::string initialQuery = "");
    ~BrowseActivity() override;

    brls::View* createContentView() override;
    void onContentAvailable() override;

  private:
    void setMode(const std::string& mode);
    void loadPage();
    void askQuery();

    std::string sourceId, sourceName;
    bool supportsLatest;
    std::string mode = "popular";
    std::string query;
    int page = 1;
    bool hasNext = true;
    bool loading = false;
    int generation = 0;
    AliveToken alive = makeAlive();

    AnimeGrid* grid = nullptr;
    brls::Button* btnPopular = nullptr;
    brls::Button* btnLatest = nullptr;
    brls::Button* btnSearch = nullptr;
};
