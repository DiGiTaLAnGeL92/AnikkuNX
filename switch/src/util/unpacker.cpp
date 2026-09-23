#include "util/unpacker.hpp"

#include <cctype>
#include <cmath>
#include <vector>

namespace unpacker {

bool detect(const std::string& script) { return script.find("eval(function(p,a,c,k,e,") != std::string::npos; }

static int unbase(const std::string& word, int base) {
    if (base >= 2 && base <= 36) {
        try {
            size_t idx = 0;
            long v = std::stol(word, &idx, base);
            return idx == word.size() ? (int)v : 0;
        } catch (...) {
            return 0;
        }
    }
    static const std::string a52 = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOP";
    static const std::string a54 = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQR";
    static const std::string a62 = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const std::string a95 =
        " !\"#$%&\\'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
    const std::string& alpha = base > 62 ? a95 : base > 54 ? a62 : base > 52 ? a54 : a52;
    long v = 0;
    for (size_t i = 0; i < word.size(); i++) {
        char c = word[word.size() - 1 - i];
        auto p = alpha.find(c);
        v += (long)(std::pow((double)base, (double)i) * (p == std::string::npos ? 0 : (double)p));
    }
    return (int)v;
}

static std::vector<std::string> split(const std::string& s, char d) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == d) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

static bool isWordChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

// Estrae i blocchi }('payload',radix,count,'a|b|c'.split('|') senza std::regex
// (le regex di libstdc++ vanno in overflow dello stack su script lunghi).
static bool parseDigits(const std::string& s, size_t& i, int& out) {
    while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
    size_t st = i;
    while (i < s.size() && std::isdigit((unsigned char)s[i])) i++;
    if (i == st) return false;
    out = std::stoi(s.substr(st, i - st));
    while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
    return true;
}

std::string unpackAndCombine(const std::string& script) {
    std::string result;
    size_t pos = 0;
    while ((pos = script.find("eval(function(p,a,c,k,e,", pos)) != std::string::npos) {
        size_t open = script.find("}('", pos);
        if (open == std::string::npos) break;
        size_t pStart = open + 3;
        // fine del payload: la prima sequenza ',<numero>,<numero>,' dopo l'inizio
        size_t search = pStart, pEnd = std::string::npos;
        int radix = 0, count = 0;
        size_t symStart = 0;
        while ((search = script.find("',", search)) != std::string::npos) {
            size_t i = search + 2;
            if (parseDigits(script, i, radix) && i < script.size() && script[i] == ',') {
                i++;
                if (parseDigits(script, i, count) && i + 1 < script.size() && script[i] == ',') {
                    i++;
                    while (i < script.size() && std::isspace((unsigned char)script[i])) i++;
                    if (i < script.size() && script[i] == '\'') {
                        pEnd = search;
                        symStart = i + 1;
                        break;
                    }
                }
            }
            search += 2;
        }
        if (pEnd == std::string::npos) break;
        size_t symEnd = script.find("'.split('|')", symStart);
        if (symEnd == std::string::npos) break;
        pos = symEnd;
        std::string payload = script.substr(pStart, pEnd - pStart);
        auto symtab = split(script.substr(symStart, symEnd - symStart), '|');
        if ((int)symtab.size() != count) continue;
        std::string out;
        size_t i = 0;
        while (i < payload.size()) {
            if (isWordChar(payload[i]) && (i == 0 || !isWordChar(payload[i - 1]))) {
                size_t j = i;
                while (j < payload.size() && isWordChar(payload[j])) j++;
                std::string word = payload.substr(i, j - i);
                int idx = unbase(word, radix);
                if (idx >= 0 && idx < (int)symtab.size() && !symtab[idx].empty())
                    out += symtab[idx];
                else
                    out += word;
                i = j;
            } else {
                out += payload[i++];
            }
        }
        if (!result.empty()) result += " ";
        result += out;
    }
    return result;
}

}  // namespace unpacker
