#pragma once

#include "core/types.hpp"
#include <atomic>
#include <unordered_map>
#include <functional>
#include <mutex>

namespace hft {

// ============================================================================
// Risk Limits Configuration
// ============================================================================

struct RiskLimits {
    // Position limits
    Quantity max_position_qty = 0;           // Max position per symbol
    double max_position_value = 0.0;         // Max position value in USD
    double max_total_exposure = 0.0;         // Max total exposure

    // Order limits
    Quantity max_order_qty = 0;              // Max single order quantity
    double max_order_value = 0.0;            // Max single order value
    uint32_t max_orders_per_second = 100;    // Rate limit
    uint32_t max_open_orders = 100;          // Max concurrent orders

    // Loss limits
    double max_loss_per_trade = 0.0;         // Max loss on single trade
    double max_daily_loss = 0.0;             // Daily loss limit
    double max_drawdown = 0.0;               // Max drawdown from peak

    // Price limits
    double max_deviation_bps = 100.0;        // Max deviation from mid price

    // Circuit breakers
    bool kill_switch_enabled = true;
    uint32_t error_threshold = 10;           // Errors before kill switch
    uint32_t reject_threshold = 20;          // Rejects before kill switch
};

// ============================================================================
// Risk Violation Types
// ============================================================================

enum class RiskViolation : uint8_t {
    NONE = 0,
    POSITION_LIMIT,
    ORDER_SIZE_LIMIT,
    ORDER_VALUE_LIMIT,
    RATE_LIMIT,
    OPEN_ORDERS_LIMIT,
    DAILY_LOSS_LIMIT,
    DRAWDOWN_LIMIT,
    PRICE_DEVIATION,
    KILL_SWITCH_ACTIVE,
    SYMBOL_DISABLED
};

// ============================================================================
// Pre-Trade Risk Check Result
// ============================================================================

struct RiskCheckResult {
    bool passed = false;
    RiskViolation violation = RiskViolation::NONE;
    std::string message;

    static RiskCheckResult pass() {
        return {true, RiskViolation::NONE, ""};
    }

    static RiskCheckResult fail(RiskViolation v, const std::string& msg) {
        return {false, v, msg};
    }
};

// ============================================================================
// Position Tracking
// ============================================================================

struct Position {
    Symbol symbol;
    Quantity quantity = 0;          // Positive = long, negative = short
    Price avg_price = 0;            // Average entry price
    double unrealized_pnl = 0.0;
    double realized_pnl = 0.0;
    Timestamp last_update = 0;

    [[nodiscard]] double notional_value(Price current_price) const {
        return from_qty(std::abs(quantity)) * from_price(current_price);
    }

    [[nodiscard]] bool is_long() const noexcept { return quantity > 0; }
    [[nodiscard]] bool is_short() const noexcept { return quantity < 0; }
    [[nodiscard]] bool is_flat() const noexcept { return quantity == 0; }
};

// ============================================================================
// Risk Manager - Real-time risk monitoring and enforcement
// ============================================================================

class RiskManager {
public:
    using KillSwitchCallback = std::function<void(const std::string&)>;

    explicit RiskManager(const RiskLimits& limits);
    ~RiskManager() = default;

    // Pre-trade risk checks
    [[nodiscard]] RiskCheckResult check_order(const Order& order);
    [[nodiscard]] RiskCheckResult check_order(const Order& order, Price reference_price);

    // Post-trade updates
    void on_order_sent(const Order& order);
    void on_order_filled(const Order& order, Quantity filled_qty, Price fill_price);
    void on_order_canceled(OrderId order_id);
    void on_order_rejected(OrderId order_id);

    // Position management
    void update_position(const Symbol& symbol, Quantity qty, Price price);
    [[nodiscard]] const Position* get_position(const Symbol& symbol) const;
    [[nodiscard]] Quantity get_position_qty(const Symbol& symbol) const;

    // P&L
    void update_mark_price(const Symbol& symbol, Price price);
    [[nodiscard]] double get_unrealized_pnl() const;
    [[nodiscard]] double get_realized_pnl() const;
    [[nodiscard]] double get_total_pnl() const;
    [[nodiscard]] double get_daily_pnl() const;

    // Exposure
    [[nodiscard]] double get_total_exposure() const;
    [[nodiscard]] double get_net_exposure() const;

    // Kill switch
    void activate_kill_switch(const std::string& reason);
    void deactivate_kill_switch();
    [[nodiscard]] bool is_kill_switch_active() const noexcept;
    void set_kill_switch_callback(KillSwitchCallback cb);

    // Symbol management
    void enable_symbol(const Symbol& symbol);
    void disable_symbol(const Symbol& symbol);
    [[nodiscard]] bool is_symbol_enabled(const Symbol& symbol) const;

    // Limits management
    void update_limits(const RiskLimits& limits);
    [[nodiscard]] const RiskLimits& limits() const noexcept { return limits_; }

    // Reset for new trading day
    void reset_daily_stats();

    // Statistics
    [[nodiscard]] uint64_t orders_checked() const noexcept { return orders_checked_; }
    [[nodiscard]] uint64_t orders_rejected() const noexcept { return orders_rejected_; }
    [[nodiscard]] uint32_t current_open_orders() const noexcept { return open_orders_; }

private:
    // Risk check helpers
    RiskCheckResult check_position_limit(const Order& order);
    RiskCheckResult check_order_size(const Order& order);
    RiskCheckResult check_rate_limit();
    RiskCheckResult check_open_orders();
    RiskCheckResult check_daily_loss();
    RiskCheckResult check_price_deviation(const Order& order, Price reference);

    // Position P&L calculation
    void calculate_unrealized_pnl(Position& pos, Price current_price);

    RiskLimits limits_;

    // Position tracking
    std::unordered_map<std::string, Position> positions_;
    mutable std::mutex position_mutex_;

    // Order tracking
    std::unordered_map<OrderId, Order> open_orders_map_;
    std::atomic<uint32_t> open_orders_{0};

    // Rate limiting
    std::atomic<uint32_t> orders_this_second_{0};
    std::atomic<uint64_t> current_second_{0};

    // P&L tracking
    std::atomic<double> daily_realized_pnl_{0.0};
    std::atomic<double> peak_equity_{0.0};
    std::atomic<double> current_equity_{0.0};

    // Kill switch
    std::atomic<bool> kill_switch_active_{false};
    std::atomic<uint32_t> error_count_{0};
    std::atomic<uint32_t> reject_count_{0};
    KillSwitchCallback kill_switch_callback_;

    // Symbol state
    std::unordered_map<std::string, bool> symbol_enabled_;

    // Statistics
    std::atomic<uint64_t> orders_checked_{0};
    std::atomic<uint64_t> orders_rejected_{0};
};

} // namespace hft
