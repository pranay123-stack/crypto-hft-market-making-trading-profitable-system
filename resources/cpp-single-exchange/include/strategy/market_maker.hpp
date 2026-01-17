#pragma once

#include "core/types.hpp"
#include "orderbook/orderbook.hpp"
#include <vector>
#include <functional>
#include <cmath>

namespace hft {

// ============================================================================
// Market Making Strategy Parameters
// ============================================================================

struct MarketMakerParams {
    // Spread parameters
    double min_spread_bps = 5.0;      // Minimum spread in basis points
    double max_spread_bps = 50.0;     // Maximum spread in basis points
    double target_spread_bps = 10.0;  // Target spread

    // Inventory management
    Quantity max_position = 0;         // Maximum position (0 = unlimited)
    double inventory_skew = 0.5;       // How much to skew quotes based on inventory
    double inventory_target = 0.0;     // Target inventory level

    // Order sizing
    Quantity default_order_size = 0;
    Quantity min_order_size = 0;
    Quantity max_order_size = 0;

    // Quote management
    int quote_levels = 1;             // Number of quote levels
    double level_spacing_bps = 5.0;   // Spacing between levels
    double level_size_multiplier = 1.5; // Size multiplier for deeper levels

    // Timing
    uint64_t quote_refresh_us = 100000;  // Quote refresh interval (100ms)
    uint64_t min_quote_life_us = 50000;  // Minimum quote lifetime (50ms)

    // Risk
    double max_loss_per_trade = 0.0;
    double daily_loss_limit = 0.0;
    bool hedge_enabled = false;
};

// ============================================================================
// Quote Decision
// ============================================================================

struct QuoteDecision {
    bool should_quote = false;
    Price bid_price = 0;
    Price ask_price = 0;
    Quantity bid_size = 0;
    Quantity ask_size = 0;
    std::string reason;
};

// ============================================================================
// Market Making Signal
// ============================================================================

struct Signal {
    double fair_value = 0.0;
    double volatility = 0.0;
    double momentum = 0.0;
    double inventory_pressure = 0.0;
    double urgency = 0.0;
    Timestamp timestamp = 0;
};

// ============================================================================
// Market Maker Strategy
// ============================================================================

class MarketMaker {
public:
    using OrderCallback = std::function<OrderId(const Order&)>;
    using CancelCallback = std::function<bool(OrderId)>;

    explicit MarketMaker(const MarketMakerParams& params);
    virtual ~MarketMaker() = default;

    // Core strategy logic
    virtual QuoteDecision compute_quotes(
        const OrderBook& book,
        Quantity current_position,
        const Signal& signal
    );

    // Event handlers
    virtual void on_trade(const Trade& trade);
    virtual void on_fill(const Order& order, Quantity filled_qty, Price fill_price);
    virtual void on_cancel(OrderId order_id);
    virtual void on_reject(OrderId order_id, const std::string& reason);

    // Order management
    void set_order_callback(OrderCallback cb) { order_callback_ = std::move(cb); }
    void set_cancel_callback(CancelCallback cb) { cancel_callback_ = std::move(cb); }

    // State management
    void enable() noexcept { enabled_ = true; }
    void disable() noexcept { enabled_ = false; }
    [[nodiscard]] bool is_enabled() const noexcept { return enabled_; }

    // Parameter updates
    void update_params(const MarketMakerParams& params);
    [[nodiscard]] const MarketMakerParams& params() const noexcept { return params_; }

    // Statistics
    [[nodiscard]] uint64_t quotes_sent() const noexcept { return quotes_sent_; }
    [[nodiscard]] uint64_t fills() const noexcept { return fills_; }
    [[nodiscard]] double realized_pnl() const noexcept { return realized_pnl_; }

protected:
    // Override these for custom behavior
    virtual Price calculate_fair_value(const OrderBook& book);
    virtual double calculate_spread(const OrderBook& book, const Signal& signal);
    virtual Quantity calculate_order_size(Side side, Quantity position);
    virtual double calculate_inventory_skew(Quantity position);

    // Order helpers
    OrderId send_order(const Order& order);
    bool cancel_order(OrderId order_id);

    MarketMakerParams params_;
    bool enabled_ = false;

    // Active quotes
    OrderId active_bid_id_ = 0;
    OrderId active_ask_id_ = 0;
    Price active_bid_price_ = 0;
    Price active_ask_price_ = 0;

    // Statistics
    uint64_t quotes_sent_ = 0;
    uint64_t fills_ = 0;
    double realized_pnl_ = 0.0;
    Quantity total_bought_ = 0;
    Quantity total_sold_ = 0;

private:
    OrderCallback order_callback_;
    CancelCallback cancel_callback_;
    Timestamp last_quote_time_ = 0;
};

// ============================================================================
// Inventory-Adjusted Market Maker
// ============================================================================

class InventoryAdjustedMM : public MarketMaker {
public:
    explicit InventoryAdjustedMM(const MarketMakerParams& params);

    QuoteDecision compute_quotes(
        const OrderBook& book,
        Quantity current_position,
        const Signal& signal
    ) override;

protected:
    double calculate_inventory_skew(Quantity position) override;

private:
    // Exponential moving average of position
    double ema_position_ = 0.0;
    double ema_alpha_ = 0.1;
};

// ============================================================================
// Avellaneda-Stoikov Market Maker
// ============================================================================

class AvellanedaStoikovMM : public MarketMaker {
public:
    struct ASParams {
        double gamma = 0.1;      // Risk aversion
        double sigma = 0.01;    // Volatility estimate
        double k = 1.5;          // Order arrival intensity
        double T = 1.0;          // Time horizon
    };

    AvellanedaStoikovMM(const MarketMakerParams& base_params, const ASParams& as_params);

    QuoteDecision compute_quotes(
        const OrderBook& book,
        Quantity current_position,
        const Signal& signal
    ) override;

private:
    Price calculate_reservation_price(Price mid, Quantity position, double t_remaining);
    double calculate_optimal_spread(double t_remaining);

    ASParams as_params_;
    Timestamp start_time_ = 0;
};

} // namespace hft
