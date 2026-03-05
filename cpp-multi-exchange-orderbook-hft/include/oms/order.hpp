#pragma once

/**
 * @file order.hpp
 * @brief Order structure and state machine for HFT trading system
 *
 * Provides complete order representation with state machine transitions,
 * modification support, and execution report handling.
 */

#include <cstdint>
#include <string>
#include <string_view>
#include <chrono>
#include <atomic>
#include <optional>
#include <functional>
#include <array>

#include "core/types.hpp"

namespace hft::oms {

using namespace hft::core;

// Forward declarations
struct ExecutionReport;
struct OrderModification;

/**
 * @brief Time in force specification
 */
enum class TimeInForce : uint8_t {
    GTC = 0,   // Good Till Cancelled
    IOC = 1,   // Immediate Or Cancel
    FOK = 2,   // Fill Or Kill
    GTD = 3,   // Good Till Date
    GTX = 4,   // Good Till Crossing (Post Only)
    DAY = 5    // Good for day
};

[[nodiscard]] constexpr std::string_view tif_to_string(TimeInForce tif) noexcept {
    switch (tif) {
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
        case TimeInForce::GTD: return "GTD";
        case TimeInForce::GTX: return "GTX";
        case TimeInForce::DAY: return "DAY";
    }
    return "UNKNOWN";
}

/**
 * @brief Order state machine states
 */
enum class OrderState : uint8_t {
    Created = 0,       // Order created locally
    PendingNew = 1,    // Sent to exchange, awaiting ack
    New = 2,           // Acknowledged by exchange
    PartiallyFilled = 3, // Some quantity executed
    Filled = 4,        // Fully executed
    PendingCancel = 5, // Cancel request sent
    Cancelled = 6,     // Order cancelled
    PendingReplace = 7, // Replace/modify request sent
    Rejected = 8,      // Rejected by exchange
    Expired = 9,       // Order expired
    Error = 10         // Error state
};

[[nodiscard]] constexpr std::string_view state_to_string(OrderState state) noexcept {
    switch (state) {
        case OrderState::Created: return "CREATED";
        case OrderState::PendingNew: return "PENDING_NEW";
        case OrderState::New: return "NEW";
        case OrderState::PartiallyFilled: return "PARTIALLY_FILLED";
        case OrderState::Filled: return "FILLED";
        case OrderState::PendingCancel: return "PENDING_CANCEL";
        case OrderState::Cancelled: return "CANCELLED";
        case OrderState::PendingReplace: return "PENDING_REPLACE";
        case OrderState::Rejected: return "REJECTED";
        case OrderState::Expired: return "EXPIRED";
        case OrderState::Error: return "ERROR";
    }
    return "UNKNOWN";
}

/**
 * @brief Check if order state is terminal (no further transitions possible)
 */
[[nodiscard]] constexpr bool is_terminal_state(OrderState state) noexcept {
    return state == OrderState::Filled ||
           state == OrderState::Cancelled ||
           state == OrderState::Rejected ||
           state == OrderState::Expired ||
           state == OrderState::Error;
}

/**
 * @brief Check if order is still active (can be filled/cancelled)
 */
[[nodiscard]] constexpr bool is_active_state(OrderState state) noexcept {
    return state == OrderState::New ||
           state == OrderState::PartiallyFilled ||
           state == OrderState::PendingCancel ||
           state == OrderState::PendingReplace;
}

/**
 * @brief Execution type for execution reports
 */
enum class ExecutionType : uint8_t {
    New = 0,
    Trade = 1,
    Cancelled = 2,
    Replaced = 3,
    Rejected = 4,
    Expired = 5,
    PendingNew = 6,
    PendingCancel = 7,
    PendingReplace = 8,
    TradeCorrect = 9,
    TradeCancel = 10,
    OrderStatus = 11
};

[[nodiscard]] constexpr std::string_view exec_type_to_string(ExecutionType type) noexcept {
    switch (type) {
        case ExecutionType::New: return "NEW";
        case ExecutionType::Trade: return "TRADE";
        case ExecutionType::Cancelled: return "CANCELLED";
        case ExecutionType::Replaced: return "REPLACED";
        case ExecutionType::Rejected: return "REJECTED";
        case ExecutionType::Expired: return "EXPIRED";
        case ExecutionType::PendingNew: return "PENDING_NEW";
        case ExecutionType::PendingCancel: return "PENDING_CANCEL";
        case ExecutionType::PendingReplace: return "PENDING_REPLACE";
        case ExecutionType::TradeCorrect: return "TRADE_CORRECT";
        case ExecutionType::TradeCancel: return "TRADE_CANCEL";
        case ExecutionType::OrderStatus: return "ORDER_STATUS";
    }
    return "UNKNOWN";
}

/**
 * @brief Order structure - complete representation of a trading order
 *
 * Designed for cache efficiency with frequently accessed fields grouped together.
 * Uses fixed-point arithmetic for prices and quantities.
 */
struct alignas(128) Order {
    // Primary identifiers (frequently accessed together)
    uint64_t orderId{0};                    // Internal unique order ID
    uint64_t clientOrderId{0};              // Client-specified order ID
    std::string exchangeOrderId;            // Exchange-assigned order ID

    // Trading parameters
    Symbol symbol;                          // Trading pair
    Exchange exchange{Exchange::Unknown};   // Target exchange
    Side side{Side::Buy};                   // Buy or Sell
    OrderType type{OrderType::Limit};       // Order type
    TimeInForce tif{TimeInForce::GTC};     // Time in force

    // Price and quantity (fixed-point)
    Price price;                            // Limit price
    Price stopPrice;                        // Stop/trigger price
    Quantity quantity;                      // Original quantity
    Quantity filledQuantity;                // Executed quantity
    Quantity remainingQuantity;             // Remaining quantity

    // State tracking
    OrderState state{OrderState::Created};  // Current state
    std::string rejectReason;               // Reason if rejected

    // Execution statistics
    Price avgFillPrice;                     // Average fill price
    uint32_t fillCount{0};                  // Number of fills
    double totalCommission{0.0};            // Total fees paid
    std::string commissionAsset;            // Fee asset

    // Timestamps (nanoseconds since epoch)
    Timestamp createTime;                   // Order creation time
    Timestamp sendTime;                     // Time sent to exchange
    Timestamp ackTime;                      // Exchange acknowledgement time
    Timestamp lastUpdateTime;               // Last state update time
    Timestamp lastFillTime;                 // Last fill time

    // Strategy metadata
    std::string strategyId;                 // Originating strategy
    uint64_t parentOrderId{0};              // Parent order (for child orders)
    std::string tag;                        // User-defined tag

    // Flags
    bool reduceOnly{false};                 // Only reduce position
    bool postOnly{false};                   // Post only (maker)
    bool hidden{false};                     // Hidden/iceberg order

    /**
     * @brief Default constructor
     */
    Order() = default;

    /**
     * @brief Construct order with essential parameters
     */
    Order(uint64_t id, const Symbol& sym, Exchange exch, Side s,
          OrderType t, Price p, Quantity q)
        : orderId(id)
        , symbol(sym)
        , exchange(exch)
        , side(s)
        , type(t)
        , price(p)
        , quantity(q)
        , remainingQuantity(q)
        , createTime(Timestamp{static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::high_resolution_clock::now().time_since_epoch()).count())})
    {}

    /**
     * @brief Check if order is still active
     */
    [[nodiscard]] bool isActive() const noexcept {
        return is_active_state(state);
    }

    /**
     * @brief Check if order is in terminal state
     */
    [[nodiscard]] bool isTerminal() const noexcept {
        return is_terminal_state(state);
    }

    /**
     * @brief Check if order is fully filled
     */
    [[nodiscard]] bool isFilled() const noexcept {
        return state == OrderState::Filled;
    }

    /**
     * @brief Get fill ratio (0.0 to 1.0)
     */
    [[nodiscard]] double fillRatio() const noexcept {
        if (quantity.value == 0) return 0.0;
        return static_cast<double>(filledQuantity.value) /
               static_cast<double>(quantity.value);
    }

    /**
     * @brief Calculate notional value of order
     */
    [[nodiscard]] double notionalValue() const noexcept {
        return price.to_double() * quantity.to_double();
    }

    /**
     * @brief Calculate remaining notional value
     */
    [[nodiscard]] double remainingNotional() const noexcept {
        return price.to_double() * remainingQuantity.to_double();
    }

    /**
     * @brief Calculate filled notional value
     */
    [[nodiscard]] double filledNotional() const noexcept {
        return avgFillPrice.to_double() * filledQuantity.to_double();
    }

    /**
     * @brief Get order age in microseconds
     */
    [[nodiscard]] uint64_t ageInMicros() const noexcept {
        auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
        return (now - createTime.nanos) / 1000;
    }

    /**
     * @brief Convert to string for logging
     */
    [[nodiscard]] std::string toString() const;
};

/**
 * @brief State transition validation
 */
class OrderStateMachine {
public:
    /**
     * @brief Check if state transition is valid
     */
    [[nodiscard]] static bool isValidTransition(OrderState from, OrderState to) noexcept;

    /**
     * @brief Transition order to new state
     * @return true if transition successful
     */
    static bool transition(Order& order, OrderState newState) noexcept;

    /**
     * @brief Get allowed transitions from current state
     */
    [[nodiscard]] static std::array<OrderState, 4> getAllowedTransitions(OrderState from) noexcept;
};

/**
 * @brief Execution report structure
 *
 * Represents an update from the exchange about an order.
 */
struct ExecutionReport {
    uint64_t orderId{0};                    // Internal order ID
    uint64_t clientOrderId{0};              // Client order ID
    std::string exchangeOrderId;            // Exchange order ID
    std::string executionId;                // Unique execution ID

    Symbol symbol;
    Exchange exchange{Exchange::Unknown};
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};

    ExecutionType execType{ExecutionType::New};
    OrderState orderState{OrderState::New};

    Price price;                            // Order price
    Price lastPrice;                        // Last fill price
    Price avgPrice;                         // Average fill price

    Quantity quantity;                      // Original quantity
    Quantity lastQuantity;                  // Last fill quantity
    Quantity filledQuantity;                // Cumulative filled
    Quantity remainingQuantity;             // Leaves quantity

    double commission{0.0};                 // Commission for this fill
    std::string commissionAsset;            // Commission asset

    Timestamp transactTime;                 // Exchange timestamp
    Timestamp receiveTime;                  // Local receive time

    std::string text;                       // Additional info/reject reason

    /**
     * @brief Check if this is a fill report
     */
    [[nodiscard]] bool isFill() const noexcept {
        return execType == ExecutionType::Trade;
    }

    /**
     * @brief Check if order is terminal
     */
    [[nodiscard]] bool isTerminal() const noexcept {
        return is_terminal_state(orderState);
    }

    /**
     * @brief Convert to string for logging
     */
    [[nodiscard]] std::string toString() const;
};

/**
 * @brief Order modification request
 */
struct OrderModification {
    uint64_t orderId{0};                    // Order to modify
    std::optional<Price> newPrice;          // New price (if changing)
    std::optional<Quantity> newQuantity;    // New quantity (if changing)
    std::optional<Price> newStopPrice;      // New stop price (if changing)
    Timestamp requestTime;                  // When modification requested

    /**
     * @brief Check if modification changes price
     */
    [[nodiscard]] bool hasNewPrice() const noexcept {
        return newPrice.has_value();
    }

    /**
     * @brief Check if modification changes quantity
     */
    [[nodiscard]] bool hasNewQuantity() const noexcept {
        return newQuantity.has_value();
    }

    /**
     * @brief Validate modification
     */
    [[nodiscard]] bool isValid() const noexcept {
        return hasNewPrice() || hasNewQuantity() || newStopPrice.has_value();
    }
};

/**
 * @brief Order factory for creating orders with unique IDs
 */
class OrderFactory {
public:
    /**
     * @brief Create new order with auto-generated ID
     */
    static Order createOrder(
        const Symbol& symbol,
        Exchange exchange,
        Side side,
        OrderType type,
        Price price,
        Quantity quantity
    );

    /**
     * @brief Create market order
     */
    static Order createMarketOrder(
        const Symbol& symbol,
        Exchange exchange,
        Side side,
        Quantity quantity
    );

    /**
     * @brief Create limit order
     */
    static Order createLimitOrder(
        const Symbol& symbol,
        Exchange exchange,
        Side side,
        Price price,
        Quantity quantity,
        TimeInForce tif = TimeInForce::GTC
    );

    /**
     * @brief Create stop market order
     */
    static Order createStopOrder(
        const Symbol& symbol,
        Exchange exchange,
        Side side,
        Price stopPrice,
        Quantity quantity
    );

    /**
     * @brief Create stop limit order
     */
    static Order createStopLimitOrder(
        const Symbol& symbol,
        Exchange exchange,
        Side side,
        Price price,
        Price stopPrice,
        Quantity quantity
    );

    /**
     * @brief Generate next unique order ID
     */
    static uint64_t generateOrderId() noexcept;

    /**
     * @brief Reset order ID sequence (for testing)
     */
    static void resetSequence(uint64_t startId = 1) noexcept;

private:
    static std::atomic<uint64_t> nextOrderId_;
};

/**
 * @brief Order event callback types
 */
using OrderCallback = std::function<void(const Order&)>;
using ExecutionCallback = std::function<void(const ExecutionReport&)>;

} // namespace hft::oms
