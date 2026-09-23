#include "activity/main_activity.hpp"
#include "util/i18n.hpp"

#include "activity/anime_activity.hpp"
#include "activity/browse_activity.hpp"
#include "activity/source_picker.hpp"
#include "activity/update_activity.hpp"

#ifndef UPDATE_REPO
#define UPDATE_REPO "DiGiTaLAnGeL92/AnikkuNX"
#endif
#define UPDATE_REPO_DISPLAY UPDATE_REPO
#include "config.hpp"
#include "app/api.hpp"
#include "sources/source.hpp"

using json = nlohmann::json;

static std::string fmtTime(double s) {
    long t = (long)s;
    char buf[24];
    snprintf(buf, sizeof(buf), "%02ld:%02ld", t / 60, t % 60);
    return buf;
}

static brls::ScrollingFrame* scrollOf(brls::Box* content) {
    auto* sf = new brls::ScrollingFrame();
    sf->setGrow(1);
    sf->setContentView(content);
    return sf;
}

static brls::Label* header(const std::string& text) {
    auto* l = new brls::Label();
    l->setText(text);
    l->setFontSize(15);
    l->setTextColor(nvgRGB(150, 150, 160));
    l->setMargins(4, 0, 10, 0);
    return l;
}

// ============================================================================ MainActivity

brls::View* MainActivity::createContentView() {
    auto* tabs = new brls::TabFrame();
    tabs->addTab(tr("Continua a guardare"), [] { return new HistoryTab(); });
    tabs->addTab(tr("Libreria"), [] { return new LibraryTab(); });
    tabs->addSeparator();
    tabs->addTab(tr("Sorgenti"), [] { return new SourcesTab(); });
    tabs->addTab(tr("Cerca ovunque"), [] { return new SearchTab(); });
    tabs->addSeparator();
    tabs->addTab(tr("Impostazioni"), [] { return new SettingsTab(); });

    tabs->getAppletFrameItem()->title = "Anikku NX";
    tabs->getAppletFrameItem()->iconPath = BRLS_ASSET("icon/icon_96.png");
    auto* frame = new brls::AppletFrame(tabs);
    return frame;
}

void MainActivity::onContentAvailable() {
    // primo avvio: scelta delle fonti da attivare
    if (!Config::instance().sourcesChosen)
        brls::delay(100, [] { brls::Application::pushActivity(new SourcePickerActivity(true)); });
    else if (Config::instance().checkUpdates)
        brls::delay(1500, [] { checkForUpdates(false); });  // nuova versione su GitHub?
}

// ============================================================================ TabBase

TabBase::TabBase() : brls::Box(brls::Axis::COLUMN) { this->setGrow(1); }

TabBase::~TabBase() { *alive = false; }

static std::vector<GridItem> gridFromAnimeArray(const json& arr) {
    std::vector<GridItem> items;
    for (auto& a : arr)
        items.push_back({a.value("sourceId", ""), a.value("url", ""), a.value("title", ""), a.value("sourceName", ""),
                         a.value("thumbnail", ""), a});
    return items;
}

// ============================================================================ Cronologia

HistoryTab::HistoryTab() {
    grid = new AnimeGrid(4, 196);
    grid->onSelect = [](const GridItem& it) {
        brls::Application::pushActivity(new AnimeActivity(it.sourceId, it.url, it.title, it.thumbnail));
    };
    grid->secondaryHint = tr("Rimuovi");
    grid->onSecondary = [this](const GridItem& it) {
        auto* d = new brls::Dialog(tr("Togliere \"{}\" da Continua a guardare?", it.title));
        d->addButton(tr("Annulla"), [] {});
        d->addButton(tr("Rimuovi"), [this, it] {
            runAsync<bool>(
                alive,
                [it] {
                    api::removeFromHistory(it.sourceId, it.url);
                    return true;
                },
                [this](bool) { reload(); });
        });
        d->open();
    };
    this->addView(grid);
    reload();
}

void HistoryTab::willAppear(bool resetState) {
    TabBase::willAppear(resetState);
    if (appeared) reload();
    appeared = true;
}

void HistoryTab::reload() {
    grid->showLoading();
    runAsync<json>(
        alive, [] { return api::history(); },
        [this](json arr) {
            grid->focusFallback = this->getParent();
            bool refocus = grid->clear();
            std::vector<GridItem> items;
            for (auto& h : arr) {
                std::string sub = h.value("episodeName", "");
                if (h.value("watched", false))
                    sub += " \xC2\xB7 " + tr("visto");
                else if (h.value("duration", 0.0) > 0)
                    sub += " \xC2\xB7 " + fmtTime(h.value("position", 0.0));
                items.push_back({h.value("sourceId", ""), h.value("animeUrl", ""), h.value("title", ""), sub,
                                 h.value("thumbnail", ""), h});
            }
            grid->hideStatus();
            grid->append(items);
            if (items.empty()) grid->showMessage(tr("Non hai ancora guardato nulla.\nApri \"Sorgenti\" per iniziare!"));
            if (refocus && !items.empty()) brls::Application::giveFocus(grid);
        },
        [this](const std::string& err) { grid->showMessage(err); });
}

// ============================================================================ Libreria

LibraryTab::LibraryTab() {
    grid = new AnimeGrid(4, 196);
    grid->onSelect = [](const GridItem& it) {
        brls::Application::pushActivity(new AnimeActivity(it.sourceId, it.url, it.title, it.thumbnail));
    };
    grid->secondaryHint = tr("Rimuovi");
    grid->onSecondary = [this](const GridItem& it) {
        auto* d = new brls::Dialog(tr("Rimuovere \"{}\" dalla libreria?", it.title));
        d->addButton(tr("Annulla"), [] {});
        d->addButton(tr("Rimuovi"), [this, it] {
            runAsync<bool>(
                alive,
                [it] {
                    api::removeLibrary(it.sourceId, it.url);
                    return true;
                },
                [this](bool) { reload(); });
        });
        d->open();
    };
    this->addView(grid);
    reload();
}

void LibraryTab::willAppear(bool resetState) {
    TabBase::willAppear(resetState);
    if (appeared) reload();
    appeared = true;
}

void LibraryTab::reload() {
    grid->showLoading();
    runAsync<json>(
        alive, [] { return api::library(); },
        [this](json arr) {
            grid->focusFallback = this->getParent();
            bool refocus = grid->clear();
            auto items = gridFromAnimeArray(arr);
            grid->hideStatus();
            grid->append(items);
            if (items.empty())
                grid->showMessage(tr("La libreria e' vuota.\nApri un anime e scegli \"Aggiungi alla libreria\"."));
            if (refocus && !items.empty()) brls::Application::giveFocus(grid);
        },
        [this](const std::string& err) { grid->showMessage(err); });
}

// ============================================================================ Sorgenti

SourcesTab::SourcesTab() {
    list = new brls::Box(brls::Axis::COLUMN);
    list->setPadding(20, 40, 30, 40);
    list->addView(header(tr("Caricamento...")));
    this->addView(scrollOf(list));
    reload();
}

void SourcesTab::willAppear(bool resetState) {
    TabBase::willAppear(resetState);
    if (appeared) reload();  // le fonti attive possono essere cambiate
    appeared = true;
}

static std::string langLabel(const std::string& l) {
    if (l == "it") return tr("Italiano");
    if (l == "en") return tr("Inglese");
    if (l == "all") return tr("Multilingua");
    return l;
}

void SourcesTab::reload() {
    runAsync<json>(
        alive, [] { return api::sources(); },
        [this](json arr) {
            bool hadFocus = list->isChildFocused();
            list->clearViews();
            if (arr.empty()) {
                list->addView(header(tr("Nessuna fonte attiva: scegline qualcuna in Impostazioni.")));
                return;
            }
            list->addView(header(tr("Scegli dove cercare gli anime")));
            for (auto& s : arr) {
                auto* cell = new brls::DetailCell();
                std::string lang = s.value("lang", "");
                cell->setText(s.value("name", ""));
                cell->setDetailText(langLabel(lang) + (s.value("nsfw", false) ? " \xC2\xB7 18+" : ""));
                std::string id = s.value("id", ""), name = s.value("name", "");
                bool latest = s.value("supportsLatest", false);
                cell->registerClickAction([id, name, latest](brls::View*) {
                    brls::Application::pushActivity(new BrowseActivity(id, name, latest));
                    return true;
                });
                list->addView(cell);
            }
            if (hadFocus) brls::Application::giveFocus(list);
        },
        [this](const std::string& err) {
            list->clearViews();
            list->addView(header(err));
        });
}

// ============================================================================ Ricerca globale

SearchTab::SearchTab() {
    button = new brls::Button();
    button->setText(tr("Cerca un anime in tutte le sorgenti"));
    button->setStyle(&brls::BUTTONSTYLE_PRIMARY);
    button->setMargins(20, 40, 0, 40);
    button->registerClickAction([this](brls::View*) {
        ask();
        return true;
    });
    this->addView(button);

    grid = new AnimeGrid(4, 196);
    grid->focusFallback = button;
    grid->onSelect = [](const GridItem& it) {
        brls::Application::pushActivity(new AnimeActivity(it.sourceId, it.url, it.title, it.thumbnail));
    };
    this->addView(grid);
}

void SearchTab::ask() {
    brls::Application::getImeManager()->openForText(
        [this](std::string text) {
            if (!text.empty()) search(text);
        },
        tr("Cerca anime"), tr("Titolo (es. One Piece)"), 64, query);
}

void SearchTab::search(const std::string& q) {
    query = q;
    int gen = ++generation;
    grid->clear();
    grid->showMessage(tr("Cerco \"{}\"...", q));
    button->setText(tr("Risultati per \"{}\" (premi per cambiare)", q));

    runAsync<json>(
        alive, [] { return api::sources(); },
        [this, gen, q](json sources) {
            if (gen != generation) return;
            pending = (int)sources.size();
            if (pending == 0) grid->showMessage(tr("Nessuna sorgente installata"));
            for (auto& s : sources) {
                std::string id = s.value("id", ""), name = s.value("name", "");
                runAsync<json>(
                    alive, [id, q] { return api::browse(id, "search", 1, q); },
                    [this, gen, name](json r) {
                        if (gen != generation) return;
                        pending--;
                        std::vector<GridItem> items;
                        int n = 0;
                        for (auto& a : r.value("animes", json::array())) {
                            if (n++ >= 8) break;  // max 8 risultati per sorgente
                            items.push_back({a.value("sourceId", ""), a.value("url", ""), a.value("title", ""), name,
                                             a.value("thumbnail", ""), a});
                        }
                        if (!items.empty()) grid->hideStatus();
                        grid->append(items);
                        if (pending == 0 && grid->count() == 0) grid->showMessage(tr("Nessun risultato"));
                    },
                    [this, gen](const std::string&) {
                        if (gen != generation) return;
                        pending--;
                        if (pending == 0 && grid->count() == 0) grid->showMessage(tr("Nessun risultato"));
                    });
            }
        },
        [this](const std::string& err) { grid->showMessage(err); });
}

// ============================================================================ Impostazioni

SettingsTab::SettingsTab() {
    auto* box = new brls::Box(brls::Axis::COLUMN);
    box->setPadding(20, 40, 30, 40);
    auto& cfg = Config::instance();

    box->addView(header(tr("Fonti")));
    auto* pick = new brls::DetailCell();
    pick->setText(tr("Scegli le fonti attive"));
    pick->setDetailText(tr("{} attive", std::to_string(api::sources().size())));
    pick->registerClickAction([](brls::View*) {
        brls::Application::pushActivity(new SourcePickerActivity(false));
        return true;
    });
    box->addView(pick);

    box->addView(header(tr("Indirizzi dei siti attivi (cambiali se una fonte smette di funzionare)")));
    for (auto& s : src::all()) {
        if (!cfg.isSourceEnabled(s->id())) continue;
        auto* cell = new brls::InputCell();
        std::string id = s->id();
        std::string current = cfg.domains.count(id) ? cfg.domains[id] : "";
        cell->init(
            s->name(), current.empty() ? s->defaultBaseUrl() : current,
            [id, cell](std::string text) {
                auto s = src::byId(id);
                if (text.empty() || (s && text == s->defaultBaseUrl()))
                    Config::instance().domains.erase(id);
                else
                    Config::instance().domains[id] = text;
                Config::instance().save();
                Config::instance().applyDomains();
                if (s) cell->setValue(s->baseUrl());
            },
            "", tr("Es. https://www.animeworld.ac (lascia vuoto per il predefinito)"), 80);
        box->addView(cell);
    }

    box->addView(header(tr("Riproduzione")));

    auto* hw = new brls::BooleanCell();
    hw->init(tr("Decodifica hardware"), cfg.hardwareDecoding, [](bool on) {
        Config::instance().hardwareDecoding = on;
        Config::instance().save();
    });
    box->addView(hw);

    auto* skip = new brls::BooleanCell();
    skip->init(tr("Salta automaticamente la sigla (se la fonte la indica)"), cfg.autoSkipOpening, [](bool on) {
        Config::instance().autoSkipOpening = on;
        Config::instance().save();
    });
    box->addView(skip);

    box->addView(header(tr("Informazioni")));
    auto* ver = new brls::DetailCell();
    ver->setText(tr("Versione"));
    ver->setDetailText("AnikkuNX v" + updater::currentVersion());
    box->addView(ver);

    auto* upd = new brls::DetailCell();
    upd->setText(tr("Controlla aggiornamenti"));
    upd->setDetailText("github.com/" UPDATE_REPO_DISPLAY);
    upd->registerClickAction([](brls::View*) {
        checkForUpdates(true);
        return true;
    });
    box->addView(upd);

    auto* autoUpd = new brls::BooleanCell();
    autoUpd->init(tr("Controlla aggiornamenti all'avvio"), cfg.checkUpdates, [](bool on) {
        Config::instance().checkUpdates = on;
        Config::instance().save();
    });
    box->addView(autoUpd);

    auto* about = new brls::Label();
    about->setText(tr(
        "App autonoma per Nintendo Switch con fonti italiane, inglesi e multilingua "
        "(porting delle estensioni di Anikku/Aniyomi).\n"
        "Libreria e progressi sono salvati in sdmc:/switch/AnikkuNX. Avvia l'app tenendo premuto R su un gioco "
        "per avere piu' memoria."));
    about->setFontSize(15);
    about->setTextColor(nvgRGB(150, 150, 160));
    about->setMarginTop(24);
    box->addView(about);

    this->addView(scrollOf(box));
}
