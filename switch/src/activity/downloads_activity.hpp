#pragma once

#include <borealis.hpp>
#include <nlohmann/json.hpp>

#include "activity/main_activity.hpp"
#include "util/async.hpp"
#include "view/anime_grid.hpp"

/** Scheda "Scaricati": pulsante della coda e copertine degli anime con episodi scaricati. */
class DownloadsTab : public TabBase {
  public:
    DownloadsTab();
    void willAppear(bool resetState) override;

  private:
    void reload();
    void refreshQueueButton();
    static std::string downloadsSignature();
    std::string shownSignature;
    brls::Button* queueButton;
    AnimeGrid* grid;
    bool appeared = false;
    std::shared_ptr<std::function<void()>> ticker;
};

/** Coda dei download: stato di ogni episodio, pausa/ripresa, riprova, rimuovi. */
class DownloadQueueActivity : public brls::Activity {
  public:
    brls::View* createContentView() override;
    ~DownloadQueueActivity() override;

  private:
    void rebuild();
    void refresh();
    void itemMenu(const std::string& id);
    brls::Box* list = nullptr;
    brls::Button* pauseButton = nullptr;
    brls::Label* emptyLabel = nullptr;
    std::vector<std::pair<std::string, brls::DetailCell*>> cells;  // id -> riga
    AliveToken alive = makeAlive();
    std::shared_ptr<std::function<void()>> ticker;
};

/** Episodi scaricati di un anime: A guarda (offline), X elimina. */
class DownloadedAnimeActivity : public brls::Activity {
  public:
    DownloadedAnimeActivity(std::string sourceId, std::string animeUrl, std::string title, std::string cover);
    brls::View* createContentView() override;
    void willAppear(bool resetState) override;

  private:
    void rebuild();
    void play(int index);
    std::string sourceId, animeUrl, title, cover;
    nlohmann::json eps = nlohmann::json::array();
    brls::Box* list = nullptr;
    bool appeared = false;
};
