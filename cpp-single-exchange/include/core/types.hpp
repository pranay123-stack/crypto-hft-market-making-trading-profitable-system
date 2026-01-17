#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <array>
#include <limits>

namespace hft {

// ============================================================================
// Fundamental Types - Optimized for cache efficiency
// ============================================================================

using Price = int64_t;      // Price in smallest unit (satoshi/wei) - avoid floating point
using Quantity = int64_t;   // Quantity in smallest unit
using OrderId = uint64_t;
using Timestamp = uint64_t; // Nanoseconds since epoch
using SequenceNum = uint64_t;

// Price conversion constants
constexpr int64_t PRICE_PRECISION = 100000000;  // 8 decimal places
constexpr int64_t QTY_PRECISION = 100000000;

// ============================================================================
// Enums - Using uint8_t for cache efficiency
// ============================================================================

enum class Side : uint8_t {
    BUY = 0,
    SELL = 1
};

enum class OrderType : uint8_t {
    LIMIT = 0,
    MARKET = 1,
    LIMIT_MAKER = 2,  // Post-only
    IOC = 3,          // Immediate or cancel
    FOK = 4           // Fill or kill
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
    GTC = 0,  // Good till cancel
    IOC = 1,  // Immediate or cancel
    FOK = 2,  // Fill or kill
    GTX = 3   // Good till crossing (post-only)
};

// ============================================================================
// Symbol - Fixed size for cache alignment
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
// Order - Cache-line aligned (64 bytes)
// ============================================================================

struct alignas(64) Order {
    OrderId id;              // 8 bytes
    OrderId client_id;       // 8 bytes
    Price price;             // 8 bytes
    Quantity quantity;       // 8 bytes
    Quantity filled_qty;     // 8 bytes
    Timestamp timestamp;     // 8 bytes
    Symbol symbol;           // 17 bytes
    Side side;               // 1 byte
    OrderType type;          // 1 byte
    OrderStatus status;      // 1 byte
    TimeInForce tif;         // 1 byte
    uint8_t padding[3];      // 3 bytes padding to 64

    [[nodiscard]] Quantity remaining() const noexcept {
        return quantity - filled_qty;
    }

    [[nodiscard]] bool is_active() const noexcept {
        return status == OrderStatus::NEW || status == OrderStatus::PARTIALLY_FILLED;
    }
};

static_assert(sizeof(Order) == 64, "Order must be cache-line aligned");

// ============================================================================
// Quote - For market making
// ============================================================================

struct Quote {
    Price bid_price;
    Price ask_price;
    Quantity bid_qty;
    Quantity ask_qty;
    Timestamp timestamp;
};

// ============================================================================
// Trade - Execution report
// ============================================================================

struct Trade {
    OrderId order_id;
    OrderId trade_id;
    Price price;
    Quantity quantity;
    Timestamp timestamp;
    Side side;
    bool is_maker;
};

// ============================================================================
// Market Data Tick
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
