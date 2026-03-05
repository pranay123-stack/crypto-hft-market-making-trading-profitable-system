#pragma once

/**
 * @file crypto_utils.hpp
 * @brief Cryptographic utilities for API signing and data security
 *
 * This module provides cryptographic functions essential for exchange API authentication.
 * Features:
 * - HMAC-SHA256 for API signing
 * - SHA256, SHA512 hashing
 * - Base64 encoding/decoding
 * - URL encoding
 * - Nonce generation
 * - AES encryption/decryption for credential storage
 *
 * @author HFT System
 * @version 1.0
 */

#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <cstdint>
#include <optional>
#include <chrono>
#include <atomic>
#include <random>

namespace hft {
namespace utils {

/**
 * @brief Hash output types
 */
using SHA256Hash = std::array<uint8_t, 32>;
using SHA512Hash = std::array<uint8_t, 64>;
using HMAC256Hash = std::array<uint8_t, 32>;
using HMAC512Hash = std::array<uint8_t, 64>;

/**
 * @brief Cryptographic utilities class
 *
 * Provides all cryptographic functions needed for exchange API authentication
 * and secure credential storage. Uses OpenSSL internally.
 */
class CryptoUtils {
public:
    // ========================================================================
    // HMAC Functions (for API signing)
    // ========================================================================

    /**
     * @brief Compute HMAC-SHA256
     * @param key Secret key
     * @param message Message to sign
     * @return HMAC-SHA256 hash as raw bytes
     */
    static HMAC256Hash hmac_sha256(std::string_view key, std::string_view message);

    /**
     * @brief Compute HMAC-SHA256 and return as hex string
     * @param key Secret key
     * @param message Message to sign
     * @return HMAC-SHA256 hash as lowercase hex string
     */
    static std::string hmac_sha256_hex(std::string_view key, std::string_view message);

    /**
     * @brief Compute HMAC-SHA256 and return as base64 string
     * @param key Secret key
     * @param message Message to sign
     * @return HMAC-SHA256 hash as base64 string
     */
    static std::string hmac_sha256_base64(std::string_view key, std::string_view message);

    /**
     * @brief Compute HMAC-SHA384
     * @param key Secret key
     * @param message Message to sign
     * @return HMAC-SHA384 hash as hex string
     */
    static std::string hmac_sha384_hex(std::string_view key, std::string_view message);

    /**
     * @brief Compute HMAC-SHA512
     * @param key Secret key
     * @param message Message to sign
     * @return HMAC-SHA512 hash as raw bytes
     */
    static HMAC512Hash hmac_sha512(std::string_view key, std::string_view message);

    /**
     * @brief Compute HMAC-SHA512 and return as hex string
     * @param key Secret key
     * @param message Message to sign
     * @return HMAC-SHA512 hash as lowercase hex string
     */
    static std::string hmac_sha512_hex(std::string_view key, std::string_view message);

    /**
     * @brief Compute HMAC-SHA512 and return as base64 string
     * @param key Secret key
     * @param message Message to sign
     * @return HMAC-SHA512 hash as base64 string
     */
    static std::string hmac_sha512_base64(std::string_view key, std::string_view message);

    // ========================================================================
    // Hash Functions
    // ========================================================================

    /**
     * @brief Compute SHA256 hash
     * @param data Data to hash
     * @return SHA256 hash as raw bytes
     */
    static SHA256Hash sha256(std::string_view data);

    /**
     * @brief Compute SHA256 hash and return as hex string
     * @param data Data to hash
     * @return SHA256 hash as lowercase hex string
     */
    static std::string sha256_hex(std::string_view data);

    /**
     * @brief Compute SHA512 hash
     * @param data Data to hash
     * @return SHA512 hash as raw bytes
     */
    static SHA512Hash sha512(std::string_view data);

    /**
     * @brief Compute SHA512 hash and return as hex string
     * @param data Data to hash
     * @return SHA512 hash as lowercase hex string
     */
    static std::string sha512_hex(std::string_view data);

    /**
     * @brief Compute MD5 hash (for legacy APIs only)
     * @param data Data to hash
     * @return MD5 hash as lowercase hex string
     */
    static std::string md5_hex(std::string_view data);

    // ========================================================================
    // Base64 Encoding/Decoding
    // ========================================================================

    /**
     * @brief Encode data to base64
     * @param data Raw data to encode
     * @return Base64 encoded string
     */
    static std::string base64_encode(std::string_view data);

    /**
     * @brief Encode binary data to base64
     * @param data Pointer to raw data
     * @param length Length of data
     * @return Base64 encoded string
     */
    static std::string base64_encode(const uint8_t* data, size_t length);

    /**
     * @brief Decode base64 to raw data
     * @param encoded Base64 encoded string
     * @return Decoded data, or nullopt if invalid
     */
    static std::optional<std::vector<uint8_t>> base64_decode(std::string_view encoded);

    /**
     * @brief Decode base64 to string
     * @param encoded Base64 encoded string
     * @return Decoded string, or nullopt if invalid
     */
    static std::optional<std::string> base64_decode_string(std::string_view encoded);

    /**
     * @brief Encode to URL-safe base64 (RFC 4648)
     * @param data Raw data to encode
     * @return URL-safe base64 encoded string
     */
    static std::string base64_url_encode(std::string_view data);

    /**
     * @brief Decode URL-safe base64
     * @param encoded URL-safe base64 encoded string
     * @return Decoded data, or nullopt if invalid
     */
    static std::optional<std::vector<uint8_t>> base64_url_decode(std::string_view encoded);

    // ========================================================================
    // URL Encoding
    // ========================================================================

    /**
     * @brief URL encode a string (percent encoding)
     * @param str String to encode
     * @return URL encoded string
     */
    static std::string url_encode(std::string_view str);

    /**
     * @brief URL decode a string
     * @param str URL encoded string
     * @return Decoded string
     */
    static std::string url_decode(std::string_view str);

    /**
     * @brief Build URL query string from parameters
     * @param params Map of parameter name to value
     * @param sorted Whether to sort parameters alphabetically
     * @return URL encoded query string
     */
    static std::string build_query_string(
        const std::vector<std::pair<std::string, std::string>>& params,
        bool sorted = false);

    // ========================================================================
    // Nonce Generation
    // ========================================================================

    /**
     * @brief Generate a nonce (number used once)
     * @return Unique nonce value (monotonically increasing)
     */
    static uint64_t generate_nonce();

    /**
     * @brief Generate a nonce as string
     * @return Unique nonce value as string
     */
    static std::string generate_nonce_string();

    /**
     * @brief Generate a timestamp-based nonce
     * @return Current timestamp in milliseconds
     */
    static uint64_t generate_timestamp_nonce_ms();

    /**
     * @brief Generate a timestamp-based nonce in microseconds
     * @return Current timestamp in microseconds
     */
    static uint64_t generate_timestamp_nonce_us();

    /**
     * @brief Generate a UUID v4
     * @return Random UUID string
     */
    static std::string generate_uuid();

    /**
     * @brief Generate a random hex string
     * @param length Length of output string (must be even)
     * @return Random hex string
     */
    static std::string generate_random_hex(size_t length);

    /**
     * @brief Generate cryptographically secure random bytes
     * @param length Number of bytes to generate
     * @return Random bytes
     */
    static std::vector<uint8_t> generate_random_bytes(size_t length);

    // ========================================================================
    // AES Encryption (for credential storage)
    // ========================================================================

    /**
     * @brief Encrypt data using AES-256-GCM
     * @param plaintext Data to encrypt
     * @param key 256-bit encryption key
     * @return Encrypted data with IV and auth tag, or nullopt on failure
     */
    static std::optional<std::vector<uint8_t>> aes_256_gcm_encrypt(
        std::string_view plaintext,
        std::string_view key);

    /**
     * @brief Decrypt AES-256-GCM encrypted data
     * @param ciphertext Encrypted data with IV and auth tag
     * @param key 256-bit decryption key
     * @return Decrypted data, or nullopt on failure
     */
    static std::optional<std::string> aes_256_gcm_decrypt(
        const std::vector<uint8_t>& ciphertext,
        std::string_view key);

    /**
     * @brief Derive encryption key from password using PBKDF2
     * @param password User password
     * @param salt Salt value
     * @param iterations Number of iterations (default 100000)
     * @return 256-bit derived key
     */
    static std::array<uint8_t, 32> derive_key_pbkdf2(
        std::string_view password,
        std::string_view salt,
        uint32_t iterations = 100000);

    // ========================================================================
    // Hex Encoding
    // ========================================================================

    /**
     * @brief Encode binary data as hex string
     * @param data Binary data
     * @return Lowercase hex string
     */
    static std::string to_hex(const uint8_t* data, size_t length);

    /**
     * @brief Encode binary data as hex string
     * @param data Binary data
     * @return Lowercase hex string
     */
    static std::string to_hex(std::string_view data);

    /**
     * @brief Decode hex string to binary data
     * @param hex Hex string (must have even length)
     * @return Binary data, or nullopt if invalid
     */
    static std::optional<std::vector<uint8_t>> from_hex(std::string_view hex);

    // ========================================================================
    // Exchange-Specific Signing
    // ========================================================================

    /**
     * @brief Sign a request for Binance API
     * @param params Query parameters
     * @param secret API secret
     * @return Signature string
     */
    static std::string sign_binance(std::string_view params, std::string_view secret);

    /**
     * @brief Sign a request for Bybit API
     * @param timestamp Timestamp string
     * @param api_key API key
     * @param recv_window Receive window
     * @param params Query or body parameters
     * @param secret API secret
     * @return Signature string
     */
    static std::string sign_bybit(
        std::string_view timestamp,
        std::string_view api_key,
        std::string_view recv_window,
        std::string_view params,
        std::string_view secret);

    /**
     * @brief Sign a request for OKX API
     * @param timestamp ISO timestamp
     * @param method HTTP method
     * @param request_path Request path
     * @param body Request body
     * @param secret API secret
     * @return Base64 encoded signature
     */
    static std::string sign_okx(
        std::string_view timestamp,
        std::string_view method,
        std::string_view request_path,
        std::string_view body,
        std::string_view secret);

    /**
     * @brief Sign a request for Kraken API
     * @param path API path
     * @param nonce Nonce value
     * @param post_data POST data
     * @param secret Base64 encoded secret
     * @return Base64 encoded signature
     */
    static std::string sign_kraken(
        std::string_view path,
        uint64_t nonce,
        std::string_view post_data,
        std::string_view secret);

    /**
     * @brief Sign a request for Coinbase Advanced Trade API
     * @param timestamp Unix timestamp in seconds
     * @param method HTTP method
     * @param request_path Request path
     * @param body Request body
     * @param secret API secret
     * @return Signature string
     */
    static std::string sign_coinbase(
        std::string_view timestamp,
        std::string_view method,
        std::string_view request_path,
        std::string_view body,
        std::string_view secret);

    /**
     * @brief Sign a request for Deribit API
     * @param timestamp Timestamp string
     * @param nonce Nonce
     * @param data Request data
     * @param secret API secret
     * @return Signature string
     */
    static std::string sign_deribit(
        std::string_view timestamp,
        std::string_view nonce,
        std::string_view data,
        std::string_view secret);

private:
    // Thread-safe nonce counter
    static std::atomic<uint64_t> nonce_counter_;

    // Random number generator for UUIDs
    static thread_local std::mt19937_64 rng_;
};

} // namespace utils
} // namespace hft
