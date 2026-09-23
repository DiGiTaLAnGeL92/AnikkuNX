#include <cctype>
#include <cstdlib>
#include <map>
#include <mutex>

#include "sources/registry.hpp"
#include "sources/source.hpp"

namespace src {

static std::mutex overridesMutex;
static std::map<std::string, std::string> overrides;

void setBaseUrlOverride(const std::string& id, const std::string& url) {
    std::lock_guard<std::mutex> lock(overridesMutex);
    std::string u = trim(url);
    while (!u.empty() && u.back() == '/') u.pop_back();
    if (!u.empty() && u.find("://") == std::string::npos) u = "https://" + u;
    if (u.empty())
        overrides.erase(id);
    else
        overrides[id] = u;
}

std::string Source::baseUrl() const {
    std::lock_guard<std::mutex> lock(overridesMutex);
    auto it = overrides.find(id());
    return it != overrides.end() ? it->second : defaultBaseUrl();
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string substringAfter(const std::string& s, const std::string& delim) {
    auto p = s.find(delim);
    return p == std::string::npos ? s : s.substr(p + delim.size());
}

std::string substringBefore(const std::string& s, const std::string& delim) {
    auto p = s.find(delim);
    return p == std::string::npos ? s : s.substr(0, p);
}

std::string base64Decode(const std::string& in) {
    static int table[256];
    static bool init = false;
    if (!init) {
        for (int& t : table) t = -1;
        const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) table[(unsigned char)chars[i]] = i;
        table[(unsigned char)'-'] = 62;  // anche la variante URL-safe
        table[(unsigned char)'_'] = 63;
        init = true;
    }
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (table[c] < 0) continue;
        val = (val << 6) + table[c];
        bits += 6;
        if (bits >= 0) {
            out += (char)((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    return out;
}

std::string digitsOnly(const std::string& s) {
    std::string out;
    for (char c : s)
        if (std::isdigit((unsigned char)c)) out += c;
    return out;
}

double parseNumber(const std::string& s, double def) {
    std::string t = trim(s);
    if (t.empty()) return def;
    char* end = nullptr;
    double v = std::strtod(t.c_str(), &end);
    return end == t.c_str() ? def : v;
}

}  // namespace src
