#pragma once

/**
 * @file string_utils.hpp
 * @brief String manipulation utilities for HFT operations
 *
 * This module provides high-performance string utilities optimized for HFT.
 * Features:
 * - String splitting, joining
 * - Number formatting (price, quantity)
 * - JSON string escaping
 * - Symbol normalization (exchange-specific formats)
 *
 * @author HFT System
 * @version 1.0
 */

#include <string>
#include <string_view>
#include <vector>
#include <charconv>
#include <cmath>
#include <optional>
#include <array>
#include <unordered_map>

namespace hft {
namespace utils {

/**
 * @brief Exchange symbol format enumeration
 */
enum class SymbolFormat {
    UNIFIED,        // BTC/USDT
    BINANCE,        // BTCUSDT
    BYBIT,          // BTCUSDT
    OKX,            // BTC-USDT
    KRAKEN,         // XXBTZUSD, XBTUSDT
    COINBASE,       // BTC-USD
    KUCOIN,         // BTC-USDT
    GATEIO,         // BTC_USDT
    BITFINEX,       // tBTCUSD
    DERIBIT,        // BTC-PERPETUAL, BTC-25MAR22
    HTX             // btcusdt (lowercase)
};

/**
 * @brief High-performance string utilities class
 */
class StringUtils {
public:
    // ========================================================================
    // String Splitting
    // ========================================================================

    /**
     * @brief Split string by delimiter
     * @param str String to split
     * @param delimiter Delimiter character
     * @return Vector of string_views (no allocation)
     */
    static std::vector<std::string_view> split_view(std::string_view str, char delimiter);

    /**
     * @brief Split string by delimiter
     * @param str String to split
     * @param delimiter Delimiter character
     * @return Vector of strings
     */
    static std::vector<std::string> split(std::string_view str, char delimiter);

    /**
     * @brief Split string by string delimiter
     * @param str String to split
     * @param delimiter Delimiter string
     * @return Vector of strings
     */
    static std::vector<std::string> split(std::string_view str, std::string_view delimiter);

    /**
     * @brief Split string into exactly N parts
     * @param str String to split
     * @param delimiter Delimiter character
     * @param max_parts Maximum number of parts
     * @return Vector of strings (up to max_parts)
     */
    static std::vector<std::string> split_n(std::string_view str, char delimiter, size_t max_parts);

    // ========================================================================
    // String Joining
    // ========================================================================

    /**
     * @brief Join strings with delimiter
     * @param parts Vector of strings to join
     * @param delimiter Delimiter string
     * @return Joined string
     */
    static std::string join(const std::vector<std::string>& parts, std::string_view delimiter);

    /**
     * @brief Join string_views with delimiter
     * @param parts Vector of string_views to join
     * @param delimiter Delimiter string
     * @return Joined string
     */
    static std::string join(const std::vector<std::string_view>& parts, std::string_view delimiter);

    /**
     * @brief Join with a transform function
     * @tparam Container Container type
     * @tparam Transform Transform function type
     * @param container Container of items
     * @param delimiter Delimiter string
     * @param transform Function to transform each item to string
     * @return Joined string
     */
    template<typename Container, typename Transform>
    static std::string join_with(const Container& container, std::string_view delimiter,
                                  Transform transform);

    // ========================================================================
    // Number Formatting
    // ========================================================================

    /**
     * @brief Format price with specified precision
     * @param price Price value
     * @param precision Decimal precision
     * @return Formatted price string
     */
    static std::string format_price(double price, int precision = 8);

    /**
     * @brief Format quantity with specified precision
     * @param quantity Quantity value
     * @param precision Decimal precision
     * @return Formatted quantity string
     */
    static std::string format_quantity(double quantity, int precision = 8);

    /**
     * @brief Format number removing trailing zeros
     * @param value Number value
     * @param max_precision Maximum decimal precision
     * @return Formatted number string
     */
    static std::string format_decimal(double value, int max_precision = 8);

    /**
     * @brief Format number with thousands separator
     * @param value Number value
     * @param separator Thousands separator character
     * @return Formatted number string
     */
    static std::string format_with_thousands(double value, char separator = ',');

    /**
     * @brief Format percentage
     * @param value Value (0.01 = 1%)
     * @param precision Decimal precision
     * @return Formatted percentage string
     */
    static std::string format_percentage(double value, int precision = 2);

    /**
     * @brief Format basis points
     * @param value Value (0.0001 = 1bp)
     * @param precision Decimal precision
     * @return Formatted basis points string
     */
    static std::string format_bps(double value, int precision = 2);

    /**
     * @brief Fast double to string (low latency)
     * @param value Double value
     * @param buffer Output buffer (must be at least 32 bytes)
     * @return Pointer to end of written string
     */
    static char* fast_dtoa(double value, char* buffer, int precision = 8);

    /**
     * @brief Fast integer to string (low latency)
     * @param value Integer value
     * @param buffer Output buffer (must be at least 21 bytes for int64)
     * @return Pointer to end of written string
     */
    static char* fast_itoa(int64_t value, char* buffer);

    /**
     * @brief Fast unsigned integer to string (low latency)
     * @param value Unsigned integer value
     * @param buffer Output buffer (must be at least 20 bytes for uint64)
     * @return Pointer to end of written string
     */
    static char* fast_utoa(uint64_t value, char* buffer);

    // ========================================================================
    // Number Parsing
    // ========================================================================

    /**
     * @brief Parse string to double
     * @param str String to parse
     * @return Parsed value or nullopt
     */
    static std::optional<double> parse_double(std::string_view str);

    /**
     * @brief Parse string to int64
     * @param str String to parse
     * @return Parsed value or nullopt
     */
    static std::optional<int64_t> parse_int64(std::string_view str);

    /**
     * @brief Parse string to uint64
     * @param str String to parse
     * @return Parsed value or nullopt
     */
    static std::optional<uint64_t> parse_uint64(std::string_view str);

    /**
     * @brief Fast string to double (low latency)
     * @param str String to parse
     * @param result Output result
     * @return true if successful
     */
    static bool fast_atod(std::string_view str, double& result);

    /**
     * @brief Fast string to int64 (low latency)
     * @param str String to parse
     * @param result Output result
     * @return true if successful
     */
    static bool fast_atoi64(std::string_view str, int64_t& result);

    // ========================================================================
    // JSON String Handling
    // ========================================================================

    /**
     * @brief Escape string for JSON
     * @param str String to escape
     * @return JSON-escaped string
     */
    static std::string json_escape(std::string_view str);

    /**
     * @brief Unescape JSON string
     * @param str JSON-escaped string
     * @return Unescaped string
     */
    static std::string json_unescape(std::string_view str);

    /**
     * @brief Fast JSON escape to buffer
     * @param str String to escape
     * @param buffer Output buffer
     * @param buffer_size Buffer size
     * @return Length of escaped string, or -1 if buffer too small
     */
    static int fast_json_escape(std::string_view str, char* buffer, size_t buffer_size);

    // ========================================================================
    // Symbol Normalization
    // ========================================================================

    /**
     * @brief Normalize symbol to unified format (BASE/QUOTE)
     * @param symbol Exchange-specific symbol
     * @param format Source format
     * @return Normalized symbol
     */
    static std::string normalize_symbol(std::string_view symbol, SymbolFormat format);

    /**
     * @brief Convert unified symbol to exchange format
     * @param unified_symbol Unified format symbol (e.g., BTC/USDT)
     * @param format Target exchange format
     * @return Exchange-specific symbol
     */
    static std::string to_exchange_symbol(std::string_view unified_symbol, SymbolFormat format);

    /**
     * @brief Extract base asset from symbol
     * @param symbol Symbol in any format
     * @param format Symbol format
     * @return Base asset
     */
    static std::string extract_base_asset(std::string_view symbol, SymbolFormat format);

    /**
     * @brief Extract quote asset from symbol
     * @param symbol Symbol in any format
     * @param format Symbol format
     * @return Quote asset
     */
    static std::string extract_quote_asset(std::string_view symbol, SymbolFormat format);

    /**
     * @brief Check if symbol is futures/perpetual
     * @param symbol Symbol to check
     * @param format Symbol format
     * @return true if futures/perpetual
     */
    static bool is_futures_symbol(std::string_view symbol, SymbolFormat format);

    // ========================================================================
    // String Manipulation
    // ========================================================================

    /**
     * @brief Trim whitespace from both ends
     * @param str String to trim
     * @return Trimmed string_view
     */
    static std::string_view trim(std::string_view str);

    /**
     * @brief Trim whitespace from left
     * @param str String to trim
     * @return Trimmed string_view
     */
    static std::string_view ltrim(std::string_view str);

    /**
     * @brief Trim whitespace from right
     * @param str String to trim
     * @return Trimmed string_view
     */
    static std::string_view rtrim(std::string_view str);

    /**
     * @brief Convert to uppercase
     * @param str String to convert
     * @return Uppercase string
     */
    static std::string to_upper(std::string_view str);

    /**
     * @brief Convert to lowercase
     * @param str String to convert
     * @return Lowercase string
     */
    static std::string to_lower(std::string_view str);

    /**
     * @brief Check if string starts with prefix
     * @param str String to check
     * @param prefix Prefix to look for
     * @return true if starts with prefix
     */
    static bool starts_with(std::string_view str, std::string_view prefix);

    /**
     * @brief Check if string ends with suffix
     * @param str String to check
     * @param suffix Suffix to look for
     * @return true if ends with suffix
     */
    static bool ends_with(std::string_view str, std::string_view suffix);

    /**
     * @brief Check if string contains substring
     * @param str String to check
     * @param substr Substring to look for
     * @return true if contains substring
     */
    static bool contains(std::string_view str, std::string_view substr);

    /**
     * @brief Replace all occurrences of a substring
     * @param str Input string
     * @param from Substring to replace
     * @param to Replacement string
     * @return String with replacements
     */
    static std::string replace_all(std::string_view str, std::string_view from, std::string_view to);

    /**
     * @brief Pad string on left
     * @param str Input string
     * @param length Target length
     * @param pad_char Padding character
     * @return Padded string
     */
    static std::string pad_left(std::string_view str, size_t length, char pad_char = ' ');

    /**
     * @brief Pad string on right
     * @param str Input string
     * @param length Target length
     * @param pad_char Padding character
     * @return Padded string
     */
    static std::string pad_right(std::string_view str, size_t length, char pad_char = ' ');

private:
    // Known quote assets for symbol parsing (longest first for greedy matching)
    static const std::vector<std::string> QUOTE_ASSETS;

    // Kraken asset name mappings
    static const std::unordered_map<std::string, std::string> KRAKEN_ASSET_MAP;
};

// ============================================================================
// Template Implementation
// ============================================================================

template<typename Container, typename Transform>
std::string StringUtils::join_with(const Container& container, std::string_view delimiter,
                                    Transform transform) {
    std::string result;
    bool first = true;

    for (const auto& item : container) {
        if (!first) {
            result.append(delimiter);
        }
        first = false;
        result.append(transform(item));
    }

    return result;
}

} // namespace utils
} // namespace hft
