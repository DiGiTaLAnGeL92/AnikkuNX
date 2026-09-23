#pragma once

#include <borealis.hpp>

#include <atomic>
#include <memory>

#include "app/updater.hpp"
#include "util/async.hpp"

/**
 * Controlla su GitHub se c'e' una versione nuova e propone l'aggiornamento.
 * manual=false (avvio): silenzioso in caso di errore o se l'utente ha saltato quella versione.
 */
void checkForUpdates(bool manual);
/** Controllo automatico: aspetta che la console sia connessa a Internet, poi controlla. */
void checkForUpdatesWhenOnline(int attempt = 0);

/** Schermata di download e installazione dell'aggiornamento. */
class UpdateActivity : public brls::Activity {
  public:
    explicit UpdateActivity(updater::Release release);
    ~UpdateActivity() override;
    brls::View* createContentView() override;
    void onContentAvailable() override;

  private:
    updater::Release release;
    AliveToken alive = makeAlive();
    std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<float>> progress = std::make_shared<std::atomic<float>>(0.f);
    brls::Label* status = nullptr;
    brls::Button* closeBtn = nullptr;
    bool running = false;
};
