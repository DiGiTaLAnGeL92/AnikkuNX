#include "activity/source_picker.hpp"
#include "util/i18n.hpp"

#include <algorithm>
#include <map>
#include <vector>

#include "config.hpp"
#include "sources/source.hpp"

static std::string langName(const std::string& l) {
    if (l == "it") return tr("Italiano");
    if (l == "en") return tr("Inglese");
    if (l == "all") return tr("Multilingua");
    if (l == "es") return tr("Spagnolo");
    if (l == "pt") return tr("Portoghese");
    if (l == "fr") return tr("Francese");
    if (l == "de") return tr("Tedesco");
    if (l == "ar") return tr("Arabo");
    return l;
}

static brls::Label* sectionHeader(const std::string& text) {
    auto* l = new brls::Label();
    l->setText(text);
    l->setFontSize(16);
    l->setTextColor(nvgRGB(150, 150, 160));
    l->setMargins(18, 0, 8, 0);
    return l;
}

SourcePickerActivity::SourcePickerActivity(bool first, std::function<void()> done)
    : firstRun(first), onDone(std::move(done)) {
    auto& cfg = Config::instance();
    showNsfw = cfg.showNsfw;
    for (auto& s : src::all())
        if (cfg.isSourceEnabled(s->id()) || (cfg.sourcesChosen && cfg.enabledSources.count(s->id())))
            selected.insert(s->id());
}

brls::View* SourcePickerActivity::createContentView() {
    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setGrow(1);

    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1);
    auto* box = new brls::Box(brls::Axis::COLUMN);
    box->setPadding(20, 60, 30, 60);

    auto* intro = new brls::Label();
    intro->setText(firstRun ? tr("Benvenuto! Scegli le fonti che vuoi usare: solo quelle attive vengono caricate, "
                              "cosi' l'app resta veloce. Puoi cambiarle quando vuoi da Impostazioni.")
                            : tr("Attiva o disattiva le fonti. Solo quelle attive compaiono in \"Sorgenti\" e nella "
                              "ricerca globale."));
    intro->setFontSize(18);
    box->addView(intro);

    auto* confirmBtn = new brls::Button();
    confirmBtn->setText(tr("Conferma"));
    confirmBtn->setStyle(&brls::BUTTONSTYLE_PRIMARY);
    confirmBtn->setMarginTop(16);
    confirmBtn->registerClickAction([this](brls::View*) {
        confirm();
        return true;
    });
    box->addView(confirmBtn);

    auto* nsfw = new brls::BooleanCell();
    nsfw->init(tr("Mostra fonti per adulti (18+)"), showNsfw, [this](bool on) {
        showNsfw = on;
        rebuild();
    });
    nsfw->setMarginTop(10);
    box->addView(nsfw);

    list = new brls::Box(brls::Axis::COLUMN);
    box->addView(list);

    scroll->setContentView(box);
    root->addView(scroll);
    rebuild();

    root->getAppletFrameItem()->title = tr("Scegli le fonti");
    auto* frame = new brls::AppletFrame(root);
    frame->registerAction(tr("Conferma"), brls::BUTTON_START, [this](brls::View*) {
        confirm();
        return true;
    });
    return frame;
}

void SourcePickerActivity::rebuild() {
    list->clearViews();
    // raggruppa per lingua mantenendo l'ordine: italiano, inglese, multilingua, altre
    std::vector<std::string> order = {"it", "en", "all"};
    for (auto& s : src::all())
        if (std::find(order.begin(), order.end(), s->lang()) == order.end()) order.push_back(s->lang());

    for (auto& lang : order) {
        bool headerAdded = false;
        for (auto& s : src::all()) {
            if (s->lang() != lang || (s->nsfw() && !showNsfw)) continue;
            if (!headerAdded) {
                list->addView(sectionHeader(langName(lang)));
                headerAdded = true;
            }
            std::string id = s->id();
            auto* cell = new brls::BooleanCell();
            cell->init(s->name() + (s->nsfw() ? "  (18+)" : ""), selected.count(id) > 0, [this, id](bool on) {
                if (on)
                    selected.insert(id);
                else
                    selected.erase(id);
            });
            list->addView(cell);
        }
    }
}

void SourcePickerActivity::confirm() {
    int visible = 0;
    for (auto& s : src::all())
        if (selected.count(s->id()) && (showNsfw || !s->nsfw())) visible++;
    if (visible == 0) {
        brls::Application::notify(tr("Attiva almeno una fonte"));
        return;
    }
    auto& cfg = Config::instance();
    cfg.enabledSources = selected;
    cfg.showNsfw = showNsfw;
    cfg.sourcesChosen = true;
    cfg.save();
    auto done = onDone;
    brls::Application::popActivity(brls::TransitionAnimation::FADE, [done] {
        if (done) done();
    });
}
