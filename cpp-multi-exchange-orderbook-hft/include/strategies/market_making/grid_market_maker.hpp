#pragma once

/**
 * @file grid_market_maker.hpp
 * @brief Grid-Based Market Making Strategy
 *
 * A grid trading strategy that places orders at multiple price levels:
 * - Geometric or arithmetic grid spacing
 * - Automatic grid rebalancing on significant price moves
 * - Profit taking at grid boundaries
 * - Anti-trending adjustments to protect against directional moves
 * - Dynamic grid density based on volatility
 *
 * Grid trading excels in ranging markets and can generate consistent
 * profits through mean reversion while managing directional risk.
 */

#include "basic_market_maker.hpp"
#include <vector>
#include <set>

namespace hft {
namespace strategies {
namespace market_making {

/**
 * @brief Grid spacing type
 */
enum class GridSpacingType : uint8_t {
    ARITHMETIC = 0,     // Equal price distance between levels
    GEOMETRIC = 1,      // Equal percentage between levels
    DYNAMIC = 2         // Volatility-adjusted spacing
};

/**
 * @brief Grid rebalance mode
 */
enum class GridRebalanceMode : uint8_t {
    NONE = 0,           // Never rebalance
    ON_BOUNDARY = 1,    // Rebalance when price hits grid boundary
    PERIODIC = 2,       // Rebalance at fixed intervals
    ADAPTIVE = 3        // Rebalance based on market conditions
};

/**
 * @brief Configuration for Grid Market Maker
 */
struct GridMMConfig : BasicMMConfig {
    // Grid structure
    GridSpacingType spacing_type = GridSpacingType::GEOMETRIC;
    int num_grid_levels = 10;               // Number of levels on each side
    double grid_spacing_bps = 50.0;         // Base spacing in bps (for arithmetic)
    double grid_spacing_pct = 0.005;        // Base spacing percentage (for geometric)
    double min_grid_spacing_bps = 10.0;     // Minimum spacing
    double max_grid_spacing_bps = 200.0;    // Maximum spacing

    // Order sizing per level
    double base_level_size = 0.1;           // Size at center level
    double size_multiplier = 1.0;           // Size increase per level from center
    double max_level_size = 1.0;            // Maximum size per level
    bool use_martingale = false;            // Double size after losses
    double martingale_multiplier = 2.0;     // Martingale factor

    // Grid boundaries
    double grid_upper_bound_pct = 0.10;     // Upper grid boundary (% from center)
    double grid_lower_bound_pct = 0.10;     // Lower grid boundary (% from center)
    bool auto_expand_grid = true;           // Automatically expand grid

    // Rebalancing
    GridRebalanceMode rebalance_mode = GridRebalanceMode::ON_BOUNDARY;
    double rebalance_threshold_pct = 0.05;  // Rebalance when price moves 5%
    int64_t rebalance_interval_ms = 3600000;// Periodic rebalance interval (1 hour)
    double rebalance_cost_threshold = 10.0; // Min expected profit to rebalance

    // Profit taking
    bool enable_profit_taking = true;
    double profit_take_threshold = 100.0;   // Take profit when grid profit exceeds
    double profit_take_ratio = 0.5;         // Take 50% of profits

    // Anti-trending protection
    bool enable_anti_trending = true;
    double trend_threshold_bps = 100.0;     // Detect trend when move > threshold
    double anti_trend_spread_mult = 1.5;    // Widen spread in trends
    double anti_trend_size_reduction = 0.5; // Reduce size in trend direction
    int trend_detection_bars = 10;          // Bars for trend detection

    // Dynamic grid
    bool use_dynamic_spacing = false;
    double volatility_spacing_mult = 2.0;   // Spacing = base * (1 + vol * mult)
    double min_volatility_for_spacing = 0.005;
    double max_volatility_for_spacing = 0.05;
};

/**
 * @brief Single grid level
 */
struct GridLevel {
    int level_index = 0;                    // Level index (-n to +n, 0 = center)
    Price price = 0.0;                      // Target price for this level
    Quantity size = 0.0;                    // Order size at this level
    uint64_t order_id = 0;                  // Active order ID (0 if none)
    OrderSide side = OrderSide::BUY;        // Buy below center, sell above
    bool is_active = false;                 // Has active order
    bool is_filled = false;                 // Level was filled
    int times_filled = 0;                   // How many times level filled
    double level_pnl = 0.0;                 // PnL from this level
    Timestamp last_fill_time{0};
    Price average_fill_price = 0.0;         // Average fill price at this level
};

/**
 * @brief Grid state tracking
 */
struct GridState {
    Price center_price = 0.0;               // Current grid center
    Price initial_center = 0.0;             // Original grid center
    Price upper_boundary = 0.0;             // Upper grid boundary
    Price lower_boundary = 0.0;             // Lower grid boundary
    int active_buy_levels = 0;              // Number of active buy orders
    int active_sell_levels = 0;             // Number of active sell orders
    int filled_buy_levels = 0;              // Filled buy orders
    int filled_sell_levels = 0;             // Filled sell orders
    double total_grid_pnl = 0.0;            // Total PnL from grid
    double grid_inventory = 0.0;            // Net inventory from grid fills
    bool is_trending_up = false;
    bool is_trending_down = false;
    Timestamp last_rebalance{0};
    int rebalance_count = 0;
};

/**
 * @brief Grid statistics
 */
struct GridStats {
    int total_levels = 0;
    int active_levels = 0;
    int filled_levels = 0;
    double total_volume = 0.0;
    double total_pnl = 0.0;
    double average_pnl_per_fill = 0.0;
    double max_level_pnl = 0.0;
    double min_level_pnl = 0.0;
    int total_fills = 0;
    double fill_rate = 0.0;
    double current_spread_coverage = 0.0;   // % of price range covered
    Timestamp first_fill_time{0};
    Timestamp last_fill_time{0};
};

/**
 * @brief Grid Market Maker Strategy
 *
 * Implements grid-based market making with:
 * - Multiple order levels on both sides of mid price
 * - Configurable grid spacing (arithmetic, geometric, dynamic)
 * - Automatic grid rebalancing
 * - Profit taking at boundaries
 * - Anti-trending protection
 */
class GridMarketMaker : public BasicMarketMaker {
public:
    /**
     * @brief Construct Grid Market Maker
     * @param config Strategy configuration
     * @param mm_config Grid MM specific configuration
     */
    explicit GridMarketMaker(StrategyConfig config, GridMMConfig mm_config = {});

    ~GridMarketMaker() override = default;

    // Lifecycle overrides
    bool initialize() override;
    bool start() override;
    void stop() override;
    void reset() override;

    // Event handlers
    void on_market_data(const MarketData& data) override;
    void on_order_update(const OrderUpdate& update) override;
    void on_trade(const Trade& trade) override;

    // =========================================================================
    // Grid Management
    // =========================================================================

    /**
     * @brief Initialize grid levels around a center price
     * @param center_price Grid center price
     */
    void initialize_grid(Price center_price);

    /**
     * @brief Recalculate grid levels
     */
    void recalculate_grid();

    /**
     * @brief Check if grid needs rebalancing
     * @param current_price Current market price
     * @return true if should rebalance
     */
    [[nodiscard]] bool should_rebalance(Price current_price) const;

    /**
     * @brief Rebalance grid around new center
     * @param new_center New center price
     */
    void rebalance_grid(Price new_center);

    /**
     * @brief Place orders for all grid levels
     */
    void place_grid_orders();

    /**
     * @brief Cancel all grid orders
     */
    void cancel_grid_orders();

    /**
     * @brief Update single grid level
     * @param level Level to update
     * @param data Current market data
     */
    void update_grid_level(GridLevel& level, const MarketData& data);

    // =========================================================================
    // Grid Calculation
    // =========================================================================

    /**
     * @brief Calculate price for a grid level
     * @param center_price Grid center
     * @param level_index Level index (-n to +n)
     * @return Price for the level
     */
    [[nodiscard]] Price calculate_level_price(Price center_price, int level_index) const;

    /**
     * @brief Calculate order size for a grid level
     * @param level_index Level index
     * @return Order quantity
     */
    [[nodiscard]] Quantity calculate_level_size(int level_index) const;

    /**
     * @brief Calculate dynamic grid spacing based on volatility
     * @param volatility Current volatility estimate
     * @return Spacing in price units
     */
    [[nodiscard]] double calculate_dynamic_spacing(double volatility) const;

    /**
     * @brief Calculate expected profit from grid
     * @return Expected profit
     */
    [[nodiscard]] double calculate_expected_grid_profit() const;

    // =========================================================================
    // Trend Detection
    // =========================================================================

    /**
     * @brief Detect current trend
     * @return 1 for uptrend, -1 for downtrend, 0 for ranging
     */
    [[nodiscard]] int detect_trend() const;

    /**
     * @brief Calculate trend strength
     * @return Trend strength (0 to 1)
     */
    [[nodiscard]] double calculate_trend_strength() const;

    /**
     * @brief Apply anti-trending adjustments
     */
    void apply_anti_trending_adjustments();

    // =========================================================================
    // Profit Management
    // =========================================================================

    /**
     * @brief Check if should take profits
     * @return true if should take profits
     */
    [[nodiscard]] bool should_take_profits() const;

    /**
     * @brief Take profits from grid
     */
    void take_profits();

    /**
     * @brief Calculate grid PnL
     * @param current_price Current market price
     * @return Grid PnL
     */
    [[nodiscard]] double calculate_grid_pnl(Price current_price) const;

    // =========================================================================
    // Grid Level Access
    // =========================================================================

    /**
     * @brief Get all grid levels
     * @return Vector of grid levels
     */
    [[nodiscard]] const std::vector<GridLevel>& grid_levels() const noexcept {
        return grid_levels_;
    }

    /**
     * @brief Get grid state
     * @return Current grid state
     */
    [[nodiscard]] const GridState& grid_state() const noexcept {
        return grid_state_;
    }

    /**
     * @brief Get grid statistics
     * @return Grid statistics
     */
    [[nodiscard]] const GridStats& grid_stats() const noexcept {
        return grid_stats_;
    }

    /**
     * @brief Get grid level by index
     * @param index Level index
     * @return Grid level or nullptr
     */
    [[nodiscard]] const GridLevel* get_level(int index) const;

    /**
     * @brief Get grid level by order ID
     * @param order_id Order ID
     * @return Grid level or nullptr
     */
    [[nodiscard]] GridLevel* get_level_by_order(uint64_t order_id);

    // =========================================================================
    // Accessors
    // =========================================================================

    [[nodiscard]] const GridMMConfig& grid_config() const noexcept {
        return static_cast<const GridMMConfig&>(mm_config_);
    }

protected:
    void on_stop() override;
    void on_reset() override;

private:
    // =========================================================================
    // Internal Methods
    // =========================================================================

    /**
     * @brief Handle level fill
     * @param level Filled level
     * @param fill Fill details
     */
    void on_level_fill(GridLevel& level, const Trade& fill);

    /**
     * @brief Update grid statistics
     */
    void update_grid_stats();

    /**
     * @brief Expand grid if needed
     * @param direction 1 for up, -1 for down
     */
    void expand_grid(int direction);

    /**
     * @brief Contract grid (remove outer levels)
     */
    void contract_grid();

    // =========================================================================
    // Internal State
    // =========================================================================

    GridMMConfig grid_config_;
    std::vector<GridLevel> grid_levels_;
    GridState grid_state_;
    GridStats grid_stats_;

    // Order ID to level index mapping
    std::unordered_map<uint64_t, int> order_to_level_;

    // Price history for trend detection
    std::deque<Price> price_history_;
    static constexpr size_t MAX_PRICE_HISTORY = 1000;

    // Volatility estimate for dynamic spacing
    double current_volatility_ = 0.0;

    // Martingale state
    int consecutive_losses_ = 0;
    double current_martingale_mult_ = 1.0;
};

/**
 * @brief Factory for creating Grid Market Maker instances
 */
class GridMarketMakerFactory : public IStrategyFactory {
public:
    [[nodiscard]] std::unique_ptr<StrategyBase> create(const StrategyConfig& config) override {
        GridMMConfig mm_config;

        // Load base config
        mm_config.target_spread_bps = config.get_param("target_spread_bps", 10.0);
        mm_config.base_order_size = config.get_param("base_order_size", 0.1);
        mm_config.max_position = config.get_param("max_position", 1.0);

        // Load grid-specific config
        mm_config.num_grid_levels = static_cast<int>(config.get_param("num_grid_levels", 10.0));
        mm_config.grid_spacing_bps = config.get_param("grid_spacing_bps", 50.0);
        mm_config.grid_spacing_pct = config.get_param("grid_spacing_pct", 0.005);
        mm_config.base_level_size = config.get_param("base_level_size", 0.1);
        mm_config.enable_profit_taking = config.get_param("enable_profit_taking", true);
        mm_config.enable_anti_trending = config.get_param("enable_anti_trending", true);
        mm_config.use_dynamic_spacing = config.get_param("use_dynamic_spacing", false);

        // Parse spacing type
        auto spacing_type_str = config.get_param<std::string>("spacing_type", "geometric");
        if (spacing_type_str == "arithmetic") {
            mm_config.spacing_type = GridSpacingType::ARITHMETIC;
        } else if (spacing_type_str == "dynamic") {
            mm_config.spacing_type = GridSpacingType::DYNAMIC;
        } else {
            mm_config.spacing_type = GridSpacingType::GEOMETRIC;
        }

        return std::make_unique<GridMarketMaker>(config, mm_config);
    }

    [[nodiscard]] std::string_view strategy_type() const noexcept override {
        return "grid_market_maker";
    }
};

}  // namespace market_making
}  // namespace strategies
}  // namespace hft
