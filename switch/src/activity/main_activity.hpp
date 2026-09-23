#pragma once

#include <borealis.hpp>
#include <nlohmann/json.hpp>

#include "util/async.hpp"
#include "view/anime_grid.hpp"

/** Controlla i nuovi episodi della libreria appena la console e' connessa a Internet. */
void checkNewEpisodesWhenOnline(int attempt = 0);

class MainActivity : public brls::Activity {
  public:
    brls::View* createContentView() override;
    void onContentAvailable() override;
};

/** Base per le schede: gestisce il token di vita per le richieste asincrone. */
class TabBase : public brls::Box {
  public:
    TabBase();
    ~TabBase() override;

  protected:
    AliveToken alive = makeAlive();
};

class HistoryTab : public TabBase {
  public:
    HistoryTab();
    void willAppear(bool resetState) override;

  private:
    void reload();
    AnimeGrid* grid;
    bool appeared = false;
};

class LibraryTab : public TabBase {
  public:
    LibraryTab();
    ~LibraryTab() override;
    void willAppear(bool resetState) override;
    void reload();
    /** Scheda Libreria attualmente creata (per aggiornarla dopo il controllo dei nuovi episodi). */
    static LibraryTab* current;

  private:
    AnimeGrid* grid;
    bool appeared = false;
};

class SourcesTab : public TabBase {
  public:
    SourcesTab();
    void willAppear(bool resetState) override;

  private:
    void reload();
    brls::Box* list;
    bool appeared = false;
};

class SearchTab : public TabBase {
  public:
    SearchTab();

  private:
    void ask();
    void search(const std::string& q);
    AnimeGrid* grid;
    brls::Button* button;
    std::string query;
    int generation = 0;
    int pending = 0;
};

class SettingsTab : public TabBase {
  public:
    SettingsTab();

};
