#include "activity/update_activity.hpp"

#include <sstream>

#include "config.hpp"
#include "util/network.hpp"

#ifdef __SWITCH__
#include <switch.h>
#endif
#include "util/i18n.hpp"

namespace {

AliveToken& globalAlive() {
    static AliveToken token = makeAlive();  // i controlli di aggiornamento vivono quanto l'app
    return token;
}

/** Barra di avanzamento disegnata con nanovg. */
class ProgressBar : public brls::View {
  public:
    explicit ProgressBar(std::shared_ptr<std::atomic<float>> v) : value(std::move(v)) {
        setHeight(14);
        setWidthPercentage(100);
    }
    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style, brls::FrameContext*) override {
        float f = std::min(1.f, std::max(0.f, value->load()));
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, height / 2);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, 40));
        nvgFill(vg);
        if (f > 0) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, y, std::max(height, width * f), height, height / 2);
            nvgFillColor(vg, nvgRGB(214, 51, 108));
            nvgFill(vg);
        }
    }

  private:
    std::shared_ptr<std::atomic<float>> value;
};

std::string shortNotes(std::string notes) {
    // le note generate da GitHub possono essere lunghe: bastano le prime righe
    // il changelog e' in Markdown: toglie titoli e grassetti, tiene gli elenchi
    std::string clean;
    std::istringstream in(notes);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        while (!line.empty() && line[0] == '#') line.erase(0, 1);
        size_t p;
        while ((p = line.find("**")) != std::string::npos) line.erase(p, 2);
        if (line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0) line = "\xE2\x80\xA2 " + line.substr(2);
        if (!clean.empty() || !line.empty()) clean += line + "\n";
    }
    notes = clean;
    while (!notes.empty() && (notes.back() == '\n' || notes.back() == ' ')) notes.pop_back();
    if (notes.size() > 700) notes = notes.substr(0, 700) + "...";
    return notes;
}

void runCheck(bool manual, int retriesLeft);

}  // namespace

void checkForUpdatesWhenOnline(int attempt) {
    // attende la connessione: un tentativo ogni 5 s per circa 10 minuti, poi rinuncia fino al prossimo avvio
    if (network::connected()) {
        runCheck(false, 2);
        return;
    }
    if (attempt >= 120) return;
    brls::delay(5000, [attempt] { checkForUpdatesWhenOnline(attempt + 1); });
}

void checkForUpdates(bool manual) {
    if (!manual) {
        checkForUpdatesWhenOnline(0);
        return;
    }
    if (!network::connected()) {
        brls::Application::notify(tr("Nessuna connessione a Internet"));
        return;
    }
    brls::Application::notify(tr("Controllo aggiornamenti..."));
    runCheck(true, 0);
}

namespace {
void runCheck(bool manual, int retriesLeft) {
    runAsync<updater::Release>(
        globalAlive(), [] { return updater::latest(); },
        [manual](updater::Release rel) {
            std::string cur = updater::currentVersion();
            auto& cfg = Config::instance();
            if (!updater::isNewer(rel.version, cur)) {
                if (manual) brls::Application::notify(tr("Hai gia' l'ultima versione (v{})", cur));
                return;
            }
            if (!manual && rel.version == cfg.skippedVersion) return;
            if (rel.nroUrl.empty()) {
                if (manual) brls::Application::notify(tr("La release non contiene il file .nro"));
                return;
            }
            std::string text = tr("E' disponibile AnikkuNX v{} (installata: v{}).", rel.version, cur);
            std::string notes = shortNotes(rel.notes);
            if (!notes.empty()) text += "\n\n" + notes;
            text += "\n\n" + tr("Vuoi aggiornare adesso?");
            auto* d = new brls::Dialog(text);
            d->addButton(tr("Piu' tardi"), [] {});
            if (!manual)
                d->addButton(tr("Salta questa versione"), [rel] {
                    Config::instance().skippedVersion = rel.version;
                    Config::instance().save();
                });
            d->addButton(tr("Aggiorna"), [rel] { brls::Application::pushActivity(new UpdateActivity(rel)); });
            d->open();
        },
        [manual, retriesLeft](const std::string& err) {
            if (manual) {
                brls::Application::notify(tr("Controllo aggiornamenti non riuscito: {}", err));
            } else if (retriesLeft > 0) {
                // appena connessi il DNS puo' non essere pronto: si riprova dopo un po'
                brls::delay(15000, [retriesLeft] { runCheck(false, retriesLeft - 1); });
            }
        });
}
}  // namespace

UpdateActivity::UpdateActivity(updater::Release r) : release(std::move(r)) {}

UpdateActivity::~UpdateActivity() {
    cancel->store(true);
    *alive = false;
}

brls::View* UpdateActivity::createContentView() {
    auto* box = new brls::Box(brls::Axis::COLUMN);
    box->setGrow(1);
    box->setPadding(60, 120, 60, 120);
    box->setJustifyContent(brls::JustifyContent::CENTER);

    auto* title = new brls::Label();
    title->setText(tr("Aggiornamento a v{}", release.version));
    title->setFontSize(28);
    box->addView(title);

    status = new brls::Label();
    status->setText(tr("Download in corso..."));
    status->setFontSize(18);
    status->setMargins(20, 0, 16, 0);
    box->addView(status);

    box->addView(new ProgressBar(progress));

    closeBtn = new brls::Button();
    closeBtn->setText(tr("Annulla"));
    closeBtn->setMarginTop(30);
    closeBtn->registerClickAction([this](brls::View*) {
        if (running) {
            cancel->store(true);
        } else {
            brls::Application::popActivity();
        }
        return true;
    });
    box->addView(closeBtn);

    box->getAppletFrameItem()->title = "AnikkuNX";
    auto* frame = new brls::AppletFrame(box);
    frame->registerAction(tr("Annulla"), brls::BUTTON_B, [this](brls::View*) {
        if (mustQuit)
            brls::Application::quit();  // file gia' sostituito: niente ritorno all'app
        else if (running)
            cancel->store(true);
        else
            brls::Application::popActivity();
        return true;
    });
    return frame;
}

void UpdateActivity::onContentAvailable() {
    running = true;
    brls::Application::giveFocus(closeBtn);
    auto rel = release;
    auto prog = progress;
    auto stop = cancel;
    std::weak_ptr<bool> weak = alive;
    auto* label = status;
    runAsync<bool>(
        alive,
        [rel, prog, stop, weak, label] {
            int lastPct = -1;
            updater::install(rel, [&](float f) {
                prog->store(f);
                int pct = (int)(f * 100);
                if (pct != lastPct) {
                    lastPct = pct;
                    brls::sync([weak, label, pct] {
                        auto a = weak.lock();
                        if (a && *a) label->setText(tr("Download in corso... {}%", std::to_string(pct)));
                    });
                }
                return !stop->load();
            });
            return true;
        },
        [this](bool) {
            running = false;
            mustQuit = true;
            progress->store(1.f);
            status->setText(tr("Aggiornamento installato in {}.\nChiudi l'app e riaprila per usare la nuova versione.",
                               updater::appPath()));
            closeBtn->setText(tr("Chiudi l'app"));
            closeBtn->registerClickAction([](brls::View*) {
                brls::Application::quit();
                return true;
            });
        },
        [this](const std::string& err) {
            running = false;
            status->setText(tr("Aggiornamento non riuscito: {}", err));
            closeBtn->setText(tr("Chiudi"));
            if (err.find(updater::appPath()) != std::string::npos) {
                // la romfs e' gia' stata smontata: l'app va chiusa
                mustQuit = true;
                closeBtn->setText(tr("Chiudi l'app"));
                closeBtn->registerClickAction([](brls::View*) {
                    brls::Application::quit();
                    return true;
                });
            }
        });
}
