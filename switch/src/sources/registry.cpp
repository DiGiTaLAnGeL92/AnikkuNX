#include "sources/registry.hpp"

#include <set>

namespace src {

/**
 * Fonti portate ma nascoste perche' oggi non utilizzabili dalla console (verificato con sonda-fonti/testa-altre-lingue):
 * il codice resta, basta toglierle da qui quando il sito torna raggiungibile.
 */
/**
 * Fonti delle nuove lingue verificate dal vivo (lista, dettagli e almeno un video riproducibile, settembre 2026).
 * Le altre restano nel codice ma nascoste finche' non superano testa-altre-lingue.bat.
 */
static const std::set<std::string> VERIFIED = {
    "all.animexin",    "all.lmanime",    "sr.animebalkan", "es.jkanime",     "es.animeav1",       "es.latanime",
    "es.mundodonghua", "es.veranime",    "es.veranimes",   "es.beatzanime",  "es.pelisplusph",    "es.pelisplusto",
    "es.cineplus123",  "es.verpelistop", "pt.animesdigital", "fr.animesama", "fr.vostfree",       "de.aniworld",
    "de.animetoast",   "ar.asia2tv",     "ar.tuktukcinema", "ru.animevost",  "ru.animevost.mirror", "ru.yummyanime",
    "id.samehadaku",   "zh.xfani",       "zh.iyf",         "zh.nivod",       "zh.xiaoxintv",
};

static bool visible(const Source& s) {
    if (s.id().find("streamingcommunity") != std::string::npos) return false;  // non ancora provata
    if (s.lang() == "it" || s.lang() == "en") return true;
    return VERIFIED.count(s.id()) > 0;
}

static const std::set<std::string> DISABLED = {
    // verifica Cloudflare "Just a moment" / "Attention Required" (serve un browser)
    "tr.tranimeci", "pt.smartanimes", "es.animelatinohd", "es.katanime", "pt.animefire", "pt.anitube", "pt.goyabu",
    "pt.sushianimes", "pt.dattebayobr", "fr.frenchanime", "ar.anime4up", "ar.animerco", "ar.animeblkom",
    "ar.arabanime", "tr.anizm", "tr.tranimeizle", "pl.ogladajanime", "id.otakudesu", "id.kuramanime", "id.kuronime",
    "all.animeonsen", "pt.animeq", "pt.animesdrive", "pt.animesonlinecc", "pt.animesonlinecloud",
    "es.animeonlineninja",
    // sito chiuso, in manutenzione o dominio scaduto (settembre 2026)
    "es.animeytes", "es.tiodonghua", "fr.mykdrama", "es.animeid", "es.lacartoons", "es.jkhentai", "pt.animesonlinevip",
    "pt.animesotaku", "pt.animescx", "pt.funanimetv", "pt.doramogo", "pt.animesgames", "fr.otakufr", "ar.animelek",
    "ar.animeiat", "ar.arabseed", "tr.animeler", "tr.hdfilmcehennemi", "id.nimegami", "hi.yomovies",
    "all.animeworldindia", "hi.animeworldindia", "pt.animeplay", "pt.animesroll", "fr.voircartoon", "tr.turkanime",
    "es.animebum", "pl.desuonline", "fr.animevostfr", "fr.empirestreaming", "fr.anisama", "fr.wiflix", "fr.hds",
    "de.kool", "de.movie4k", "de.animestream", "de.kinoking", "pt.animesgratis", "pt.betteranimeio", "id.oploverz",
    "id.neonime", "ar.witanime",
};

static std::vector<std::shared_ptr<Source>> build() {
    std::vector<std::shared_ptr<Source>> list = {makeAnimeWorld(), makeAnimeUnity(), makeAnimeSaturn()};
    auto add = [&list](std::vector<std::shared_ptr<Source>> more) {
        for (auto& s : more)
            if (s) list.push_back(s);
    };
    add(makeAnikotoThemeSources());
    // AnimePahe e' dietro la verifica Cloudflare "Just a moment...", che senza browser non si supera:
    // resta nel codice (animepahe.cpp) ma non viene mostrata finche' il sito non la toglie.
    add({makeKickAssAnime()});
    add(makeWcoThemeSources());
    {
        // ChineseAnime usa solo Rumble, ora dietro la verifica Cloudflare: nessun video raggiungibile
        std::vector<std::shared_ptr<Source>> as;
        for (auto& s : makeAnimeStreamSources())
            if (s && s->id() != "all.chineseanime") as.push_back(s);
        add(as);
    }
    add(makeAdultSources());
    // altre lingue
    add(makeSpanishSources());
    add(makePortugueseSources());
    add(makeFrenchGermanSources());
    add(makeArTrRuPlSources());
    add(makeAsiaOtherSources());
    add(makeDooplaySources());
    // id duplicati (piu' porting della stessa estensione): tiene il primo
    std::vector<std::shared_ptr<Source>> unique;
    for (auto& s : list) {
        bool dup = false;
        for (auto& u : unique)
            if (u->id() == s->id()) dup = true;
        if (!dup && !DISABLED.count(s->id()) && visible(*s)) unique.push_back(s);
    }
    return unique;
}

const std::vector<std::shared_ptr<Source>>& all() {
    static std::vector<std::shared_ptr<Source>> list = build();
    return list;
}

std::shared_ptr<Source> byId(const std::string& id) {
    for (auto& s : all())
        if (s->id() == id) return s;
    return nullptr;
}

}  // namespace src
