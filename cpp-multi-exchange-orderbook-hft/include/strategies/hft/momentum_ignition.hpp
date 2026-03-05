#pragma once

#include "../strategy_base.hpp"

#include <array>
#include <cmath>
#include <deque>
#include <unordered_map>

namespace hft {
namespace strategies {

/**
 * Momentum Ignition Strategy
 *
 * Detects and trades momentum patterns in order flow and price action.
 * Identifies moments when momentum is likely to continue or accelerate,
 * and enters positions for quick profits on momentum bursts.
 *
 * Key Features:
 * - Volume-weighted price momentum (VWAP momentum)
 * - Order book imbalance detection
 * - Trade flow analysis
 * - Momentum burst detection
 * - Quick entry/exit on momentum signals
 * - Adaptive thresholds based on volatility
 */
class MomentumIgnitionStrategy : public StrategyBase {
public:
    // Configuration parameters
    struct Parameters {
        // Momentum detection
        double momentum_threshold{0.001};     // 0.1% minimum momentum
        double acceleration_threshold{0.5};   // Momentum acceleration factor
        size_t momentum_window{20};           // Samples for momentum calculation
        size_t acceleration_window{5};        // Samples for acceleration

        // Volume analysis
        double volume_surge_threshold{2.0};   // Volume surge multiplier
        size_t volume_window{50};             // Samples for average volume
        double min_volume_ratio{1.5};         // Minimum volume vs average

        // Order book imbalance
        double imbalance_threshold{0.6};      // 60% imbalance to trigger
        size_t orderbook_levels{5};           // Levels to consider

        // Position management
        double max_position{1.0};
        double position_size_pct{0.1};        // 10% of max per signal
        int64_t max_hold_time_us{30000000};   // 30 seconds max hold
        int64_t min_hold_time_us{1000000};    // 1 second min hold

        // Risk management
        double stop_loss_pct{0.002};          // 0.2% stop loss
        double take_profit_pct{0.003};        // 0.3% take profit
        double trailing_stop_pct{0.001};      // 0.1% trailing stop

        // Signal quality
        double min_confidence{0.6};
        size_t min_samples{30};               // Minimum samples before trading

        // Exchange configuration
        std::string exchange{"binance"};

        // Adaptive parameters
        bool use_adaptive_thresholds{true};
        double volatility_window{100};        // Samples for volatility
        double volatility_scale{1.5};         // Scale thresholds by vol
    };

    explicit MomentumIgnitionStrategy(StrategyConfig config);
    ~MomentumIgnitionStrategy() override = default;

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

    // ========================================================================
    // Analytics
    // ========================================================================
    struct MomentumStats {
        double current_momentum{0.0};
        double momentum_acceleration{0.0};
        double volume_ratio{0.0};
        double orderbook_imbalance{0.0};
        double volatility{0.0};
        double vwap{0.0};
        int64_t trades_today{0};
        int64_t winning_trades{0};
        double total_pnl{0.0};
    };

    [[nodiscard]] MomentumStats get_momentum_stats(const std::string& symbol) const;
    [[nodiscard]] double get_current_momentum(const std::string& symbol) const;

private:
    // ========================================================================
    // Internal Types
    // ========================================================================

    struct PriceBar {
        Price open{0.0};
        Price high{0.0};
        Price low{0.0};
        Price close{0.0};
        Quantity volume{0.0};
        Timestamp timestamp{0};
        double vwap{0.0};
        int trade_count{0};

        void reset(Price price, Timestamp ts) {
            open = high = low = close = price;
            volume = 0.0;
            vwap = 0.0;
            timestamp = ts;
            trade_count = 0;
        }

        void update(Price price, Quantity qty) {
            high = std::max(high, price);
            low = std::min(low, price);
            close = price;
            volume += qty;
            vwap = (vwap * trade_count + price * qty) / (trade_count + qty);
            trade_count++;
        }
    };

    struct SymbolState {
        std::string symbol;
        ExchangeId exchange_id{0};

        // Price data
        Price last_price{0.0};
        Price bid_price{0.0};
        Price ask_price{0.0};
        Quantity bid_size{0.0};
        Quantity ask_size{0.0};

        // Historical data
        std::deque<PriceBar> bars;
        std::deque<double> returns;
        std::deque<double> volumes;
        std::deque<double> momenta;

        // Order book imbalance history
        std::deque<double> imbalances;

        // VWAP tracking
        double session_vwap{0.0};
        double session_volume{0.0};

        // Volatility
        double current_volatility{0.0};

        // Current bar
        PriceBar current_bar;
        int64_t bar_interval_ns{1000000000};  // 1 second bars

        // Position
        double position_qty{0.0};
        Price entry_price{0.0};
        Price highest_since_entry{0.0};
        Price lowest_since_entry{0.0};
        Timestamp entry_time{0};

        // Statistics
        int64_t total_trades{0};
        int64_t winning_trades{0};
        double total_pnl{0.0};

        [[nodiscard]] bool has_position() const { return std::abs(position_qty) > 1e-10; }
        [[nodiscard]] bool is_long() const { return position_qty > 0; }
        [[nodiscard]] bool is_short() const { return position_qty < 0; }
    };

    struct MomentumSignal {
        SignalType type{SignalType::NONE};
        double momentum{0.0};
        double acceleration{0.0};
        double volume_ratio{0.0};
        double imbalance{0.0};
        double confidence{0.0};
        std::string reason;
    };

    // ========================================================================
    // Core Logic Methods
    // ========================================================================

    /**
     * Update state from market data
     */
    void update_state(SymbolState& state, const MarketData& data);

    /**
     * Update bar data
     */
    void update_bars(SymbolState& state, const MarketData& data);

    /**
     * Calculate momentum metrics
     */
    double calculate_momentum(const SymbolState& state) const;

    /**
     * Calculate momentum acceleration
     */
    double calculate_acceleration(const SymbolState& state) const;

    /**
     * Calculate volume ratio vs average
     */
    double calculate_volume_ratio(const SymbolState& state) const;

    /**
     * Calculate order book imbalance
     */
    double calculate_imbalance(const MarketData& data) const;

    /**
     * Calculate realized volatility
     */
    double calculate_volatility(const SymbolState& state) const;

    /**
     * Detect momentum ignition signal
     */
    MomentumSignal detect_momentum_signal(const SymbolState& state) const;

    /**
     * Calculate signal confidence
     */
    double calculate_confidence(const MomentumSignal& signal, const SymbolState& state) const;

    /**
     * Check entry conditions
     */
    bool should_enter(const SymbolState& state, const MomentumSignal& signal) const;

    /**
     * Check exit conditions
     */
    bool should_exit(const SymbolState& state) const;

    /**
     * Calculate position size
     */
    double calculate_size(const SymbolState& state, const MomentumSignal& signal) const;

    /**
     * Execute entry signal
     */
    void execute_entry(SymbolState& state, const MomentumSignal& signal);

    /**
     * Execute exit
     */
    void execute_exit(SymbolState& state, const std::string& reason);

    /**
     * Check stop loss and take profit
     */
    void check_exit_conditions(SymbolState& state);

    /**
     * Get adaptive threshold based on volatility
     */
    double get_adaptive_threshold(double base_threshold, double volatility) const;

    // ========================================================================
    // State
    // ========================================================================

    Parameters params_;

    // Per-symbol state
    std::unordered_map<std::string, SymbolState> states_;

    // Order tracking
    std::unordered_map<uint64_t, std::string> order_to_symbol_;

    // Minimum interval between signals per symbol
    std::unordered_map<std::string, Timestamp> last_signal_time_;

    static constexpr int64_t MIN_SIGNAL_INTERVAL_NS = 100000000;  // 100ms
};

// ============================================================================
// Factory Registration
// ============================================================================

class MomentumIgnitionFactory : public IStrategyFactory {
public:
    [[nodiscard]] std::unique_ptr<StrategyBase> create(const StrategyConfig& config) override {
        return std::make_unique<MomentumIgnitionStrategy>(config);
    }

    [[nodiscard]] std::string_view strategy_type() const noexcept override {
        return "momentum_ignition";
    }
};

}  // namespace strategies
}  // namespace hft
