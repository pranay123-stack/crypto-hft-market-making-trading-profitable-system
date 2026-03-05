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
 * Order Flow Imbalance Strategy
 *
 * Analyzes bid/ask imbalance in the order book and trade flow
 * to generate predictive signals based on microstructure patterns.
 *
 * Key Features:
 * - Order book imbalance analysis (multi-level)
 * - Trade flow toxicity (VPIN-like metric)
 * - Volume-synchronized probability of informed trading
 * - Order book pressure indicators
 * - Microstructure-based directional signals
 * - Adaptive thresholds based on market conditions
 */
class OrderFlowImbalanceStrategy : public StrategyBase {
public:
    // Configuration parameters
    struct Parameters {
        // Order book analysis
        size_t orderbook_levels{10};          // Levels to analyze
        double level_decay{0.8};              // Weight decay per level
        double imbalance_threshold{0.3};      // Threshold for significant imbalance

        // VPIN parameters
        size_t vpin_buckets{50};              // Number of volume buckets
        double bucket_size{1000.0};           // Volume per bucket (in base currency)
        double vpin_threshold{0.7};           // VPIN threshold for toxicity

        // Trade flow analysis
        size_t trade_window{100};             // Trades to analyze
        double trade_imbalance_threshold{0.6}; // Trade direction imbalance
        double aggressor_threshold{0.65};     // Aggressive order threshold

        // Pressure indicators
        size_t pressure_window{20};           // Samples for pressure calculation
        double pressure_threshold{0.4};       // Pressure imbalance threshold

        // Signal generation
        double min_confidence{0.6};
        double signal_decay{0.95};            // Signal strength decay
        size_t signal_lookback{10};           // Recent signals to consider

        // Position management
        double max_position{1.0};
        double position_size_pct{0.05};       // 5% of max per signal
        int64_t max_hold_time_us{60000000};   // 60 seconds
        int64_t min_signal_interval_us{500000}; // 500ms between signals

        // Risk management
        double stop_loss_pct{0.003};          // 0.3%
        double take_profit_pct{0.004};        // 0.4%
        double max_spread_bps{10.0};          // Max spread to trade

        // Exchange configuration
        std::string exchange{"binance"};

        // Analysis modes
        bool use_vpin{true};
        bool use_orderbook_pressure{true};
        bool use_trade_flow{true};
        bool use_multi_level_imbalance{true};
    };

    explicit OrderFlowImbalanceStrategy(StrategyConfig config);
    ~OrderFlowImbalanceStrategy() override = default;

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
    struct FlowStats {
        // Order book metrics
        double orderbook_imbalance{0.0};      // -1 to 1
        double multi_level_imbalance{0.0};    // Weighted multi-level
        double bid_pressure{0.0};
        double ask_pressure{0.0};

        // Trade flow metrics
        double trade_imbalance{0.0};
        double aggressor_ratio{0.0};          // Ratio of aggressive buys
        double vpin{0.0};                     // Volume-sync probability

        // Combined signals
        double buy_signal_strength{0.0};
        double sell_signal_strength{0.0};
        double net_signal{0.0};

        // Statistics
        int64_t total_signals{0};
        int64_t successful_signals{0};
        double total_pnl{0.0};
    };

    [[nodiscard]] FlowStats get_flow_stats(const std::string& symbol) const;
    [[nodiscard]] double get_vpin(const std::string& symbol) const;
    [[nodiscard]] double get_orderbook_imbalance(const std::string& symbol) const;

private:
    // ========================================================================
    // Internal Types
    // ========================================================================

    struct TradeRecord {
        Price price{0.0};
        Quantity quantity{0.0};
        OrderSide aggressor_side{OrderSide::BUY};
        Timestamp timestamp{0};
        bool is_buy{true};  // Based on tick rule or exchange flag
    };

    struct VPINBucket {
        Quantity buy_volume{0.0};
        Quantity sell_volume{0.0};
        Timestamp start_time{0};
        Timestamp end_time{0};
        int trade_count{0};

        [[nodiscard]] double imbalance() const {
            Quantity total = buy_volume + sell_volume;
            return total > 0 ? (buy_volume - sell_volume) / total : 0.0;
        }

        [[nodiscard]] Quantity total_volume() const {
            return buy_volume + sell_volume;
        }
    };

    struct OrderBookLevel {
        Price price{0.0};
        Quantity quantity{0.0};
        Quantity quantity_change{0.0};  // Change since last update
        int order_count{0};
    };

    struct PressurePoint {
        double bid_pressure{0.0};
        double ask_pressure{0.0};
        double net_pressure{0.0};
        Timestamp timestamp{0};
    };

    struct SymbolState {
        std::string symbol;
        ExchangeId exchange_id{0};

        // Current prices
        Price last_price{0.0};
        Price bid_price{0.0};
        Price ask_price{0.0};
        Price prev_mid_price{0.0};

        // Order book
        std::vector<OrderBookLevel> bids;
        std::vector<OrderBookLevel> asks;
        std::vector<OrderBookLevel> prev_bids;
        std::vector<OrderBookLevel> prev_asks;

        // Trade history
        std::deque<TradeRecord> trades;

        // VPIN buckets
        std::deque<VPINBucket> vpin_buckets;
        VPINBucket current_bucket;

        // Pressure history
        std::deque<PressurePoint> pressure_history;

        // Signal history
        std::deque<double> signal_history;

        // Position
        double position_qty{0.0};
        Price entry_price{0.0};
        Timestamp entry_time{0};

        // Statistics
        int64_t total_signals{0};
        int64_t successful_signals{0};
        double total_pnl{0.0};

        [[nodiscard]] bool has_position() const { return std::abs(position_qty) > 1e-10; }
        [[nodiscard]] bool is_long() const { return position_qty > 0; }
        [[nodiscard]] bool is_short() const { return position_qty < 0; }

        [[nodiscard]] double spread_bps() const {
            double mid = (bid_price + ask_price) / 2.0;
            return mid > 0 ? ((ask_price - bid_price) / mid) * 10000.0 : 0.0;
        }
    };

    struct FlowSignal {
        SignalType type{SignalType::NONE};
        double orderbook_score{0.0};
        double trade_flow_score{0.0};
        double vpin_score{0.0};
        double pressure_score{0.0};
        double combined_score{0.0};
        double confidence{0.0};
        std::string reason;
    };

    // ========================================================================
    // Core Logic Methods
    // ========================================================================

    /**
     * Update order book from market data
     */
    void update_orderbook(SymbolState& state, const MarketData& data);

    /**
     * Record a trade for analysis
     */
    void record_trade(SymbolState& state, const Trade& trade);

    /**
     * Calculate multi-level order book imbalance
     */
    double calculate_orderbook_imbalance(const SymbolState& state) const;

    /**
     * Calculate weighted multi-level imbalance
     */
    double calculate_multi_level_imbalance(const SymbolState& state) const;

    /**
     * Calculate order book pressure
     */
    std::pair<double, double> calculate_pressure(const SymbolState& state) const;

    /**
     * Update VPIN calculation
     */
    void update_vpin(SymbolState& state, const Trade& trade);

    /**
     * Calculate current VPIN value
     */
    double calculate_vpin(const SymbolState& state) const;

    /**
     * Calculate trade flow imbalance
     */
    double calculate_trade_imbalance(const SymbolState& state) const;

    /**
     * Calculate aggressor ratio
     */
    double calculate_aggressor_ratio(const SymbolState& state) const;

    /**
     * Generate trading signal from flow analysis
     */
    FlowSignal generate_signal(const SymbolState& state) const;

    /**
     * Calculate signal confidence
     */
    double calculate_confidence(const FlowSignal& signal, const SymbolState& state) const;

    /**
     * Check if we should enter a position
     */
    bool should_enter(const SymbolState& state, const FlowSignal& signal) const;

    /**
     * Check if we should exit current position
     */
    bool should_exit(const SymbolState& state) const;

    /**
     * Calculate position size
     */
    double calculate_size(const SymbolState& state, const FlowSignal& signal) const;

    /**
     * Execute entry
     */
    void execute_entry(SymbolState& state, const FlowSignal& signal);

    /**
     * Execute exit
     */
    void execute_exit(SymbolState& state, const std::string& reason);

    /**
     * Check stop loss and take profit
     */
    void check_exit_conditions(SymbolState& state);

    /**
     * Classify trade direction using tick rule
     */
    bool classify_trade_direction(const SymbolState& state, Price price) const;

    // ========================================================================
    // State
    // ========================================================================

    Parameters params_;

    // Per-symbol state
    std::unordered_map<std::string, SymbolState> states_;

    // Order tracking
    std::unordered_map<uint64_t, std::string> order_to_symbol_;

    // Last signal time per symbol
    std::unordered_map<std::string, Timestamp> last_signal_time_;
};

// ============================================================================
// Factory Registration
// ============================================================================

class OrderFlowImbalanceFactory : public IStrategyFactory {
public:
    [[nodiscard]] std::unique_ptr<StrategyBase> create(const StrategyConfig& config) override {
        return std::make_unique<OrderFlowImbalanceStrategy>(config);
    }

    [[nodiscard]] std::string_view strategy_type() const noexcept override {
        return "order_flow_imbalance";
    }
};

}  // namespace strategies
}  // namespace hft
