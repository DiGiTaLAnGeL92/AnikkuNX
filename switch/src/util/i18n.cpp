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

std::string i18n::languageName(const std::string& l) {
    if (l == "it") return tr("Italiano");
    if (l == "en") return tr("Inglese");
    if (l == "all") return tr("Multilingua");
    if (l == "es") return tr("Spagnolo");
    if (l == "pt") return tr("Portoghese");
    if (l == "fr") return tr("Francese");
    if (l == "de") return tr("Tedesco");
    if (l == "ar") return tr("Arabo");
    if (l == "id") return tr("Indonesiano");
    if (l == "tr") return tr("Turco");
    if (l == "ru") return tr("Russo");
    if (l == "pl") return tr("Polacco");
    if (l == "zh") return tr("Cinese");
    if (l == "ko") return tr("Coreano");
    if (l == "sr") return tr("Serbo");
    if (l == "uk") return tr("Ucraino");
    if (l == "hi") return tr("Hindi");
    if (l == "ja") return tr("Giapponese");
    if (l == "nl") return tr("Olandese");
    return l;
}
