#pragma once

#include <string>

/**
 * Traduzione dell'interfaccia in base alla lingua di sistema della console.
 * I testi nel codice sono in italiano e fanno da chiave; le traduzioni stanno in
 * resources/lang/<codice>.json ({"testo italiano": "traduzione"}).
 * Lingue senza file (diverse dall'italiano) usano l'inglese.
 */
namespace i18n {

/** Da chiamare dopo brls::Application::init(). */
void init();
/** Codice della lingua attiva ("it", "en", "es", ...). */
const std::string& language();

}  // namespace i18n

/** Traduce un testo italiano nella lingua attiva. */
std::string tr(const std::string& italian);
/** Come tr(), sostituendo in ordine ogni "{}" con gli argomenti. */
std::string tr(const std::string& italian, const std::string& a1);
std::string tr(const std::string& italian, const std::string& a1, const std::string& a2);
