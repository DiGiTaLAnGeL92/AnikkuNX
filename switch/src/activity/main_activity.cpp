#include "activity/main_activity.hpp"
#include "util/i18n.hpp"

#include "activity/anime_activity.hpp"
#include "activity/browse_activity.hpp"
#include "activity/source_picker.hpp"
#include "activity/update_activity.hpp"
#include "util/network.hpp"
#include "util/platform.hpp"

#ifndef UPDATE_REPO
#define UPDATE_REPO "DiGiTaLAnGeL92/AnikkuNX"
#endif
#define UPDATE_REPO_DISPLAY UPDATE_REPO
#include "config.hpp"
#include "util/sublang.hpp"
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

namespace {

void showFullMemoryHelp() {
    auto* d = new brls::Dialog(tr(
        "AnikkuNX e' stata avviata in modalita' applet (dall'Album): la memoria disponibile e' molto ridotta e "
        "i video possono bloccarsi o chiudere l'app.\n\n"
        "Per avere la memoria piena:\n"
        "\xE2\x80\xA2 tieni premuto R mentre avvii un gioco qualsiasi, poi apri AnikkuNX dal menu homebrew;\n"
        "\xE2\x80\xA2 oppure crea un forwarder con Sphaira (X su AnikkuNX > Install Forwarder) e avviala dalla Home."));
    d->addButton(tr("OK"), [] {});
    d->open();
}

/** Avviso in alto a destra quando l'app gira in modalita' applet; toccandolo spiega come avere piu' memoria. */
class AppletWarning : public brls::Box {
  public:
    AppletWarning() {
        setFocusable(true);
        setHideHighlightBackground(true);
        setAlignItems(brls::AlignItems::CENTER);
        setPadding(4, 14, 4, 40);  // a sinistra lo spazio per l'icona
        setCornerRadius(16);
        setBackgroundColor(nvgRGBA(230, 160, 20, 45));
        setMarginTop(18);
        auto* l = new brls::Label();
        l->setText(tr("Modalita' applet: memoria limitata"));
        l->setFontSize(17);
        l->setTextColor(nvgRGB(255, 196, 64));
        addView(l);
        registerClickAction([](brls::View*) {
            showFullMemoryHelp();
            return true;
        });
        addGestureRecognizer(new brls::TapGestureRecognizer(this));
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
              brls::FrameContext* ctx) override {
        brls::Box::draw(vg, x, y, width, height, style, ctx);
        int f = brls::Application::getFont(brls::FONT_MATERIAL_ICONS);
        if (f < 0) return;
        nvgFontFaceId(vg, f);
        nvgFontSize(vg, 22);
        nvgFillColor(vg, nvgRGB(255, 196, 64));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(vg, x + 22, y + height / 2, "\xEE\x80\x82", nullptr);  // Material "warning" (U+E002)
        nvgFontFaceId(vg, brls::Application::getDefaultFont());
    }
};

}  // namespace

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
    if (platform::isAppletMode()) frame->getHeader()->addView(new AppletWarning());
    return frame;
}

void MainActivity::onContentAvailable() {
    // il player si era chiuso in modo anomalo: spiega cosa e' stato disattivato
    std::string notice = Config::instance().crashNotice;
    Config::instance().crashNotice.clear();
    if (!notice.empty()) {
        std::string text = tr("L'app si e' chiusa durante la riproduzione. Ho disattivato il proxy per gli stream "
                              "camuffati (Impostazioni > Riproduzione): se il problema sparisce, era quella la causa.");
        brls::delay(800, [text] {
            auto* d = new brls::Dialog(text);
            d->addButton(tr("OK"), [] {});
            d->open();
        });
    }
    // primo avvio: scelta delle fonti da attivare
    if (!Config::instance().sourcesChosen)
        brls::delay(100, [] { brls::Application::pushActivity(new SourcePickerActivity(true)); });
    else {
        if (Config::instance().checkUpdates)
            brls::delay(1500, [] { checkForUpdates(false); });  // nuova versione su GitHub?
        if (Config::instance().checkNewEpisodes)
            brls::delay(4000, [] { checkNewEpisodesWhenOnline(); });  // nuovi episodi in libreria
    }
}

// ============================================================================ TabBase

TabBase::TabBase() : brls::Box(brls::Axis::COLUMN) { this->setGrow(1); }

TabBase::~TabBase() { *alive = false; }

static std::vector<GridItem> gridFromAnimeArray(const json& arr) {
    std::vector<GridItem> items;
    for (auto& a : arr) {
        GridItem it{a.value("sourceId", ""), a.value("url", ""), a.value("title", ""), a.value("sourceName", ""),
                    a.value("thumbnail", ""), a};
        int n = a.value("newEpisodes", 0);
        if (n > 0) {
            it.badge = "+" + std::to_string(n);
            it.subtitle = n == 1 ? tr("1 episodio nuovo") : tr("{} episodi nuovi", std::to_string(n));
        }
        items.push_back(it);
    }
    return items;
}

// ============================================================================ Nuovi episodi

void checkNewEpisodesWhenOnline(int attempt) {
    if (!network::connected()) {
        if (attempt < 120) brls::delay(5000, [attempt] { checkNewEpisodesWhenOnline(attempt + 1); });
        return;
    }
    static AliveToken appAlive = makeAlive();
    runAsync<json>(
        appAlive, [] { return api::refreshLibrary(); },
        [](json found) {
            if (LibraryTab::current) LibraryTab::current->reload();
            if (found.empty()) return;
            if (found.size() == 1)
                brls::Application::notify(tr("Nuovi episodi di {}", found[0].value("title", "")));
            else
                brls::Application::notify(tr("{} anime della libreria hanno nuovi episodi", std::to_string(found.size())));
        },
        [](const std::string&) {});
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

LibraryTab* LibraryTab::current = nullptr;

LibraryTab::~LibraryTab() {
    if (current == this) current = nullptr;
}

LibraryTab::LibraryTab() {
    current = this;
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

static std::string langLabel(const std::string& l) { return i18n::languageName(l); }

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
    pick->registerClickAction([pick](brls::View*) {
        // dopo la conferma aggiorna subito il numero di fonti attive
        brls::Application::pushActivity(new SourcePickerActivity(false, [pick] {
            pick->setDetailText(tr("{} attive", std::to_string(api::sources().size())));
        }));
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

    {
        // lingua dei sottotitoli quando il video ne ha piu' di una
        std::vector<std::string> labels;
        int selected = 0;
        const auto& codes = sublang::choices();
        for (size_t i = 0; i < codes.size(); i++) {
            const std::string& c = codes[i];
            labels.push_back(c == "auto"  ? tr("Lingua della console")
                             : c == "off" ? tr("Nessuno (sottotitoli spenti)")
                                          : i18n::languageName(c));
            if (c == cfg.subtitleLang) selected = (int)i;
        }
        auto* subLang = new brls::SelectorCell();
        subLang->init(tr("Lingua dei sottotitoli"), labels, selected, [](int i) {
            const auto& codes = sublang::choices();
            if (i < 0 || i >= (int)codes.size()) return;
            Config::instance().subtitleLang = codes[i];
            Config::instance().save();
        });
        box->addView(subLang);
    }

    auto* proxy = new brls::BooleanCell();
    proxy->init(tr("Ripara gli stream con segmenti camuffati (proxy locale)"), cfg.hlsProxy, [](bool on) {
        Config::instance().hlsProxy = on;
        Config::instance().save();
    });
    box->addView(proxy);

    auto* skip = new brls::BooleanCell();
    skip->init(tr("Salta automaticamente la sigla (se la fonte la indica)"), cfg.autoSkipOpening, [](bool on) {
        Config::instance().autoSkipOpening = on;
        Config::instance().save();
    });
    box->addView(skip);

    box->addView(header(tr("Informazioni")));
    auto* fwd = new brls::DetailCell();
    fwd->setText(tr("Icona nella schermata Home (forwarder)"));
    fwd->setDetailText("Sphaira");
    fwd->registerClickAction([](brls::View*) {
        auto* d = new brls::Dialog(tr(
            "Puoi avviare AnikkuNX direttamente dalla schermata Home della console creando un forwarder con Sphaira:\n\n"
            "1. Apri Sphaira (il menu homebrew).\n"
            "2. Seleziona AnikkuNX e premi X.\n"
            "3. Scegli \"Installa forwarder\" (Install Forwarder) e conferma.\n\n"
            "L'icona di AnikkuNX comparira' nella Home insieme ai giochi."));
        d->addButton(tr("OK"), [] {});
        d->open();
        return true;
    });
    box->addView(fwd);

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

    auto* newEps = new brls::BooleanCell();
    newEps->init(tr("Controlla i nuovi episodi della libreria all'avvio"), cfg.checkNewEpisodes, [](bool on) {
        Config::instance().checkNewEpisodes = on;
        Config::instance().save();
    });
    box->addView(newEps);

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
