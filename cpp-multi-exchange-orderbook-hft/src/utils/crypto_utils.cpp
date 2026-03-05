/**
 * @file crypto_utils.cpp
 * @brief Implementation of cryptographic utilities
 *
 * Uses OpenSSL for cryptographic operations
 */

#include "utils/crypto_utils.hpp"
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/md5.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <stdexcept>

namespace hft {
namespace utils {

// Static member initialization
std::atomic<uint64_t> CryptoUtils::nonce_counter_{0};
thread_local std::mt19937_64 CryptoUtils::rng_{std::random_device{}()};

// ============================================================================
// HMAC Functions
// ============================================================================

HMAC256Hash CryptoUtils::hmac_sha256(std::string_view key, std::string_view message) {
    HMAC256Hash result;
    unsigned int len = 0;

    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(message.data()), message.size(),
         result.data(), &len);

    return result;
}

std::string CryptoUtils::hmac_sha256_hex(std::string_view key, std::string_view message) {
    auto hash = hmac_sha256(key, message);
    return to_hex(hash.data(), hash.size());
}

std::string CryptoUtils::hmac_sha256_base64(std::string_view key, std::string_view message) {
    auto hash = hmac_sha256(key, message);
    return base64_encode(hash.data(), hash.size());
}

std::string CryptoUtils::hmac_sha384_hex(std::string_view key, std::string_view message) {
    unsigned char result[48];
    unsigned int len = 0;

    HMAC(EVP_sha384(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(message.data()), message.size(),
         result, &len);

    return to_hex(result, len);
}

HMAC512Hash CryptoUtils::hmac_sha512(std::string_view key, std::string_view message) {
    HMAC512Hash result;
    unsigned int len = 0;

    HMAC(EVP_sha512(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(message.data()), message.size(),
         result.data(), &len);

    return result;
}

std::string CryptoUtils::hmac_sha512_hex(std::string_view key, std::string_view message) {
    auto hash = hmac_sha512(key, message);
    return to_hex(hash.data(), hash.size());
}

std::string CryptoUtils::hmac_sha512_base64(std::string_view key, std::string_view message) {
    auto hash = hmac_sha512(key, message);
    return base64_encode(hash.data(), hash.size());
}

// ============================================================================
// Hash Functions
// ============================================================================

SHA256Hash CryptoUtils::sha256(std::string_view data) {
    SHA256Hash result;
    SHA256(reinterpret_cast<const unsigned char*>(data.data()),
           data.size(), result.data());
    return result;
}

std::string CryptoUtils::sha256_hex(std::string_view data) {
    auto hash = sha256(data);
    return to_hex(hash.data(), hash.size());
}

SHA512Hash CryptoUtils::sha512(std::string_view data) {
    SHA512Hash result;
    SHA512(reinterpret_cast<const unsigned char*>(data.data()),
           data.size(), result.data());
    return result;
}

std::string CryptoUtils::sha512_hex(std::string_view data) {
    auto hash = sha512(data);
    return to_hex(hash.data(), hash.size());
}

std::string CryptoUtils::md5_hex(std::string_view data) {
    unsigned char result[MD5_DIGEST_LENGTH];
    MD5(reinterpret_cast<const unsigned char*>(data.data()),
        data.size(), result);
    return to_hex(result, MD5_DIGEST_LENGTH);
}

// ============================================================================
// Base64 Encoding/Decoding
// ============================================================================

std::string CryptoUtils::base64_encode(std::string_view data) {
    return base64_encode(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

std::string CryptoUtils::base64_encode(const uint8_t* data, size_t length) {
    static const char encoding_table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    size_t output_length = 4 * ((length + 2) / 3);
    std::string result;
    result.reserve(output_length);

    for (size_t i = 0; i < length; i += 3) {
        uint32_t triple = (data[i] << 16);
        if (i + 1 < length) triple |= (data[i + 1] << 8);
        if (i + 2 < length) triple |= data[i + 2];

        result += encoding_table[(triple >> 18) & 0x3F];
        result += encoding_table[(triple >> 12) & 0x3F];
        result += (i + 1 < length) ? encoding_table[(triple >> 6) & 0x3F] : '=';
        result += (i + 2 < length) ? encoding_table[triple & 0x3F] : '=';
    }

    return result;
}

std::optional<std::vector<uint8_t>> CryptoUtils::base64_decode(std::string_view encoded) {
    static const unsigned char decoding_table[256] = {
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,  62, 255, 255, 255,  63,
         52,  53,  54,  55,  56,  57,  58,  59,  60,  61, 255, 255, 255, 255, 255, 255,
        255,   0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,
         15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25, 255, 255, 255, 255, 255,
        255,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,
         41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255
    };

    if (encoded.empty()) {
        return std::vector<uint8_t>{};
    }

    size_t input_length = encoded.size();

    // Remove padding for length calculation
    size_t padding = 0;
    if (input_length >= 1 && encoded[input_length - 1] == '=') padding++;
    if (input_length >= 2 && encoded[input_length - 2] == '=') padding++;

    size_t output_length = (input_length / 4) * 3 - padding;
    std::vector<uint8_t> result(output_length);

    size_t j = 0;
    for (size_t i = 0; i < input_length; i += 4) {
        uint32_t sextet_a = encoded[i] == '=' ? 0 : decoding_table[static_cast<unsigned char>(encoded[i])];
        uint32_t sextet_b = encoded[i + 1] == '=' ? 0 : decoding_table[static_cast<unsigned char>(encoded[i + 1])];
        uint32_t sextet_c = encoded[i + 2] == '=' ? 0 : decoding_table[static_cast<unsigned char>(encoded[i + 2])];
        uint32_t sextet_d = encoded[i + 3] == '=' ? 0 : decoding_table[static_cast<unsigned char>(encoded[i + 3])];

        if (sextet_a == 255 || sextet_b == 255 || sextet_c == 255 || sextet_d == 255) {
            return std::nullopt;
        }

        uint32_t triple = (sextet_a << 18) | (sextet_b << 12) | (sextet_c << 6) | sextet_d;

        if (j < output_length) result[j++] = (triple >> 16) & 0xFF;
        if (j < output_length) result[j++] = (triple >> 8) & 0xFF;
        if (j < output_length) result[j++] = triple & 0xFF;
    }

    return result;
}

std::optional<std::string> CryptoUtils::base64_decode_string(std::string_view encoded) {
    auto decoded = base64_decode(encoded);
    if (!decoded) return std::nullopt;
    return std::string(decoded->begin(), decoded->end());
}

std::string CryptoUtils::base64_url_encode(std::string_view data) {
    std::string result = base64_encode(data);

    // Replace + with -, / with _, and remove padding
    for (char& c : result) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }

    // Remove padding
    while (!result.empty() && result.back() == '=') {
        result.pop_back();
    }

    return result;
}

std::optional<std::vector<uint8_t>> CryptoUtils::base64_url_decode(std::string_view encoded) {
    std::string standard(encoded);

    // Replace URL-safe characters with standard base64
    for (char& c : standard) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }

    // Add padding if necessary
    while (standard.size() % 4 != 0) {
        standard += '=';
    }

    return base64_decode(standard);
}

// ============================================================================
// URL Encoding
// ============================================================================

std::string CryptoUtils::url_encode(std::string_view str) {
    static const char* hex = "0123456789ABCDEF";
    std::string result;
    result.reserve(str.size() * 3);

    for (unsigned char c : str) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            result += c;
        } else {
            result += '%';
            result += hex[c >> 4];
            result += hex[c & 0x0F];
        }
    }

    return result;
}

std::string CryptoUtils::url_decode(std::string_view str) {
    std::string result;
    result.reserve(str.size());

    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '%' && i + 2 < str.size()) {
            int high = 0, low = 0;

            if (str[i + 1] >= '0' && str[i + 1] <= '9') high = str[i + 1] - '0';
            else if (str[i + 1] >= 'A' && str[i + 1] <= 'F') high = str[i + 1] - 'A' + 10;
            else if (str[i + 1] >= 'a' && str[i + 1] <= 'f') high = str[i + 1] - 'a' + 10;
            else { result += str[i]; continue; }

            if (str[i + 2] >= '0' && str[i + 2] <= '9') low = str[i + 2] - '0';
            else if (str[i + 2] >= 'A' && str[i + 2] <= 'F') low = str[i + 2] - 'A' + 10;
            else if (str[i + 2] >= 'a' && str[i + 2] <= 'f') low = str[i + 2] - 'a' + 10;
            else { result += str[i]; continue; }

            result += static_cast<char>((high << 4) | low);
            i += 2;
        } else if (str[i] == '+') {
            result += ' ';
        } else {
            result += str[i];
        }
    }

    return result;
}

std::string CryptoUtils::build_query_string(
    const std::vector<std::pair<std::string, std::string>>& params,
    bool sorted) {

    std::vector<std::pair<std::string, std::string>> sorted_params = params;

    if (sorted) {
        std::sort(sorted_params.begin(), sorted_params.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
    }

    std::string result;
    bool first = true;

    for (const auto& [key, value] : sorted_params) {
        if (!first) result += '&';
        first = false;

        result += url_encode(key);
        result += '=';
        result += url_encode(value);
    }

    return result;
}

// ============================================================================
// Nonce Generation
// ============================================================================

uint64_t CryptoUtils::generate_nonce() {
    return nonce_counter_.fetch_add(1, std::memory_order_relaxed);
}

std::string CryptoUtils::generate_nonce_string() {
    return std::to_string(generate_nonce());
}

uint64_t CryptoUtils::generate_timestamp_nonce_ms() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    return static_cast<uint64_t>(ms);
}

uint64_t CryptoUtils::generate_timestamp_nonce_us() {
    auto now = std::chrono::high_resolution_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();
    return static_cast<uint64_t>(us);
}

std::string CryptoUtils::generate_uuid() {
    std::uniform_int_distribution<uint64_t> dist;

    uint64_t ab = dist(rng_);
    uint64_t cd = dist(rng_);

    // Set version to 4 (random UUID)
    ab = (ab & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    // Set variant to RFC 4122
    cd = (cd & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    ss << std::setw(8) << ((ab >> 32) & 0xFFFFFFFF);
    ss << '-';
    ss << std::setw(4) << ((ab >> 16) & 0xFFFF);
    ss << '-';
    ss << std::setw(4) << (ab & 0xFFFF);
    ss << '-';
    ss << std::setw(4) << ((cd >> 48) & 0xFFFF);
    ss << '-';
    ss << std::setw(12) << (cd & 0xFFFFFFFFFFFFULL);

    return ss.str();
}

std::string CryptoUtils::generate_random_hex(size_t length) {
    std::vector<uint8_t> bytes = generate_random_bytes((length + 1) / 2);
    std::string result = to_hex(bytes.data(), bytes.size());
    return result.substr(0, length);
}

std::vector<uint8_t> CryptoUtils::generate_random_bytes(size_t length) {
    std::vector<uint8_t> result(length);
    if (RAND_bytes(result.data(), static_cast<int>(length)) != 1) {
        // Fallback to std::mt19937 if OpenSSL fails
        std::uniform_int_distribution<int> dist(0, 255);
        for (size_t i = 0; i < length; ++i) {
            result[i] = static_cast<uint8_t>(dist(rng_));
        }
    }
    return result;
}

// ============================================================================
// AES Encryption
// ============================================================================

std::optional<std::vector<uint8_t>> CryptoUtils::aes_256_gcm_encrypt(
    std::string_view plaintext,
    std::string_view key) {

    if (key.size() != 32) {
        return std::nullopt;
    }

    // Generate random IV (12 bytes for GCM)
    std::vector<uint8_t> iv = generate_random_bytes(12);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return std::nullopt;

    std::vector<uint8_t> ciphertext(plaintext.size() + 16);  // Extra for padding
    std::array<uint8_t, 16> tag;
    int len = 0;
    int ciphertext_len = 0;

    bool success = true;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr,
            reinterpret_cast<const unsigned char*>(key.data()), iv.data()) != 1) {
        success = false;
    }

    if (success && EVP_EncryptUpdate(ctx, ciphertext.data(), &len,
            reinterpret_cast<const unsigned char*>(plaintext.data()),
            static_cast<int>(plaintext.size())) != 1) {
        success = false;
    }
    ciphertext_len = len;

    if (success && EVP_EncryptFinal_ex(ctx, ciphertext.data() + len, &len) != 1) {
        success = false;
    }
    ciphertext_len += len;

    if (success && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag.data()) != 1) {
        success = false;
    }

    EVP_CIPHER_CTX_free(ctx);

    if (!success) {
        return std::nullopt;
    }

    // Format: IV (12 bytes) + ciphertext + tag (16 bytes)
    std::vector<uint8_t> result;
    result.reserve(12 + ciphertext_len + 16);
    result.insert(result.end(), iv.begin(), iv.end());
    result.insert(result.end(), ciphertext.begin(), ciphertext.begin() + ciphertext_len);
    result.insert(result.end(), tag.begin(), tag.end());

    return result;
}

std::optional<std::string> CryptoUtils::aes_256_gcm_decrypt(
    const std::vector<uint8_t>& ciphertext,
    std::string_view key) {

    if (key.size() != 32 || ciphertext.size() < 28) {  // 12 (IV) + 16 (tag) minimum
        return std::nullopt;
    }

    // Extract IV, ciphertext, and tag
    const uint8_t* iv = ciphertext.data();
    size_t encrypted_len = ciphertext.size() - 12 - 16;
    const uint8_t* encrypted = ciphertext.data() + 12;
    const uint8_t* tag = ciphertext.data() + ciphertext.size() - 16;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return std::nullopt;

    std::vector<uint8_t> plaintext(encrypted_len);
    int len = 0;
    int plaintext_len = 0;

    bool success = true;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr,
            reinterpret_cast<const unsigned char*>(key.data()), iv) != 1) {
        success = false;
    }

    if (success && EVP_DecryptUpdate(ctx, plaintext.data(), &len,
            encrypted, static_cast<int>(encrypted_len)) != 1) {
        success = false;
    }
    plaintext_len = len;

    if (success && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16,
            const_cast<uint8_t*>(tag)) != 1) {
        success = false;
    }

    if (success && EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len) != 1) {
        success = false;
    }
    plaintext_len += len;

    EVP_CIPHER_CTX_free(ctx);

    if (!success) {
        return std::nullopt;
    }

    return std::string(plaintext.begin(), plaintext.begin() + plaintext_len);
}

std::array<uint8_t, 32> CryptoUtils::derive_key_pbkdf2(
    std::string_view password,
    std::string_view salt,
    uint32_t iterations) {

    std::array<uint8_t, 32> key;

    PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                      reinterpret_cast<const unsigned char*>(salt.data()),
                      static_cast<int>(salt.size()),
                      static_cast<int>(iterations),
                      EVP_sha256(),
                      32, key.data());

    return key;
}

// ============================================================================
// Hex Encoding
// ============================================================================

std::string CryptoUtils::to_hex(const uint8_t* data, size_t length) {
    static const char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(length * 2);

    for (size_t i = 0; i < length; ++i) {
        result += hex_chars[data[i] >> 4];
        result += hex_chars[data[i] & 0x0F];
    }

    return result;
}

std::string CryptoUtils::to_hex(std::string_view data) {
    return to_hex(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

std::optional<std::vector<uint8_t>> CryptoUtils::from_hex(std::string_view hex) {
    if (hex.size() % 2 != 0) {
        return std::nullopt;
    }

    std::vector<uint8_t> result;
    result.reserve(hex.size() / 2);

    for (size_t i = 0; i < hex.size(); i += 2) {
        int high = 0, low = 0;

        if (hex[i] >= '0' && hex[i] <= '9') high = hex[i] - '0';
        else if (hex[i] >= 'A' && hex[i] <= 'F') high = hex[i] - 'A' + 10;
        else if (hex[i] >= 'a' && hex[i] <= 'f') high = hex[i] - 'a' + 10;
        else return std::nullopt;

        if (hex[i + 1] >= '0' && hex[i + 1] <= '9') low = hex[i + 1] - '0';
        else if (hex[i + 1] >= 'A' && hex[i + 1] <= 'F') low = hex[i + 1] - 'A' + 10;
        else if (hex[i + 1] >= 'a' && hex[i + 1] <= 'f') low = hex[i + 1] - 'a' + 10;
        else return std::nullopt;

        result.push_back(static_cast<uint8_t>((high << 4) | low));
    }

    return result;
}

// ============================================================================
// Exchange-Specific Signing
// ============================================================================

std::string CryptoUtils::sign_binance(std::string_view params, std::string_view secret) {
    return hmac_sha256_hex(secret, params);
}

std::string CryptoUtils::sign_bybit(
    std::string_view timestamp,
    std::string_view api_key,
    std::string_view recv_window,
    std::string_view params,
    std::string_view secret) {

    std::string message;
    message.reserve(timestamp.size() + api_key.size() + recv_window.size() + params.size());
    message.append(timestamp);
    message.append(api_key);
    message.append(recv_window);
    message.append(params);

    return hmac_sha256_hex(secret, message);
}

std::string CryptoUtils::sign_okx(
    std::string_view timestamp,
    std::string_view method,
    std::string_view request_path,
    std::string_view body,
    std::string_view secret) {

    std::string message;
    message.reserve(timestamp.size() + method.size() + request_path.size() + body.size());
    message.append(timestamp);
    message.append(method);
    message.append(request_path);
    message.append(body);

    return hmac_sha256_base64(secret, message);
}

std::string CryptoUtils::sign_kraken(
    std::string_view path,
    uint64_t nonce,
    std::string_view post_data,
    std::string_view secret) {

    // Decode secret from base64
    auto decoded_secret = base64_decode(secret);
    if (!decoded_secret) {
        return "";
    }

    // Create the message: nonce + post_data
    std::string nonce_str = std::to_string(nonce);
    std::string message = nonce_str + std::string(post_data);

    // SHA256(nonce + post_data)
    auto sha256_hash = sha256(message);

    // path + SHA256(nonce + post_data)
    std::vector<uint8_t> hmac_input;
    hmac_input.reserve(path.size() + sha256_hash.size());
    hmac_input.insert(hmac_input.end(), path.begin(), path.end());
    hmac_input.insert(hmac_input.end(), sha256_hash.begin(), sha256_hash.end());

    // HMAC-SHA512 with decoded secret
    std::string hmac_key(decoded_secret->begin(), decoded_secret->end());
    std::string hmac_data(hmac_input.begin(), hmac_input.end());
    auto signature = hmac_sha512(hmac_key, hmac_data);

    return base64_encode(signature.data(), signature.size());
}

std::string CryptoUtils::sign_coinbase(
    std::string_view timestamp,
    std::string_view method,
    std::string_view request_path,
    std::string_view body,
    std::string_view secret) {

    std::string message;
    message.reserve(timestamp.size() + method.size() + request_path.size() + body.size());
    message.append(timestamp);
    message.append(method);
    message.append(request_path);
    message.append(body);

    return hmac_sha256_hex(secret, message);
}

std::string CryptoUtils::sign_deribit(
    std::string_view timestamp,
    std::string_view nonce,
    std::string_view data,
    std::string_view secret) {

    std::string message;
    message.reserve(timestamp.size() + 1 + nonce.size() + 1 + data.size());
    message.append(timestamp);
    message.append("\n");
    message.append(nonce);
    message.append("\n");
    message.append(data);

    return hmac_sha256_hex(secret, message);
}

} // namespace utils
} // namespace hft
