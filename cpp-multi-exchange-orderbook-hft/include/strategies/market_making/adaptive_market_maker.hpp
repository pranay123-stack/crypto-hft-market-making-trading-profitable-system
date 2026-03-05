#pragma once

/**
 * @file adaptive_market_maker.hpp
 * @brief Adaptive Market Making Strategy
 *
 * A sophisticated market making strategy that dynamically adjusts parameters based on:
 * - Market volatility (wider spreads in volatile markets)
 * - Trading volume (adjust sizes based on liquidity)
 * - Order book imbalance (skew quotes based on pressure)
 * - Recent fill rate (optimize quote placement)
 * - Time-of-day patterns (adapt to market regimes)
 *
 * This strategy continuously learns from market conditions and adapts in real-time.
 */

#include "basic_market_maker.hpp"
#include <array>
#include <numeric>

namespace hft {
namespace strategies {
namespace market_making {

/**
 * @brief Volatility estimation methods
 */
enum class VolatilityMethod : uint8_t {
    EWMA = 0,           // Exponentially Weighted Moving Average
    PARKINSON = 1,      // Parkinson's Range-based estimator
    GARMAN_KLASS = 2,   // Garman-Klass estimator (uses OHLC)
    YANG_ZHANG = 3,     // Yang-Zhang estimator (most accurate)
    REALIZED = 4        // Simple realized volatility
};

/**
 * @brief Configuration for Adaptive Market Maker
 */
struct AdaptiveMMConfig : BasicMMConfig {
    // Volatility parameters
    VolatilityMethod volatility_method = VolatilityMethod::EWMA;
    double volatility_lookback_seconds = 300.0;     // 5 minute lookback
    double volatility_decay_factor = 0.94;          // EWMA decay (approx 1-min half-life)
    double min_volatility_estimate = 0.0001;        // 0.01% minimum vol
    double max_volatility_estimate = 0.10;          // 10% maximum vol

    // Spread adjustment
    double spread_volatility_multiplier = 2.0;      // Spread = base * (1 + mult * vol)
    double spread_imbalance_multiplier = 0.5;       // Spread adjustment for imbalance
    double spread_adjustment_speed = 0.1;           // How fast to adjust spread (0-1)

    // Volume adaptation
    double volume_lookback_seconds = 60.0;          // Volume calculation window
    double volume_size_multiplier = 0.1;            // Size = base * vol_ratio * mult
    double min_volume_ratio = 0.1;                  // Minimum volume ratio for sizing
    double max_volume_ratio = 10.0;                 // Maximum volume ratio

    // Fill rate optimization
    double target_fill_rate = 0.3;                  // Target 30% fill rate
    double fill_rate_adjustment_factor = 0.1;       // How much to adjust per period
    double fill_rate_lookback_seconds = 300.0;      // Fill rate calculation window

    // Order book analysis
    int orderbook_depth_levels = 10;                // Levels to analyze
    double imbalance_threshold = 0.3;               // Significant imbalance threshold
    double imbalance_skew_factor = 0.2;             // How much to skew for imbalance

    // Regime detection
    bool enable_regime_detection = true;
    double regime_lookback_seconds = 3600.0;        // 1 hour lookback
    double high_volatility_threshold = 0.02;        // Above this = high vol regime
    double low_volatility_threshold = 0.005;        // Below this = low vol regime

    // Momentum filter
    bool enable_momentum_filter = true;
    double momentum_lookback_seconds = 60.0;
    double momentum_threshold = 0.001;              // 0.1% move to detect momentum
    double momentum_spread_multiplier = 1.5;        // Widen spread during momentum
};

/**
 * @brief Market regime classification
 */
enum class MarketRegime : uint8_t {
    UNKNOWN = 0,
    LOW_VOLATILITY = 1,
    NORMAL = 2,
    HIGH_VOLATILITY = 3,
    TRENDING_UP = 4,
    TRENDING_DOWN = 5,
    MEAN_REVERTING = 6
};

/**
 * @brief Volatility statistics
 */
struct VolatilityStats {
    double current_volatility = 0.0;        // Current volatility estimate
    double ewma_volatility = 0.0;           // EWMA volatility
    double realized_volatility = 0.0;       // Realized volatility
    double high_low_volatility = 0.0;       // High-low based volatility
    double volatility_of_volatility = 0.0;  // Vol-of-vol
    double volatility_percentile = 0.5;     // Current vol vs historical
    Timestamp last_update{0};
};

/**
 * @brief Order book analysis results
 */
struct OrderBookAnalysis {
    double bid_depth = 0.0;                 // Total bid liquidity
    double ask_depth = 0.0;                 // Total ask liquidity
    double imbalance = 0.0;                 // (bid - ask) / (bid + ask), -1 to 1
    double weighted_mid_price = 0.0;        // Volume-weighted mid
    double microprice = 0.0;                // Imbalance-adjusted mid
    double bid_slope = 0.0;                 // Bid depth curve slope
    double ask_slope = 0.0;                 // Ask depth curve slope
    int bid_levels = 0;
    int ask_levels = 0;
};

/**
 * @brief Adaptive parameters computed in real-time
 */
struct AdaptiveParams {
    double adjusted_spread_bps = 10.0;      // Current adjusted spread
    double adjusted_bid_size = 0.1;         // Current bid size
    double adjusted_ask_size = 0.1;         // Current ask size
    double quote_offset = 0.0;              // Quote center offset from mid
    double urgency_factor = 1.0;            // Urgency multiplier
    MarketRegime current_regime = MarketRegime::NORMAL;
    Timestamp last_computation{0};
};

/**
 * @brief Price/Return sample for volatility calculation
 */
struct PriceSample {
    Price price = 0.0;
    double return_value = 0.0;
    Timestamp timestamp{0};
};

/**
 * @brief Adaptive Market Maker Strategy
 *
 * Extends BasicMarketMaker with dynamic parameter adaptation:
 * - Real-time volatility estimation using multiple methods
 * - Order book imbalance analysis
 * - Fill rate optimization
 * - Market regime detection
 * - Momentum filtering
 */
class AdaptiveMarketMaker : public BasicMarketMaker {
public:
    /**
     * @brief Construct Adaptive Market Maker
     * @param config Strategy configuration
     * @param mm_config Adaptive MM specific configuration
     */
    explicit AdaptiveMarketMaker(StrategyConfig config, AdaptiveMMConfig mm_config = {});

    ~AdaptiveMarketMaker() override = default;

    // Lifecycle overrides
    bool initialize() override;
    void reset() override;

    // Event handlers
    void on_market_data(const MarketData& data) override;
    void on_order_update(const OrderUpdate& update) override;
    void on_trade(const Trade& trade) override;

    // =========================================================================
    // Volatility Estimation
    // =========================================================================

    /**
     * @brief Update volatility estimate with new price
     * @param price New price observation
     * @param timestamp Observation timestamp
     */
    void update_volatility(Price price, Timestamp timestamp);

    /**
     * @brief Calculate EWMA volatility
     * @return EWMA volatility estimate
     */
    [[nodiscard]] double calculate_ewma_volatility() const;

    /**
     * @brief Calculate Parkinson volatility (high-low based)
     * @return Parkinson volatility estimate
     */
    [[nodiscard]] double calculate_parkinson_volatility() const;

    /**
     * @brief Calculate realized volatility
     * @return Realized volatility
     */
    [[nodiscard]] double calculate_realized_volatility() const;

    /**
     * @brief Get current volatility estimate
     * @return Current volatility
     */
    [[nodiscard]] double current_volatility() const noexcept {
        return volatility_stats_.current_volatility;
    }

    // =========================================================================
    // Order Book Analysis
    // =========================================================================

    /**
     * @brief Analyze order book for imbalance and liquidity
     * @param orderbook Order book snapshot
     * @return Analysis results
     */
    [[nodiscard]] OrderBookAnalysis analyze_orderbook(const OrderBookSnapshot& orderbook) const;

    /**
     * @brief Calculate microprice from order book
     * @param orderbook Order book snapshot
     * @return Microprice estimate
     */
    [[nodiscard]] Price calculate_microprice(const OrderBookSnapshot& orderbook) const;

    // =========================================================================
    // Adaptive Parameter Calculation
    // =========================================================================

    /**
     * @brief Compute adaptive spread based on market conditions
     * @return Adjusted spread in basis points
     */
    [[nodiscard]] double compute_adaptive_spread() const;

    /**
     * @brief Compute adaptive order sizes
     * @return pair of (bid_size, ask_size)
     */
    [[nodiscard]] std::pair<Quantity, Quantity> compute_adaptive_sizes() const;

    /**
     * @brief Detect current market regime
     * @return Current market regime
     */
    [[nodiscard]] MarketRegime detect_regime() const;

    /**
     * @brief Calculate momentum indicator
     * @return Momentum value (-1 to 1, negative = down, positive = up)
     */
    [[nodiscard]] double calculate_momentum() const;

    /**
     * @brief Update all adaptive parameters
     */
    void update_adaptive_params();

    // =========================================================================
    // Quote Calculation Override
    // =========================================================================

    /**
     * @brief Calculate quotes using adaptive parameters
     * @param mid_price Current mid price
     * @return pair of (bid_price, ask_price)
     */
    [[nodiscard]] std::pair<Price, Price> calculate_quotes(Price mid_price) const override;

    /**
     * @brief Calculate order size using adaptive sizing
     * @param side Order side
     * @return Adapted order quantity
     */
    [[nodiscard]] Quantity calculate_order_size(OrderSide side) const override;

    // =========================================================================
    // Fill Rate Optimization
    // =========================================================================

    /**
     * @brief Calculate current fill rate
     * @return Fill rate (0 to 1)
     */
    [[nodiscard]] double calculate_fill_rate() const;

    /**
     * @brief Optimize spread for target fill rate
     * @return Spread adjustment factor
     */
    [[nodiscard]] double optimize_for_fill_rate() const;

    // =========================================================================
    // Accessors
    // =========================================================================

    [[nodiscard]] const AdaptiveMMConfig& adaptive_config() const noexcept {
        return static_cast<const AdaptiveMMConfig&>(mm_config_);
    }

    [[nodiscard]] const VolatilityStats& volatility_stats() const noexcept {
        return volatility_stats_;
    }

    [[nodiscard]] const OrderBookAnalysis& last_orderbook_analysis() const noexcept {
        return last_ob_analysis_;
    }

    [[nodiscard]] const AdaptiveParams& adaptive_params() const noexcept {
        return adaptive_params_;
    }

    [[nodiscard]] MarketRegime current_regime() const noexcept {
        return adaptive_params_.current_regime;
    }

protected:
    void on_reset() override;

private:
    // =========================================================================
    // Internal State
    // =========================================================================

    AdaptiveMMConfig adaptive_config_;
    VolatilityStats volatility_stats_;
    OrderBookAnalysis last_ob_analysis_;
    AdaptiveParams adaptive_params_;

    // Price history for volatility calculation
    std::deque<PriceSample> price_history_;
    static constexpr size_t MAX_PRICE_HISTORY = 10000;

    // High/Low tracking for Parkinson volatility
    struct HighLowBar {
        Price high = 0.0;
        Price low = std::numeric_limits<double>::max();
        Price open = 0.0;
        Price close = 0.0;
        Timestamp start_time{0};
    };
    std::deque<HighLowBar> high_low_bars_;
    HighLowBar current_bar_;
    static constexpr int64_t BAR_DURATION_MS = 60000;  // 1-minute bars

    // Fill tracking for fill rate calculation
    struct FillRecord {
        Timestamp time{0};
        bool was_filled = false;
    };
    std::deque<FillRecord> fill_history_;

    // Volume tracking
    struct VolumeRecord {
        Timestamp time{0};
        double volume = 0.0;
    };
    std::deque<VolumeRecord> volume_history_;
    double baseline_volume_ = 0.0;

    // Squared returns for EWMA variance
    double ewma_variance_ = 0.0;
    Price last_price_for_vol_ = 0.0;
};

/**
 * @brief Factory for creating Adaptive Market Maker instances
 */
class AdaptiveMarketMakerFactory : public IStrategyFactory {
public:
    [[nodiscard]] std::unique_ptr<StrategyBase> create(const StrategyConfig& config) override {
        AdaptiveMMConfig mm_config;

        // Load base config
        mm_config.target_spread_bps = config.get_param("target_spread_bps", 10.0);
        mm_config.base_order_size = config.get_param("base_order_size", 0.1);
        mm_config.max_position = config.get_param("max_position", 1.0);

        // Load adaptive-specific config
        mm_config.volatility_lookback_seconds = config.get_param("volatility_lookback_seconds", 300.0);
        mm_config.volatility_decay_factor = config.get_param("volatility_decay_factor", 0.94);
        mm_config.spread_volatility_multiplier = config.get_param("spread_volatility_multiplier", 2.0);
        mm_config.target_fill_rate = config.get_param("target_fill_rate", 0.3);
        mm_config.enable_regime_detection = config.get_param("enable_regime_detection", true);
        mm_config.enable_momentum_filter = config.get_param("enable_momentum_filter", true);
        mm_config.imbalance_skew_factor = config.get_param("imbalance_skew_factor", 0.2);

        return std::make_unique<AdaptiveMarketMaker>(config, mm_config);
    }

    [[nodiscard]] std::string_view strategy_type() const noexcept override {
        return "adaptive_market_maker";
    }
};

}  // namespace market_making
}  // namespace strategies
}  // namespace hft
