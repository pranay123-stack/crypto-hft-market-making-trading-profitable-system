#pragma once

#include "core/types.hpp"
#include "orderbook/consolidated_book.hpp"
#include <functional>
#include <vector>
#include <atomic>
#include <mutex>

namespace hft {

// ============================================================================
// Arbitrage Configuration
// ============================================================================

struct ArbitrageConfig {
    double min_profit_bps = 5.0;        // Minimum profit in basis points
    double max_slippage_bps = 2.0;      // Maximum acceptable slippage
    Quantity min_quantity = 0;           // Minimum trade size
    Quantity max_quantity = 0;           // Maximum trade size
    Timestamp max_opportunity_age_ns = 100000000;  // 100ms
    bool require_both_sides_liquid = true;
    double min_liquidity_ratio = 0.5;   // Min available qty vs desired
};

// ============================================================================
// Cross-Exchange Arbitrage Detector
// ============================================================================

class ArbitrageDetector {
public:
    using OpportunityCallback = std::function<void(const ArbitrageOpportunity&)>;

    explicit ArbitrageDetector(const ArbitrageConfig& config);
    ~ArbitrageDetector() = default;

    // Detection
    [[nodiscard]] std::vector<ArbitrageOpportunity> detect(
        const ConsolidatedBook& book) const;

    [[nodiscard]] ArbitrageOpportunity find_best_opportunity(
        const ConsolidatedBook& book) const;

    // Real-time monitoring
    void on_book_update(const ConsolidatedBook& book);
    void set_opportunity_callback(OpportunityCallback cb);

    // Configuration
    void update_config(const ArbitrageConfig& config);
    [[nodiscard]] const ArbitrageConfig& config() const { return config_; }

    // Statistics
    [[nodiscard]] uint64_t opportunities_detected() const {
        return opportunities_detected_.load();
    }
    [[nodiscard]] uint64_t opportunities_executed() const {
        return opportunities_executed_.load();
    }

    void record_execution(bool success);

private:
    [[nodiscard]] bool validate_opportunity(
        const ArbitrageOpportunity& opp,
        const ConsolidatedBook& book) const;

    [[nodiscard]] Quantity calculate_safe_quantity(
        const ConsolidatedBook& book,
        ExchangeId buy_exchange,
        ExchangeId sell_exchange,
        Price buy_price,
        Price sell_price) const;

    ArbitrageConfig config_;
    OpportunityCallback callback_;
    std::atomic<uint64_t> opportunities_detected_{0};
    std::atomic<uint64_t> opportunities_executed_{0};
    mutable std::mutex mutex_;
};

// ============================================================================
// Triangular Arbitrage Detector
// ============================================================================

struct TriangularOpportunity {
    // Path: A -> B -> C -> A
    Symbol symbol_ab;  // e.g., BTC/USDT
    Symbol symbol_bc;  // e.g., ETH/BTC
    Symbol symbol_ca;  // e.g., ETH/USDT

    ExchangeId exchange;
    Quantity quantity;
    double profit_bps;
    Timestamp detected_at;

    // Execution details
    Side side_ab;
    Side side_bc;
    Side side_ca;
    Price price_ab;
    Price price_bc;
    Price price_ca;
};

class TriangularArbitrageDetector {
public:
    using TriangularCallback = std::function<void(const TriangularOpportunity&)>;

    struct TriangularConfig {
        double min_profit_bps = 10.0;
        Quantity min_quantity = 0;
        Timestamp max_age_ns = 50000000;  // 50ms
    };

    explicit TriangularArbitrageDetector(const TriangularConfig& config);

    // Define triangular paths
    void add_path(const Symbol& ab, const Symbol& bc, const Symbol& ca);

    // Detection
    [[nodiscard]] std::vector<TriangularOpportunity> detect(
        const ConsolidatedBookManager& books,
        ExchangeId exchange) const;

    void set_callback(TriangularCallback cb);

private:
    struct Path {
        Symbol ab, bc, ca;
    };

    TriangularConfig config_;
    std::vector<Path> paths_;
    TriangularCallback callback_;
};

// ============================================================================
// Arbitrage Executor
// ============================================================================

class ArbitrageExecutor {
public:
    struct ExecutionResult {
        bool success;
        OrderId buy_order_id;
        OrderId sell_order_id;
        Quantity executed_qty;
        double realized_profit;
        std::string error_message;
        Timestamp execution_time_ns;
    };

    using OrderSender = std::function<OrderId(const Order&)>;

    explicit ArbitrageExecutor(OrderSender sender);

    // Execute cross-exchange arbitrage
    ExecutionResult execute(const ArbitrageOpportunity& opportunity);

    // Execute triangular arbitrage (single exchange)
    ExecutionResult execute(const TriangularOpportunity& opportunity);

    // Configuration
    void set_max_retries(int retries) { max_retries_ = retries; }
    void set_timeout_ns(Timestamp timeout) { timeout_ns_ = timeout; }

    // Statistics
    [[nodiscard]] uint64_t successful_executions() const {
        return successful_.load();
    }
    [[nodiscard]] uint64_t failed_executions() const {
        return failed_.load();
    }
    [[nodiscard]] double total_profit() const {
        return total_profit_.load();
    }

private:
    OrderSender order_sender_;
    int max_retries_ = 3;
    Timestamp timeout_ns_ = 1000000000;  // 1 second

    std::atomic<uint64_t> successful_{0};
    std::atomic<uint64_t> failed_{0};
    std::atomic<double> total_profit_{0.0};
};

} // namespace hft
