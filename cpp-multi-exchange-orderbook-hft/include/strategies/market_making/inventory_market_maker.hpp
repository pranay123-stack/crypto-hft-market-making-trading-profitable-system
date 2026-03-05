#pragma once

/**
 * @file inventory_market_maker.hpp
 * @brief Inventory-Aware Market Making Strategy
 *
 * An advanced market making strategy focused on inventory management:
 * - Skews quotes aggressively based on inventory position
 * - Reduces adverse selection through intelligent quote placement
 * - Maintains target inventory levels with mean-reversion
 * - Adjusts spreads based on PnL performance
 * - Implements position-based risk controls
 *
 * The strategy is based on the principle that market makers face inventory risk
 * and should adjust their quotes to encourage trades that reduce inventory imbalance.
 */

#include "basic_market_maker.hpp"
#include <cmath>

namespace hft {
namespace strategies {
namespace market_making {

/**
 * @brief Inventory management mode
 */
enum class InventoryMode : uint8_t {
    PASSIVE = 0,        // Gentle skewing, prioritize making money
    NEUTRAL = 1,        // Balanced approach
    AGGRESSIVE = 2,     // Aggressively return to target inventory
    LIQUIDATE = 3       // Emergency liquidation mode
};

/**
 * @brief Configuration for Inventory Market Maker
 */
struct InventoryMMConfig : BasicMMConfig {
    // Inventory targets
    double target_inventory = 0.0;              // Target inventory level
    double inventory_half_life_seconds = 300.0; // Time to reduce inventory by half
    double max_inventory_deviation = 0.5;       // Max deviation from target before aggressive mode
    double critical_inventory_deviation = 0.9;  // Max deviation before liquidation mode

    // Quote skewing
    double skew_per_unit_bps = 2.0;            // BPS to skew per unit of inventory
    double max_skew_bps = 50.0;                // Maximum total skew
    double asymmetric_size_ratio = 0.5;        // Reduce size on side that increases inventory
    bool enable_aggressive_skew = true;        // Enable quadratic skew at high inventory

    // Adverse selection protection
    double adverse_selection_threshold = 0.5;  // Fill rate threshold for adverse selection
    double adverse_selection_spread_mult = 1.5;// Spread multiplier when adverse selection detected
    int adverse_selection_window = 20;         // Number of trades to analyze

    // PnL-based adjustment
    bool enable_pnl_adjustment = true;
    double pnl_lookback_seconds = 3600.0;      // PnL calculation window
    double loss_spread_multiplier = 1.2;       // Spread multiplier when losing
    double profit_spread_multiplier = 0.9;     // Spread multiplier when profitable
    double pnl_threshold = 100.0;              // PnL amount to trigger adjustment

    // Mean reversion
    double mean_reversion_strength = 0.1;      // How strongly to mean-revert
    double mean_reversion_threshold = 0.3;     // Inventory level to start mean-reverting

    // Emergency controls
    double emergency_liquidation_threshold = 0.95;  // Position % that triggers liquidation
    double liquidation_discount_bps = 5.0;          // Price discount for liquidation
    bool enable_stop_loss = true;
    double stop_loss_pct = 0.02;                    // 2% stop loss per position
};

/**
 * @brief Inventory position analysis
 */
struct InventoryAnalysis {
    double current_inventory = 0.0;         // Current position
    double inventory_ratio = 0.0;           // Position / max_position (-1 to 1)
    double target_deviation = 0.0;          // Distance from target
    double inventory_value = 0.0;           // Position value at current price
    double inventory_pnl = 0.0;             // Unrealized PnL on inventory
    InventoryMode current_mode = InventoryMode::NEUTRAL;
    double time_to_target_seconds = 0.0;    // Estimated time to reach target
    double required_skew_bps = 0.0;         // Calculated skew to apply
};

/**
 * @brief Adverse selection metrics
 */
struct AdverseSelectionMetrics {
    double fill_rate_bid = 0.0;             // Fill rate on bid side
    double fill_rate_ask = 0.0;             // Fill rate on ask side
    double post_fill_move_bid = 0.0;        // Price move after bid fill
    double post_fill_move_ask = 0.0;        // Price move after ask fill
    bool adverse_selection_detected = false;
    double adverse_selection_score = 0.0;   // 0-1, higher = more adverse
    int consecutive_adverse_fills = 0;
};

/**
 * @brief PnL tracking for spread adjustment
 */
struct PnLTracker {
    double realized_pnl = 0.0;
    double unrealized_pnl = 0.0;
    double total_pnl = 0.0;
    double rolling_pnl = 0.0;               // Rolling window PnL
    double daily_pnl = 0.0;
    double peak_pnl = 0.0;
    double drawdown = 0.0;
    double max_drawdown = 0.0;
    int winning_trades = 0;
    int losing_trades = 0;
    double win_rate = 0.0;
    double profit_factor = 0.0;
    Timestamp last_update{0};
};

/**
 * @brief Fill event with price movement tracking
 */
struct FillEvent {
    uint64_t order_id = 0;
    OrderSide side = OrderSide::BUY;
    Price fill_price = 0.0;
    Quantity fill_qty = 0.0;
    Price mid_price_at_fill = 0.0;
    Price mid_price_after = 0.0;            // Price 1 second after fill
    double price_move_bps = 0.0;            // Move after fill in bps
    Timestamp fill_time{0};
    bool was_adverse = false;               // Did price move against us?
};

/**
 * @brief Inventory Market Maker Strategy
 *
 * Implements inventory-aware market making with:
 * - Position-based quote skewing
 * - Adverse selection detection and protection
 * - PnL-based spread adjustment
 * - Mean-reversion to target inventory
 * - Emergency liquidation controls
 */
class InventoryMarketMaker : public BasicMarketMaker {
public:
    /**
     * @brief Construct Inventory Market Maker
     * @param config Strategy configuration
     * @param mm_config Inventory MM specific configuration
     */
    explicit InventoryMarketMaker(StrategyConfig config, InventoryMMConfig mm_config = {});

    ~InventoryMarketMaker() override = default;

    // Lifecycle overrides
    bool initialize() override;
    void reset() override;

    // Event handlers
    void on_market_data(const MarketData& data) override;
    void on_order_update(const OrderUpdate& update) override;
    void on_trade(const Trade& trade) override;

    // =========================================================================
    // Inventory Management
    // =========================================================================

    /**
     * @brief Analyze current inventory position
     * @return Inventory analysis results
     */
    [[nodiscard]] InventoryAnalysis analyze_inventory() const;

    /**
     * @brief Calculate inventory skew for quotes
     * @return Skew in basis points (positive = raise bid, lower ask)
     */
    [[nodiscard]] double calculate_inventory_skew() const;

    /**
     * @brief Determine current inventory mode
     * @return Current inventory management mode
     */
    [[nodiscard]] InventoryMode determine_inventory_mode() const;

    /**
     * @brief Calculate target position based on signals
     * @return Target inventory level
     */
    [[nodiscard]] double calculate_target_inventory() const;

    /**
     * @brief Get time estimate to reach target inventory
     * @return Estimated seconds to target
     */
    [[nodiscard]] double estimate_time_to_target() const;

    // =========================================================================
    // Adverse Selection Protection
    // =========================================================================

    /**
     * @brief Update adverse selection metrics
     * @param data Current market data
     */
    void update_adverse_selection_metrics(const MarketData& data);

    /**
     * @brief Check if adverse selection is detected
     * @return true if adverse selection detected
     */
    [[nodiscard]] bool is_adverse_selection_detected() const;

    /**
     * @brief Calculate adverse selection adjustment
     * @return Spread multiplier for adverse selection
     */
    [[nodiscard]] double calculate_adverse_selection_adjustment() const;

    /**
     * @brief Record fill for adverse selection tracking
     * @param fill Fill details
     * @param mid_price Mid price at fill time
     */
    void record_fill_event(const Trade& fill, Price mid_price);

    // =========================================================================
    // PnL-Based Adjustment
    // =========================================================================

    /**
     * @brief Update PnL tracker
     * @param current_price Current market price
     */
    void update_pnl_tracker(Price current_price);

    /**
     * @brief Calculate spread adjustment based on PnL
     * @return Spread multiplier
     */
    [[nodiscard]] double calculate_pnl_adjustment() const;

    /**
     * @brief Check if stop loss triggered
     * @return true if stop loss should trigger
     */
    [[nodiscard]] bool check_stop_loss() const;

    // =========================================================================
    // Quote Calculation Overrides
    // =========================================================================

    /**
     * @brief Calculate inventory-adjusted quotes
     * @param mid_price Current mid price
     * @return pair of (bid_price, ask_price)
     */
    [[nodiscard]] std::pair<Price, Price> calculate_quotes(Price mid_price) const override;

    /**
     * @brief Calculate inventory-adjusted order size
     * @param side Order side
     * @return Adjusted order quantity
     */
    [[nodiscard]] Quantity calculate_order_size(OrderSide side) const override;

    // =========================================================================
    // Mean Reversion
    // =========================================================================

    /**
     * @brief Calculate mean reversion signal
     * @return Mean reversion strength (-1 to 1)
     */
    [[nodiscard]] double calculate_mean_reversion_signal() const;

    /**
     * @brief Apply mean reversion to quotes
     * @param bid_price Original bid price
     * @param ask_price Original ask price
     * @return Adjusted (bid, ask) prices
     */
    [[nodiscard]] std::pair<Price, Price> apply_mean_reversion(
        Price bid_price, Price ask_price) const;

    // =========================================================================
    // Emergency Controls
    // =========================================================================

    /**
     * @brief Check if emergency liquidation needed
     * @return true if should liquidate
     */
    [[nodiscard]] bool should_liquidate() const;

    /**
     * @brief Generate liquidation orders
     */
    void initiate_liquidation();

    // =========================================================================
    // Accessors
    // =========================================================================

    [[nodiscard]] const InventoryMMConfig& inventory_config() const noexcept {
        return static_cast<const InventoryMMConfig&>(mm_config_);
    }

    [[nodiscard]] const InventoryAnalysis& inventory_analysis() const noexcept {
        return inventory_analysis_;
    }

    [[nodiscard]] const AdverseSelectionMetrics& adverse_selection() const noexcept {
        return adverse_selection_metrics_;
    }

    [[nodiscard]] const PnLTracker& pnl_tracker() const noexcept {
        return pnl_tracker_;
    }

    [[nodiscard]] InventoryMode current_mode() const noexcept {
        return inventory_analysis_.current_mode;
    }

protected:
    void on_reset() override;

private:
    // =========================================================================
    // Internal State
    // =========================================================================

    InventoryMMConfig inventory_config_;
    InventoryAnalysis inventory_analysis_;
    AdverseSelectionMetrics adverse_selection_metrics_;
    PnLTracker pnl_tracker_;

    // Fill tracking for adverse selection
    std::deque<FillEvent> fill_events_;
    static constexpr size_t MAX_FILL_EVENTS = 1000;

    // Price tracking after fills (for adverse selection measurement)
    struct PendingFillCheck {
        uint64_t order_id = 0;
        Price fill_price = 0.0;
        Price mid_at_fill = 0.0;
        OrderSide side = OrderSide::BUY;
        Timestamp fill_time{0};
        Timestamp check_time{0};  // When to check price movement
    };
    std::deque<PendingFillCheck> pending_fill_checks_;
    static constexpr int64_t ADVERSE_CHECK_DELAY_MS = 1000;  // Check 1 second after fill

    // Rolling PnL tracking
    struct PnLSample {
        double pnl = 0.0;
        Timestamp time{0};
    };
    std::deque<PnLSample> pnl_history_;

    // Inventory history for mean reversion
    struct InventorySample {
        double inventory = 0.0;
        Timestamp time{0};
    };
    std::deque<InventorySample> inventory_history_;

    // Entry price tracking
    double average_entry_price_ = 0.0;
    double position_cost_basis_ = 0.0;
};

/**
 * @brief Factory for creating Inventory Market Maker instances
 */
class InventoryMarketMakerFactory : public IStrategyFactory {
public:
    [[nodiscard]] std::unique_ptr<StrategyBase> create(const StrategyConfig& config) override {
        InventoryMMConfig mm_config;

        // Load base config
        mm_config.target_spread_bps = config.get_param("target_spread_bps", 10.0);
        mm_config.base_order_size = config.get_param("base_order_size", 0.1);
        mm_config.max_position = config.get_param("max_position", 1.0);

        // Load inventory-specific config
        mm_config.target_inventory = config.get_param("target_inventory", 0.0);
        mm_config.inventory_half_life_seconds = config.get_param("inventory_half_life_seconds", 300.0);
        mm_config.skew_per_unit_bps = config.get_param("skew_per_unit_bps", 2.0);
        mm_config.max_skew_bps = config.get_param("max_skew_bps", 50.0);
        mm_config.enable_pnl_adjustment = config.get_param("enable_pnl_adjustment", true);
        mm_config.mean_reversion_strength = config.get_param("mean_reversion_strength", 0.1);
        mm_config.enable_stop_loss = config.get_param("enable_stop_loss", true);
        mm_config.stop_loss_pct = config.get_param("stop_loss_pct", 0.02);

        return std::make_unique<InventoryMarketMaker>(config, mm_config);
    }

    [[nodiscard]] std::string_view strategy_type() const noexcept override {
        return "inventory_market_maker";
    }
};

}  // namespace market_making
}  // namespace strategies
}  // namespace hft
