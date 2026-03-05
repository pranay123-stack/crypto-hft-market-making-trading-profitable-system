#pragma once

#include "../strategy_base.hpp"

#include <array>
#include <cmath>
#include <deque>
#include <unordered_map>
#include <vector>

namespace hft {
namespace strategies {

/**
 * Statistical Arbitrage Strategy
 *
 * Pairs trading based on cointegration and mean reversion.
 * Uses Kalman filter for dynamic hedge ratio estimation and
 * z-score based entry/exit signals.
 *
 * Key Features:
 * - Cointegration-based pair selection
 * - Kalman filter for adaptive hedge ratio
 * - Z-score calculation for spread
 * - Mean reversion signals
 * - Rolling window statistics
 * - Half-life based position sizing
 */
class StatisticalArbitrageStrategy : public StrategyBase {
public:
    // Configuration parameters
    struct Parameters {
        // Z-score thresholds
        double entry_zscore{2.0};         // Enter position when |z| > this
        double exit_zscore{0.5};          // Exit position when |z| < this
        double stop_zscore{4.0};          // Stop loss when |z| > this

        // Rolling window sizes
        size_t lookback_period{100};       // For spread statistics
        size_t correlation_window{50};     // For correlation calculation
        size_t cointegration_window{200};  // For cointegration test

        // Kalman filter parameters
        double kalman_process_noise{1e-5};       // Q - process noise
        double kalman_measurement_noise{1e-3};   // R - measurement noise
        double kalman_initial_variance{1.0};     // P0 - initial variance

        // Position sizing
        double max_position{1.0};
        double position_size_mult{1.0};   // Multiplier for calculated size

        // Pair configuration
        std::string leg1_symbol;          // First leg symbol
        std::string leg2_symbol;          // Second leg symbol
        std::string exchange{"binance"};

        // Timing
        int64_t min_holding_period_us{60000000};  // 60 seconds minimum hold
        int64_t max_holding_period_us{3600000000}; // 1 hour max hold

        // Risk parameters
        double max_spread_deviation{5.0};  // Max z-score deviation
        double min_correlation{0.7};       // Minimum correlation to trade
        double max_half_life{100};         // Maximum half-life in samples

        // Enable/disable features
        bool use_kalman_hedge{true};
        bool use_half_life_sizing{true};
        bool require_cointegration{false};
    };

    explicit StatisticalArbitrageStrategy(StrategyConfig config);
    ~StatisticalArbitrageStrategy() override = default;

    // ========================================================================
    // Lifecycle
    // ========================================================================
    bool initialize() override;
    void on_stop() override;
    void on_reset() override;

    // ========================================================================
    // Event Handlers
    // ========================================================================
    void on_market_data(const MarketData& data) override;
    void on_order_update(const OrderUpdate& update) override;
    void on_trade(const Trade& trade) override;

    // ========================================================================
    // Configuration
    // ========================================================================
    void set_parameters(const Parameters& params) { params_ = params; }
    [[nodiscard]] const Parameters& parameters() const noexcept { return params_; }

    /**
     * Set the pair to trade
     */
    void set_pair(const std::string& leg1, const std::string& leg2);

    // ========================================================================
    // Analytics
    // ========================================================================
    struct PairStats {
        double current_spread{0.0};
        double spread_mean{0.0};
        double spread_std{0.0};
        double current_zscore{0.0};
        double correlation{0.0};
        double hedge_ratio{0.0};
        double half_life{0.0};
        bool is_cointegrated{false};
        int64_t samples_collected{0};
    };

    [[nodiscard]] PairStats get_pair_stats() const;
    [[nodiscard]] double get_current_zscore() const;
    [[nodiscard]] double get_hedge_ratio() const;

private:
    // ========================================================================
    // Internal Types
    // ========================================================================

    struct KalmanState {
        double beta{0.0};           // Hedge ratio estimate
        double variance{1.0};       // Estimation variance

        // Kalman gains
        double process_noise{1e-5};
        double measurement_noise{1e-3};

        void predict() {
            // State prediction: beta stays same, variance increases
            variance += process_noise;
        }

        void update(double y1, double y2) {
            // Measurement update
            // y1 = beta * y2 + epsilon
            // Innovation: y1 - beta * y2
            double innovation = y1 - beta * y2;

            // Kalman gain
            double y2_sq = y2 * y2;
            double S = y2_sq * variance + measurement_noise;
            double K = variance * y2 / S;

            // Update
            beta += K * innovation;
            variance = (1.0 - K * y2) * variance;

            // Ensure variance stays positive
            variance = std::max(variance, 1e-10);
        }
    };

    struct RollingStats {
        std::deque<double> values;
        double sum{0.0};
        double sum_sq{0.0};
        size_t max_size{100};

        void add(double value) {
            values.push_back(value);
            sum += value;
            sum_sq += value * value;

            while (values.size() > max_size) {
                double old = values.front();
                values.pop_front();
                sum -= old;
                sum_sq -= old * old;
            }
        }

        [[nodiscard]] double mean() const {
            return values.empty() ? 0.0 : sum / values.size();
        }

        [[nodiscard]] double variance() const {
            if (values.size() < 2) return 0.0;
            double n = static_cast<double>(values.size());
            return (sum_sq - sum * sum / n) / (n - 1);
        }

        [[nodiscard]] double stddev() const {
            return std::sqrt(std::max(0.0, variance()));
        }

        [[nodiscard]] size_t size() const { return values.size(); }

        void clear() {
            values.clear();
            sum = 0.0;
            sum_sq = 0.0;
        }
    };

    struct LegData {
        std::string symbol;
        Price last_price{0.0};
        Price bid_price{0.0};
        Price ask_price{0.0};
        Timestamp timestamp{0};
        RollingStats prices;

        [[nodiscard]] bool is_valid() const {
            return last_price > 0 && bid_price > 0 && ask_price > 0;
        }
    };

    struct PairPosition {
        bool is_active{false};
        double leg1_quantity{0.0};   // Positive = long, negative = short
        double leg2_quantity{0.0};
        double entry_zscore{0.0};
        double entry_spread{0.0};
        Timestamp entry_time{0};
    };

    // ========================================================================
    // Core Logic Methods
    // ========================================================================

    /**
     * Update price data for a leg
     */
    void update_leg_data(const MarketData& data);

    /**
     * Calculate spread using current hedge ratio
     */
    double calculate_spread() const;

    /**
     * Calculate z-score of current spread
     */
    double calculate_zscore() const;

    /**
     * Update Kalman filter with new observations
     */
    void update_kalman_filter();

    /**
     * Calculate correlation between two legs
     */
    double calculate_correlation() const;

    /**
     * Estimate half-life of mean reversion using OLS
     */
    double estimate_half_life() const;

    /**
     * Check cointegration using Engle-Granger method
     * Returns ADF test statistic
     */
    double test_cointegration() const;

    /**
     * Generate trading signals based on z-score
     */
    void generate_signals();

    /**
     * Calculate position size based on half-life and other factors
     */
    double calculate_position_size() const;

    /**
     * Check if current position should be closed
     */
    bool should_close_position() const;

    /**
     * Open a new position
     */
    void open_position(bool long_spread);

    /**
     * Close the current position
     */
    void close_position(const std::string& reason);

    /**
     * Check holding period limits
     */
    void check_holding_limits();

    // ========================================================================
    // State
    // ========================================================================

    Parameters params_;

    // Leg data
    LegData leg1_;
    LegData leg2_;

    // Kalman filter state
    KalmanState kalman_;

    // Spread statistics
    RollingStats spread_stats_;

    // Spread history for cointegration/half-life
    RollingStats spread_history_;

    // Current position
    PairPosition position_;

    // Statistics
    double current_correlation_{0.0};
    double current_half_life_{0.0};
    bool is_cointegrated_{false};

    // Order tracking
    uint64_t pending_leg1_order_{0};
    uint64_t pending_leg2_order_{0};
    bool leg1_order_filled_{false};
    bool leg2_order_filled_{false};

    // Last signal time
    Timestamp last_signal_time_{0};

    // Minimum interval between signals
    static constexpr int64_t MIN_SIGNAL_INTERVAL_NS = 1000000000;  // 1 second
};

// ============================================================================
// Factory Registration
// ============================================================================

class StatisticalArbitrageFactory : public IStrategyFactory {
public:
    [[nodiscard]] std::unique_ptr<StrategyBase> create(const StrategyConfig& config) override {
        return std::make_unique<StatisticalArbitrageStrategy>(config);
    }

    [[nodiscard]] std::string_view strategy_type() const noexcept override {
        return "statistical_arbitrage";
    }
};

}  // namespace strategies
}  // namespace hft
