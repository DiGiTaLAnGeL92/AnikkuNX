#include "sources/registry.hpp"

namespace src {

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
    return list;
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
