#pragma once

#include <memory>
#include <string>

#include "sources/source.hpp"

namespace src {

std::shared_ptr<Source> makeAnimeWorld();
std::shared_ptr<Source> makeAnimeUnity();
std::shared_ptr<Source> makeAnimeSaturn();

// Fonti inglesi / multilingua (ogni funzione puo' restituire piu' siti dello stesso "tema")
std::vector<std::shared_ptr<Source>> makeAnikotoThemeSources();  // anikototheme.cpp
std::vector<std::shared_ptr<Source>> makeWcoThemeSources();      // wcotheme.cpp
std::vector<std::shared_ptr<Source>> makeAnimeStreamSources();   // animestream.cpp
std::shared_ptr<Source> makeAnimePahe();                         // animepahe.cpp
std::shared_ptr<Source> makeKickAssAnime();                      // kickassanime.cpp
std::vector<std::shared_ptr<Source>> makeAdultSources();         // adult.cpp (18+)

// Altre lingue
std::vector<std::shared_ptr<Source>> makeDooplaySources();       // dooplay.cpp (de, en, es, fr, pt)
std::vector<std::shared_ptr<Source>> makeSpanishSources();       // lang_es.cpp (+ tema pelisplus)
std::vector<std::shared_ptr<Source>> makePortugueseSources();    // lang_pt.cpp
std::vector<std::shared_ptr<Source>> makeFrenchGermanSources();  // lang_fr_de.cpp (+ tema datalifeengine)
std::vector<std::shared_ptr<Source>> makeArTrRuPlSources();      // lang_ar_tr_ru_pl.cpp
std::vector<std::shared_ptr<Source>> makeAsiaOtherSources();     // lang_asia_other.cpp (id, zh, ko, sr, uk, hi, all)

/** Imposta un dominio alternativo per una fonte (stringa vuota = predefinito). */
void setBaseUrlOverride(const std::string& id, const std::string& url);

}  // namespace src
