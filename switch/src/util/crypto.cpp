#include "util/crypto.hpp"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "sources/source.hpp"

namespace crypto {

// ============================================================================ MD5
namespace {

inline uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
inline uint32_t ror(uint32_t x, int c) { return (x >> c) | (x << (32 - c)); }

}  // namespace

std::string md5(const std::string& data) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                              14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe, h3 = 0x10325476;
    std::string msg = data;
    uint64_t bitLen = (uint64_t)data.size() * 8;
    msg += (char)0x80;
    while (msg.size() % 64 != 56) msg += (char)0;
    for (int i = 0; i < 8; i++) msg += (char)((bitLen >> (8 * i)) & 0xff);
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[16];
        for (int i = 0; i < 16; i++) {
            const unsigned char* p = (const unsigned char*)msg.data() + off + i * 4;
            w[i] = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
        }
        uint32_t a = h0, b = h1, c = h2, d = h3;
        for (int i = 0; i < 64; i++) {
            uint32_t f;
            int g;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
            }
            uint32_t tmp = d;
            d = c;
            c = b;
            b = b + rol(a + f + K[i] + w[g], R[i]);
            a = tmp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
    }
    std::string out;
    for (uint32_t h : {h0, h1, h2, h3})
        for (int i = 0; i < 4; i++) out += (char)((h >> (8 * i)) & 0xff);
    return out;
}

// ============================================================================ SHA-1 / SHA-256
static std::string padBigEndian(const std::string& data) {
    std::string msg = data;
    uint64_t bitLen = (uint64_t)data.size() * 8;
    msg += (char)0x80;
    while (msg.size() % 64 != 56) msg += (char)0;
    for (int i = 7; i >= 0; i--) msg += (char)((bitLen >> (8 * i)) & 0xff);
    return msg;
}

static uint32_t be32(const unsigned char* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

std::string sha1(const std::string& data) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string msg = padBigEndian(data);
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = be32((const unsigned char*)msg.data() + off + i * 4);
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    std::string out;
    for (uint32_t v : h)
        for (int i = 3; i >= 0; i--) out += (char)((v >> (8 * i)) & 0xff);
    return out;
}

std::string sha256(const std::string& data) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string msg = padBigEndian(data);
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = be32((const unsigned char*)msg.data() + off + i * 4);
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
    std::string out;
    for (uint32_t v : h)
        for (int i = 3; i >= 0; i--) out += (char)((v >> (8 * i)) & 0xff);
    return out;
}

std::string hmacSha256(const std::string& key, const std::string& data) {
    std::string k = key.size() > 64 ? sha256(key) : key;
    k.resize(64, '\0');
    std::string ipad(64, 0), opad(64, 0);
    for (int i = 0; i < 64; i++) {
        ipad[i] = (char)(k[i] ^ 0x36);
        opad[i] = (char)(k[i] ^ 0x5c);
    }
    return sha256(opad + sha256(ipad + data));
}

// ============================================================================ AES
namespace {

const uint8_t SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9,
    0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f,
    0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07,
    0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3,
    0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58,
    0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3,
    0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f,
    0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88,
    0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac,
    0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a,
    0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70,
    0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, 0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42,
    0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

uint8_t INV_SBOX[256];
bool invInit = false;

inline uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
inline uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    while (b) {
        if (b & 1) p ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return p;
}

struct Aes {
    int rounds;
    uint8_t rk[240];

    explicit Aes(const std::string& key) {
        if (!invInit) {
            for (int i = 0; i < 256; i++) INV_SBOX[SBOX[i]] = (uint8_t)i;
            invInit = true;
        }
        int nk = (int)key.size() / 4;
        if (key.size() != 16 && key.size() != 24 && key.size() != 32) throw std::runtime_error("chiave AES non valida");
        rounds = nk + 6;
        int total = 4 * (rounds + 1);
        memcpy(rk, key.data(), key.size());
        uint8_t rcon = 1;
        for (int i = nk; i < total; i++) {
            uint8_t t[4];
            memcpy(t, rk + (i - 1) * 4, 4);
            if (i % nk == 0) {
                uint8_t u = t[0];
                t[0] = SBOX[t[1]] ^ rcon;
                t[1] = SBOX[t[2]];
                t[2] = SBOX[t[3]];
                t[3] = SBOX[u];
                rcon = xtime(rcon);
            } else if (nk > 6 && i % nk == 4) {
                for (auto& b : t) b = SBOX[b];
            }
            for (int j = 0; j < 4; j++) rk[i * 4 + j] = rk[(i - nk) * 4 + j] ^ t[j];
        }
    }

    void addRoundKey(uint8_t* s, int r) const {
        for (int i = 0; i < 16; i++) s[i] ^= rk[r * 16 + i];
    }

    void encryptBlock(uint8_t* s) const {
        addRoundKey(s, 0);
        for (int r = 1; r <= rounds; r++) {
            for (int i = 0; i < 16; i++) s[i] = SBOX[s[i]];
            uint8_t t[16];
            for (int c = 0; c < 4; c++)
                for (int row = 0; row < 4; row++) t[c * 4 + row] = s[((c + row) % 4) * 4 + row];
            memcpy(s, t, 16);
            if (r != rounds) {
                for (int c = 0; c < 4; c++) {
                    uint8_t* col = s + c * 4;
                    uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                    col[0] = xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3;
                    col[1] = a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3;
                    col[2] = a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3);
                    col[3] = (xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3);
                }
            }
            addRoundKey(s, r);
        }
    }

    void decryptBlock(uint8_t* s) const {
        addRoundKey(s, rounds);
        for (int r = rounds - 1; r >= 0; r--) {
            uint8_t t[16];
            for (int c = 0; c < 4; c++)
                for (int row = 0; row < 4; row++) t[((c + row) % 4) * 4 + row] = s[c * 4 + row];
            for (int i = 0; i < 16; i++) s[i] = INV_SBOX[t[i]];
            addRoundKey(s, r);
            if (r != 0) {
                for (int c = 0; c < 4; c++) {
                    uint8_t* col = s + c * 4;
                    uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                    col[0] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
                    col[1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
                    col[2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
                    col[3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
                }
            }
        }
    }
};

std::string unpad(std::string s) {
    if (s.empty()) return s;
    unsigned char n = (unsigned char)s.back();
    if (n == 0 || n > 16 || n > s.size()) throw std::runtime_error("padding AES non valido");
    s.resize(s.size() - n);
    return s;
}

}  // namespace

std::string aesCbcDecrypt(const std::string& data, const std::string& key, const std::string& iv, bool pkcs7) {
    if (data.size() % 16 != 0) throw std::runtime_error("dati AES di lunghezza non valida");
    Aes aes(key);
    std::string out(data.size(), '\0');
    uint8_t prev[16] = {0};
    memcpy(prev, iv.data(), std::min<size_t>(16, iv.size()));
    for (size_t off = 0; off < data.size(); off += 16) {
        uint8_t block[16];
        memcpy(block, data.data() + off, 16);
        uint8_t cipher[16];
        memcpy(cipher, block, 16);
        aes.decryptBlock(block);
        for (int i = 0; i < 16; i++) out[off + i] = (char)(block[i] ^ prev[i]);
        memcpy(prev, cipher, 16);
    }
    return pkcs7 ? unpad(out) : out;
}

std::string aesCbcEncrypt(const std::string& data, const std::string& key, const std::string& iv, bool pkcs7) {
    std::string in = data;
    if (pkcs7) {
        size_t n = 16 - in.size() % 16;
        in.append(n, (char)n);
    } else if (in.size() % 16 != 0) {
        in.append(16 - in.size() % 16, '\0');
    }
    Aes aes(key);
    std::string out(in.size(), '\0');
    uint8_t prev[16] = {0};
    memcpy(prev, iv.data(), std::min<size_t>(16, iv.size()));
    for (size_t off = 0; off < in.size(); off += 16) {
        uint8_t block[16];
        for (int i = 0; i < 16; i++) block[i] = (uint8_t)in[off + i] ^ prev[i];
        aes.encryptBlock(block);
        memcpy(&out[off], block, 16);
        memcpy(prev, block, 16);
    }
    return out;
}

std::string aesEcbDecrypt(const std::string& data, const std::string& key, bool pkcs7) {
    if (data.size() % 16 != 0) throw std::runtime_error("dati AES di lunghezza non valida");
    Aes aes(key);
    std::string out = data;
    for (size_t off = 0; off < out.size(); off += 16) aes.decryptBlock((uint8_t*)&out[off]);
    return pkcs7 ? unpad(out) : out;
}

std::string aesCtr(const std::string& data, const std::string& key, const std::string& iv) {
    Aes aes(key);
    uint8_t counter[16] = {0};
    memcpy(counter, iv.data(), std::min<size_t>(16, iv.size()));
    std::string out = data;
    for (size_t off = 0; off < out.size(); off += 16) {
        uint8_t ks[16];
        memcpy(ks, counter, 16);
        aes.encryptBlock(ks);
        for (size_t i = 0; i < 16 && off + i < out.size(); i++) out[off + i] = (char)(out[off + i] ^ ks[i]);
        for (int i = 15; i >= 0; i--)
            if (++counter[i] != 0) break;
    }
    return out;
}

// ============================================================================ RC4
std::string rc4(const std::string& key, const std::string& data) {
    uint8_t S[256];
    for (int i = 0; i < 256; i++) S[i] = (uint8_t)i;
    if (!key.empty()) {
        int j = 0;
        for (int i = 0; i < 256; i++) {
            j = (j + S[i] + (uint8_t)key[i % key.size()]) & 0xff;
            std::swap(S[i], S[j]);
        }
    }
    std::string out = data;
    int i = 0, j = 0;
    for (auto& c : out) {
        i = (i + 1) & 0xff;
        j = (j + S[i]) & 0xff;
        std::swap(S[i], S[j]);
        c = (char)(c ^ S[(S[i] + S[j]) & 0xff]);
    }
    return out;
}

// ============================================================================ OpenSSL / CryptoJS
std::string evpBytesToKey(const std::string& password, const std::string& salt, int keyLen, int ivLen) {
    std::string out, prev;
    while ((int)out.size() < keyLen + ivLen) {
        prev = md5(prev + password + salt);
        out += prev;
    }
    return out.substr(0, keyLen + ivLen);
}

std::string cryptoJsDecrypt(const std::string& base64Cipher, const std::string& passphrase) {
    std::string raw = src::base64Decode(base64Cipher);
    if (raw.size() < 16 || raw.compare(0, 8, "Salted__") != 0) throw std::runtime_error("formato CryptoJS non valido");
    std::string salt = raw.substr(8, 8);
    std::string kiv = evpBytesToKey(passphrase, salt, 32, 16);
    return aesCbcDecrypt(raw.substr(16), kiv.substr(0, 32), kiv.substr(32, 16), true);
}

// ============================================================================ codifiche
std::string base64Encode(const std::string& data, bool urlSafe, bool padding) {
    const char* chars = urlSafe ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
                                : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -6;
    for (unsigned char c : data) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out += chars[(val >> bits) & 0x3F];
            bits -= 6;
        }
    }
    if (bits > -6) out += chars[((val << 8) >> (bits + 8)) & 0x3F];
    if (padding)
        while (out.size() % 4) out += '=';
    return out;
}

std::string toHex(const std::string& data) {
    static const char* h = "0123456789abcdef";
    std::string out;
    for (unsigned char c : data) {
        out += h[c >> 4];
        out += h[c & 15];
    }
    return out;
}

std::string fromHex(const std::string& hex) {
    auto v = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        int a = v(hex[i]), b = v(hex[i + 1]);
        if (a < 0 || b < 0) break;
        out += (char)((a << 4) | b);
    }
    return out;
}

}  // namespace crypto
