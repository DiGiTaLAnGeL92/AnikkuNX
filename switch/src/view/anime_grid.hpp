#pragma once

#include <borealis.hpp>
#include <nlohmann/json.hpp>

#include <functional>

#include "view/cover_image.hpp"

/** Elemento generico mostrato nella griglia (anime, stagione, voce di cronologia...). */
struct GridItem {
    std::string sourceId;
    std::string url;
    std::string title;
    std::string subtitle;
    std::string thumbnail;
    nlohmann::json extra;
};

brls::Label* makeLabel(const std::string& text, float size = 18, bool wrap = false);

class AnimeCard : public brls::Box {
  public:
    AnimeCard(const GridItem& item, float width);
    GridItem item;
};

/**
 * Griglia di copertine con caricamento "infinito": quando il focus arriva
 * sulle ultime righe viene chiamato onNeedMore.
 */
class AnimeGrid : public brls::ScrollingFrame {
  public:
    explicit AnimeGrid(int columns = 5, float cardWidth = 186);

    /** Svuota la griglia; ritorna true se aveva il focus (e l'ha spostato su focusFallback). */
    bool clear();
    void append(const std::vector<GridItem>& items);
    void showMessage(const std::string& text);
    void showLoading();
    void hideStatus();
    size_t count() const { return total; }

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
              brls::FrameContext* ctx) override;

    std::function<void(const GridItem&)> onSelect;
    std::function<void(const GridItem&)> onSecondary;  // tasto X (opzionale)
    std::string secondaryHint;
    std::function<void()> onNeedMore;
    /** Vista a cui spostare il focus prima di svuotare la griglia. */
    brls::View* focusFallback = nullptr;

  private:
    brls::Box* content;
    brls::Box* currentRow = nullptr;
    brls::Label* status;
    int columns;
    float cardWidth;
    size_t total = 0;
    int rows = 0;
};
