#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <array>
#include <limits>

namespace hft {

// ============================================================================
// Fundamental Types
// ============================================================================

using Price = int64_t;
using Quantity = int64_t;
using OrderId = uint64_t;
using Timestamp = uint64_t;
using SequenceNum = uint64_t;

constexpr int64_t PRICE_PRECISION = 100000000;
constexpr int64_t QTY_PRECISION = 100000000;

// ============================================================================
// Exchange Identifier
// ============================================================================

enum class ExchangeId : uint8_t {
    UNKNOWN = 0,
    BINANCE = 1,
    BYBIT = 2,
    OKX = 3,
    COINBASE = 4,
    KRAKEN = 5,
    KUCOIN = 6,
    HUOBI = 7,
    GATE = 8,
    MAX_EXCHANGES = 16
};

inline const char* exchange_name(ExchangeId id) {
    static const char* names[] = {
        "UNKNOWN", "BINANCE", "BYBIT", "OKX", "COINBASE",
        "KRAKEN", "KUCOIN", "HUOBI", "GATE"
    };
    return names[static_cast<int>(id)];
}

inline ExchangeId exchange_from_string(std::string_view name) {
    if (name == "binance") return ExchangeId::BINANCE;
    if (name == "bybit") return ExchangeId::BYBIT;
    if (name == "okx") return ExchangeId::OKX;
    if (name == "coinbase") return ExchangeId::COINBASE;
    if (name == "kraken") return ExchangeId::KRAKEN;
    if (name == "kucoin") return ExchangeId::KUCOIN;
    return ExchangeId::UNKNOWN;
}

// ============================================================================
// Enums
// ============================================================================

enum class Side : uint8_t {
    BUY = 0,
    SELL = 1
};

enum class OrderType : uint8_t {
    LIMIT = 0,
    MARKET = 1,
    LIMIT_MAKER = 2,
    IOC = 3,
    FOK = 4
};

enum class OrderStatus : uint8_t {
    NEW = 0,
    PARTIALLY_FILLED = 1,
    FILLED = 2,
    CANCELED = 3,
    REJECTED = 4,
    EXPIRED = 5
};

enum class TimeInForce : uint8_t {
    GTC = 0,
    IOC = 1,
    FOK = 2,
    GTX = 3
};

// ============================================================================
// Symbol
// ============================================================================

struct Symbol {
    std::array<char, 16> data{};
    uint8_t length = 0;

    Symbol() = default;
    explicit Symbol(std::string_view s) {
        length = static_cast<uint8_t>(std::min(s.size(), data.size() - 1));
        std::copy_n(s.begin(), length, data.begin());
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return {data.data(), length};
    }

    [[nodiscard]] std::string str() const {
        return std::string(data.data(), length);
    }

    bool operator==(const Symbol& other) const noexcept {
        return length == other.length &&
               std::equal(data.begin(), data.begin() + length, other.data.begin());
    }
};

// ============================================================================
// Multi-Exchange Order
// ============================================================================

struct alignas(64) Order {
    OrderId id;
    OrderId client_id;
    Price price;
    Quantity quantity;
    Quantity filled_qty;
    Timestamp timestamp;
    Symbol symbol;
    ExchangeId exchange;      // Which exchange
    Side side;
    OrderType type;
    OrderStatus status;
    TimeInForce tif;
    uint8_t padding[2];

    [[nodiscard]] Quantity remaining() const noexcept {
        return quantity - filled_qty;
    }

    [[nodiscard]] bool is_active() const noexcept {
        return status == OrderStatus::NEW || status == OrderStatus::PARTIALLY_FILLED;
    }
};

// ============================================================================
// Multi-Exchange Quote
// ============================================================================

struct ExchangeQuote {
    ExchangeId exchange;
    Price bid_price;
    Price ask_price;
    Quantity bid_qty;
    Quantity ask_qty;
    Timestamp timestamp;
    Timestamp latency_ns;    // Network latency to this exchange
};

// ============================================================================
// Trade
// ============================================================================

struct Trade {
    OrderId order_id;
    OrderId trade_id;
    Price price;
    Quantity quantity;
    Timestamp timestamp;
    ExchangeId exchange;
    Side side;
    bool is_maker;
};

// ============================================================================
// Market Data Tick - Multi Exchange
// ============================================================================

struct alignas(64) Tick {
    Price bid;
    Price ask;
    Quantity bid_qty;
    Quantity ask_qty;
    Price last_price;
    Quantity last_qty;
    Timestamp exchange_ts;
    Timestamp local_ts;
    SequenceNum seq;
    ExchangeId exchange;
    uint8_t padding[7];
};

// ============================================================================
// Arbitrage Opportunity
// ============================================================================

struct ArbitrageOpportunity {
    Symbol symbol;
    ExchangeId buy_exchange;
    ExchangeId sell_exchange;
    Price buy_price;
    Price sell_price;
    Quantity quantity;
    double profit_bps;
    Timestamp detected_at;
    bool is_valid;
};

// ============================================================================
// Helper Functions
// ============================================================================

inline constexpr Price to_price(double p) noexcept {
    return static_cast<Price>(p * PRICE_PRECISION);
}

inline constexpr double from_price(Price p) noexcept {
    return static_cast<double>(p) / PRICE_PRECISION;
}

inline constexpr Quantity to_qty(double q) noexcept {
    return static_cast<Quantity>(q * QTY_PRECISION);
}

inline constexpr double from_qty(Quantity q) noexcept {
    return static_cast<double>(q) / QTY_PRECISION;
}

inline constexpr Side opposite_side(Side s) noexcept {
    return s == Side::BUY ? Side::SELL : Side::BUY;
}

} // namespace hft
