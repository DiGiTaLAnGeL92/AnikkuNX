#include "activity/browse_activity.hpp"
#include "util/i18n.hpp"

#include "activity/anime_activity.hpp"
#include "app/api.hpp"

using json = nlohmann::json;

BrowseActivity::BrowseActivity(std::string sid, std::string name, bool latest, std::string initialQuery)
    : sourceId(std::move(sid)), sourceName(std::move(name)), supportsLatest(latest), query(std::move(initialQuery)) {
    if (!query.empty()) mode = "search";
}

BrowseActivity::~BrowseActivity() {
    *alive = false;
}

brls::View* BrowseActivity::createContentView() {
    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setGrow(1);

    auto* bar = new brls::Box(brls::Axis::ROW);
    bar->setPadding(14, 40, 0, 40);
    auto mk = [bar](const std::string& text) {
        auto* b = new brls::Button();
        b->setText(text);
        b->setStyle(&brls::BUTTONSTYLE_BORDERLESS);
        b->setMarginRight(12);
        b->setWidth(220);
        bar->addView(b);
        return b;
    };
    btnPopular = mk(tr("Popolari"));
    btnLatest = mk(tr("Ultime uscite"));
    btnSearch = mk(tr("Cerca"));
    if (!supportsLatest) btnLatest->setVisibility(brls::Visibility::GONE);
    btnPopular->registerClickAction([this](brls::View*) {
        setMode("popular");
        return true;
    });
    btnLatest->registerClickAction([this](brls::View*) {
        setMode("latest");
        return true;
    });
    btnSearch->registerClickAction([this](brls::View*) {
        askQuery();
        return true;
    });
    root->addView(bar);

    grid = new AnimeGrid(5, 206);
    grid->focusFallback = btnPopular;
    grid->onSelect = [](const GridItem& it) {
        brls::Application::pushActivity(new AnimeActivity(it.sourceId, it.url, it.title, it.thumbnail));
    };
    grid->onNeedMore = [this] {
        if (hasNext && !loading) loadPage();
    };
    root->addView(grid);

    root->getAppletFrameItem()->title = sourceName;
    auto* frame = new brls::AppletFrame(root);
    frame->registerAction(tr("Cerca"), brls::BUTTON_Y, [this](brls::View*) {
        askQuery();
        return true;
    });
    return frame;
}

void BrowseActivity::onContentAvailable() {
    std::string m = mode;
    mode.clear();
    setMode(m);
}

void BrowseActivity::askQuery() {
    brls::Application::getImeManager()->openForText(
        [this](std::string text) {
            if (text.empty()) return;
            query = text;
            mode.clear();
            setMode("search");
        },
        tr("Cerca su {}", sourceName), tr("Titolo dell'anime"), 64, query);
}

void BrowseActivity::setMode(const std::string& m) {
    if (m == mode && m != "search") return;
    mode = m;
    page = 1;
    hasNext = true;
    generation++;
    grid->clear();
    grid->showMessage(mode == "search" ? tr("Cerco \"{}\"...", query) : tr("Caricamento..."));
    loadPage();
}

void BrowseActivity::loadPage() {
    loading = true;
    int gen = generation;
    auto sid = sourceId, m = mode, q = query;
    int p = page;
    runAsync<json>(
        alive, [sid, m, p, q] { return api::browse(sid, m, p, m == "search" ? q : ""); },
        [this, gen](json r) {
            if (gen != generation) return;
            loading = false;
            std::vector<GridItem> items;
            for (auto& a : r.value("animes", json::array()))
                items.push_back({a.value("sourceId", sourceId), a.value("url", ""), a.value("title", ""), "",
                                 a.value("thumbnail", ""), a});
            hasNext = r.value("hasNextPage", false) && !items.empty();
            page++;
            grid->hideStatus();
            grid->append(items);
            if (grid->count() == 0) grid->showMessage(tr("Nessun risultato"));
            if (page == 2 && grid->count() > 0) brls::Application::giveFocus(grid);
        },
        [this, gen](const std::string& err) {
            if (gen != generation) return;
            loading = false;
            hasNext = false;  // niente nuovi tentativi automatici mentre si scorre
            if (grid->count() == 0)
                grid->showMessage(tr("Errore: {}", err));
            else
                brls::Application::notify(err);
        });
}
