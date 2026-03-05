#pragma once

/**
 * @file types.hpp
 * @brief Core type definitions for the HFT trading system
 *
 * This file contains fundamental type definitions used throughout the system.
 * All price values use fixed-point representation for deterministic arithmetic.
 */

#include <cstdint>
#include <string_view>
#include <array>
#include <limits>
#include <compare>

namespace hft::core {

// ============================================================================
// Fixed-Point Price Representation
// ============================================================================

/**
 * @brief Price scaling factor for fixed-point arithmetic
 *
 * We use 8 decimal places which is sufficient for most crypto exchanges.
 * BTC at $100,000 with 8 decimals = 10,000,000,000,000 (fits in uint64_t)
 */
inline constexpr uint64_t PRICE_SCALE = 100'000'000ULL;  // 10^8
inline constexpr uint64_t QUANTITY_SCALE = 100'000'000ULL;  // 10^8

/**
 * @brief Fixed-point price type
 *
 * Stores prices as integers scaled by PRICE_SCALE.
 * Example: $50,123.45678901 is stored as 5,012,345,678,901
 */
struct Price {
    uint64_t value{0};

    constexpr Price() noexcept = default;
    constexpr explicit Price(uint64_t v) noexcept : value(v) {}

    [[nodiscard]] constexpr auto operator<=>(const Price&) const noexcept = default;

    [[nodiscard]] constexpr Price operator+(const Price& other) const noexcept {
        return Price{value + other.value};
    }

    [[nodiscard]] constexpr Price operator-(const Price& other) const noexcept {
        return Price{value - other.value};
    }

    [[nodiscard]] constexpr Price operator*(uint64_t multiplier) const noexcept {
        return Price{value * multiplier};
    }

    [[nodiscard]] constexpr Price operator/(uint64_t divisor) const noexcept {
        return Price{value / divisor};
    }

    [[nodiscard]] constexpr bool is_zero() const noexcept {
        return value == 0;
    }

    [[nodiscard]] static constexpr Price from_double(double d) noexcept {
        return Price{static_cast<uint64_t>(d * static_cast<double>(PRICE_SCALE))};
    }

    [[nodiscard]] constexpr double to_double() const noexcept {
        return static_cast<double>(value) / static_cast<double>(PRICE_SCALE);
    }

    [[nodiscard]] static constexpr Price max() noexcept {
        return Price{std::numeric_limits<uint64_t>::max()};
    }

    [[nodiscard]] static constexpr Price min() noexcept {
        return Price{0};
    }
};

/**
 * @brief Fixed-point quantity type
 */
struct Quantity {
    uint64_t value{0};

    constexpr Quantity() noexcept = default;
    constexpr explicit Quantity(uint64_t v) noexcept : value(v) {}

    [[nodiscard]] constexpr auto operator<=>(const Quantity&) const noexcept = default;

    [[nodiscard]] constexpr Quantity operator+(const Quantity& other) const noexcept {
        return Quantity{value + other.value};
    }

    [[nodiscard]] constexpr Quantity operator-(const Quantity& other) const noexcept {
        return Quantity{value - other.value};
    }

    [[nodiscard]] constexpr Quantity operator*(uint64_t multiplier) const noexcept {
        return Quantity{value * multiplier};
    }

    [[nodiscard]] constexpr Quantity operator/(uint64_t divisor) const noexcept {
        return Quantity{value / divisor};
    }

    [[nodiscard]] constexpr bool is_zero() const noexcept {
        return value == 0;
    }

    [[nodiscard]] static constexpr Quantity from_double(double d) noexcept {
        return Quantity{static_cast<uint64_t>(d * static_cast<double>(QUANTITY_SCALE))};
    }

    [[nodiscard]] constexpr double to_double() const noexcept {
        return static_cast<double>(value) / static_cast<double>(QUANTITY_SCALE);
    }

    [[nodiscard]] static constexpr Quantity max() noexcept {
        return Quantity{std::numeric_limits<uint64_t>::max()};
    }

    [[nodiscard]] static constexpr Quantity zero() noexcept {
        return Quantity{0};
    }
};

// ============================================================================
// Identifier Types
// ============================================================================

/**
 * @brief Order identifier type
 *
 * High bits encode exchange, low bits encode sequence number
 */
struct OrderId {
    uint64_t value{0};

    constexpr OrderId() noexcept = default;
    constexpr explicit OrderId(uint64_t v) noexcept : value(v) {}

    [[nodiscard]] constexpr auto operator<=>(const OrderId&) const noexcept = default;

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return value != 0;
    }

    [[nodiscard]] static constexpr OrderId invalid() noexcept {
        return OrderId{0};
    }
};

/**
 * @brief Trade identifier type
 */
struct TradeId {
    uint64_t value{0};

    constexpr TradeId() noexcept = default;
    constexpr explicit TradeId(uint64_t v) noexcept : value(v) {}

    [[nodiscard]] constexpr auto operator<=>(const TradeId&) const noexcept = default;

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return value != 0;
    }
};

/**
 * @brief Nanosecond timestamp type
 *
 * Represents nanoseconds since epoch (or system start for relative timing)
 */
struct Timestamp {
    uint64_t nanos{0};

    constexpr Timestamp() noexcept = default;
    constexpr explicit Timestamp(uint64_t n) noexcept : nanos(n) {}

    [[nodiscard]] constexpr auto operator<=>(const Timestamp&) const noexcept = default;

    [[nodiscard]] constexpr Timestamp operator+(const Timestamp& other) const noexcept {
        return Timestamp{nanos + other.nanos};
    }

    [[nodiscard]] constexpr Timestamp operator-(const Timestamp& other) const noexcept {
        return Timestamp{nanos - other.nanos};
    }

    [[nodiscard]] constexpr uint64_t to_micros() const noexcept {
        return nanos / 1000ULL;
    }

    [[nodiscard]] constexpr uint64_t to_millis() const noexcept {
        return nanos / 1'000'000ULL;
    }

    [[nodiscard]] constexpr uint64_t to_seconds() const noexcept {
        return nanos / 1'000'000'000ULL;
    }

    [[nodiscard]] static constexpr Timestamp from_micros(uint64_t us) noexcept {
        return Timestamp{us * 1000ULL};
    }

    [[nodiscard]] static constexpr Timestamp from_millis(uint64_t ms) noexcept {
        return Timestamp{ms * 1'000'000ULL};
    }

    [[nodiscard]] static constexpr Timestamp from_seconds(uint64_t s) noexcept {
        return Timestamp{s * 1'000'000'000ULL};
    }

    [[nodiscard]] static constexpr Timestamp max() noexcept {
        return Timestamp{std::numeric_limits<uint64_t>::max()};
    }

    [[nodiscard]] static constexpr Timestamp zero() noexcept {
        return Timestamp{0};
    }
};

// ============================================================================
// Enumerations
// ============================================================================

/**
 * @brief Order side enumeration
 */
enum class Side : uint8_t {
    Buy = 0,
    Sell = 1
};

[[nodiscard]] constexpr std::string_view side_to_string(Side side) noexcept {
    switch (side) {
        case Side::Buy:  return "BUY";
        case Side::Sell: return "SELL";
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr Side opposite_side(Side side) noexcept {
    return side == Side::Buy ? Side::Sell : Side::Buy;
}

/**
 * @brief Order type enumeration
 */
enum class OrderType : uint8_t {
    Market = 0,          // Execute immediately at market price
    Limit = 1,           // Execute at specified price or better
    LimitMaker = 2,      // Post-only limit order (rejected if would cross)
    StopLoss = 3,        // Market order triggered when stop price reached
    StopLossLimit = 4,   // Limit order triggered when stop price reached
    TakeProfit = 5,      // Market order triggered when take profit reached
    TakeProfitLimit = 6, // Limit order triggered when take profit reached
    TrailingStop = 7,    // Stop that follows price at fixed distance
    Iceberg = 8,         // Large order split into smaller visible chunks
    FOK = 9,             // Fill or Kill - execute entirely or cancel
    IOC = 10,            // Immediate or Cancel - fill what possible, cancel rest
    GTC = 11,            // Good Till Cancelled
    GTD = 12,            // Good Till Date
    PostOnly = 13        // Maker only, rejected if would take liquidity
};

[[nodiscard]] constexpr std::string_view order_type_to_string(OrderType type) noexcept {
    switch (type) {
        case OrderType::Market:          return "MARKET";
        case OrderType::Limit:           return "LIMIT";
        case OrderType::LimitMaker:      return "LIMIT_MAKER";
        case OrderType::StopLoss:        return "STOP_LOSS";
        case OrderType::StopLossLimit:   return "STOP_LOSS_LIMIT";
        case OrderType::TakeProfit:      return "TAKE_PROFIT";
        case OrderType::TakeProfitLimit: return "TAKE_PROFIT_LIMIT";
        case OrderType::TrailingStop:    return "TRAILING_STOP";
        case OrderType::Iceberg:         return "ICEBERG";
        case OrderType::FOK:             return "FOK";
        case OrderType::IOC:             return "IOC";
        case OrderType::GTC:             return "GTC";
        case OrderType::GTD:             return "GTD";
        case OrderType::PostOnly:        return "POST_ONLY";
    }
    return "UNKNOWN";
}

/**
 * @brief Order status enumeration
 */
enum class OrderStatus : uint8_t {
    Pending = 0,         // Order created but not yet sent
    New = 1,             // Order accepted by exchange
    PartiallyFilled = 2, // Order partially executed
    Filled = 3,          // Order fully executed
    Cancelled = 4,       // Order cancelled
    Rejected = 5,        // Order rejected by exchange
    Expired = 6,         // Order expired (GTD)
    PendingCancel = 7,   // Cancel request sent, awaiting confirmation
    PendingReplace = 8,  // Replace request sent, awaiting confirmation
    Error = 9            // Order in error state
};

[[nodiscard]] constexpr std::string_view order_status_to_string(OrderStatus status) noexcept {
    switch (status) {
        case OrderStatus::Pending:         return "PENDING";
        case OrderStatus::New:             return "NEW";
        case OrderStatus::PartiallyFilled: return "PARTIALLY_FILLED";
        case OrderStatus::Filled:          return "FILLED";
        case OrderStatus::Cancelled:       return "CANCELLED";
        case OrderStatus::Rejected:        return "REJECTED";
        case OrderStatus::Expired:         return "EXPIRED";
        case OrderStatus::PendingCancel:   return "PENDING_CANCEL";
        case OrderStatus::PendingReplace:  return "PENDING_REPLACE";
        case OrderStatus::Error:           return "ERROR";
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr bool is_terminal_status(OrderStatus status) noexcept {
    return status == OrderStatus::Filled ||
           status == OrderStatus::Cancelled ||
           status == OrderStatus::Rejected ||
           status == OrderStatus::Expired ||
           status == OrderStatus::Error;
}

[[nodiscard]] constexpr bool is_active_status(OrderStatus status) noexcept {
    return status == OrderStatus::New ||
           status == OrderStatus::PartiallyFilled ||
           status == OrderStatus::PendingCancel ||
           status == OrderStatus::PendingReplace;
}

/**
 * @brief Trading mode enumeration
 */
enum class TradingMode : uint8_t {
    Paper = 0,    // Paper trading with simulated execution
    Live = 1,     // Live trading with real money
    Backtest = 2  // Backtesting on historical data
};

[[nodiscard]] constexpr std::string_view trading_mode_to_string(TradingMode mode) noexcept {
    switch (mode) {
        case TradingMode::Paper:    return "PAPER";
        case TradingMode::Live:     return "LIVE";
        case TradingMode::Backtest: return "BACKTEST";
    }
    return "UNKNOWN";
}

/**
 * @brief Supported exchanges enumeration
 */
enum class Exchange : uint8_t {
    Unknown = 0,
    Binance = 1,
    BinanceUS = 2,
    BinanceFutures = 3,
    Coinbase = 4,
    Kraken = 5,
    KrakenFutures = 6,
    FTX = 7,       // Historical reference
    Bybit = 8,
    OKX = 9,
    Huobi = 10,
    Bitfinex = 11,
    Deribit = 12,  // Options/Futures
    Kucoin = 13,
    Gemini = 14,

    // Keep count for iteration
    COUNT = 15
};

[[nodiscard]] constexpr std::string_view exchange_to_string(Exchange exchange) noexcept {
    switch (exchange) {
        case Exchange::Unknown:        return "UNKNOWN";
        case Exchange::Binance:        return "BINANCE";
        case Exchange::BinanceUS:      return "BINANCE_US";
        case Exchange::BinanceFutures: return "BINANCE_FUTURES";
        case Exchange::Coinbase:       return "COINBASE";
        case Exchange::Kraken:         return "KRAKEN";
        case Exchange::KrakenFutures:  return "KRAKEN_FUTURES";
        case Exchange::FTX:            return "FTX";
        case Exchange::Bybit:          return "BYBIT";
        case Exchange::OKX:            return "OKX";
        case Exchange::Huobi:          return "HUOBI";
        case Exchange::Bitfinex:       return "BITFINEX";
        case Exchange::Deribit:        return "DERIBIT";
        case Exchange::Kucoin:         return "KUCOIN";
        case Exchange::Gemini:         return "GEMINI";
        case Exchange::COUNT:          return "INVALID";
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr bool is_futures_exchange(Exchange exchange) noexcept {
    return exchange == Exchange::BinanceFutures ||
           exchange == Exchange::KrakenFutures ||
           exchange == Exchange::Deribit ||
           exchange == Exchange::Bybit ||
           exchange == Exchange::OKX;
}

[[nodiscard]] constexpr bool is_spot_exchange(Exchange exchange) noexcept {
    return exchange == Exchange::Binance ||
           exchange == Exchange::BinanceUS ||
           exchange == Exchange::Coinbase ||
           exchange == Exchange::Kraken ||
           exchange == Exchange::Huobi ||
           exchange == Exchange::Bitfinex ||
           exchange == Exchange::Kucoin ||
           exchange == Exchange::Gemini;
}

// ============================================================================
// Symbol Types
// ============================================================================

/**
 * @brief Fixed-size symbol representation
 *
 * Stores trading pair symbols in a fixed-size buffer for cache efficiency.
 * Maximum length is 23 characters (e.g., "1000SHIBUSDTPERP")
 */
struct Symbol {
    static constexpr size_t MAX_LENGTH = 24;
    std::array<char, MAX_LENGTH> data{};
    uint8_t length{0};

    constexpr Symbol() noexcept = default;

    constexpr explicit Symbol(std::string_view sv) noexcept {
        length = static_cast<uint8_t>(std::min(sv.size(), MAX_LENGTH - 1));
        for (size_t i = 0; i < length; ++i) {
            data[i] = sv[i];
        }
        data[length] = '\0';
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return std::string_view{data.data(), length};
    }

    [[nodiscard]] constexpr const char* c_str() const noexcept {
        return data.data();
    }

    [[nodiscard]] constexpr bool operator==(const Symbol& other) const noexcept {
        if (length != other.length) return false;
        for (size_t i = 0; i < length; ++i) {
            if (data[i] != other.data[i]) return false;
        }
        return true;
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        return length == 0;
    }
};

// ============================================================================
// Cache Line Alignment
// ============================================================================

inline constexpr size_t CACHE_LINE_SIZE = 64;

template<typename T>
struct alignas(CACHE_LINE_SIZE) CacheAligned {
    T value;

    CacheAligned() = default;
    explicit CacheAligned(T v) : value(std::move(v)) {}

    operator T&() noexcept { return value; }
    operator const T&() const noexcept { return value; }
};

// ============================================================================
// Error Codes
// ============================================================================

enum class ErrorCode : uint32_t {
    Success = 0,

    // Connection errors (1xx)
    ConnectionFailed = 100,
    ConnectionTimeout = 101,
    ConnectionLost = 102,
    AuthenticationFailed = 103,
    RateLimited = 104,

    // Order errors (2xx)
    OrderRejected = 200,
    OrderNotFound = 201,
    InsufficientBalance = 202,
    InvalidPrice = 203,
    InvalidQuantity = 204,
    InvalidSymbol = 205,
    DuplicateOrderId = 206,
    OrderAlreadyCancelled = 207,
    OrderAlreadyFilled = 208,

    // Market data errors (3xx)
    InvalidMarketData = 300,
    StaleMarketData = 301,
    MarketDataTimeout = 302,

    // System errors (4xx)
    InternalError = 400,
    MemoryAllocationFailed = 401,
    QueueFull = 402,
    Timeout = 403,
    InvalidConfiguration = 404,

    // Unknown
    Unknown = 999
};

[[nodiscard]] constexpr std::string_view error_code_to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Success:              return "SUCCESS";
        case ErrorCode::ConnectionFailed:     return "CONNECTION_FAILED";
        case ErrorCode::ConnectionTimeout:    return "CONNECTION_TIMEOUT";
        case ErrorCode::ConnectionLost:       return "CONNECTION_LOST";
        case ErrorCode::AuthenticationFailed: return "AUTHENTICATION_FAILED";
        case ErrorCode::RateLimited:          return "RATE_LIMITED";
        case ErrorCode::OrderRejected:        return "ORDER_REJECTED";
        case ErrorCode::OrderNotFound:        return "ORDER_NOT_FOUND";
        case ErrorCode::InsufficientBalance:  return "INSUFFICIENT_BALANCE";
        case ErrorCode::InvalidPrice:         return "INVALID_PRICE";
        case ErrorCode::InvalidQuantity:      return "INVALID_QUANTITY";
        case ErrorCode::InvalidSymbol:        return "INVALID_SYMBOL";
        case ErrorCode::DuplicateOrderId:     return "DUPLICATE_ORDER_ID";
        case ErrorCode::OrderAlreadyCancelled:return "ORDER_ALREADY_CANCELLED";
        case ErrorCode::OrderAlreadyFilled:   return "ORDER_ALREADY_FILLED";
        case ErrorCode::InvalidMarketData:    return "INVALID_MARKET_DATA";
        case ErrorCode::StaleMarketData:      return "STALE_MARKET_DATA";
        case ErrorCode::MarketDataTimeout:    return "MARKET_DATA_TIMEOUT";
        case ErrorCode::InternalError:        return "INTERNAL_ERROR";
        case ErrorCode::MemoryAllocationFailed: return "MEMORY_ALLOCATION_FAILED";
        case ErrorCode::QueueFull:            return "QUEUE_FULL";
        case ErrorCode::Timeout:              return "TIMEOUT";
        case ErrorCode::InvalidConfiguration: return "INVALID_CONFIGURATION";
        case ErrorCode::Unknown:              return "UNKNOWN";
    }
    return "UNKNOWN";
}

}  // namespace hft::core
