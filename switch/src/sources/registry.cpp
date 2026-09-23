#include "sources/registry.hpp"

namespace src {

static std::vector<std::shared_ptr<Source>> build() {
    std::vector<std::shared_ptr<Source>> list = {makeAnimeWorld(), makeAnimeUnity(), makeAnimeSaturn()};
    auto add = [&list](std::vector<std::shared_ptr<Source>> more) {
        for (auto& s : more)
            if (s) list.push_back(s);
    };
    add(makeAnikotoThemeSources());
    add({makeAnimePahe(), makeKickAssAnime()});
    add(makeWcoThemeSources());
    add(makeAnimeStreamSources());
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
