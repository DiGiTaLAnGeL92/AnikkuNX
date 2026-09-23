#pragma once

#include <borealis.hpp>

#include <functional>
#include <set>
#include <string>

/**
 * Scelta delle fonti da attivare (mostrata al primo avvio e dalle Impostazioni).
 * Solo le fonti attivate compaiono in "Sorgenti" e nella ricerca globale.
 */
class SourcePickerActivity : public brls::Activity {
  public:
    SourcePickerActivity(bool firstRun, std::function<void()> onDone = nullptr);
    brls::View* createContentView() override;

  private:
    void rebuild();
    void confirm();

    bool firstRun;
    std::function<void()> onDone;
    std::set<std::string> selected;
    bool showNsfw;
    brls::Box* list = nullptr;
};
