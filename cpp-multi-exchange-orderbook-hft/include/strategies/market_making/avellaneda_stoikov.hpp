#pragma once

/**
 * @file avellaneda_stoikov.hpp
 * @brief Avellaneda-Stoikov Optimal Market Making Strategy
 *
 * Implementation of the seminal Avellaneda-Stoikov (2008) optimal market making model.
 *
 * The model computes optimal bid/ask quotes based on:
 * - Reservation price: r(s,q,t) = s - q * gamma * sigma^2 * (T - t)
 * - Optimal spread: delta(q,t) = gamma * sigma^2 * (T - t) + (2/gamma) * ln(1 + gamma/k)
 *
 * Where:
 *   s = mid price
 *   q = inventory (positive = long)
 *   gamma = inventory risk aversion parameter
 *   sigma = volatility
 *   T = terminal time
 *   t = current time
 *   k = order arrival intensity parameter
 *
 * The strategy minimizes expected terminal wealth variance while maximizing expected PnL.
 *
 * Reference:
 * Avellaneda, M., & Stoikov, S. (2008). High-frequency trading in a limit order book.
 * Quantitative Finance, 8(3), 217-224.
 */

#include "basic_market_maker.hpp"
#include <cmath>

namespace hft {
namespace strategies {
namespace market_making {

/**
 * @brief Intensity function type for order arrivals
 */
enum class IntensityFunction : uint8_t {
    CONSTANT = 0,           // Lambda = k (constant arrival rate)
    LINEAR_SPREAD = 1,      // Lambda = A * exp(-k * spread)
    EXPONENTIAL = 2,        // Lambda = A * exp(-k * (price - fair))
    POWER_LAW = 3           // Lambda = A / (1 + k * spread)^alpha
};

/**
 * @brief Configuration for Avellaneda-Stoikov Market Maker
 */
struct AvellanedaStoikovConfig : BasicMMConfig {
    // Core A-S parameters
    double gamma = 0.1;                     // Risk aversion parameter (higher = more risk-averse)
    double sigma = 0.02;                    // Volatility (annualized, will be converted)
    double T_seconds = 3600.0;              // Trading horizon in seconds (1 hour)
    double k = 1.5;                         // Order arrival intensity parameter

    // Intensity function parameters
    IntensityFunction intensity_func = IntensityFunction::EXPONENTIAL;
    double intensity_A = 1.0;               // Intensity scaling factor
    double intensity_alpha = 1.5;           // Power law exponent

    // Volatility estimation
    bool use_realized_volatility = true;
    double volatility_lookback_seconds = 300.0;  // 5 minute lookback
    double volatility_ewma_decay = 0.94;
    double min_sigma = 0.001;               // Minimum volatility (0.1%)
    double max_sigma = 0.50;                // Maximum volatility (50%)

    // Risk limits
    double max_inventory_units = 10.0;      // Maximum inventory in units
    double inventory_penalty_mult = 1.0;    // Additional inventory penalty
    bool enable_inventory_limits = true;

    // Quote computation
    double quote_update_interval_ms = 100.0;    // Update quotes every 100ms
    bool use_microprice = true;                  // Use microprice instead of mid
    double min_spread_bps = 2.0;                 // Minimum spread floor
    double max_spread_bps = 200.0;               // Maximum spread cap

    // Terminal handling
    bool enable_end_of_period = true;
    double end_of_period_urgency = 2.0;     // Spread reduction near T
    double liquidation_threshold = 0.95;    // Start liquidating at 95% of T

    // Extensions
    bool enable_drift_adjustment = false;   // Adjust for price drift
    double drift_lookback_seconds = 60.0;
    bool enable_jump_detection = false;     // Detect and handle price jumps
    double jump_threshold_sigma = 3.0;      // Jump = move > 3 sigma
};

/**
 * @brief Model state for A-S computation
 */
struct ASModelState {
    // Time variables
    double t = 0.0;                     // Current time (0 to T)
    double T = 3600.0;                  // Terminal time
    double tau = 3600.0;                // Time remaining (T - t)

    // Price/Inventory
    Price mid_price = 0.0;
    Price microprice = 0.0;
    double inventory = 0.0;             // Current inventory (q)

    // Volatility
    double sigma = 0.02;                // Current volatility estimate
    double sigma_squared = 0.0004;      // sigma^2

    // Computed values
    Price reservation_price = 0.0;      // r(s,q,t)
    double optimal_spread = 0.0;        // delta(q,t)
    Price optimal_bid = 0.0;
    Price optimal_ask = 0.0;

    // Intensity
    double lambda_bid = 0.0;            // Order arrival intensity for bid
    double lambda_ask = 0.0;            // Order arrival intensity for ask

    // Risk metrics
    double inventory_risk = 0.0;        // q * gamma * sigma^2 * tau
    double spread_risk_component = 0.0; // gamma * sigma^2 * tau
    double spread_intensity_component = 0.0;  // (2/gamma) * ln(1 + gamma/k)
};

/**
 * @brief Volatility estimation state
 */
struct VolatilityEstimate {
    double realized_vol = 0.0;          // Realized volatility
    double ewma_vol = 0.0;              // EWMA volatility
    double parkinson_vol = 0.0;         // High-low volatility
    double current_vol = 0.0;           // Current estimate used
    double vol_of_vol = 0.0;            // Volatility of volatility
    int sample_count = 0;
    Timestamp last_update{0};
};

/**
 * @brief A-S strategy performance metrics
 */
struct ASMetrics {
    double total_pnl = 0.0;
    double inventory_pnl = 0.0;         // PnL from inventory
    double spread_pnl = 0.0;            // PnL from spread capture
    double average_spread_captured = 0.0;
    double average_inventory = 0.0;
    double max_inventory = 0.0;
    double inventory_time_weighted = 0.0;
    int periods_completed = 0;
    double sharpe_ratio = 0.0;
    double sortino_ratio = 0.0;

    // Fill analysis
    int bid_fills = 0;
    int ask_fills = 0;
    double average_fill_rate = 0.0;
    double expected_fill_rate = 0.0;    // From model
};

/**
 * @brief Avellaneda-Stoikov Market Maker Strategy
 *
 * Implements the optimal market making model with:
 * - Reservation price calculation
 * - Optimal spread computation
 * - Real-time volatility estimation
 * - Order arrival intensity modeling
 * - Terminal period handling
 */
class AvellanedaStoikov : public BasicMarketMaker {
public:
    /**
     * @brief Construct Avellaneda-Stoikov Market Maker
     * @param config Strategy configuration
     * @param as_config A-S specific configuration
     */
    explicit AvellanedaStoikov(StrategyConfig config, AvellanedaStoikovConfig as_config = {});

    ~AvellanedaStoikov() override = default;

    // Lifecycle overrides
    bool initialize() override;
    bool start() override;
    void reset() override;

    // Event handlers
    void on_market_data(const MarketData& data) override;
    void on_order_update(const OrderUpdate& update) override;
    void on_trade(const Trade& trade) override;

    // =========================================================================
    // Core A-S Model Implementation
    // =========================================================================

    /**
     * @brief Calculate reservation price
     *
     * r(s,q,t) = s - q * gamma * sigma^2 * (T - t)
     *
     * The reservation price is the "fair" price adjusted for inventory risk.
     * When long (q > 0), reservation price is below mid (want to sell).
     * When short (q < 0), reservation price is above mid (want to buy).
     *
     * @param mid_price Current mid price (s)
     * @param inventory Current inventory (q)
     * @param time_remaining Time to horizon (T - t)
     * @return Reservation price
     */
    [[nodiscard]] Price calculate_reservation_price(
        Price mid_price,
        double inventory,
        double time_remaining) const;

    /**
     * @brief Calculate optimal spread
     *
     * delta(q,t) = gamma * sigma^2 * (T - t) + (2/gamma) * ln(1 + gamma/k)
     *
     * The spread has two components:
     * 1. Risk component: gamma * sigma^2 * tau (increases with volatility and time)
     * 2. Intensity component: (2/gamma) * ln(1 + gamma/k) (based on order arrivals)
     *
     * @param time_remaining Time to horizon (T - t)
     * @return Optimal spread (full spread, not half)
     */
    [[nodiscard]] double calculate_optimal_spread(double time_remaining) const;

    /**
     * @brief Calculate order arrival intensity
     *
     * For exponential intensity: Lambda(delta) = A * exp(-k * delta)
     *
     * @param spread_half Half spread (distance from mid)
     * @return Arrival intensity
     */
    [[nodiscard]] double calculate_intensity(double spread_half) const;

    /**
     * @brief Calculate optimal quotes given current state
     * @return pair of (bid_price, ask_price)
     */
    [[nodiscard]] std::pair<Price, Price> calculate_optimal_quotes() const;

    /**
     * @brief Update model state with current market data
     * @param data Market data
     */
    void update_model_state(const MarketData& data);

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
     * @brief Calculate realized volatility over lookback period
     * @return Realized volatility (annualized)
     */
    [[nodiscard]] double calculate_realized_volatility() const;

    /**
     * @brief Calculate EWMA volatility
     * @param new_return New return observation
     * @return Updated EWMA volatility
     */
    [[nodiscard]] double calculate_ewma_volatility(double new_return);

    /**
     * @brief Get current volatility estimate
     * @return Current sigma
     */
    [[nodiscard]] double current_sigma() const noexcept {
        return model_state_.sigma;
    }

    // =========================================================================
    // Time Management
    // =========================================================================

    /**
     * @brief Reset trading period
     */
    void reset_period();

    /**
     * @brief Get time remaining in period
     * @return Time remaining in seconds
     */
    [[nodiscard]] double time_remaining() const noexcept {
        return model_state_.tau;
    }

    /**
     * @brief Check if in liquidation period
     * @return true if should start liquidating
     */
    [[nodiscard]] bool in_liquidation_period() const;

    /**
     * @brief Calculate urgency factor based on time remaining
     * @return Urgency multiplier (1.0 = normal, >1.0 = urgent)
     */
    [[nodiscard]] double calculate_urgency() const;

    // =========================================================================
    // Drift and Jump Handling
    // =========================================================================

    /**
     * @brief Estimate price drift
     * @return Estimated drift (per second)
     */
    [[nodiscard]] double estimate_drift() const;

    /**
     * @brief Detect price jump
     * @param price_move Price movement
     * @return true if jump detected
     */
    [[nodiscard]] bool detect_jump(double price_move) const;

    /**
     * @brief Adjust quotes for detected drift
     * @param bid Original bid
     * @param ask Original ask
     * @return Adjusted (bid, ask)
     */
    [[nodiscard]] std::pair<Price, Price> adjust_for_drift(
        Price bid, Price ask) const;

    // =========================================================================
    // Quote Calculation Override
    // =========================================================================

    /**
     * @brief Calculate A-S optimal quotes
     * @param mid_price Current mid price
     * @return pair of (bid_price, ask_price)
     */
    [[nodiscard]] std::pair<Price, Price> calculate_quotes(Price mid_price) const override;

    // =========================================================================
    // Accessors
    // =========================================================================

    [[nodiscard]] const AvellanedaStoikovConfig& as_config() const noexcept {
        return static_cast<const AvellanedaStoikovConfig&>(mm_config_);
    }

    [[nodiscard]] const ASModelState& model_state() const noexcept {
        return model_state_;
    }

    [[nodiscard]] const VolatilityEstimate& volatility_estimate() const noexcept {
        return volatility_estimate_;
    }

    [[nodiscard]] const ASMetrics& as_metrics() const noexcept {
        return as_metrics_;
    }

    /**
     * @brief Get gamma (risk aversion)
     */
    [[nodiscard]] double gamma() const noexcept {
        return as_config_.gamma;
    }

    /**
     * @brief Set gamma (risk aversion) - allows dynamic adjustment
     * @param new_gamma New gamma value
     */
    void set_gamma(double new_gamma);

    /**
     * @brief Get k (intensity parameter)
     */
    [[nodiscard]] double k() const noexcept {
        return as_config_.k;
    }

protected:
    void on_reset() override;

private:
    // =========================================================================
    // Internal Methods
    // =========================================================================

    /**
     * @brief Convert volatility between time scales
     * @param vol Volatility
     * @param from_seconds Source time scale in seconds
     * @param to_seconds Target time scale in seconds
     * @return Converted volatility
     */
    [[nodiscard]] static double convert_volatility(
        double vol, double from_seconds, double to_seconds);

    /**
     * @brief Compute all model components
     */
    void compute_model();

    /**
     * @brief Update A-S specific metrics
     */
    void update_as_metrics();

    /**
     * @brief Handle end of period
     */
    void handle_period_end();

    // =========================================================================
    // Internal State
    // =========================================================================

    AvellanedaStoikovConfig as_config_;
    ASModelState model_state_;
    VolatilityEstimate volatility_estimate_;
    ASMetrics as_metrics_;

    // Price history for volatility
    struct PriceReturn {
        Price price = 0.0;
        double log_return = 0.0;
        Timestamp timestamp{0};
    };
    std::deque<PriceReturn> price_returns_;
    static constexpr size_t MAX_RETURNS = 10000;

    // EWMA state
    double ewma_variance_ = 0.0;
    Price last_price_for_vol_ = 0.0;
    Timestamp last_vol_update_{0};

    // Period tracking
    Timestamp period_start_time_{0};
    double period_start_inventory_ = 0.0;
    int periods_completed_ = 0;

    // Drift estimation
    std::deque<std::pair<Price, Timestamp>> drift_samples_;

    // Jump detection
    double recent_max_move_ = 0.0;
    bool jump_detected_ = false;

    // Constants
    static constexpr double SECONDS_PER_YEAR = 365.25 * 24.0 * 3600.0;
    static constexpr double MIN_TAU = 1.0;  // Minimum 1 second remaining
};

/**
 * @brief Factory for creating Avellaneda-Stoikov Market Maker instances
 */
class AvellanedaStoikovFactory : public IStrategyFactory {
public:
    [[nodiscard]] std::unique_ptr<StrategyBase> create(const StrategyConfig& config) override {
        AvellanedaStoikovConfig as_config;

        // Load base config
        as_config.target_spread_bps = config.get_param("target_spread_bps", 10.0);
        as_config.base_order_size = config.get_param("base_order_size", 0.1);
        as_config.max_position = config.get_param("max_position", 1.0);

        // Load A-S specific config
        as_config.gamma = config.get_param("gamma", 0.1);
        as_config.sigma = config.get_param("sigma", 0.02);
        as_config.T_seconds = config.get_param("T_seconds", 3600.0);
        as_config.k = config.get_param("k", 1.5);
        as_config.volatility_lookback_seconds = config.get_param("volatility_lookback_seconds", 300.0);
        as_config.use_realized_volatility = config.get_param("use_realized_volatility", true);
        as_config.use_microprice = config.get_param("use_microprice", true);
        as_config.enable_end_of_period = config.get_param("enable_end_of_period", true);
        as_config.enable_drift_adjustment = config.get_param("enable_drift_adjustment", false);

        return std::make_unique<AvellanedaStoikov>(config, as_config);
    }

    [[nodiscard]] std::string_view strategy_type() const noexcept override {
        return "avellaneda_stoikov";
    }
};

}  // namespace market_making
}  // namespace strategies
}  // namespace hft
