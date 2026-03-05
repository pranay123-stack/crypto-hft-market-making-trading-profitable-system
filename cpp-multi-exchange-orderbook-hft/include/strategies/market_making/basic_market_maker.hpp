#pragma once

/**
 * @file basic_market_maker.hpp
 * @brief Basic Market Making Strategy
 *
 * A simple spread-based market making strategy that:
 * - Places symmetric bid/ask quotes around the mid price
 * - Maintains configurable spread width and order sizes
 * - Refreshes quotes on fills or timeout
 * - Implements basic inventory management with position limits
 *
 * This serves as the foundation for more sophisticated market making strategies.
 */

#include "../strategy_base.hpp"
#include <deque>
#include <cmath>
#include <algorithm>

namespace hft {
namespace strategies {
namespace market_making {

/**
 * @brief Configuration for Basic Market Maker
 */
struct BasicMMConfig {
    // Spread parameters
    double target_spread_bps = 10.0;        // Target spread in basis points
    double min_spread_bps = 2.0;            // Minimum spread (safety floor)
    double max_spread_bps = 100.0;          // Maximum spread (safety ceiling)

    // Order sizing
    double base_order_size = 0.1;           // Base order size in base currency
    double min_order_size = 0.001;          // Minimum order size
    double max_order_size = 10.0;           // Maximum order size

    // Quote management
    int64_t quote_refresh_interval_ms = 1000;   // Quote refresh interval
    int64_t order_timeout_ms = 5000;            // Order validity timeout
    bool use_post_only = true;                  // Use post-only (maker) orders

    // Inventory management
    double max_position = 1.0;              // Maximum position size
    double target_position = 0.0;           // Target inventory level
    double inventory_skew_factor = 0.5;     // How much to skew based on inventory

    // Risk limits
    double max_daily_loss = 1000.0;         // Maximum daily loss
    double max_order_value = 10000.0;       // Maximum single order value
    int max_open_orders = 10;               // Maximum open orders

    // Price precision
    double tick_size = 0.01;                // Minimum price increment
    double lot_size = 0.001;                // Minimum quantity increment
    int price_decimals = 2;                 // Price decimal places
    int qty_decimals = 3;                   // Quantity decimal places
};

/**
 * @brief Active quote tracking structure
 */
struct ActiveQuote {
    uint64_t bid_order_id = 0;
    uint64_t ask_order_id = 0;
    Price bid_price = 0.0;
    Price ask_price = 0.0;
    Quantity bid_qty = 0.0;
    Quantity ask_qty = 0.0;
    Timestamp quote_time{0};
    bool bid_active = false;
    bool ask_active = false;

    [[nodiscard]] bool has_active_quote() const noexcept {
        return bid_active || ask_active;
    }

    [[nodiscard]] Price mid_price() const noexcept {
        if (bid_price > 0 && ask_price > 0) {
            return (bid_price + ask_price) / 2.0;
        }
        return 0.0;
    }

    [[nodiscard]] double spread_bps() const noexcept {
        Price mid = mid_price();
        if (mid > 0) {
            return ((ask_price - bid_price) / mid) * 10000.0;
        }
        return 0.0;
    }
};

/**
 * @brief Quote statistics for monitoring
 */
struct QuoteStats {
    int64_t quotes_sent = 0;
    int64_t quotes_filled = 0;
    int64_t quotes_canceled = 0;
    int64_t partial_fills = 0;
    double total_volume_quoted = 0.0;
    double total_volume_filled = 0.0;
    double average_spread_bps = 0.0;
    double fill_rate = 0.0;
    Timestamp last_quote_time{0};
    Timestamp last_fill_time{0};
};

/**
 * @brief Basic Market Maker Strategy
 *
 * Implements a simple market making strategy with the following features:
 * - Symmetric quote placement around mid price
 * - Configurable spread and order sizes
 * - Automatic quote refresh on fills or timeout
 * - Basic inventory management with position skewing
 * - Risk limits for position and daily loss
 */
class BasicMarketMaker : public StrategyBase {
public:
    /**
     * @brief Construct Basic Market Maker
     * @param config Strategy configuration
     * @param mm_config Market making specific configuration
     */
    explicit BasicMarketMaker(StrategyConfig config, BasicMMConfig mm_config = {});

    ~BasicMarketMaker() override = default;

    // Lifecycle
    bool initialize() override;
    bool start() override;
    void stop() override;
    void reset() override;

    // Event handlers
    void on_market_data(const MarketData& data) override;
    void on_order_update(const OrderUpdate& update) override;
    void on_trade(const Trade& trade) override;

    // =========================================================================
    // Market Making Specific Methods
    // =========================================================================

    /**
     * @brief Calculate optimal bid/ask prices
     * @param mid_price Current mid price
     * @return pair of (bid_price, ask_price)
     */
    [[nodiscard]] std::pair<Price, Price> calculate_quotes(Price mid_price) const;

    /**
     * @brief Calculate order size based on inventory
     * @param side Order side
     * @return Order quantity
     */
    [[nodiscard]] Quantity calculate_order_size(OrderSide side) const;

    /**
     * @brief Submit new quotes
     * @param data Current market data
     */
    void submit_quotes(const MarketData& data);

    /**
     * @brief Cancel current active quotes
     */
    void cancel_quotes();

    /**
     * @brief Update quotes (cancel and replace if needed)
     * @param data Current market data
     */
    void update_quotes(const MarketData& data);

    /**
     * @brief Check if quotes need refresh
     * @param current_mid Current mid price
     * @return true if quotes should be refreshed
     */
    [[nodiscard]] bool should_refresh_quotes(Price current_mid) const;

    // =========================================================================
    // Accessors
    // =========================================================================

    [[nodiscard]] const BasicMMConfig& mm_config() const noexcept { return mm_config_; }
    [[nodiscard]] const ActiveQuote& active_quote() const noexcept { return active_quote_; }
    [[nodiscard]] const QuoteStats& quote_stats() const noexcept { return quote_stats_; }
    [[nodiscard]] Price last_mid_price() const noexcept { return last_mid_price_; }

    /**
     * @brief Update market making configuration
     * @param config New configuration
     */
    void set_mm_config(const BasicMMConfig& config);

protected:
    // =========================================================================
    // Internal Methods
    // =========================================================================

    /**
     * @brief Round price to tick size
     * @param price Raw price
     * @return Rounded price
     */
    [[nodiscard]] Price round_price(Price price) const;

    /**
     * @brief Round quantity to lot size
     * @param qty Raw quantity
     * @return Rounded quantity
     */
    [[nodiscard]] Quantity round_quantity(Quantity qty) const;

    /**
     * @brief Calculate inventory skew for quote adjustment
     * @return Skew factor (-1 to 1, negative = reduce asks, positive = reduce bids)
     */
    [[nodiscard]] double calculate_inventory_skew() const;

    /**
     * @brief Check if we can place new quotes (risk checks)
     * @return true if quotes can be placed
     */
    [[nodiscard]] bool can_quote() const;

    /**
     * @brief Generate order signal
     * @param side Order side
     * @param price Order price
     * @param quantity Order quantity
     */
    void generate_order_signal(OrderSide side, Price price, Quantity quantity);

    /**
     * @brief Update quote statistics
     */
    void update_quote_stats();

    // Override hooks
    void on_stop() override;
    void on_reset() override;

protected:
    BasicMMConfig mm_config_;
    ActiveQuote active_quote_;
    QuoteStats quote_stats_;

    // State tracking
    Price last_mid_price_ = 0.0;
    Price last_bid_price_ = 0.0;
    Price last_ask_price_ = 0.0;
    Timestamp last_quote_update_time_{0};
    Timestamp last_market_data_time_{0};

    // Order tracking
    std::unordered_map<uint64_t, OrderUpdate> pending_orders_;
    std::deque<double> recent_spreads_;  // For average spread calculation

    // Constants
    static constexpr size_t MAX_SPREAD_HISTORY = 100;
    static constexpr double PRICE_DEVIATION_THRESHOLD = 0.001;  // 0.1% price move triggers refresh
};

/**
 * @brief Factory for creating Basic Market Maker instances
 */
class BasicMarketMakerFactory : public IStrategyFactory {
public:
    [[nodiscard]] std::unique_ptr<StrategyBase> create(const StrategyConfig& config) override {
        BasicMMConfig mm_config;

        // Load MM-specific config from strategy config
        mm_config.target_spread_bps = config.get_param("target_spread_bps", 10.0);
        mm_config.min_spread_bps = config.get_param("min_spread_bps", 2.0);
        mm_config.max_spread_bps = config.get_param("max_spread_bps", 100.0);
        mm_config.base_order_size = config.get_param("base_order_size", 0.1);
        mm_config.max_position = config.get_param("max_position", 1.0);
        mm_config.target_position = config.get_param("target_position", 0.0);
        mm_config.inventory_skew_factor = config.get_param("inventory_skew_factor", 0.5);
        mm_config.quote_refresh_interval_ms = static_cast<int64_t>(
            config.get_param("quote_refresh_interval_ms", 1000.0));
        mm_config.use_post_only = config.get_param("use_post_only", true);
        mm_config.tick_size = config.get_param("tick_size", 0.01);
        mm_config.lot_size = config.get_param("lot_size", 0.001);

        return std::make_unique<BasicMarketMaker>(config, mm_config);
    }

    [[nodiscard]] std::string_view strategy_type() const noexcept override {
        return "basic_market_maker";
    }
};

}  // namespace market_making
}  // namespace strategies
}  // namespace hft
