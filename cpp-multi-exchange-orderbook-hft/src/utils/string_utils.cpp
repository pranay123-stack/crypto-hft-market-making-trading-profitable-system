/**
 * @file string_utils.cpp
 * @brief Implementation of string manipulation utilities
 */

#include "utils/string_utils.hpp"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <iomanip>
#include <cctype>

namespace hft {
namespace utils {

// Known quote assets (ordered by length descending for greedy matching)
const std::vector<std::string> StringUtils::QUOTE_ASSETS = {
    "USDT", "USDC", "BUSD", "TUSD", "USDP", "GUSD", "USDD",
    "USD", "EUR", "GBP", "JPY", "AUD", "CAD", "CHF",
    "BTC", "ETH", "BNB", "XRP", "SOL", "DOGE", "ADA",
    "DAI", "UST", "PAXG"
};

// Kraken asset mappings
const std::unordered_map<std::string, std::string> StringUtils::KRAKEN_ASSET_MAP = {
    {"XBT", "BTC"},
    {"XXBT", "BTC"},
    {"XETH", "ETH"},
    {"XXRP", "XRP"},
    {"XLTC", "LTC"},
    {"XXLM", "XLM"},
    {"XDOGE", "DOGE"},
    {"ZUSD", "USD"},
    {"ZEUR", "EUR"},
    {"ZGBP", "GBP"},
    {"ZJPY", "JPY"},
    {"ZCAD", "CAD"},
    {"ZAUD", "AUD"}
};

// ============================================================================
// String Splitting
// ============================================================================

std::vector<std::string_view> StringUtils::split_view(std::string_view str, char delimiter) {
    std::vector<std::string_view> result;

    size_t start = 0;
    size_t end = 0;

    while ((end = str.find(delimiter, start)) != std::string_view::npos) {
        result.push_back(str.substr(start, end - start));
        start = end + 1;
    }

    result.push_back(str.substr(start));
    return result;
}

std::vector<std::string> StringUtils::split(std::string_view str, char delimiter) {
    std::vector<std::string> result;

    size_t start = 0;
    size_t end = 0;

    while ((end = str.find(delimiter, start)) != std::string_view::npos) {
        result.emplace_back(str.substr(start, end - start));
        start = end + 1;
    }

    result.emplace_back(str.substr(start));
    return result;
}

std::vector<std::string> StringUtils::split(std::string_view str, std::string_view delimiter) {
    std::vector<std::string> result;

    if (delimiter.empty()) {
        result.emplace_back(str);
        return result;
    }

    size_t start = 0;
    size_t end = 0;

    while ((end = str.find(delimiter, start)) != std::string_view::npos) {
        result.emplace_back(str.substr(start, end - start));
        start = end + delimiter.length();
    }

    result.emplace_back(str.substr(start));
    return result;
}

std::vector<std::string> StringUtils::split_n(std::string_view str, char delimiter, size_t max_parts) {
    std::vector<std::string> result;

    if (max_parts == 0) {
        return result;
    }

    size_t start = 0;
    size_t end = 0;

    while (result.size() < max_parts - 1 && (end = str.find(delimiter, start)) != std::string_view::npos) {
        result.emplace_back(str.substr(start, end - start));
        start = end + 1;
    }

    result.emplace_back(str.substr(start));
    return result;
}

// ============================================================================
// String Joining
// ============================================================================

std::string StringUtils::join(const std::vector<std::string>& parts, std::string_view delimiter) {
    if (parts.empty()) return "";

    size_t total_size = 0;
    for (const auto& part : parts) {
        total_size += part.size();
    }
    total_size += delimiter.size() * (parts.size() - 1);

    std::string result;
    result.reserve(total_size);

    bool first = true;
    for (const auto& part : parts) {
        if (!first) {
            result.append(delimiter);
        }
        first = false;
        result.append(part);
    }

    return result;
}

std::string StringUtils::join(const std::vector<std::string_view>& parts, std::string_view delimiter) {
    if (parts.empty()) return "";

    size_t total_size = 0;
    for (const auto& part : parts) {
        total_size += part.size();
    }
    total_size += delimiter.size() * (parts.size() - 1);

    std::string result;
    result.reserve(total_size);

    bool first = true;
    for (const auto& part : parts) {
        if (!first) {
            result.append(delimiter);
        }
        first = false;
        result.append(part);
    }

    return result;
}

// ============================================================================
// Number Formatting
// ============================================================================

std::string StringUtils::format_price(double price, int precision) {
    if (!std::isfinite(price)) {
        return "0";
    }

    char buffer[64];
    int len = snprintf(buffer, sizeof(buffer), "%.*f", precision, price);

    // Remove trailing zeros after decimal point
    if (precision > 0) {
        char* end = buffer + len - 1;
        while (end > buffer && *end == '0') {
            --end;
        }
        if (*end == '.') {
            --end;
        }
        return std::string(buffer, end - buffer + 1);
    }

    return std::string(buffer, len);
}

std::string StringUtils::format_quantity(double quantity, int precision) {
    return format_price(quantity, precision);
}

std::string StringUtils::format_decimal(double value, int max_precision) {
    if (!std::isfinite(value)) {
        return "0";
    }

    // Find appropriate precision
    int precision = max_precision;
    double test = value;

    for (int i = 0; i < max_precision; ++i) {
        test *= 10;
        if (std::abs(test - std::round(test)) < 1e-10) {
            precision = i + 1;
            break;
        }
    }

    return format_price(value, precision);
}

std::string StringUtils::format_with_thousands(double value, char separator) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2) << value;
    std::string str = ss.str();

    // Find decimal point
    size_t decimal_pos = str.find('.');
    if (decimal_pos == std::string::npos) {
        decimal_pos = str.length();
    }

    // Insert separators
    std::string result;
    size_t start = (value < 0) ? 1 : 0;

    for (size_t i = start; i < decimal_pos; ++i) {
        if (i > start && (decimal_pos - i) % 3 == 0) {
            result += separator;
        }
        result += str[i];
    }

    // Add decimal part
    if (decimal_pos < str.length()) {
        result += str.substr(decimal_pos);
    }

    if (value < 0) {
        result = "-" + result;
    }

    return result;
}

std::string StringUtils::format_percentage(double value, int precision) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%.*f%%", precision, value * 100.0);
    return buffer;
}

std::string StringUtils::format_bps(double value, int precision) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%.*f bps", precision, value * 10000.0);
    return buffer;
}

char* StringUtils::fast_dtoa(double value, char* buffer, int precision) {
    if (!std::isfinite(value)) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return buffer + 1;
    }

    int len = snprintf(buffer, 32, "%.*f", precision, value);

    // Remove trailing zeros
    char* end = buffer + len - 1;
    while (end > buffer && *end == '0') {
        --end;
    }
    if (*end == '.') {
        --end;
    }
    *(end + 1) = '\0';

    return end + 1;
}

char* StringUtils::fast_itoa(int64_t value, char* buffer) {
    char* p = buffer;

    if (value < 0) {
        *p++ = '-';
        value = -value;
    }

    return fast_utoa(static_cast<uint64_t>(value), p);
}

char* StringUtils::fast_utoa(uint64_t value, char* buffer) {
    char* p = buffer;
    char* first_digit = p;

    do {
        *p++ = '0' + (value % 10);
        value /= 10;
    } while (value > 0);

    *p = '\0';

    // Reverse the string
    char* end = p - 1;
    while (first_digit < end) {
        char tmp = *first_digit;
        *first_digit = *end;
        *end = tmp;
        ++first_digit;
        --end;
    }

    return p;
}

// ============================================================================
// Number Parsing
// ============================================================================

std::optional<double> StringUtils::parse_double(std::string_view str) {
    str = trim(str);
    if (str.empty()) return std::nullopt;

    double result;
    auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), result);

    if (ec == std::errc() && ptr == str.data() + str.size()) {
        return result;
    }

    // Fallback for some edge cases
    try {
        size_t pos;
        result = std::stod(std::string(str), &pos);
        if (pos == str.size()) {
            return result;
        }
    } catch (...) {}

    return std::nullopt;
}

std::optional<int64_t> StringUtils::parse_int64(std::string_view str) {
    str = trim(str);
    if (str.empty()) return std::nullopt;

    int64_t result;
    auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), result);

    if (ec == std::errc() && ptr == str.data() + str.size()) {
        return result;
    }

    return std::nullopt;
}

std::optional<uint64_t> StringUtils::parse_uint64(std::string_view str) {
    str = trim(str);
    if (str.empty()) return std::nullopt;

    uint64_t result;
    auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), result);

    if (ec == std::errc() && ptr == str.data() + str.size()) {
        return result;
    }

    return std::nullopt;
}

bool StringUtils::fast_atod(std::string_view str, double& result) {
    auto parsed = parse_double(str);
    if (parsed) {
        result = *parsed;
        return true;
    }
    return false;
}

bool StringUtils::fast_atoi64(std::string_view str, int64_t& result) {
    auto parsed = parse_int64(str);
    if (parsed) {
        result = *parsed;
        return true;
    }
    return false;
}

// ============================================================================
// JSON String Handling
// ============================================================================

std::string StringUtils::json_escape(std::string_view str) {
    std::string result;
    result.reserve(str.size() + str.size() / 8);  // Estimate

    for (char c : str) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    result += buf;
                } else {
                    result += c;
                }
        }
    }

    return result;
}

std::string StringUtils::json_unescape(std::string_view str) {
    std::string result;
    result.reserve(str.size());

    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '\\' && i + 1 < str.size()) {
            switch (str[i + 1]) {
                case '"':  result += '"'; ++i; break;
                case '\\': result += '\\'; ++i; break;
                case '/':  result += '/'; ++i; break;
                case 'b':  result += '\b'; ++i; break;
                case 'f':  result += '\f'; ++i; break;
                case 'n':  result += '\n'; ++i; break;
                case 'r':  result += '\r'; ++i; break;
                case 't':  result += '\t'; ++i; break;
                case 'u':
                    if (i + 5 < str.size()) {
                        // Parse \uXXXX
                        std::string hex(str.substr(i + 2, 4));
                        try {
                            int code = std::stoi(hex, nullptr, 16);
                            if (code < 0x80) {
                                result += static_cast<char>(code);
                            } else if (code < 0x800) {
                                result += static_cast<char>(0xC0 | (code >> 6));
                                result += static_cast<char>(0x80 | (code & 0x3F));
                            } else {
                                result += static_cast<char>(0xE0 | (code >> 12));
                                result += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                                result += static_cast<char>(0x80 | (code & 0x3F));
                            }
                            i += 5;
                        } catch (...) {
                            result += str[i];
                        }
                    } else {
                        result += str[i];
                    }
                    break;
                default:
                    result += str[i];
            }
        } else {
            result += str[i];
        }
    }

    return result;
}

int StringUtils::fast_json_escape(std::string_view str, char* buffer, size_t buffer_size) {
    size_t j = 0;

    for (size_t i = 0; i < str.size() && j < buffer_size - 1; ++i) {
        char c = str[i];

        if (c == '"' || c == '\\') {
            if (j + 2 > buffer_size - 1) return -1;
            buffer[j++] = '\\';
            buffer[j++] = c;
        } else if (c == '\n') {
            if (j + 2 > buffer_size - 1) return -1;
            buffer[j++] = '\\';
            buffer[j++] = 'n';
        } else if (c == '\r') {
            if (j + 2 > buffer_size - 1) return -1;
            buffer[j++] = '\\';
            buffer[j++] = 'r';
        } else if (c == '\t') {
            if (j + 2 > buffer_size - 1) return -1;
            buffer[j++] = '\\';
            buffer[j++] = 't';
        } else if (static_cast<unsigned char>(c) < 0x20) {
            if (j + 6 > buffer_size - 1) return -1;
            j += snprintf(buffer + j, buffer_size - j, "\\u%04x", static_cast<unsigned char>(c));
        } else {
            buffer[j++] = c;
        }
    }

    buffer[j] = '\0';
    return static_cast<int>(j);
}

// ============================================================================
// Symbol Normalization
// ============================================================================

std::string StringUtils::normalize_symbol(std::string_view symbol, SymbolFormat format) {
    std::string sym(symbol);

    switch (format) {
        case SymbolFormat::UNIFIED:
            return sym;

        case SymbolFormat::BINANCE:
        case SymbolFormat::BYBIT: {
            // BTCUSDT -> BTC/USDT
            std::string upper = to_upper(sym);
            for (const auto& quote : QUOTE_ASSETS) {
                if (ends_with(upper, quote)) {
                    size_t base_len = upper.size() - quote.size();
                    return upper.substr(0, base_len) + "/" + quote;
                }
            }
            return sym;
        }

        case SymbolFormat::OKX:
        case SymbolFormat::KUCOIN:
        case SymbolFormat::COINBASE: {
            // BTC-USDT or BTC-USD -> BTC/USDT
            auto parts = split(symbol, '-');
            if (parts.size() >= 2) {
                return to_upper(parts[0]) + "/" + to_upper(parts[1]);
            }
            return sym;
        }

        case SymbolFormat::GATEIO: {
            // BTC_USDT -> BTC/USDT
            auto parts = split(symbol, '_');
            if (parts.size() >= 2) {
                return to_upper(parts[0]) + "/" + to_upper(parts[1]);
            }
            return sym;
        }

        case SymbolFormat::KRAKEN: {
            // XXBTZUSD -> BTC/USD, XBTUSDT -> BTC/USDT
            std::string upper = to_upper(sym);

            // Check for known mappings
            for (const auto& [kraken, standard] : KRAKEN_ASSET_MAP) {
                if (starts_with(upper, kraken)) {
                    std::string rest = upper.substr(kraken.size());

                    // Check quote asset
                    for (const auto& [kraken_quote, standard_quote] : KRAKEN_ASSET_MAP) {
                        if (rest == kraken_quote) {
                            return standard + "/" + standard_quote;
                        }
                    }

                    // Try standard quote assets
                    for (const auto& quote : QUOTE_ASSETS) {
                        if (rest == quote) {
                            return standard + "/" + quote;
                        }
                    }
                }
            }

            // Fallback: try to split by quote assets
            for (const auto& quote : QUOTE_ASSETS) {
                if (ends_with(upper, quote)) {
                    size_t base_len = upper.size() - quote.size();
                    std::string base = upper.substr(0, base_len);

                    // Clean up X prefix
                    if (base.size() > 1 && base[0] == 'X') {
                        base = base.substr(1);
                    }

                    return base + "/" + quote;
                }
            }
            return sym;
        }

        case SymbolFormat::BITFINEX: {
            // tBTCUSD -> BTC/USD
            std::string upper = to_upper(sym);
            if (starts_with(upper, "T")) {
                upper = upper.substr(1);
            }

            for (const auto& quote : QUOTE_ASSETS) {
                if (ends_with(upper, quote)) {
                    size_t base_len = upper.size() - quote.size();
                    return upper.substr(0, base_len) + "/" + quote;
                }
            }
            return sym;
        }

        case SymbolFormat::DERIBIT: {
            // BTC-PERPETUAL, BTC-25MAR22
            auto parts = split(symbol, '-');
            if (!parts.empty()) {
                std::string base = to_upper(parts[0]);
                if (parts.size() >= 2) {
                    if (to_upper(parts[1]) == "PERPETUAL") {
                        return base + "/USD";
                    }
                    // Options/futures with expiry
                    return base + "/USD";
                }
            }
            return sym;
        }

        case SymbolFormat::HTX: {
            // btcusdt -> BTC/USDT
            std::string upper = to_upper(sym);
            for (const auto& quote : QUOTE_ASSETS) {
                if (ends_with(upper, quote)) {
                    size_t base_len = upper.size() - quote.size();
                    return upper.substr(0, base_len) + "/" + quote;
                }
            }
            return sym;
        }

        default:
            return sym;
    }
}

std::string StringUtils::to_exchange_symbol(std::string_view unified_symbol, SymbolFormat format) {
    auto parts = split(unified_symbol, '/');
    if (parts.size() != 2) {
        return std::string(unified_symbol);
    }

    std::string base = to_upper(parts[0]);
    std::string quote = to_upper(parts[1]);

    switch (format) {
        case SymbolFormat::UNIFIED:
            return base + "/" + quote;

        case SymbolFormat::BINANCE:
        case SymbolFormat::BYBIT:
            return base + quote;

        case SymbolFormat::OKX:
        case SymbolFormat::KUCOIN:
        case SymbolFormat::COINBASE:
            return base + "-" + quote;

        case SymbolFormat::GATEIO:
            return base + "_" + quote;

        case SymbolFormat::KRAKEN:
            // Use standard format for now
            return base + quote;

        case SymbolFormat::BITFINEX:
            return "t" + base + quote;

        case SymbolFormat::DERIBIT:
            if (quote == "USD" || quote == "USDT") {
                return base + "-PERPETUAL";
            }
            return base + "-" + quote;

        case SymbolFormat::HTX:
            return to_lower(base + quote);

        default:
            return base + quote;
    }
}

std::string StringUtils::extract_base_asset(std::string_view symbol, SymbolFormat format) {
    std::string normalized = normalize_symbol(symbol, format);
    auto parts = split(normalized, '/');
    return parts.empty() ? "" : parts[0];
}

std::string StringUtils::extract_quote_asset(std::string_view symbol, SymbolFormat format) {
    std::string normalized = normalize_symbol(symbol, format);
    auto parts = split(normalized, '/');
    return parts.size() < 2 ? "" : parts[1];
}

bool StringUtils::is_futures_symbol(std::string_view symbol, SymbolFormat format) {
    std::string upper = to_upper(std::string(symbol));

    switch (format) {
        case SymbolFormat::DERIBIT:
            return contains(upper, "PERPETUAL") || contains(upper, "-");

        case SymbolFormat::BINANCE:
        case SymbolFormat::BYBIT:
            // Check for futures suffixes
            return ends_with(upper, "_PERP") || contains(upper, "_");

        default:
            return false;
    }
}

// ============================================================================
// String Manipulation
// ============================================================================

std::string_view StringUtils::trim(std::string_view str) {
    size_t start = 0;
    while (start < str.size() && std::isspace(static_cast<unsigned char>(str[start]))) {
        ++start;
    }

    size_t end = str.size();
    while (end > start && std::isspace(static_cast<unsigned char>(str[end - 1]))) {
        --end;
    }

    return str.substr(start, end - start);
}

std::string_view StringUtils::ltrim(std::string_view str) {
    size_t start = 0;
    while (start < str.size() && std::isspace(static_cast<unsigned char>(str[start]))) {
        ++start;
    }
    return str.substr(start);
}

std::string_view StringUtils::rtrim(std::string_view str) {
    size_t end = str.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(str[end - 1]))) {
        --end;
    }
    return str.substr(0, end);
}

std::string StringUtils::to_upper(std::string_view str) {
    std::string result(str);
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return std::toupper(c); });
    return result;
}

std::string StringUtils::to_lower(std::string_view str) {
    std::string result(str);
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return result;
}

bool StringUtils::starts_with(std::string_view str, std::string_view prefix) {
    if (prefix.size() > str.size()) return false;
    return str.substr(0, prefix.size()) == prefix;
}

bool StringUtils::ends_with(std::string_view str, std::string_view suffix) {
    if (suffix.size() > str.size()) return false;
    return str.substr(str.size() - suffix.size()) == suffix;
}

bool StringUtils::contains(std::string_view str, std::string_view substr) {
    return str.find(substr) != std::string_view::npos;
}

std::string StringUtils::replace_all(std::string_view str, std::string_view from, std::string_view to) {
    if (from.empty()) {
        return std::string(str);
    }

    std::string result;
    result.reserve(str.size());

    size_t pos = 0;
    size_t prev_pos = 0;

    while ((pos = str.find(from, prev_pos)) != std::string_view::npos) {
        result.append(str.substr(prev_pos, pos - prev_pos));
        result.append(to);
        prev_pos = pos + from.size();
    }

    result.append(str.substr(prev_pos));
    return result;
}

std::string StringUtils::pad_left(std::string_view str, size_t length, char pad_char) {
    if (str.size() >= length) {
        return std::string(str);
    }

    return std::string(length - str.size(), pad_char) + std::string(str);
}

std::string StringUtils::pad_right(std::string_view str, size_t length, char pad_char) {
    if (str.size() >= length) {
        return std::string(str);
    }

    return std::string(str) + std::string(length - str.size(), pad_char);
}

} // namespace utils
} // namespace hft
