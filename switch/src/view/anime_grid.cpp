#include "view/anime_grid.hpp"
#include "util/i18n.hpp"

brls::Label* makeLabel(const std::string& text, float size, bool wrap) {
    auto* l = new brls::Label();
    l->setText(text);
    l->setFontSize(size);
    if (!wrap) l->setSingleLine(true);
    return l;
}

AnimeCard::AnimeCard(const GridItem& it, float width) : brls::Box(brls::Axis::COLUMN), item(it) {
    this->setWidth(width);
    this->setMargins(0, 8, 18, 8);
    this->setFocusable(true);
    this->setCornerRadius(8);
    this->setHighlightCornerRadius(10);
    this->setPadding(6);

    float imgW = width - 12;
    auto* img = new CoverImage();
    img->setWidth(imgW);
    img->setHeight(imgW * 1.42f);
    img->setCornerRadius(6);
    this->addView(img);
    img->setUrl(item.thumbnail);

    auto* title = new brls::Label();
    title->setText(item.title);
    title->setFontSize(15);
    title->setWidth(imgW);
    title->setHeight(40);
    title->setMarginTop(6);
    title->setVerticalAlign(brls::VerticalAlign::TOP);
    this->addView(title);

    if (!item.subtitle.empty()) {
        auto* sub = makeLabel(item.subtitle, 13);
        sub->setWidth(imgW);
        sub->setTextColor(nvgRGB(150, 150, 160));
        this->addView(sub);
    }
}

AnimeGrid::AnimeGrid(int cols, float cw) : columns(cols), cardWidth(cw) {
    this->setGrow(1.0f);
    this->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

    content = new brls::Box(brls::Axis::COLUMN);
    content->setPadding(20, 30, 30, 30);
    content->setAlignItems(brls::AlignItems::FLEX_START);

    status = new brls::Label();
    status->setFontSize(20);
    status->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    status->setMargins(40, 0, 40, 0);
    status->setVisibility(brls::Visibility::GONE);
    content->addView(status);

    this->setContentView(content);
}

bool AnimeGrid::clear() {
    bool hadFocus = content->isChildFocused() || this->isFocused();
    if (hadFocus && focusFallback) brls::Application::giveFocus(focusFallback);
    // rimuove tutte le righe ma non l'etichetta di stato (sempre in posizione 0)
    auto children = content->getChildren();
    for (auto* v : children)
        if (v != status) content->removeView(v);
    // borealis ricorda l'ultima riga selezionata anche dopo averla eliminata: senza questo reset
    // il focus successivo punta a memoria liberata e l'app va in crash (es. "Ultime uscite").
    content->setLastFocusedView(nullptr);
    currentRow = nullptr;
    total = 0;
    rows = 0;
    return hadFocus;
}

void AnimeGrid::append(const std::vector<GridItem>& items) {
    for (const auto& it : items) {
        if (!currentRow || currentRow->getChildren().size() >= (size_t)columns) {
            currentRow = new brls::Box(brls::Axis::ROW);
            currentRow->setAlignItems(brls::AlignItems::FLEX_START);
            content->addView(currentRow);
            rows++;
        }
        auto* card = new AnimeCard(it, cardWidth);
        int myRow = rows;
        card->registerClickAction([this, card](brls::View*) {
            if (onSelect) onSelect(card->item);
            return true;
        });
        card->addGestureRecognizer(new brls::TapGestureRecognizer(card));  // selezione col tocco
        if (onSecondary) {
            card->registerAction(secondaryHint.empty() ? tr("Opzioni") : secondaryHint, brls::BUTTON_X,
                                 [this, card](brls::View*) {
                                     if (onSecondary) onSecondary(card->item);
                                     return true;
                                 });
        }
        card->getFocusEvent()->subscribe([this, myRow](brls::View*) {
            if (onNeedMore && myRow >= rows - 1) onNeedMore();
        });
        currentRow->addView(card);
        total++;
    }
    if (total > 0 && status->getVisibility() == brls::Visibility::VISIBLE) {
        // lo stato "caricamento" rimane solo se richiesto esplicitamente
    }
}

void AnimeGrid::draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
                     brls::FrameContext* ctx) {
    brls::ScrollingFrame::draw(vg, x, y, width, height, style, ctx);
    // scorrendo col dito il focus non si sposta: carica la pagina successiva vicino al fondo
    if (onNeedMore && total > 0 && getContentOffsetY() + height > getContentHeight() - 500) onNeedMore();
}

void AnimeGrid::showMessage(const std::string& text) {
    status->setText(text);
    status->setVisibility(brls::Visibility::VISIBLE);
}

void AnimeGrid::showLoading() { showMessage(tr("Caricamento...")); }

void AnimeGrid::hideStatus() { status->setVisibility(brls::Visibility::GONE); }
