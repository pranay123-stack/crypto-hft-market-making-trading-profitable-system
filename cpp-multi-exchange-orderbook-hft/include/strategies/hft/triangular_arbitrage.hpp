#pragma once

#include "../strategy_base.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <deque>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace hft {
namespace strategies {

/**
 * Triangular Arbitrage Strategy
 *
 * Exploits price inefficiencies in triangular currency paths.
 * Example: BTC -> ETH -> USDT -> BTC
 *
 * If the product of exchange rates around the triangle != 1.0
 * (after accounting for fees), there's an arbitrage opportunity.
 *
 * Key Features:
 * - Real-time triangular path calculation
 * - Fee-adjusted profit calculation
 * - Atomic execution of all three legs
 * - Support for multiple currency triangles
 * - Forward and reverse path detection
 */
class TriangularArbitrageStrategy : public StrategyBase {
public:
    // Configuration parameters
    struct Parameters {
        // Minimum profit threshold in basis points
        double min_profit_bps{3.0};

        // Maximum position size in base currency (e.g., BTC)
        double max_position{0.5};

        // Maximum time to execute all legs (microseconds)
        int64_t execution_timeout_us{500000};  // 500ms

        // Minimum confidence for execution
        double min_confidence{0.8};

        // Maximum quote age to consider valid (nanoseconds)
        int64_t max_quote_age_ns{100000000};  // 100ms

        // Fee rate per trade (decimal)
        double fee_rate{0.001};  // 0.1%

        // Minimum trade size
        double min_trade_size{0.001};

        // Slippage tolerance in bps per leg
        double slippage_tolerance_bps{1.0};

        // Target currencies for triangles (e.g., {"BTC", "ETH", "USDT"})
        std::vector<std::string> triangle_currencies;

        // Exchange to trade on
        std::string exchange{"binance"};

        // Enable reverse path detection
        bool check_reverse_paths{true};
    };

    explicit TriangularArbitrageStrategy(StrategyConfig config);
    ~TriangularArbitrageStrategy() override = default;

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
     * Add a triangular path to monitor
     * @param base Base currency (e.g., "BTC")
     * @param quote1 First quote currency (e.g., "ETH")
     * @param quote2 Second quote currency (e.g., "USDT")
     */
    void add_triangle(const std::string& base, const std::string& quote1, const std::string& quote2);

    // ========================================================================
    // Analytics
    // ========================================================================
    struct TriangleStats {
        std::string path_description;
        double mean_profit_bps{0.0};
        double max_profit_bps{0.0};
        int64_t opportunities_detected{0};
        int64_t trades_executed{0};
        double total_profit{0.0};
        double success_rate{0.0};
    };

    [[nodiscard]] std::vector<TriangleStats> get_triangle_stats() const;
    [[nodiscard]] double get_current_triangle_profit(const std::string& base,
                                                      const std::string& quote1,
                                                      const std::string& quote2) const;

private:
    // ========================================================================
    // Internal Types
    // ========================================================================

    // Represents a trading pair with direction
    struct TradingPair {
        std::string symbol;           // e.g., "BTCETH"
        std::string base_currency;    // e.g., "BTC"
        std::string quote_currency;   // e.g., "ETH"
        bool is_inverted{false};      // true if we need to invert the rate

        // Current market data
        Price bid_price{0.0};
        Price ask_price{0.0};
        Quantity bid_size{0.0};
        Quantity ask_size{0.0};
        Timestamp timestamp{0};

        [[nodiscard]] bool is_valid() const noexcept {
            return bid_price > 0 && ask_price > 0 && bid_price < ask_price;
        }

        // Get rate for buying base currency (pay quote, receive base)
        [[nodiscard]] double buy_rate() const noexcept {
            return is_inverted ? (1.0 / bid_price) : ask_price;
        }

        // Get rate for selling base currency (pay base, receive quote)
        [[nodiscard]] double sell_rate() const noexcept {
            return is_inverted ? (1.0 / ask_price) : bid_price;
        }
    };

    // Represents a triangular path
    struct Triangle {
        std::string id;  // Unique identifier

        // The three currencies
        std::string currency_a;  // Starting currency
        std::string currency_b;  // First intermediate
        std::string currency_c;  // Second intermediate

        // The three legs (A->B, B->C, C->A)
        std::string leg1_symbol;  // A to B
        std::string leg2_symbol;  // B to C
        std::string leg3_symbol;  // C to A

        // Whether each leg is a buy (true) or sell (false) operation
        bool leg1_is_buy{true};
        bool leg2_is_buy{true};
        bool leg3_is_buy{true};

        // Statistics
        int64_t opportunities_count{0};
        int64_t executions_count{0};
        double total_profit{0.0};
        std::deque<double> recent_profits;
    };

    // Triangular arbitrage opportunity
    struct TriangleOpportunity {
        const Triangle* triangle{nullptr};

        // Forward path rates (after fees)
        double forward_profit_bps{0.0};
        double forward_rate_product{0.0};

        // Reverse path rates (after fees)
        double reverse_profit_bps{0.0};
        double reverse_rate_product{0.0};

        // Best direction
        bool use_forward{true};
        double best_profit_bps{0.0};

        // Execution details
        Quantity max_size{0.0};
        double confidence{0.0};
        Timestamp detected_at{0};

        // Leg prices for execution
        Price leg1_price{0.0};
        Price leg2_price{0.0};
        Price leg3_price{0.0};
        Quantity leg1_quantity{0.0};
        Quantity leg2_quantity{0.0};
        Quantity leg3_quantity{0.0};

        [[nodiscard]] bool is_profitable() const noexcept {
            return best_profit_bps > 0;
        }
    };

    // Pending triangle execution
    struct PendingTriangle {
        TriangleOpportunity opportunity;

        // Order tracking
        uint64_t leg1_order_id{0};
        uint64_t leg2_order_id{0};
        uint64_t leg3_order_id{0};

        // Fill status
        bool leg1_filled{false};
        bool leg2_filled{false};
        bool leg3_filled{false};

        Quantity leg1_filled_qty{0.0};
        Quantity leg2_filled_qty{0.0};
        Quantity leg3_filled_qty{0.0};

        Price leg1_avg_price{0.0};
        Price leg2_avg_price{0.0};
        Price leg3_avg_price{0.0};

        Timestamp start_time{0};
        int current_leg{1};  // Which leg we're currently executing
    };

    // ========================================================================
    // Core Logic Methods
    // ========================================================================

    /**
     * Update market data for a trading pair
     */
    void update_pair_data(const MarketData& data);

    /**
     * Calculate triangular arbitrage opportunity
     */
    std::optional<TriangleOpportunity> calculate_opportunity(const Triangle& triangle);

    /**
     * Calculate the rate product for a path (should be 1.0 in perfect market)
     */
    double calculate_rate_product(const Triangle& triangle, bool forward) const;

    /**
     * Calculate maximum tradeable size for a triangle
     */
    Quantity calculate_max_size(const Triangle& triangle, bool forward) const;

    /**
     * Execute a triangular arbitrage
     */
    void execute_triangle(const TriangleOpportunity& opp);

    /**
     * Execute the next leg of a pending triangle
     */
    void execute_next_leg(PendingTriangle& pending);

    /**
     * Calculate confidence for an opportunity
     */
    double calculate_confidence(const TriangleOpportunity& opp) const;

    /**
     * Check for stale data that would invalidate opportunities
     */
    bool has_fresh_data(const Triangle& triangle) const;

    /**
     * Build trading pair key from currencies
     */
    std::string make_pair_key(const std::string& base, const std::string& quote) const;

    /**
     * Find or create trading pair for two currencies
     */
    TradingPair* find_pair(const std::string& currency1, const std::string& currency2);

    /**
     * Parse symbol to extract base and quote currencies
     */
    std::pair<std::string, std::string> parse_symbol(const std::string& symbol) const;

    /**
     * Handle timeout on pending triangles
     */
    void check_pending_timeouts();

    // ========================================================================
    // State
    // ========================================================================

    Parameters params_;

    // All trading pairs we're tracking
    std::unordered_map<std::string, TradingPair> pairs_;

    // All triangles we're monitoring
    std::vector<Triangle> triangles_;

    // Pending triangle executions
    std::unordered_map<uint64_t, PendingTriangle> pending_triangles_;

    // Order ID to triangle mapping
    std::unordered_map<uint64_t, uint64_t> order_to_triangle_;

    // Next triangle ID
    std::atomic<uint64_t> next_triangle_id_{1};

    // Known trading pair mappings (currency pair -> symbol)
    std::unordered_map<std::string, std::string> pair_to_symbol_;

    // Last check time
    Timestamp last_check_time_{0};

    // Minimum interval between checks
    static constexpr int64_t MIN_CHECK_INTERVAL_NS = 50000;  // 50us
};

// ============================================================================
// Factory Registration
// ============================================================================

class TriangularArbitrageFactory : public IStrategyFactory {
public:
    [[nodiscard]] std::unique_ptr<StrategyBase> create(const StrategyConfig& config) override {
        return std::make_unique<TriangularArbitrageStrategy>(config);
    }

    [[nodiscard]] std::string_view strategy_type() const noexcept override {
        return "triangular_arbitrage";
    }
};

}  // namespace strategies
}  // namespace hft
