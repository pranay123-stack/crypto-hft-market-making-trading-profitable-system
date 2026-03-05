#pragma once

/**
 * @file strategy_config.hpp
 * @brief Strategy parameter configuration and management
 *
 * This module provides configuration structures for trading strategies.
 * Features:
 * - Strategy parameter configuration
 * - Per-strategy risk limits
 * - Trading pairs per strategy
 * - Enable/disable flags
 * - Strategy-specific tuning parameters
 *
 * @author HFT System
 * @version 1.0
 */

#include <yaml-cpp/yaml.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <chrono>
#include <memory>
#include <shared_mutex>

namespace hft {
namespace config {

/**
 * @brief Strategy type enumeration
 */
enum class StrategyType : uint8_t {
    // HFT Strategies
    LATENCY_ARBITRAGE = 0,
    TRIANGULAR_ARBITRAGE,
    STATISTICAL_ARBITRAGE,
    ORDER_FLOW_IMBALANCE,
    MICROSTRUCTURE_ALPHA,

    // Market Making Strategies
    BASIC_MARKET_MAKING,
    ADAPTIVE_MARKET_MAKING,
    INVENTORY_MARKET_MAKING,
    MULTI_EXCHANGE_MARKET_MAKING,
    VOLATILITY_MARKET_MAKING,

    UNKNOWN
};

/**
 * @brief Convert strategy type to string
 */
constexpr const char* strategy_type_to_string(StrategyType type) {
    switch (type) {
        case StrategyType::LATENCY_ARBITRAGE:           return "latency_arbitrage";
        case StrategyType::TRIANGULAR_ARBITRAGE:        return "triangular_arbitrage";
        case StrategyType::STATISTICAL_ARBITRAGE:       return "statistical_arbitrage";
        case StrategyType::ORDER_FLOW_IMBALANCE:        return "order_flow_imbalance";
        case StrategyType::MICROSTRUCTURE_ALPHA:        return "microstructure_alpha";
        case StrategyType::BASIC_MARKET_MAKING:         return "basic_market_making";
        case StrategyType::ADAPTIVE_MARKET_MAKING:      return "adaptive_market_making";
        case StrategyType::INVENTORY_MARKET_MAKING:     return "inventory_market_making";
        case StrategyType::MULTI_EXCHANGE_MARKET_MAKING: return "multi_exchange_market_making";
        case StrategyType::VOLATILITY_MARKET_MAKING:    return "volatility_market_making";
        default:                                         return "unknown";
    }
}

/**
 * @brief Convert string to strategy type
 */
StrategyType string_to_strategy_type(const std::string& name);

/**
 * @brief Strategy risk limits
 */
struct StrategyRiskLimits {
    // Position limits
    double max_position_size{0.0};          // Maximum position in base currency
    double max_position_value{0.0};         // Maximum position value in quote currency
    double max_position_pct{1.0};           // Maximum position as % of portfolio

    // Order limits
    double max_order_size{0.0};             // Maximum single order size
    double max_order_value{0.0};            // Maximum single order value
    uint32_t max_open_orders{100};          // Maximum concurrent open orders
    uint32_t max_orders_per_second{10};     // Rate limit on order submissions

    // Loss limits
    double max_daily_loss{0.0};             // Maximum daily loss
    double max_drawdown{0.0};               // Maximum drawdown
    double max_loss_per_trade{0.0};         // Maximum loss per trade

    // Profit limits
    double daily_profit_target{0.0};        // Stop trading after this profit
    bool stop_on_profit_target{false};      // Whether to stop on profit target

    // Exposure limits
    double max_notional_exposure{0.0};      // Maximum total notional exposure
    double max_delta_exposure{0.0};         // Maximum delta exposure
    double max_concentration{0.5};          // Maximum concentration in single asset

    // Time-based limits
    uint32_t max_trades_per_minute{60};
    uint32_t max_trades_per_hour{1000};
    uint32_t cooldown_after_loss_ms{0};     // Cooldown period after loss
};

/**
 * @brief Strategy trading pair configuration
 */
struct StrategyPairConfig {
    std::string symbol;                     // Trading symbol
    std::string exchange;                   // Target exchange
    bool enabled{true};
    double allocation_weight{1.0};          // Weight for capital allocation
    double min_spread_bps{0.0};             // Minimum required spread
    double max_spread_bps{1000.0};          // Maximum allowed spread

    // Pair-specific overrides
    std::optional<double> max_position_size;
    std::optional<double> max_order_size;
};

/**
 * @brief Base strategy parameters (common to all strategies)
 */
struct BaseStrategyParams {
    std::string name;
    StrategyType type{StrategyType::UNKNOWN};
    bool enabled{false};
    int priority{0};                        // Execution priority

    // Capital allocation
    double capital_allocation{0.0};         // Allocated capital
    double capital_allocation_pct{0.0};     // % of total capital

    // Trading pairs
    std::vector<StrategyPairConfig> trading_pairs;

    // Risk limits
    StrategyRiskLimits risk_limits;

    // Timing
    uint32_t tick_interval_us{1000};        // Strategy tick interval in microseconds
    uint32_t signal_timeout_ms{100};        // Signal validity timeout

    // Generic parameters (strategy-specific)
    std::unordered_map<std::string, double> numeric_params;
    std::unordered_map<std::string, std::string> string_params;
    std::unordered_map<std::string, bool> bool_params;
};

/**
 * @brief Latency arbitrage strategy parameters
 */
struct LatencyArbitrageParams : public BaseStrategyParams {
    double min_profit_bps{1.0};             // Minimum profit in basis points
    double max_latency_ms{10.0};            // Maximum acceptable latency
    double stale_price_threshold_ms{50.0};  // Price staleness threshold
    bool use_maker_orders{false};           // Try to capture maker rebates
    double aggression_factor{1.0};          // Order aggressiveness
    double confidence_threshold{0.8};       // Signal confidence threshold
    std::vector<std::string> fast_exchanges;
    std::vector<std::string> slow_exchanges;
};

/**
 * @brief Triangular arbitrage strategy parameters
 */
struct TriangularArbitrageParams : public BaseStrategyParams {
    double min_profit_bps{1.0};             // Minimum profit in basis points
    double max_leg_slippage_bps{5.0};       // Maximum acceptable slippage per leg
    uint32_t max_execution_time_ms{100};    // Maximum time to complete triangle
    bool partial_fills_allowed{false};      // Allow partial fills
    double volume_threshold{1000.0};        // Minimum volume threshold
    std::vector<std::vector<std::string>> triangles; // Triangle routes
};

/**
 * @brief Statistical arbitrage strategy parameters
 */
struct StatisticalArbitrageParams : public BaseStrategyParams {
    double entry_z_score{2.0};              // Z-score threshold for entry
    double exit_z_score{0.5};               // Z-score threshold for exit
    double stop_loss_z_score{4.0};          // Z-score for stop loss
    uint32_t lookback_period{100};          // Lookback period for statistics
    double half_life{20.0};                 // Mean reversion half-life
    double correlation_threshold{0.7};      // Minimum correlation
    double cointegration_pvalue{0.05};      // Cointegration p-value threshold
    bool dynamic_hedge_ratio{true};         // Use dynamic hedge ratios
    uint32_t rebalance_interval_ms{60000};  // Hedge ratio rebalance interval
    std::vector<std::pair<std::string, std::string>> pairs; // Asset pairs
};

/**
 * @brief Order flow imbalance strategy parameters
 */
struct OrderFlowImbalanceParams : public BaseStrategyParams {
    double imbalance_threshold{0.6};        // Imbalance ratio threshold
    uint32_t depth_levels{10};              // Order book depth levels
    uint32_t flow_window_ms{1000};          // Order flow analysis window
    double volume_threshold{10000.0};       // Minimum volume threshold
    double momentum_factor{0.5};            // Momentum weighting
    bool use_trade_imbalance{true};         // Include trade imbalance
    bool use_cancel_imbalance{true};        // Include cancel imbalance
    double aggression_decay{0.95};          // Aggression decay factor
};

/**
 * @brief Microstructure alpha strategy parameters
 */
struct MicrostructureAlphaParams : public BaseStrategyParams {
    double tick_intensity_threshold{0.7};   // Tick intensity threshold
    double spread_percentile{0.25};         // Spread percentile for entry
    uint32_t book_pressure_depth{5};        // Depth for book pressure calc
    double queue_position_alpha{0.3};       // Queue position importance
    bool use_tick_rule{true};               // Use tick rule for direction
    double microprice_weight{0.5};          // Weight of microprice signal
    uint32_t signal_decay_ticks{10};        // Signal decay in ticks
};

/**
 * @brief Basic market making strategy parameters
 */
struct BasicMarketMakingParams : public BaseStrategyParams {
    double spread_bps{10.0};                // Target spread in basis points
    double skew_factor{0.5};                // Price skew factor
    uint32_t num_levels{3};                 // Number of quote levels
    double level_spacing_bps{2.0};          // Spacing between levels
    double quote_size{100.0};               // Quote size per level
    double min_spread_bps{5.0};             // Minimum spread
    double max_spread_bps{50.0};            // Maximum spread
    bool fade_inventory{true};              // Fade quotes with inventory
    double inventory_skew_factor{0.1};      // Inventory skew sensitivity
    uint32_t quote_refresh_ms{100};         // Quote refresh interval
};

/**
 * @brief Adaptive market making strategy parameters
 */
struct AdaptiveMarketMakingParams : public BaseStrategyParams {
    double base_spread_bps{10.0};           // Base spread
    double volatility_scaling{1.0};         // Volatility adjustment factor
    double inventory_aversion{0.5};         // Inventory risk aversion
    double adverse_selection_factor{0.3};   // Adverse selection protection
    uint32_t regime_lookback{100};          // Lookback for regime detection
    bool use_fair_value_model{true};        // Use fair value estimation
    double learning_rate{0.01};             // Online learning rate
    std::string volatility_model{"ewma"};   // Volatility model type
    double vol_ewma_span{100.0};            // EWMA span for volatility
};

/**
 * @brief Inventory market making strategy parameters
 */
struct InventoryMarketMakingParams : public BaseStrategyParams {
    double target_inventory{0.0};           // Target inventory level
    double inventory_half_life{300.0};      // Inventory mean reversion half-life
    double risk_aversion{0.5};              // Risk aversion parameter
    double max_inventory{1000.0};           // Maximum inventory
    double skew_intensity{1.0};             // Price skew intensity
    bool use_avellaneda_stoikov{true};      // Use A-S framework
    double terminal_time{3600.0};           // Terminal time for A-S model
    double gamma{0.1};                      // Gamma parameter
};

/**
 * @brief Multi-exchange market making strategy parameters
 */
struct MultiExchangeMMParams : public BaseStrategyParams {
    std::vector<std::string> exchanges;     // Participating exchanges
    double min_cross_exchange_spread{5.0};  // Min spread for cross-exchange
    double latency_penalty_factor{0.1};     // Penalty for latency differences
    bool enable_hedging{true};              // Enable cross-exchange hedging
    double hedge_ratio{1.0};                // Hedge ratio
    uint32_t max_hedge_delay_ms{50};        // Maximum hedging delay
    double concentration_limit{0.5};        // Max concentration per exchange
};

/**
 * @brief Volatility market making strategy parameters
 */
struct VolatilityMarketMakingParams : public BaseStrategyParams {
    double vol_regime_threshold{0.02};      // Volatility regime threshold
    double spread_vol_multiplier{5.0};      // Spread = base + vol * multiplier
    double gamma_scalping_threshold{0.01};  // Threshold for gamma scalping
    bool adjust_for_skew{true};             // Adjust for volatility skew
    double delta_hedge_threshold{0.1};      // Delta threshold for hedging
    uint32_t vol_update_interval_ms{1000};  // Volatility update interval
    std::string implied_vol_model{"sabr"};  // IV model type
};

/**
 * @brief Strategy configuration container
 */
class StrategyConfig {
public:
    std::string id;
    StrategyType type{StrategyType::UNKNOWN};
    BaseStrategyParams base_params;

    // Type-specific params (union-like access)
    std::optional<LatencyArbitrageParams> latency_arb_params;
    std::optional<TriangularArbitrageParams> triangular_arb_params;
    std::optional<StatisticalArbitrageParams> stat_arb_params;
    std::optional<OrderFlowImbalanceParams> order_flow_params;
    std::optional<MicrostructureAlphaParams> microstructure_params;
    std::optional<BasicMarketMakingParams> basic_mm_params;
    std::optional<AdaptiveMarketMakingParams> adaptive_mm_params;
    std::optional<InventoryMarketMakingParams> inventory_mm_params;
    std::optional<MultiExchangeMMParams> multi_exchange_mm_params;
    std::optional<VolatilityMarketMakingParams> vol_mm_params;

    /**
     * @brief Check if strategy is enabled
     */
    bool is_enabled() const { return base_params.enabled; }

    /**
     * @brief Get strategy name
     */
    const std::string& get_name() const { return base_params.name; }

    /**
     * @brief Get numeric parameter with default
     */
    double get_param(const std::string& name, double default_value = 0.0) const;

    /**
     * @brief Get string parameter with default
     */
    std::string get_param(const std::string& name, const std::string& default_value) const;

    /**
     * @brief Get bool parameter with default
     */
    bool get_param(const std::string& name, bool default_value) const;
};

/**
 * @brief Strategy configuration manager
 */
class StrategyConfigManager {
public:
    /**
     * @brief Get singleton instance
     */
    static StrategyConfigManager& instance();

    // Prevent copying
    StrategyConfigManager(const StrategyConfigManager&) = delete;
    StrategyConfigManager& operator=(const StrategyConfigManager&) = delete;

    /**
     * @brief Load strategy configurations from YAML file
     * @param filepath Path to YAML configuration file
     */
    void load(const std::string& filepath);

    /**
     * @brief Load strategy configurations from YAML node
     * @param node YAML node containing strategy configurations
     */
    void load_from_node(const YAML::Node& node);

    /**
     * @brief Get configuration for a specific strategy
     * @param strategy_id Strategy identifier
     * @return Pointer to strategy config or nullptr
     */
    const StrategyConfig* get(const std::string& strategy_id) const;

    /**
     * @brief Get all enabled strategies
     */
    std::vector<const StrategyConfig*> get_enabled_strategies() const;

    /**
     * @brief Get all strategies of a specific type
     */
    std::vector<const StrategyConfig*> get_strategies_by_type(StrategyType type) const;

    /**
     * @brief Get all strategy IDs
     */
    std::vector<std::string> get_all_strategy_ids() const;

    /**
     * @brief Check if strategy exists and is enabled
     */
    bool is_enabled(const std::string& strategy_id) const;

    /**
     * @brief Update strategy parameters at runtime
     */
    void update_params(const std::string& strategy_id, const YAML::Node& params);

    /**
     * @brief Reload configurations
     */
    void reload();

    /**
     * @brief Clear all configurations
     */
    void clear();

private:
    StrategyConfigManager() = default;

    /**
     * @brief Parse strategy configuration from YAML
     */
    StrategyConfig parse_strategy_config(const std::string& id, const YAML::Node& node);

    /**
     * @brief Parse base strategy parameters
     */
    BaseStrategyParams parse_base_params(const YAML::Node& node);

    /**
     * @brief Parse risk limits
     */
    StrategyRiskLimits parse_risk_limits(const YAML::Node& node);

    /**
     * @brief Parse trading pair configuration
     */
    StrategyPairConfig parse_pair_config(const YAML::Node& node);

    // Type-specific parsers
    LatencyArbitrageParams parse_latency_arb_params(const YAML::Node& node);
    TriangularArbitrageParams parse_triangular_arb_params(const YAML::Node& node);
    StatisticalArbitrageParams parse_stat_arb_params(const YAML::Node& node);
    OrderFlowImbalanceParams parse_order_flow_params(const YAML::Node& node);
    MicrostructureAlphaParams parse_microstructure_params(const YAML::Node& node);
    BasicMarketMakingParams parse_basic_mm_params(const YAML::Node& node);
    AdaptiveMarketMakingParams parse_adaptive_mm_params(const YAML::Node& node);
    InventoryMarketMakingParams parse_inventory_mm_params(const YAML::Node& node);
    MultiExchangeMMParams parse_multi_exchange_mm_params(const YAML::Node& node);
    VolatilityMarketMakingParams parse_vol_mm_params(const YAML::Node& node);

    std::unordered_map<std::string, StrategyConfig> configs_;
    std::string config_filepath_;
    mutable std::shared_mutex mutex_;
};

} // namespace config
} // namespace hft
