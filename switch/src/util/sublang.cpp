#include "util/sublang.hpp"

#include <algorithm>
#include <cctype>
#include <map>

#include <borealis.hpp>

#include "config.hpp"

namespace sublang {

const std::vector<std::string>& choices() {
    static const std::vector<std::string> list = {"auto", "it", "en", "es", "fr", "de", "pt", "ru", "ja", "ko",
                                                  "zh",   "ar", "hi", "id", "tr", "pl", "nl", "off"};
    return list;
}

std::string preferred() {
    std::string c = Config::instance().subtitleLang;
    if (c.empty() || c == "auto") {
        c = brls::Application::getPlatform()->getLocale().substr(0, 2);
        std::transform(c.begin(), c.end(), c.begin(), [](unsigned char ch) { return std::tolower(ch); });
    }
    return c;
}

std::vector<std::string> keys(const std::string& code) {
    static const std::map<std::string, std::vector<std::string>> names = {
        {"it", {"it", "ita", "italian", "italiano"}},
        {"en", {"en", "eng", "english", "inglese"}},
        {"es", {"es", "spa", "spanish", "español", "espanol", "castellano", "latino"}},
        {"fr", {"fr", "fre", "fra", "french", "français", "francais"}},
        {"de", {"de", "ger", "deu", "german", "deutsch"}},
        {"pt", {"pt", "por", "portuguese", "português", "portugues"}},
        {"ru", {"ru", "rus", "russian", "русский"}},
        {"ja", {"ja", "jpn", "japanese", "日本語"}},
        {"ko", {"ko", "kor", "korean", "한국어"}},
        {"zh", {"zh", "chi", "zho", "chinese", "中文"}},
        {"ar", {"ar", "ara", "arabic", "العربية"}},
        {"hi", {"hi", "hin", "hindi", "हिन्दी"}},
        {"id", {"id", "ind", "indonesian", "indonesia"}},
        {"tr", {"tr", "tur", "turkish", "türkçe"}},
        {"pl", {"pl", "pol", "polish", "polski"}},
        {"nl", {"nl", "dut", "nld", "dutch", "nederlands"}},
    };
    auto it = names.find(code);
    if (it != names.end()) return it->second;
    return {code};
}

bool matches(const std::string& trackLang, const std::string& code) {
    std::string l = trackLang;
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return std::tolower(c); });
    for (auto& k : keys(code))
        if (l == k || l.rfind(k + "-", 0) == 0 || l.rfind(k + "_", 0) == 0 || l.rfind(k + " ", 0) == 0 ||
            (k.size() > 3 && l.find(k) != std::string::npos))
            return true;
    return false;
}

std::string mpvSlang() {
    std::string pref = preferred();
    if (pref == "off") return "";
    std::string out;
    for (const std::string& code : {pref, std::string("en")})
        for (auto& k : keys(code)) {
            if (k.find_first_of(" ,") != std::string::npos) continue;
            if (!out.empty()) out += ",";
            out += k;
        }
    return out;
}

}  // namespace sublang
