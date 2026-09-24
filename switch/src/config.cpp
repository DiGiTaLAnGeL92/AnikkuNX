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
        checkUpdates = j.value("checkUpdates", true);
        checkNewEpisodes = j.value("checkNewEpisodes", true);
        skippedVersion = j.value("skippedVersion", "");
        autoSkipOpening = j.value("autoSkipOpening", false);
        seekSeconds = j.value("seekSeconds", 10);
        subtitleFonts = j.value("subtitleFonts", true);
        hlsProxy = j.value("hlsProxy", true);
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
        {"checkUpdates", checkUpdates},
        {"checkNewEpisodes", checkNewEpisodes},
        {"skippedVersion", skippedVersion},
        {"autoSkipOpening", autoSkipOpening},
        {"seekSeconds", seekSeconds},
        {"subtitleFonts", subtitleFonts},
        {"hlsProxy", hlsProxy},
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

void Config::markPlaying(bool playing) {
    std::string path = configDir() + "/player.lock";
    if (playing) {
        std::ofstream(path) << "1";
    } else {
        remove(path.c_str());
    }
}

void Config::checkPreviousCrash() {
    std::string path = configDir() + "/player.lock";
    std::ifstream in(path);
    if (!in) return;
    in.close();
    remove(path.c_str());
    // l'app si e' chiusa durante la riproduzione: disattiva per prima cosa le novita' del player
    if (subtitleFonts) {
        subtitleFonts = false;
        crashNotice = "subs";
    } else if (hlsProxy) {
        hlsProxy = false;
        crashNotice = "proxy";
    }
    save();
}

