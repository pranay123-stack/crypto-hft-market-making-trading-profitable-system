#pragma once

#include "core/types.hpp"
#include "orderbook/consolidated_book.hpp"
#include "exchange/exchange_manager.hpp"
#include <functional>
#include <unordered_map>
#include <vector>

namespace hft {

// ============================================================================
// Cross-Exchange Market Making Parameters
// ============================================================================

struct CrossExchangeMMParams {
    // Spread parameters
    double min_spread_bps = 5.0;
    double max_spread_bps = 100.0;
    double target_spread_bps = 15.0;

    // Position limits per exchange
    Quantity max_position_per_exchange = 0;
    Quantity max_total_position = 0;

    // Order sizing
    Quantity default_order_size = 0;
    Quantity min_order_size = 0;
    Quantity max_order_size = 0;

    // Cross-exchange specific
    double cross_exchange_spread_target_bps = 20.0;
    bool hedge_immediately = true;
    double hedge_threshold_percent = 50.0;  // Hedge when filled > 50%

    // Exchange preferences
    std::vector<ExchangeId> quote_exchanges;   // Where to quote
    std::vector<ExchangeId> hedge_exchanges;   // Where to hedge

    // Timing
    uint64_t quote_refresh_us = 100000;
    uint64_t hedge_timeout_us = 500000;
};

// ============================================================================
// Quote Decision - Multi Exchange
// ============================================================================

struct MultiExchangeQuoteDecision {
    struct ExchangeQuote {
        ExchangeId exchange;
        Price bid_price;
        Price ask_price;
        Quantity bid_size;
        Quantity ask_size;
        bool should_quote;
    };

    std::vector<ExchangeQuote> quotes;
    std::string reason;
};

// ============================================================================
// Position State Across Exchanges
// ============================================================================

struct CrossExchangePosition {
    std::unordered_map<ExchangeId, Quantity> positions;
    Quantity net_position = 0;
    double total_exposure = 0.0;
    double unrealized_pnl = 0.0;

    [[nodiscard]] Quantity get_position(ExchangeId exchange) const {
        auto it = positions.find(exchange);
        return (it != positions.end()) ? it->second : 0;
    }

    void update_position(ExchangeId exchange, Quantity qty) {
        positions[exchange] = qty;
        recalculate_net();
    }

    void recalculate_net() {
        net_position = 0;
        for (const auto& [ex, qty] : positions) {
            net_position += qty;
        }
    }
};

// ============================================================================
// Cross-Exchange Market Maker
// ============================================================================

class CrossExchangeMarketMaker {
public:
    using OrderCallback = std::function<OrderId(const Order&)>;
    using CancelCallback = std::function<bool(ExchangeId, OrderId)>;

    explicit CrossExchangeMarketMaker(const CrossExchangeMMParams& params);
    virtual ~CrossExchangeMarketMaker() = default;

    // Core strategy
    virtual MultiExchangeQuoteDecision compute_quotes(
        const ConsolidatedBook& book,
        const CrossExchangePosition& position
    );

    // Event handlers
    virtual void on_fill(ExchangeId exchange, const Order& order,
                        Quantity filled_qty, Price fill_price);
    virtual void on_cancel(ExchangeId exchange, OrderId order_id);

    // Hedging
    virtual Order compute_hedge_order(
        ExchangeId fill_exchange,
        Side fill_side,
        Quantity fill_qty,
        Price fill_price,
        const ConsolidatedBook& book
    );

    // Callbacks
    void set_order_callback(OrderCallback cb) { order_callback_ = std::move(cb); }
    void set_cancel_callback(CancelCallback cb) { cancel_callback_ = std::move(cb); }

    // State
    void enable() { enabled_ = true; }
    void disable() { enabled_ = false; }
    [[nodiscard]] bool is_enabled() const { return enabled_; }

    // Parameters
    void update_params(const CrossExchangeMMParams& params);
    [[nodiscard]] const CrossExchangeMMParams& params() const { return params_; }

    // Statistics
    [[nodiscard]] uint64_t total_quotes() const { return total_quotes_; }
    [[nodiscard]] uint64_t total_fills() const { return total_fills_; }
    [[nodiscard]] uint64_t hedge_orders() const { return hedge_orders_; }

protected:
    // Override for custom logic
    virtual Price calculate_fair_value(const ConsolidatedBook& book);
    virtual double calculate_spread(const ConsolidatedBook& book,
                                   const CrossExchangePosition& position);
    virtual Quantity calculate_order_size(ExchangeId exchange, Side side,
                                         const CrossExchangePosition& position);
    virtual ExchangeId select_hedge_exchange(const ConsolidatedBook& book,
                                            Side hedge_side);

    // Order helpers
    OrderId send_order(const Order& order);
    bool cancel_order(ExchangeId exchange, OrderId order_id);

    CrossExchangeMMParams params_;
    bool enabled_ = false;

    // Active quotes per exchange
    struct ActiveQuotes {
        OrderId bid_id = 0;
        OrderId ask_id = 0;
        Price bid_price = 0;
        Price ask_price = 0;
    };
    std::unordered_map<ExchangeId, ActiveQuotes> active_quotes_;

    // Statistics
    uint64_t total_quotes_ = 0;
    uint64_t total_fills_ = 0;
    uint64_t hedge_orders_ = 0;

private:
    OrderCallback order_callback_;
    CancelCallback cancel_callback_;
};

// ============================================================================
// Latency-Optimized Cross-Exchange MM
// ============================================================================

class LatencyOptimizedMM : public CrossExchangeMarketMaker {
public:
    explicit LatencyOptimizedMM(const CrossExchangeMMParams& params);

    MultiExchangeQuoteDecision compute_quotes(
        const ConsolidatedBook& book,
        const CrossExchangePosition& position
    ) override;

protected:
    ExchangeId select_hedge_exchange(const ConsolidatedBook& book,
                                    Side hedge_side) override;

private:
    // Track exchange latencies
    std::unordered_map<ExchangeId, Timestamp> exchange_latencies_;
};

// ============================================================================
// Inventory-Balanced Cross-Exchange MM
// ============================================================================

class InventoryBalancedMM : public CrossExchangeMarketMaker {
public:
    explicit InventoryBalancedMM(const CrossExchangeMMParams& params);

    MultiExchangeQuoteDecision compute_quotes(
        const ConsolidatedBook& book,
        const CrossExchangePosition& position
    ) override;

protected:
    double calculate_spread(const ConsolidatedBook& book,
                           const CrossExchangePosition& position) override;
    Quantity calculate_order_size(ExchangeId exchange, Side side,
                                 const CrossExchangePosition& position) override;

private:
    // Try to balance inventory across exchanges
    void calculate_rebalance_orders(
        const CrossExchangePosition& position,
        const ConsolidatedBook& book,
        std::vector<Order>& orders
    );

    double target_balance_ratio_ = 0.5;  // Try to keep 50% on each exchange
};

} // namespace hft
