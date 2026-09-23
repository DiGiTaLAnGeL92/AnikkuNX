#include "activity/anime_activity.hpp"
#include "util/i18n.hpp"

#include <algorithm>
#include <cmath>

#include "activity/player_activity.hpp"
#include "app/api.hpp"
#include "view/anime_grid.hpp"

using json = nlohmann::json;

static std::string fmtTime(double s) {
    long t = (long)s;
    char buf[24];
    snprintf(buf, sizeof(buf), "%02ld:%02ld", t / 60, t % 60);
    return buf;
}

// ----------------------------------------------------------------------------- cella episodio

/** Riga episodio con pulsante "Qualita'" toccabile a destra (il tasto X fa lo stesso col controller). */
class EpisodeCell : public brls::DetailCell {
  public:
    std::function<void(int row, bool chooseVideo)> onSelect;

    EpisodeCell() {
        pill = new brls::Box();
        pill->setAlignItems(brls::AlignItems::CENTER);
        pill->setJustifyContent(brls::JustifyContent::CENTER);
        pill->setHeight(40);
        pill->setPaddingLeft(16);
        pill->setPaddingRight(16);
        pill->setMarginLeft(16);
        pill->setCornerRadius(20);
        pill->setBackgroundColor(nvgRGBA(255, 255, 255, 30));
        pill->setShrink(0);
        auto* l = new brls::Label();
        l->setText(tr("Qualit\xC3\xA0"));
        l->setFontSize(16);
        pill->addView(l);
        this->addView(pill);

        // sostituisce il tocco standard: a destra apre la scelta del video, altrove riproduce
        for (auto* g : this->getGestureRecognizers()) g->setEnabled(false);
        this->addGestureRecognizer(new brls::TapGestureRecognizer([this](brls::TapGestureStatus st, brls::Sound* snd) {
            if (st.state != brls::GestureState::END) return;
            *snd = brls::SOUND_CLICK;
            bool choose = pill->getVisibility() == brls::Visibility::VISIBLE &&
                          st.position.x >= pill->getFrame().getMinX() - 20;
            if (onSelect) onSelect(this->getIndexPath().row, choose);
        }));
    }

    void setPillVisible(bool v) { pill->setVisibility(v ? brls::Visibility::VISIBLE : brls::Visibility::GONE); }

  private:
    brls::Box* pill;
};

// ----------------------------------------------------------------------------- data source

class EpisodeDataSource : public brls::RecyclerDataSource {
  public:
    explicit EpisodeDataSource(AnimeActivity* a) : activity(a) {}

    json items = json::array();
    bool seasons = false;

    int numberOfSections(brls::RecyclerFrame*) override { return 1; }
    int numberOfRows(brls::RecyclerFrame*, int) override { return (int)items.size(); }
    std::string titleForHeader(brls::RecyclerFrame*, int) override {
        return seasons ? tr("Stagioni ({})", std::to_string(items.size()))
                       : tr("Episodi ({})", std::to_string(items.size()));
    }

    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override {
        auto* cell = (EpisodeCell*)recycler->dequeueReusableCell("Cell");
        const auto& it = items[index.row];
        cell->setPillVisible(!seasons);
        if (seasons) {
            cell->setText(it.value("title", ""));
            cell->setDetailText("");
            return cell;
        }
        cell->setText(it.value("name", tr("Episodio")));
        std::string detail;
        if (it.value("watched", false)) {
            detail = tr("Visto");
        } else if (it.value("position", 0.0) > 5) {
            detail = fmtTime(it.value("position", 0.0)) + " / " + fmtTime(it.value("duration", 0.0));
        } else if (it.value("filler", false)) {
            detail = tr("Filler");
        }
        cell->setDetailText(detail);
        cell->setDetailTextColor(it.value("watched", false) ? nvgRGB(120, 200, 120) : nvgRGB(170, 170, 180));
        return cell;
    }

    void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath index) override {
        if (seasons)
            activity->openSeason(index.row);
        else
            activity->playEpisode(index.row);
    }

  private:
    AnimeActivity* activity;
};

// ----------------------------------------------------------------------------- activity

AnimeActivity::AnimeActivity(std::string sid, std::string u, std::string t, std::string thumb)
    : sourceId(std::move(sid)), url(std::move(u)), title(std::move(t)), thumbnail(std::move(thumb)) {}

AnimeActivity::~AnimeActivity() { *alive = false; }

brls::View* AnimeActivity::createContentView() {
    auto* root = new brls::Box(brls::Axis::ROW);
    root->setGrow(1);

    // ---- pannello sinistro
    auto* leftScroll = new brls::ScrollingFrame();
    leftScroll->setWidth(400);
    auto* left = new brls::Box(brls::Axis::COLUMN);
    left->setPadding(20, 20, 30, 40);
    left->setAlignItems(brls::AlignItems::FLEX_START);

    cover = new CoverImage();
    cover->setWidth(210);
    cover->setHeight(298);
    cover->setCornerRadius(8);
    left->addView(cover);
    cover->setUrl(thumbnail);

    titleLabel = new brls::Label();
    titleLabel->setText(title);
    titleLabel->setFontSize(24);
    titleLabel->setWidth(340);
    titleLabel->setMarginTop(14);
    left->addView(titleLabel);

    metaLabel = new brls::Label();
    metaLabel->setFontSize(15);
    metaLabel->setWidth(340);
    metaLabel->setTextColor(nvgRGB(160, 160, 170));
    metaLabel->setMarginTop(6);
    left->addView(metaLabel);

    resumeButton = new brls::Button();
    resumeButton->setText(tr("Guarda"));
    resumeButton->setStyle(&brls::BUTTONSTYLE_PRIMARY);
    resumeButton->setWidth(340);
    resumeButton->setMarginTop(14);
    resumeButton->setVisibility(brls::Visibility::GONE);
    resumeButton->registerClickAction([this](brls::View*) {
        int i = continueIndex();
        if (i >= 0) playEpisode(i);
        return true;
    });
    left->addView(resumeButton);

    libraryButton = new brls::Button();
    libraryButton->setText(tr("+ Aggiungi alla libreria"));
    libraryButton->setStyle(&brls::BUTTONSTYLE_BORDERED);
    libraryButton->setWidth(340);
    libraryButton->setMarginTop(10);
    libraryButton->registerClickAction([this](brls::View*) {
        bool add = !inLibrary;
        auto sid = sourceId, u = url, t = title;
        runAsync<bool>(
            alive,
            [add, sid, u, t] {
                if (add)
                    api::addLibrary(sid, u, t);
                else
                    api::removeLibrary(sid, u);
                return add;
            },
            [this](bool added) {
                inLibrary = added;
                updateLibraryButton();
                brls::Application::notify(added ? tr("Aggiunto alla libreria") : tr("Rimosso dalla libreria"));
            });
        return true;
    });
    left->addView(libraryButton);

    orderButton = new brls::Button();
    orderButton->setText(tr("Ordine: dall'ultimo episodio"));
    orderButton->setStyle(&brls::BUTTONSTYLE_BORDERED);
    orderButton->setWidth(340);
    orderButton->setMarginTop(10);
    orderButton->registerClickAction([this](brls::View*) {
        toggleOrder();
        return true;
    });
    left->addView(orderButton);

    descLabel = new brls::Label();
    descLabel->setFontSize(16);
    descLabel->setWidth(340);
    descLabel->setMarginTop(16);
    descLabel->setTextColor(nvgRGB(200, 200, 205));
    left->addView(descLabel);

    leftScroll->setContentView(left);
    root->addView(leftScroll);

    // ---- lista episodi
    auto* right = new brls::Box(brls::Axis::COLUMN);
    right->setGrow(1);

    statusLabel = new brls::Label();
    statusLabel->setText(tr("Caricamento episodi..."));
    statusLabel->setFontSize(20);
    statusLabel->setMargins(40, 40, 0, 40);
    right->addView(statusLabel);

    recycler = new brls::RecyclerFrame();
    recycler->setGrow(1);
    recycler->setPadding(10, 40, 30, 20);
    recycler->estimatedRowHeight = 70;
    recycler->registerCell("Header", [] { return brls::RecyclerHeader::create(); });
    recycler->registerCell("Cell", [this] {
        auto* cell = new EpisodeCell();
        cell->registerAction(tr("Scegli video"), brls::BUTTON_X, [this, cell](brls::View*) {
            if (dataSource && !dataSource->seasons) chooseVideo(cell->getIndexPath().row);
            return true;
        });
        cell->onSelect = [this](int row, bool choose) {
            if (!dataSource) return;
            if (dataSource->seasons)
                openSeason(row);
            else if (choose)
                chooseVideo(row);
            else
                playEpisode(row);
        };
        return (brls::RecyclerCell*)cell;
    });
    dataSource = new EpisodeDataSource(this);
    recycler->setDataSource(dataSource);
    recycler->setVisibility(brls::Visibility::GONE);
    right->addView(recycler);
    root->addView(right);

    root->getAppletFrameItem()->title = title;
    auto* frame = new brls::AppletFrame(root);
    frame->registerAction(tr("Aggiorna"), brls::BUTTON_Y, [this](brls::View*) {
        load();
        return true;
    });
    frame->registerAction(tr("Inverti ordine"), brls::BUTTON_RSB, [this](brls::View*) {
        toggleOrder();
        return true;
    });
    return frame;
}

void AnimeActivity::onContentAvailable() { load(); }

void AnimeActivity::willAppear(bool resetState) {
    brls::Activity::willAppear(resetState);
    // tornando dal player aggiorniamo i progressi
    if (loadedOnce) load(true);
}

void AnimeActivity::load(bool cached) {
    auto sid = sourceId, u = url, t = title;
    runAsync<json>(
        alive, [sid, u, t, cached] { return api::anime(sid, u, t, cached); },
        [this](json d) {
            data = std::move(d);
            loadedOnce = true;
            render();
        },
        [this](const std::string& err) {
            statusLabel->setText(tr("Errore: {}\n\nY per riprovare", err));
            statusLabel->setVisibility(brls::Visibility::VISIBLE);
        });
}

void AnimeActivity::render() {
    std::string t = data.value("title", title);
    if (!t.empty()) title = t;
    titleLabel->setText(title);

    std::string meta = data.value("sourceName", std::string());
    auto add = [&meta](const std::string& s) {
        if (s.empty()) return;
        meta += (meta.empty() ? "" : " \xC2\xB7 ") + s;
    };
    add(data.value("status", std::string()));
    if (data["author"].is_string()) add(data["author"].get<std::string>());
    if (data["genre"].is_string()) meta += "\n" + data["genre"].get<std::string>();
    metaLabel->setText(meta);

    std::string desc = data["description"].is_string() ? data["description"].get<std::string>() : "";
    if (desc.size() > 1500) desc = desc.substr(0, 1500) + "...";
    descLabel->setText(desc);

    if (data["thumbnail"].is_string()) {
        std::string th = data["thumbnail"].get<std::string>();
        if (th != thumbnail) {
            thumbnail = th;
            cover->setUrl(th);
        }
    }

    inLibrary = data.value("inLibrary", false);
    updateLibraryButton();

    bool seasons = data.value("fetchType", std::string("episodes")) == "seasons";
    dataSource->seasons = seasons;
    dataSource->items = seasons ? data.value("seasons", json::array()) : data.value("episodes", json::array());
    if (!seasons && oldestFirst) std::reverse(dataSource->items.begin(), dataSource->items.end());
    orderButton->setVisibility(seasons ? brls::Visibility::GONE : brls::Visibility::VISIBLE);

    if (dataSource->items.empty()) {
        std::string err = data["error"].is_string() ? data["error"].get<std::string>() : "";
        statusLabel->setText(err.empty() ? tr("Nessun episodio disponibile") : tr("Errore: {}\n\nY per riprovare", err));
        statusLabel->setVisibility(brls::Visibility::VISIBLE);
        recycler->setVisibility(brls::Visibility::GONE);
    } else {
        statusLabel->setVisibility(brls::Visibility::GONE);
        recycler->setVisibility(brls::Visibility::VISIBLE);
        recycler->reloadData();
    }

    int ci = seasons ? -1 : continueIndex();
    if (ci >= 0) {
        const auto& ep = dataSource->items[ci];
        std::string epName = ep.value("name", tr("episodio"));
        resumeButton->setText(ep.value("position", 0.0) > 5 ? tr("Riprendi: {}", epName) : tr("Guarda: {}", epName));
        resumeButton->setVisibility(brls::Visibility::VISIBLE);
    } else {
        resumeButton->setVisibility(brls::Visibility::GONE);
    }
}

void AnimeActivity::toggleOrder() {
    if (!dataSource || dataSource->seasons || data.is_null()) return;
    oldestFirst = !oldestFirst;
    orderButton->setText(oldestFirst ? tr("Ordine: dal primo episodio") : tr("Ordine: dall'ultimo episodio"));
    render();
}

void AnimeActivity::updateLibraryButton() {
    libraryButton->setText(inLibrary ? tr("Nella libreria (premi per rimuovere)") : tr("+ Aggiungi alla libreria"));
}

int AnimeActivity::continueIndex() const {
    const json& eps = dataSource->items;
    if (eps.empty()) return -1;
    int inProgress = -1;
    double lastWatched = -1;
    int lastWatchedIdx = -1;
    for (int i = 0; i < (int)eps.size(); i++) {
        double n = eps[i].value("number", -1.0);
        bool w = eps[i].value("watched", false);
        double pos = eps[i].value("position", 0.0);
        if (pos > 5 && !w && (inProgress < 0 || n > eps[inProgress].value("number", -1.0))) inProgress = i;
        if (w && n > lastWatched) {
            lastWatched = n;
            lastWatchedIdx = i;
        }
    }
    if (inProgress >= 0) return inProgress;
    if (lastWatchedIdx >= 0) {
        int best = -1;
        for (int i = 0; i < (int)eps.size(); i++) {
            double n = eps[i].value("number", -1.0);
            if (n > lastWatched && (best < 0 || n < eps[best].value("number", -1.0))) best = i;
        }
        if (best >= 0) return best;
        int next = oldestFirst ? lastWatchedIdx + 1 : lastWatchedIdx - 1;
        return next >= 0 && next < (int)eps.size() ? next : -1;
    }
    // mai visto: primo episodio (numero piu' basso, altrimenti l'ultimo della lista)
    int first = -1;
    for (int i = 0; i < (int)eps.size(); i++) {
        double n = eps[i].value("number", -1.0);
        if (n >= 0 && (first < 0 || n < eps[first].value("number", -1.0))) first = i;
    }
    if (first >= 0) return first;
    return oldestFirst ? 0 : (int)eps.size() - 1;
}

void AnimeActivity::playEpisode(int index, const std::string& forcedToken, double startAt) {
    PlayRequest req;
    req.sourceId = sourceId;
    req.animeUrl = url;
    req.animeTitle = title;
    req.thumbnail = thumbnail;
    for (auto& e : dataSource->items)
        req.episodes.push_back({e.value("url", ""), e.value("name", ""), e.value("number", -1.0)});
    req.index = index;
    req.oldestFirst = oldestFirst;
    req.forcedToken = forcedToken;
    const auto& ep = dataSource->items[index];
    if (startAt >= 0)
        req.startAt = startAt;
    else if (!ep.value("watched", false))
        req.startAt = ep.value("position", 0.0);
    brls::Application::pushActivity(new PlayerActivity(req), brls::TransitionAnimation::NONE);
}

void AnimeActivity::chooseVideo(int index) {
    const auto& ep = dataSource->items[index];
    auto sid = sourceId;
    std::string epUrl = ep.value("url", ""), epName = ep.value("name", "");
    brls::Application::notify(tr("Cerco i video..."));
    runAsync<json>(
        alive, [sid, epUrl, epName] { return api::hosters(sid, epUrl, epName); },
        [this, index, sid, epUrl](json hs) {
            struct Opt {
                std::string label;
                std::string token;
                int lazyIndex;
            };
            auto opts = std::make_shared<std::vector<Opt>>();
            for (auto& h : hs.value("hosters", json::array())) {
                std::string hn = h.value("name", "");
                if (h["videos"].is_array()) {
                    for (auto& v : h["videos"]) opts->push_back({hn + " - " + v.value("title", ""), v.value("token", ""), -1});
                    if (h["videos"].empty() && h["error"].is_string())
                        opts->push_back({tr("{} (errore)", hn), "", -2});
                } else {
                    opts->push_back({tr("{} (carica...)", hn), "", h.value("index", 0)});
                }
            }
            if (opts->empty()) {
                brls::Application::notify(tr("Nessun video disponibile"));
                return;
            }
            std::vector<std::string> labels;
            for (auto& o : *opts) labels.push_back(o.label);
            // l'azione va eseguita nel dismissCb: il cb normale viene chiamato prima che il menu si chiuda
            auto* dd = new brls::Dropdown(tr("Scegli il video"), labels, [](int) {}, 0, [this, opts, index, sid, epUrl](int sel) {
                if (sel < 0 || sel >= (int)opts->size()) return;
                auto o = (*opts)[sel];
                if (o.lazyIndex == -2) return;
                if (o.lazyIndex < 0) {
                    playEpisode(index, o.token);
                    return;
                }
                runAsync<json>(
                    alive, [sid, epUrl, o] { return api::hosterVideos(sid, epUrl, o.lazyIndex); },
                    [this, index](json r) {
                        auto vids = r.value("videos", json::array());
                        if (vids.empty()) {
                            brls::Application::notify(tr("Nessun video in questo hoster"));
                            return;
                        }
                        std::vector<std::string> l2;
                        for (auto& v : vids) l2.push_back(v.value("title", tr("Video")));
                        auto* d2 = new brls::Dropdown(tr("Qualita'"), l2, [](int) {}, 0, [this, index, vids](int s) {
                            if (s >= 0 && s < (int)vids.size()) playEpisode(index, vids[s].value("token", ""));
                        });
                        brls::Application::pushActivity(new brls::Activity(d2));
                    });
            });
            brls::Application::pushActivity(new brls::Activity(dd));
        });
}

void AnimeActivity::openSeason(int index) {
    const auto& s = dataSource->items[index];
    brls::Application::pushActivity(
        new AnimeActivity(sourceId, s.value("url", ""), s.value("title", ""),
                          s["thumbnail"].is_string() ? s["thumbnail"].get<std::string>() : ""));
}
