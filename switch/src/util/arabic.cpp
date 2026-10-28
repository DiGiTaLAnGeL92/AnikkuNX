#include "util/arabic.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

// forme: isolata, finale, iniziale, centrale (0 = non esiste)
struct Forms {
    uint32_t cp, iso, fin, ini, med;
};

const Forms kForms[] = {
    {0x0621, 0xFE80, 0, 0, 0},
    {0x0622, 0xFE81, 0xFE82, 0, 0},
    {0x0623, 0xFE83, 0xFE84, 0, 0},
    {0x0624, 0xFE85, 0xFE86, 0, 0},
    {0x0625, 0xFE87, 0xFE88, 0, 0},
    {0x0626, 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C},
    {0x0627, 0xFE8D, 0xFE8E, 0, 0},
    {0x0628, 0xFE8F, 0xFE90, 0xFE91, 0xFE92},
    {0x0629, 0xFE93, 0xFE94, 0, 0},
    {0x062A, 0xFE95, 0xFE96, 0xFE97, 0xFE98},
    {0x062B, 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C},
    {0x062C, 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0},
    {0x062D, 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4},
    {0x062E, 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8},
    {0x062F, 0xFEA9, 0xFEAA, 0, 0},
    {0x0630, 0xFEAB, 0xFEAC, 0, 0},
    {0x0631, 0xFEAD, 0xFEAE, 0, 0},
    {0x0632, 0xFEAF, 0xFEB0, 0, 0},
    {0x0633, 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4},
    {0x0634, 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8},
    {0x0635, 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC},
    {0x0636, 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0},
    {0x0637, 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4},
    {0x0638, 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8},
    {0x0639, 0xFEC9, 0xFECA, 0xFECB, 0xFECC},
    {0x063A, 0xFECD, 0xFECE, 0xFECF, 0xFED0},
    {0x0641, 0xFED1, 0xFED2, 0xFED3, 0xFED4},
    {0x0642, 0xFED5, 0xFED6, 0xFED7, 0xFED8},
    {0x0643, 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC},
    {0x0644, 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0},
    {0x0645, 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4},
    {0x0646, 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8},
    {0x0647, 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC},
    {0x0648, 0xFEED, 0xFEEE, 0, 0},
    {0x0649, 0xFEEF, 0xFEF0, 0, 0},
    {0x064A, 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4},
    // lettere persiane/urdu piu' comuni
    {0x067E, 0xFB56, 0xFB57, 0xFB58, 0xFB59},
    {0x0686, 0xFB7A, 0xFB7B, 0xFB7C, 0xFB7D},
    {0x0698, 0xFB8A, 0xFB8B, 0, 0},
    {0x06A9, 0xFB8E, 0xFB8F, 0xFB90, 0xFB91},
    {0x06AF, 0xFB92, 0xFB93, 0xFB94, 0xFB95},
    {0x06CC, 0xFBFC, 0xFBFD, 0xFBFE, 0xFBFF},
};

const Forms* findForms(uint32_t cp) {
    for (const auto& f : kForms)
        if (f.cp == cp) return &f;
    return nullptr;
}

bool isArabicChar(uint32_t c) {
    return (c >= 0x0600 && c <= 0x06FF) || (c >= 0x0750 && c <= 0x077F) || (c >= 0xFB50 && c <= 0xFDFF) ||
           (c >= 0xFE70 && c <= 0xFEFF);
}
bool isMark(uint32_t c) { return (c >= 0x064B && c <= 0x065F) || c == 0x0670 || (c >= 0x06D6 && c <= 0x06ED); }
bool joinsNext(uint32_t c) {  // si collega alla lettera che segue
    const Forms* f = findForms(c);
    return c == 0x0640 || (f && f->ini);
}
bool joinsPrev(uint32_t c) {  // si collega alla lettera che precede
    return c == 0x0640 || findForms(c) != nullptr;
}

std::vector<uint32_t> decode(const std::string& s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        uint32_t cp;
        int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; n = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; n = 3; }
        else if ((c >> 3) == 30) { cp = c & 0x07; n = 4; }
        else { cp = c; n = 1; }
        for (int k = 1; k < n && i + k < s.size(); k++) cp = (cp << 6) | (s[i + k] & 0x3F);
        out.push_back(cp);
        i += n;
    }
    return out;
}

void encode(std::string& s, uint32_t c) {
    if (c < 0x80) s += (char)c;
    else if (c < 0x800) { s += (char)(0xC0 | (c >> 6)); s += (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { s += (char)(0xE0 | (c >> 12)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
    else { s += (char)(0xF0 | (c >> 18)); s += (char)(0x80 | ((c >> 12) & 0x3F)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
}

// ---- 1. scelta della forma di ogni lettera (ordine logico)
std::vector<uint32_t> shape(const std::vector<uint32_t>& in) {
    std::vector<uint32_t> t;  // senza segni di vocalizzazione, che NanoVG non sa posizionare
    for (auto c : in)
        if (!isMark(c)) t.push_back(c);
    std::vector<uint32_t> out;
    for (size_t i = 0; i < t.size(); i++) {
        uint32_t c = t[i];
        bool prevJoin = i > 0 && joinsNext(t[i - 1]) && joinsPrev(c);
        // lam + alef: legatura obbligatoria
        if (c == 0x0644 && i + 1 < t.size()) {
            uint32_t n = t[i + 1], lig = 0, ligFin = 0;
            if (n == 0x0622) { lig = 0xFEF5; ligFin = 0xFEF6; }
            else if (n == 0x0623) { lig = 0xFEF7; ligFin = 0xFEF8; }
            else if (n == 0x0625) { lig = 0xFEF9; ligFin = 0xFEFA; }
            else if (n == 0x0627) { lig = 0xFEFB; ligFin = 0xFEFC; }
            if (lig) {
                out.push_back(prevJoin ? ligFin : lig);
                i++;
                continue;
            }
        }
        const Forms* f = findForms(c);
        if (!f) { out.push_back(c); continue; }
        bool nextJoin = f->ini && i + 1 < t.size() && joinsPrev(t[i + 1]);
        uint32_t g;
        if (prevJoin && nextJoin) g = f->med ? f->med : f->fin;
        else if (prevJoin) g = f->fin ? f->fin : f->iso;
        else if (nextJoin) g = f->ini ? f->ini : f->iso;
        else g = f->iso;
        out.push_back(g ? g : c);
    }
    return out;
}

// ---- 2. riordino visivo (bidi semplificato)
enum Dir { L, R, N };

Dir dirOf(uint32_t c) {
    if (isArabicChar(c)) return R;
    if (c >= '0' && c <= '9') return L;  // le cifre restano da sinistra a destra
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c < 0x600) || (c >= 0x0900 && c < 0x2000) || c >= 0x3000) return L;
    return N;
}

uint32_t mirror(uint32_t c) {
    switch (c) {
        case '(': return ')';
        case ')': return '(';
        case '[': return ']';
        case ']': return '[';
        case '{': return '}';
        case '}': return '{';
        case '<': return '>';
        case '>': return '<';
        default: return c;
    }
}

std::vector<uint32_t> reorder(const std::vector<uint32_t>& s) {
    std::vector<Dir> d(s.size());
    Dir base = L;
    bool found = false;
    for (size_t i = 0; i < s.size(); i++) {
        d[i] = dirOf(s[i]);
        if (!found && d[i] != N && !(s[i] >= '0' && s[i] <= '9')) { base = d[i]; found = true; }
    }
    // neutri: prendono la direzione dei vicini se coincide, altrimenti quella di base
    for (size_t i = 0; i < s.size(); i++) {
        if (d[i] != N) continue;
        size_t a = i, b = i;
        while (a > 0 && d[a - 1] == N) a--;
        while (b + 1 < s.size() && d[b + 1] == N) b++;
        Dir before = a > 0 ? d[a - 1] : base, after = b + 1 < s.size() ? d[b + 1] : base;
        Dir r = before == after ? before : base;
        for (size_t k = a; k <= b; k++) d[k] = r;
        i = b;
    }
    // tratti della stessa direzione
    struct Run { Dir d; std::vector<uint32_t> ch; };
    std::vector<Run> runs;
    for (size_t i = 0; i < s.size(); i++) {
        if (runs.empty() || runs.back().d != d[i]) runs.push_back({d[i], {}});
        runs.back().ch.push_back(s[i]);
    }
    for (auto& r : runs)
        if (r.d == R) {
            std::reverse(r.ch.begin(), r.ch.end());
            for (auto& c : r.ch) c = mirror(c);
        }
    if (base == R) std::reverse(runs.begin(), runs.end());
    std::vector<uint32_t> out;
    for (auto& r : runs) out.insert(out.end(), r.ch.begin(), r.ch.end());
    return out;
}

}  // namespace

std::string shapeArabic(const std::string& utf8) {
    // percorso veloce: niente arabo, niente da fare
    bool arabic = false;
    for (size_t i = 0; i + 1 < utf8.size(); i++) {
        unsigned char c = utf8[i];
        if (c == 0xD8 || c == 0xD9 || c == 0xDA || c == 0xDB || c == 0xDD || c == 0xEF) { arabic = true; break; }
    }
    if (!arabic) return utf8;
    std::vector<uint32_t> cps = decode(utf8);
    bool real = false;
    for (auto c : cps)
        if (isArabicChar(c)) { real = true; break; }
    if (!real) return utf8;
    // le righe vanno riordinate una alla volta
    std::string out;
    size_t start = 0;
    for (size_t i = 0; i <= cps.size(); i++) {
        if (i == cps.size() || cps[i] == '\n') {
            std::vector<uint32_t> line(cps.begin() + start, cps.begin() + i);
            for (auto c : reorder(shape(line))) encode(out, c);
            if (i < cps.size()) out += '\n';
            start = i + 1;
        }
    }
    return out;
}

std::string brls_shapeArabic(const std::string& utf8) { return shapeArabic(utf8); }
