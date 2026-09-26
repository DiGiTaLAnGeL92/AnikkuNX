#include "activity/downloads_activity.hpp"

#include "activity/anime_activity.hpp"
#include "activity/player_activity.hpp"
#include "app/api.hpp"
#include "app/downloads.hpp"
#include "util/i18n.hpp"

using json = nlohmann::json;

static std::string fmtSize(long long bytes) {
    char buf[32];
    if (bytes >= 1024LL * 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0 * 1024 * 1024));
    else
        snprintf(buf, sizeof(buf), "%.0f MB", bytes / (1024.0 * 1024));
    return buf;
}

static std::string statusText(const json& it) {
    std::string st = it.value("status", "");
    if (st == "downloading") {
        int pct = (int)(it.value("progress", 0.0) * 100);
        std::string t = tr("Download {}%", std::to_string(pct));
        long long b = it.value("bytes", 0LL);
        if (b > 0) t += " \xC2\xB7 " + fmtSize(b);
        return t;
    }
    if (st == "queued") return downloads::paused() ? tr("In pausa") : tr("In coda");
    if (st == "failed") return tr("Non riuscito: {}", it.value("error", ""));
    if (st == "done") return tr("Scaricato") + " \xC2\xB7 " + fmtSize(it.value("bytes", 0LL));
    return st;
}

static brls::Label* sectionLabel(const std::string& text) {
    auto* l = new brls::Label();
    l->setText(text);
    l->setFontSize(16);
    l->setTextColor(nvgRGB(150, 150, 160));
    l->setMarginTop(18);
    l->setMarginBottom(6);
    return l;
}

// ============================================================================ scheda "Scaricati"

DownloadsTab::DownloadsTab() {
    auto* top = new brls::Box(brls::Axis::ROW);
    top->setPadding(16, 40, 0, 40);
    queueButton = new brls::Button();
    queueButton->setStyle(&brls::BUTTONSTYLE_BORDERED);
    queueButton->setText(tr("Coda download"));
    queueButton->registerClickAction([](brls::View*) {
        brls::Application::pushActivity(new DownloadQueueActivity());
        return true;
    });
    top->addView(queueButton);
    this->addView(top);

    grid = new AnimeGrid(4, 196);
    grid->setGrow(1);
    grid->onSelect = [](const GridItem& it) {
        brls::Application::pushActivity(new DownloadedAnimeActivity(it.sourceId, it.url, it.title, it.thumbnail));
    };
    grid->secondaryHint = tr("Elimina");
    grid->onSecondary = [this](const GridItem& it) {
        auto* d = new brls::Dialog(tr("Eliminare tutti gli episodi scaricati di \"{}\"?", it.title));
        d->addButton(tr("Annulla"), [] {});
        d->addButton(tr("Elimina"), [this, it] {
            for (auto& e : downloads::episodes(it.sourceId, it.url)) downloads::remove(e.value("id", ""));
            reload();
        });
        d->open();
    };
    this->addView(grid);
    reload();

    // il pulsante della coda mostra i download in corso: aggiornato ogni secondo
    std::weak_ptr<bool> weak = alive;
    auto tick = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weakTick = tick;
    *tick = [this, weak, weakTick] {
        auto a = weak.lock();
        auto t = weakTick.lock();
        if (!a || !*a || !t) return;
        refreshQueueButton();
        // un download finito (o un episodio eliminato) mentre si era in un'altra schermata: aggiorna le copertine
        std::string sig = downloadsSignature();
        if (sig != shownSignature) reload();
        brls::delay(1000, [t] { (*t)(); });
    };
    ticker = tick;
    (*tick)();
}

void DownloadsTab::refreshQueueButton() {
    int n = downloads::pendingCount();
    std::string text = n > 0 ? tr("Coda download ({})", std::to_string(n)) : tr("Coda download");
    if (downloads::paused() && n > 0) text += " \xC2\xB7 " + tr("in pausa");
    queueButton->setText(text);
}

void DownloadsTab::willAppear(bool resetState) {
    TabBase::willAppear(resetState);
    if (appeared) reload();
    appeared = true;
}

std::string DownloadsTab::downloadsSignature() {
    std::string sig;
    for (auto& a : downloads::animes()) sig += a.value("animeUrl", "") + "#" + std::to_string(a.value("count", 0)) + ";";
    return sig;
}

void DownloadsTab::reload() {
    refreshQueueButton();
    shownSignature = downloadsSignature();
    grid->focusFallback = queueButton;
    bool refocus = grid->clear();
    std::vector<GridItem> items;
    for (auto& a : downloads::animes()) {
        std::string cover = a.value("cover", "");
        if (cover.empty()) cover = a.value("thumbnail", "");
        int n = a.value("count", 0);
        items.push_back({a.value("sourceId", ""), a.value("animeUrl", ""), a.value("title", ""),
                         n == 1 ? tr("1 episodio") : tr("{} episodi", std::to_string(n)), cover, a});
    }
    grid->hideStatus();
    grid->append(items);
    if (items.empty())
        grid->showMessage(tr("Nessun episodio scaricato.\nApri un anime e premi il pulsante di download accanto a "
                             "\"Qualita'\" (o R)."));
    if (refocus && !items.empty()) brls::Application::giveFocus(grid);
}

// ============================================================================ coda dei download

DownloadQueueActivity::~DownloadQueueActivity() { *alive = false; }

brls::View* DownloadQueueActivity::createContentView() {
    auto* scroll = new brls::ScrollingFrame();
    auto* box = new brls::Box(brls::Axis::COLUMN);
    box->setPadding(20, 60, 30, 60);

    pauseButton = new brls::Button();
    pauseButton->setStyle(&brls::BUTTONSTYLE_BORDERED);
    pauseButton->registerClickAction([this](brls::View*) {
        downloads::setPaused(!downloads::paused());
        refresh();
        return true;
    });
    box->addView(pauseButton);

    auto* note = new brls::Label();
    note->setText(tr("Si scarica un episodio alla volta. Per ogni episodio vengono provati tutti i video della fonte: "
                     "vale solo quello che parte davvero. Tieni l'app aperta mentre scarica."));
    note->setFontSize(15);
    note->setTextColor(nvgRGB(150, 150, 160));
    note->setMarginTop(12);
    box->addView(note);

    list = new brls::Box(brls::Axis::COLUMN);
    box->addView(list);
    emptyLabel = sectionLabel(tr("La coda e' vuota."));
    box->addView(emptyLabel);

    scroll->setContentView(box);
    rebuild();

    // aggiornamento ogni secondo finche' la schermata e' aperta
    std::weak_ptr<bool> weak = alive;
    auto tick = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weakTick = tick;
    *tick = [this, weak, weakTick] {
        auto a = weak.lock();
        auto t = weakTick.lock();
        if (!a || !*a || !t) return;
        refresh();
        brls::delay(1000, [t] { (*t)(); });
    };
    ticker = tick;
    brls::delay(1000, [tick] { (*tick)(); });

    scroll->getAppletFrameItem()->title = tr("Coda download");
    return new brls::AppletFrame(scroll);
}

void DownloadQueueActivity::rebuild() {
    bool hadFocus = list->isChildFocused();
    if (hadFocus) brls::Application::giveFocus(pauseButton);
    list->clearViews();
    cells.clear();
    for (auto& it : downloads::items()) {
        if (it.value("status", "") == "done") continue;
        std::string id = it.value("id", "");
        auto* cell = new brls::DetailCell();
        cell->setText(it.value("animeTitle", "") + " \xC2\xB7 " + it.value("episodeName", ""));
        cell->setDetailText(statusText(it));
        cell->registerClickAction([this, id](brls::View*) {
            itemMenu(id);
            return true;
        });
        list->addView(cell);
        cells.push_back({id, cell});
    }
    refresh();
}

void DownloadQueueActivity::refresh() {
    pauseButton->setText(downloads::paused() ? tr("Riprendi i download") : tr("Metti in pausa i download"));
    auto all = downloads::items();
    std::vector<json> pending;
    for (auto& it : all)
        if (it.value("status", "") != "done") pending.push_back(it);
    // cambiato l'elenco (aggiunte, rimozioni, completati): si ricostruisce
    bool same = pending.size() == cells.size();
    for (size_t i = 0; same && i < pending.size(); i++) same = pending[i].value("id", "") == cells[i].first;
    if (!same) {
        rebuild();
        return;
    }
    for (size_t i = 0; i < pending.size(); i++) {
        cells[i].second->setDetailText(statusText(pending[i]));
        std::string st = pending[i].value("status", "");
        cells[i].second->setDetailTextColor(st == "failed"        ? nvgRGB(230, 110, 110)
                                            : st == "downloading" ? nvgRGB(255, 120, 170)
                                                                  : nvgRGB(170, 170, 180));
    }
    emptyLabel->setVisibility(pending.empty() ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
}

void DownloadQueueActivity::itemMenu(const std::string& id) {
    json item;
    for (auto& it : downloads::items())
        if (it.value("id", "") == id) item = it;
    if (item.is_null()) return;
    std::string title = item.value("animeTitle", "") + " \xC2\xB7 " + item.value("episodeName", "");
    std::string text = title + "\n" + statusText(item);
    auto* d = new brls::Dialog(text);
    if (item.value("status", "") == "failed")
        d->addButton(tr("Riprova"), [this, id] {
            downloads::retry(id);
            refresh();
        });
    else if (item.value("status", "") == "queued")
        d->addButton(tr("Scarica per primo"), [this, id] {
            downloads::moveToTop(id);
            refresh();
        });
    d->addButton(tr("Rimuovi"), [this, id] {
        downloads::remove(id);
        refresh();
    });
    d->addButton(tr("Annulla"), [] {});
    d->open();
}

// ============================================================================ episodi scaricati di un anime

DownloadedAnimeActivity::DownloadedAnimeActivity(std::string sid, std::string url, std::string t, std::string c)
    : sourceId(std::move(sid)), animeUrl(std::move(url)), title(std::move(t)), cover(std::move(c)) {}

brls::View* DownloadedAnimeActivity::createContentView() {
    auto* root = new brls::Box(brls::Axis::ROW);
    root->setGrow(1);

    auto* left = new brls::Box(brls::Axis::COLUMN);
    left->setWidth(300);
    left->setPadding(20, 20, 30, 40);
    auto* img = new CoverImage();
    img->setWidth(210);
    img->setHeight(298);
    img->setCornerRadius(8);
    img->setUrl(cover);
    left->addView(img);
    auto* online = new brls::Button();
    online->setStyle(&brls::BUTTONSTYLE_BORDERED);
    online->setText(tr("Pagina dell'anime"));
    online->setWidth(210);
    online->setMarginTop(14);
    std::string sid = sourceId, u = animeUrl, t = title, c = cover;
    online->registerClickAction([sid, u, t, c](brls::View*) {
        brls::Application::pushActivity(new AnimeActivity(sid, u, t, c));
        return true;
    });
    left->addView(online);
    root->addView(left);

    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1);
    list = new brls::Box(brls::Axis::COLUMN);
    list->setPadding(20, 40, 30, 20);
    scroll->setContentView(list);
    root->addView(scroll);
    rebuild();

    root->getAppletFrameItem()->title = title;
    return new brls::AppletFrame(root);
}

void DownloadedAnimeActivity::willAppear(bool resetState) {
    brls::Activity::willAppear(resetState);
    if (appeared) rebuild();  // tornando dal player
    appeared = true;
}

void DownloadedAnimeActivity::rebuild() {
    eps = downloads::episodes(sourceId, animeUrl);
    list->clearViews();
    list->addView(sectionLabel(tr("Episodi scaricati ({})", std::to_string(eps.size()))));
    for (size_t i = 0; i < eps.size(); i++) {
        const auto& e = eps[i];
        auto* cell = new brls::DetailCell();
        cell->setText(e.value("episodeName", tr("Episodio")));
        cell->setDetailText(fmtSize(e.value("bytes", 0LL)));
        int index = (int)i;
        cell->registerClickAction([this, index](brls::View*) {
            play(index);
            return true;
        });
        std::string id = e.value("id", ""), name = e.value("episodeName", "");
        cell->registerAction(tr("Elimina"), brls::BUTTON_X, [this, id, name](brls::View*) {
            auto* d = new brls::Dialog(tr("Eliminare \"{}\" dalla scheda SD?", name));
            d->addButton(tr("Annulla"), [] {});
            d->addButton(tr("Elimina"), [this, id] {
                downloads::remove(id);
                brls::sync([this] {
                    rebuild();
                    if (eps.empty()) brls::Application::popActivity();
                });
            });
            d->open();
            return true;
        });
        list->addView(cell);
    }
}

void DownloadedAnimeActivity::play(int index) {
    PlayRequest req;
    req.sourceId = sourceId;
    req.animeUrl = animeUrl;
    req.animeTitle = title;
    req.thumbnail = cover;
    for (auto& e : eps) req.episodes.push_back({e.value("episodeUrl", ""), e.value("episodeName", ""), e.value("number", -1.0)});
    req.index = index;
    req.oldestFirst = true;  // elenco ordinato dal primo episodio
    // riprende da dove si era arrivati (stessi progressi della visione online)
    json p = api::progress(sourceId, eps[index].value("episodeUrl", ""));
    if (p.is_object() && !p.value("watched", false)) req.startAt = p.value("position", 0.0);
    brls::Application::pushActivity(new PlayerActivity(req), brls::TransitionAnimation::NONE);
}
