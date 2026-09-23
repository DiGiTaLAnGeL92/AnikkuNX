#include "util/i18n.hpp"

#include <borealis.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <unordered_map>

namespace {
std::string lang = "it";
std::unordered_map<std::string, std::string> table;

bool loadFile(const std::string& code) {
    std::ifstream in(std::string(BRLS_RESOURCES) + "lang/" + code + ".json");
    if (!in) return false;
    try {
        nlohmann::json j;
        in >> j;
        table.clear();
        for (auto& kv : j.items())
            if (kv.value().is_string()) table[kv.key()] = kv.value().get<std::string>();
        return true;
    } catch (const std::exception& e) {
        brls::Logger::error("Traduzione {} non valida: {}", code, e.what());
        return false;
    }
}

std::string fill(std::string s, const std::string& a) {
    auto p = s.find("{}");
    if (p != std::string::npos) s.replace(p, 2, a);
    return s;
}
}  // namespace

namespace i18n {

void init() {
    // es. "it", "en-US", "zh-Hans", "pt-BR"
    std::string locale = brls::Application::getLocale();
    std::string code = locale.substr(0, locale.find('-'));
    if (locale.rfind("zh", 0) == 0) code = (locale.find("Hant") != std::string::npos || locale.find("TW") != std::string::npos) ? "zh-Hant" : "zh-Hans";
    if (code.empty() || code == "it") {
        lang = "it";
        return;
    }
    if (loadFile(code)) {
        lang = code;
    } else if (loadFile("en")) {
        lang = "en";
    }
}

const std::string& language() { return lang; }

}  // namespace i18n

std::string tr(const std::string& italian) {
    if (lang == "it") return italian;
    auto it = table.find(italian);
    return it == table.end() ? italian : it->second;
}

std::string tr(const std::string& italian, const std::string& a1) { return fill(tr(italian), a1); }

std::string tr(const std::string& italian, const std::string& a1, const std::string& a2) {
    return fill(fill(tr(italian), a1), a2);
}
