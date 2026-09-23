#include "config.hpp"

#include "sources/registry.hpp"

#include <borealis.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <sys/stat.h>

std::string Config::configDir() const {
#ifdef __SWITCH__
    return "sdmc:/switch/AnikkuNX";
#else
    const char* home = getenv("HOME");
    return std::string(home ? home : ".") + "/.config/AnikkuNX";
#endif
}

static void makeDirs(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); i++) {
        cur += path[i];
        if ((path[i] == '/' && i > 0 && path[i - 1] != ':') || i == path.size() - 1) mkdir(cur.c_str(), 0777);
    }
}

void Config::load() {
    std::ifstream in(configDir() + "/config.json");
    if (!in) return;
    try {
        nlohmann::json j;
        in >> j;
        domains.clear();
        if (j.contains("domains") && j["domains"].is_object())
            for (auto& kv : j["domains"].items())
                if (kv.value().is_string()) domains[kv.key()] = kv.value().get<std::string>();
        enabledSources.clear();
        if (j.contains("enabledSources") && j["enabledSources"].is_array())
            for (auto& e : j["enabledSources"])
                if (e.is_string()) enabledSources.insert(e.get<std::string>());
        sourcesChosen = j.value("sourcesChosen", false);
        showNsfw = j.value("showNsfw", false);
        hardwareDecoding = j.value("hardwareDecoding", true);
        autoSkipOpening = j.value("autoSkipOpening", false);
        seekSeconds = j.value("seekSeconds", 10);
    } catch (const std::exception& e) {
        brls::Logger::error("config.json non valido: {}", e.what());
    }
}

void Config::save() {
    makeDirs(configDir());
    nlohmann::json j = {
        {"domains", domains},
        {"enabledSources", enabledSources},
        {"sourcesChosen", sourcesChosen},
        {"showNsfw", showNsfw},
        {"hardwareDecoding", hardwareDecoding},
        {"autoSkipOpening", autoSkipOpening},
        {"seekSeconds", seekSeconds},
    };
    std::ofstream out(configDir() + "/config.json");
    out << j.dump(2);
}

bool Config::isSourceEnabled(const std::string& id) const {
    if (!sourcesChosen) {
        // prima della scelta sono attive solo le italiane
        auto s = src::byId(id);
        return s && s->lang() == "it";
    }
    if (!enabledSources.count(id)) return false;
    auto s = src::byId(id);
    return s && (showNsfw || !s->nsfw());
}

void Config::applyDomains() {
    for (auto& s : src::all()) {
        auto it = domains.find(s->id());
        src::setBaseUrlOverride(s->id(), it == domains.end() ? "" : it->second);
    }
}
