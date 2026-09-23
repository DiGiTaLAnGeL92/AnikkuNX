#pragma once

#include <string>

/**
 * Primitive crittografiche minime usate dalle fonti (porting di javax.crypto / MessageDigest).
 * Tutte lavorano su stringhe di byte grezzi.
 */
namespace crypto {

std::string md5(const std::string& data);
std::string sha1(const std::string& data);
std::string sha256(const std::string& data);
std::string hmacSha256(const std::string& key, const std::string& data);

/** AES-128/192/256 CBC. Con pkcs7=true rimuove/aggiunge il padding PKCS#5/7. */
std::string aesCbcDecrypt(const std::string& data, const std::string& key, const std::string& iv, bool pkcs7 = true);
std::string aesCbcEncrypt(const std::string& data, const std::string& key, const std::string& iv, bool pkcs7 = true);
std::string aesEcbDecrypt(const std::string& data, const std::string& key, bool pkcs7 = true);
/** AES CTR (cifra e decifra sono la stessa operazione). */
std::string aesCtr(const std::string& data, const std::string& key, const std::string& iv);

/** RC4 (cifra e decifra sono la stessa operazione). */
std::string rc4(const std::string& key, const std::string& data);

/** Formato OpenSSL/CryptoJS "Salted__" in base64 con passphrase (EVP_BytesToKey MD5, AES-256-CBC). */
std::string cryptoJsDecrypt(const std::string& base64Cipher, const std::string& passphrase);
/** EVP_BytesToKey con MD5: restituisce key(keyLen) + iv(ivLen) concatenati. */
std::string evpBytesToKey(const std::string& password, const std::string& salt, int keyLen = 32, int ivLen = 16);

std::string base64Encode(const std::string& data, bool urlSafe = false, bool padding = true);
std::string toHex(const std::string& data);
std::string fromHex(const std::string& hex);

}  // namespace crypto
