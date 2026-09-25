#include <borealis.hpp>

#include <cstdlib>
#include <cstring>

#include "activity/main_activity.hpp"
#include "app/api.hpp"
#include "config.hpp"
#include "net/http.hpp"
#include "util/i18n.hpp"
#include "app/updater.hpp"
#include "util/async.hpp"
#include "view/cover_image.hpp"

int main(int argc, char* argv[]) {
    if (argc > 0 && argv[0]) updater::setAppPath(argv[0]);  // per sostituire il .nro negli aggiornamenti
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-d") == 0) brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
    }

    if (!brls::Application::init()) {
        brls::Logger::error("Impossibile inizializzare borealis");
        return EXIT_FAILURE;
    }
    i18n::init();  // lingua dell'interfaccia = lingua di sistema


#ifdef __SWITCH__
    http::globalInit("romfs:/cacert.pem");
#else
    http::globalInit("resources/cacert.pem");
#endif
    Config::instance().load();
    Config::instance().checkPreviousCrash();
    Config::instance().save();  // crea la cartella dati se manca
    Config::instance().applyDomains();
    api::init(Config::instance().configDir());

    brls::Application::createWindow("AnikkuNX");
#ifdef __SWITCH__
    // (dopo createWindow: prima non esistono ne' il contesto grafico ne' i font di sistema)
    // Font predefinito = font standard della console (vedi scripts/setup.sh): i caratteri che non ha
    // (cinese, coreano, simboli dei pulsanti, icone) vengono presi dagli altri font di sistema.
    {
        NVGcontext* vg = brls::Application::getNVGContext();
        int regular = brls::Application::getFont(brls::FONT_REGULAR);
        for (const std::string& name :
             {brls::FONT_CHINESE_SIMPLIFIED, brls::FONT_CHINESE_SIMPLIFIED_EXT, brls::FONT_CHINESE_TRADITIONAL,
              brls::FONT_KOREAN_REGULAR, brls::FONT_SWITCH_ICONS, brls::FONT_MATERIAL_ICONS}) {
            int f = brls::Application::getFont(name);
            if (regular >= 0 && f >= 0) nvgAddFallbackFontId(vg, regular, f);
        }
        // hindi (devanagari), arabo, thai, ebraico...: i font della console non li hanno.
        // GNU FreeSans (GPL con eccezione per i font) li copre; senza shaping le legature indiane sono semplificate.
        if (regular >= 0 && brls::Application::loadFontFromFile("freesans", "romfs:/font/FreeSans.ttf"))
            nvgAddFallbackFontId(vg, regular, brls::Application::getFont("freesans"));
    }
#endif
    brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
    brls::Application::setGlobalQuit(false);

    // Tema: accento rosa come Anikku
    brls::Theme::getDarkTheme().addColor("brls/accent", nvgRGB(214, 51, 108));
    brls::Theme::getDarkTheme().addColor("brls/highlight/color1", nvgRGB(214, 51, 108));
    brls::Theme::getDarkTheme().addColor("brls/highlight/color2", nvgRGB(255, 120, 170));
    brls::Theme::getDarkTheme().addColor("brls/button/primary_enabled_background", nvgRGB(214, 51, 108));
    brls::Theme::getDarkTheme().addColor("brls/sidebar/active_item", nvgRGB(214, 51, 108));
    brls::Theme::getDarkTheme().addColor("brls/slider/line_filled", nvgRGB(214, 51, 108));

    // Barra laterale piu' stretta: lascia spazio alla griglia delle copertine
    brls::getStyle().addMetric("brls/tab_frame/sidebar_width", 300);
    brls::getStyle().addMetric("brls/sidebar/padding_left", 30);
    brls::getStyle().addMetric("brls/sidebar/padding_right", 20);

    brls::Application::pushActivity(new MainActivity());

    while (brls::Application::mainLoop())
        ;

    ThreadPool::instance().stop();
    CoverImage::clearCache();
    http::globalCleanup();
    return EXIT_SUCCESS;
}
