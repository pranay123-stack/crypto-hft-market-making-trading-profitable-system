#pragma once

#include "../strategy_base.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <deque>
#include <unordered_map>

namespace hft {
namespace strategies {

/**
 * Latency Arbitrage Strategy
 *
 * Exploits price differences between exchanges due to latency.
 * When the same asset trades at different prices across exchanges
 * (beyond transaction costs), simultaneously buy on the cheaper
 * exchange and sell on the more expensive one.
 *
 * Key Features:
 * - Multi-exchange price monitoring
 * - Latency-adjusted price comparison
 * - Simultaneous order execution
 * - Position neutrality maintenance
 * - Real-time spread tracking
 */
class LatencyArbitrageStrategy : public StrategyBase {
public:
    // Configuration parameters
    struct Parameters {
        // Minimum spread in basis points to trigger arbitrage
        double min_spread_bps{5.0};

        // Maximum position size per symbol
        double max_position{1.0};

        // Maximum time to hold an arbitrage position (microseconds)
        int64_t timeout_us{1000000};  // 1 second

        // Minimum confidence for signal generation
        double min_confidence{0.7};

        // Maximum latency difference to consider prices valid (nanoseconds)
        int64_t max_latency_diff_ns{50000000};  // 50ms

        // Fee rate per exchange (in decimal, e.g., 0.001 = 0.1%)
        std::unordered_map<std::string, double> exchange_fees;

        // Minimum size to trade
        double min_trade_size{0.001};

        // Maximum slippage tolerance in bps
        double max_slippage_bps{2.0};

        // Enable/disable spread tracking
        bool track_spreads{true};

        // Rolling window size for spread statistics
        size_t spread_window_size{100};
    };

    explicit LatencyArbitrageStrategy(StrategyConfig config);
    ~LatencyArbitrageStrategy() override = default;

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
    struct SpreadStats {
        double mean_spread_bps{0.0};
        double std_spread_bps{0.0};
        double max_spread_bps{0.0};
        double min_spread_bps{0.0};
        int64_t arbitrage_opportunities{0};
        int64_t executed_arbitrages{0};
        double total_profit{0.0};
    };

    [[nodiscard]] SpreadStats get_spread_stats(const std::string& symbol) const;
    [[nodiscard]] double get_current_spread(const std::string& symbol) const;

private:
    // ========================================================================
    // Internal Types
    // ========================================================================

    struct ExchangeQuote {
        std::string exchange;
        ExchangeId exchange_id{0};
        Price bid_price{0.0};
        Price ask_price{0.0};
        Quantity bid_size{0.0};
        Quantity ask_size{0.0};
        Timestamp timestamp{0};
        Timestamp local_timestamp{0};
        int64_t latency_ns{0};

        [[nodiscard]] bool is_valid() const noexcept {
            return bid_price > 0 && ask_price > 0 && bid_price < ask_price;
        }

        [[nodiscard]] bool is_stale(Timestamp now, int64_t max_age_ns) const noexcept {
            return (now - local_timestamp).count() > max_age_ns;
        }
    };

    struct ArbitrageOpportunity {
        std::string symbol;

        std::string buy_exchange;
        std::string sell_exchange;
        ExchangeId buy_exchange_id{0};
        ExchangeId sell_exchange_id{0};

        Price buy_price{0.0};   // Ask on buy exchange
        Price sell_price{0.0};  // Bid on sell exchange
        Quantity max_size{0.0};

        double gross_spread_bps{0.0};
        double net_spread_bps{0.0};  // After fees
        double expected_profit{0.0};
        double confidence{0.0};

        Timestamp detected_at{0};
        int64_t timeout_us{0};

        [[nodiscard]] bool is_profitable() const noexcept {
            return net_spread_bps > 0 && expected_profit > 0;
        }

        [[nodiscard]] bool is_expired(Timestamp now) const noexcept {
            return (now - detected_at).count() > timeout_us * 1000;
        }
    };

    struct PendingArbitrage {
        ArbitrageOpportunity opportunity;
        uint64_t buy_order_id{0};
        uint64_t sell_order_id{0};
        bool buy_filled{false};
        bool sell_filled{false};
        Quantity buy_filled_qty{0.0};
        Quantity sell_filled_qty{0.0};
        Price buy_avg_price{0.0};
        Price sell_avg_price{0.0};
        Timestamp start_time{0};
    };

    struct SpreadHistory {
        std::deque<double> spreads;
        double sum{0.0};
        double sum_sq{0.0};
        double max_spread{-std::numeric_limits<double>::infinity()};
        double min_spread{std::numeric_limits<double>::infinity()};
        int64_t opportunity_count{0};
        int64_t execution_count{0};
        double total_profit{0.0};

        void add_spread(double spread, size_t max_size) {
            spreads.push_back(spread);
            sum += spread;
            sum_sq += spread * spread;
            max_spread = std::max(max_spread, spread);
            min_spread = std::min(min_spread, spread);

            while (spreads.size() > max_size) {
                double old = spreads.front();
                spreads.pop_front();
                sum -= old;
                sum_sq -= old * old;
            }
        }

        [[nodiscard]] double mean() const {
            return spreads.empty() ? 0.0 : sum / spreads.size();
        }

        [[nodiscard]] double stddev() const {
            if (spreads.size() < 2) return 0.0;
            double n = static_cast<double>(spreads.size());
            double variance = (sum_sq - sum * sum / n) / (n - 1);
            return std::sqrt(std::max(0.0, variance));
        }
    };

    // ========================================================================
    // Core Logic Methods
    // ========================================================================

    /**
     * Update quotes for a symbol on an exchange
     */
    void update_quote(const std::string& symbol, const MarketData& data);

    /**
     * Detect arbitrage opportunities across all exchanges for a symbol
     */
    std::vector<ArbitrageOpportunity> detect_opportunities(const std::string& symbol);

    /**
     * Evaluate and potentially execute an arbitrage opportunity
     */
    void evaluate_opportunity(const ArbitrageOpportunity& opp);

    /**
     * Execute an arbitrage by emitting buy and sell signals
     */
    void execute_arbitrage(const ArbitrageOpportunity& opp);

    /**
     * Calculate fee-adjusted spread
     */
    double calculate_net_spread(const std::string& buy_exchange,
                                const std::string& sell_exchange,
                                Price buy_price,
                                Price sell_price) const;

    /**
     * Get effective fee rate for an exchange
     */
    double get_exchange_fee(const std::string& exchange) const;

    /**
     * Check if quotes are synchronized (not too far apart in time)
     */
    bool are_quotes_synchronized(const ExchangeQuote& q1, const ExchangeQuote& q2) const;

    /**
     * Calculate confidence based on various factors
     */
    double calculate_confidence(const ArbitrageOpportunity& opp,
                               const ExchangeQuote& buy_quote,
                               const ExchangeQuote& sell_quote) const;

    /**
     * Handle timeout of pending arbitrages
     */
    void check_pending_timeouts();

    /**
     * Update position after arbitrage leg fills
     */
    void update_arbitrage_position(const Trade& trade);

    // ========================================================================
    // State
    // ========================================================================

    Parameters params_;

    // Symbol -> Exchange -> Quote
    std::unordered_map<std::string, std::unordered_map<std::string, ExchangeQuote>> quotes_;

    // Pending arbitrages (keyed by combined order ID)
    std::unordered_map<uint64_t, PendingArbitrage> pending_arbitrages_;

    // Order ID to arbitrage mapping
    std::unordered_map<uint64_t, uint64_t> order_to_arbitrage_;

    // Spread history per symbol
    std::unordered_map<std::string, SpreadHistory> spread_history_;

    // Next arbitrage ID
    std::atomic<uint64_t> next_arb_id_{1};

    // Last opportunity check time per symbol
    std::unordered_map<std::string, Timestamp> last_check_time_;

    // Minimum check interval (prevents excessive calculations)
    static constexpr int64_t MIN_CHECK_INTERVAL_NS = 100000;  // 100us
};

// ============================================================================
// Factory Registration
// ============================================================================

class LatencyArbitrageFactory : public IStrategyFactory {
public:
    [[nodiscard]] std::unique_ptr<StrategyBase> create(const StrategyConfig& config) override {
        return std::make_unique<LatencyArbitrageStrategy>(config);
    }

    [[nodiscard]] std::string_view strategy_type() const noexcept override {
        return "latency_arbitrage";
    }
};

}  // namespace strategies
}  // namespace hft
