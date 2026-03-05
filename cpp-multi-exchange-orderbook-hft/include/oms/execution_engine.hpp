#pragma once

/**
 * @file execution_engine.hpp
 * @brief Smart Order Routing and Execution Engine for HFT trading system
 *
 * Provides intelligent order execution including:
 * - Smart order routing across multiple exchanges
 * - Best execution logic
 * - Order splitting across venues
 * - Execution algorithm support (TWAP, VWAP, etc.)
 */

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <functional>
#include <atomic>
#include <optional>
#include <chrono>
#include <queue>

#include "core/types.hpp"
#include "oms/order.hpp"
#include "oms/order_manager.hpp"

namespace hft::oms {

using namespace hft::core;

// Forward declarations
class OrderManager;
class PositionManager;

/**
 * @brief Market data snapshot for routing decisions
 */
struct MarketSnapshot {
    Exchange exchange{Exchange::Unknown};
    Symbol symbol;

    Price bestBid;
    Price bestAsk;
    Quantity bidSize;
    Quantity askSize;

    // Order book depth (top 5 levels)
    std::array<Price, 5> bidPrices;
    std::array<Quantity, 5> bidSizes;
    std::array<Price, 5> askPrices;
    std::array<Quantity, 5> askSizes;

    // Exchange metrics
    double latencyMs{0.0};       // Estimated latency to exchange
    double fillProbability{1.0}; // Historical fill rate
    double makerFee{0.0};        // Maker fee rate
    double takerFee{0.0};        // Taker fee rate

    Timestamp lastUpdate;

    [[nodiscard]] double spread() const {
        return bestAsk.to_double() - bestBid.to_double();
    }

    [[nodiscard]] double midPrice() const {
        return (bestBid.to_double() + bestAsk.to_double()) / 2.0;
    }

    [[nodiscard]] bool isStale(uint64_t maxAgeNanos) const {
        auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
        return (now - lastUpdate.nanos) > maxAgeNanos;
    }
};

/**
 * @brief Execution algorithm type
 */
enum class ExecutionAlgorithm : uint8_t {
    Direct = 0,      // Send directly to single exchange
    SOR = 1,         // Smart Order Routing
    TWAP = 2,        // Time-Weighted Average Price
    VWAP = 3,        // Volume-Weighted Average Price
    Iceberg = 4,     // Iceberg/Hidden orders
    Peg = 5,         // Pegged to BBO
    Sniper = 6,      // Aggressive liquidity taking
    Maker = 7        // Passive maker strategy
};

[[nodiscard]] constexpr std::string_view algo_to_string(ExecutionAlgorithm algo) noexcept {
    switch (algo) {
        case ExecutionAlgorithm::Direct: return "DIRECT";
        case ExecutionAlgorithm::SOR: return "SOR";
        case ExecutionAlgorithm::TWAP: return "TWAP";
        case ExecutionAlgorithm::VWAP: return "VWAP";
        case ExecutionAlgorithm::Iceberg: return "ICEBERG";
        case ExecutionAlgorithm::Peg: return "PEG";
        case ExecutionAlgorithm::Sniper: return "SNIPER";
        case ExecutionAlgorithm::Maker: return "MAKER";
    }
    return "UNKNOWN";
}

/**
 * @brief Execution request parameters
 */
struct ExecutionRequest {
    uint64_t requestId{0};
    Symbol symbol;
    Side side{Side::Buy};
    Quantity quantity;
    Price limitPrice;          // Optional limit price

    ExecutionAlgorithm algorithm{ExecutionAlgorithm::Direct};
    std::optional<Exchange> preferredExchange;

    // Algorithm-specific parameters
    uint32_t durationSeconds{0};     // For TWAP/VWAP
    uint32_t numSlices{1};           // Number of child orders
    double participationRate{0.0};   // Target participation (0-1)
    bool postOnly{false};            // Maker only
    bool reduceOnly{false};          // Only reduce position

    // Constraints
    std::optional<Price> maxPrice;   // Max price to pay (buy)
    std::optional<Price> minPrice;   // Min price to accept (sell)
    std::optional<Quantity> minFillQty; // Minimum fill quantity

    Timestamp createTime;
    Timestamp deadline;              // Must complete by

    std::string strategyId;
    std::string tag;

    [[nodiscard]] bool hasDeadline() const {
        return deadline.nanos > 0;
    }

    [[nodiscard]] bool isExpired() const {
        if (!hasDeadline()) return false;
        auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
        return now > deadline.nanos;
    }
};

/**
 * @brief Execution status
 */
enum class ExecutionStatus : uint8_t {
    Pending = 0,
    Active = 1,
    PartiallyFilled = 2,
    Filled = 3,
    Cancelled = 4,
    Failed = 5,
    Expired = 6
};

/**
 * @brief Execution result tracking
 */
struct ExecutionResult {
    uint64_t requestId{0};
    ExecutionStatus status{ExecutionStatus::Pending};

    Quantity targetQuantity;
    Quantity filledQuantity;
    Quantity remainingQuantity;

    double avgFillPrice{0.0};
    double totalNotional{0.0};
    double totalCommission{0.0};

    uint32_t childOrderCount{0};
    uint32_t fillCount{0};

    // Execution quality metrics
    double arrivalPrice{0.0};       // Price when order arrived
    double slippage{0.0};           // Actual vs arrival price
    double implementationShortfall{0.0};

    Timestamp startTime;
    Timestamp endTime;

    std::vector<uint64_t> childOrderIds;

    std::string failureReason;

    [[nodiscard]] bool isComplete() const {
        return status == ExecutionStatus::Filled ||
               status == ExecutionStatus::Cancelled ||
               status == ExecutionStatus::Failed ||
               status == ExecutionStatus::Expired;
    }

    [[nodiscard]] double fillRatio() const {
        if (targetQuantity.value == 0) return 0.0;
        return static_cast<double>(filledQuantity.value) /
               static_cast<double>(targetQuantity.value);
    }
};

/**
 * @brief Routing decision for order
 */
struct RoutingDecision {
    Exchange exchange{Exchange::Unknown};
    Price price;
    Quantity quantity;
    OrderType orderType{OrderType::Limit};
    double score{0.0};             // Ranking score
    std::string reason;
};

/**
 * @brief Exchange routing metrics
 */
struct ExchangeMetrics {
    Exchange exchange{Exchange::Unknown};
    double avgLatencyMs{0.0};
    double fillRate{0.0};
    double uptimePercent{100.0};
    double makerFee{0.0};
    double takerFee{0.0};
    uint64_t totalOrders{0};
    uint64_t totalFills{0};
    uint64_t rejectCount{0};
    Timestamp lastUpdate;
};

/**
 * @brief Smart Order Router (SOR)
 *
 * Determines optimal routing for orders across multiple exchanges.
 */
class SmartOrderRouter {
public:
    SmartOrderRouter();
    ~SmartOrderRouter();

    /**
     * @brief Update market data for exchange
     */
    void updateMarketData(const MarketSnapshot& snapshot);

    /**
     * @brief Update exchange metrics
     */
    void updateExchangeMetrics(const ExchangeMetrics& metrics);

    /**
     * @brief Get best routing for order
     */
    [[nodiscard]] std::vector<RoutingDecision> route(
        const Symbol& symbol,
        Side side,
        Quantity quantity,
        std::optional<Price> limitPrice = std::nullopt
    );

    /**
     * @brief Get best exchange for symbol
     */
    [[nodiscard]] std::optional<Exchange> getBestExchange(
        const Symbol& symbol,
        Side side
    ) const;

    /**
     * @brief Get aggregated order book across exchanges
     */
    [[nodiscard]] std::vector<std::pair<Price, Quantity>> getAggregatedBook(
        const Symbol& symbol,
        Side side,
        size_t depth = 10
    ) const;

    /**
     * @brief Set routing preference weights
     */
    void setWeights(double priceWeight, double latencyWeight,
                    double fillRateWeight, double feeWeight);

private:
    mutable std::shared_mutex mutex_;

    // Market data per symbol per exchange
    std::unordered_map<std::string,
        std::unordered_map<Exchange, MarketSnapshot>> marketData_;

    // Exchange metrics
    std::unordered_map<Exchange, ExchangeMetrics> exchangeMetrics_;

    // Routing weights
    double priceWeight_{0.5};
    double latencyWeight_{0.2};
    double fillRateWeight_{0.2};
    double feeWeight_{0.1};

    double scoreExchange(const MarketSnapshot& snapshot,
                         const ExchangeMetrics& metrics,
                         Side side, Price targetPrice) const;
};

/**
 * @brief Execution Algorithm Base Class
 */
class ExecutionAlgo {
public:
    virtual ~ExecutionAlgo() = default;

    /**
     * @brief Initialize algorithm with request
     */
    virtual void initialize(const ExecutionRequest& request,
                            std::shared_ptr<OrderManager> orderManager) = 0;

    /**
     * @brief Execute next step
     * @return true if algorithm should continue
     */
    virtual bool step() = 0;

    /**
     * @brief Handle order update
     */
    virtual void onOrderUpdate(const Order& order) = 0;

    /**
     * @brief Handle fill
     */
    virtual void onFill(uint64_t orderId, Price price, Quantity qty) = 0;

    /**
     * @brief Cancel algorithm
     */
    virtual void cancel() = 0;

    /**
     * @brief Get current result
     */
    [[nodiscard]] virtual const ExecutionResult& getResult() const = 0;

    /**
     * @brief Check if algorithm is complete
     */
    [[nodiscard]] virtual bool isComplete() const = 0;
};

/**
 * @brief TWAP Algorithm Implementation
 */
class TWAPAlgo : public ExecutionAlgo {
public:
    void initialize(const ExecutionRequest& request,
                    std::shared_ptr<OrderManager> orderManager) override;
    bool step() override;
    void onOrderUpdate(const Order& order) override;
    void onFill(uint64_t orderId, Price price, Quantity qty) override;
    void cancel() override;
    [[nodiscard]] const ExecutionResult& getResult() const override { return result_; }
    [[nodiscard]] bool isComplete() const override;

private:
    ExecutionRequest request_;
    ExecutionResult result_;
    std::shared_ptr<OrderManager> orderManager_;

    Timestamp startTime_;
    Timestamp endTime_;
    uint32_t currentSlice_{0};
    Quantity sliceQuantity_;
    std::chrono::milliseconds sliceInterval_;
    std::chrono::steady_clock::time_point nextSliceTime_;

    std::vector<uint64_t> activeOrders_;
    bool cancelled_{false};
};

/**
 * @brief VWAP Algorithm Implementation
 */
class VWAPAlgo : public ExecutionAlgo {
public:
    void initialize(const ExecutionRequest& request,
                    std::shared_ptr<OrderManager> orderManager) override;
    bool step() override;
    void onOrderUpdate(const Order& order) override;
    void onFill(uint64_t orderId, Price price, Quantity qty) override;
    void cancel() override;
    [[nodiscard]] const ExecutionResult& getResult() const override { return result_; }
    [[nodiscard]] bool isComplete() const override;

    /**
     * @brief Update volume profile for symbol
     */
    void updateVolumeProfile(const std::vector<double>& volumeProfile);

private:
    ExecutionRequest request_;
    ExecutionResult result_;
    std::shared_ptr<OrderManager> orderManager_;

    std::vector<double> volumeProfile_; // Normalized volume distribution
    uint32_t currentBucket_{0};
    Timestamp startTime_;
    std::chrono::milliseconds bucketDuration_;

    std::vector<uint64_t> activeOrders_;
    bool cancelled_{false};
};

/**
 * @brief Iceberg Algorithm Implementation
 */
class IcebergAlgo : public ExecutionAlgo {
public:
    void initialize(const ExecutionRequest& request,
                    std::shared_ptr<OrderManager> orderManager) override;
    bool step() override;
    void onOrderUpdate(const Order& order) override;
    void onFill(uint64_t orderId, Price price, Quantity qty) override;
    void cancel() override;
    [[nodiscard]] const ExecutionResult& getResult() const override { return result_; }
    [[nodiscard]] bool isComplete() const override;

private:
    ExecutionRequest request_;
    ExecutionResult result_;
    std::shared_ptr<OrderManager> orderManager_;

    Quantity displayQuantity_;
    uint64_t currentOrderId_{0};
    bool cancelled_{false};
};

/**
 * @brief Execution Engine
 *
 * Manages execution algorithms and smart order routing.
 */
class ExecutionEngine {
public:
    /**
     * @brief Constructor
     */
    ExecutionEngine(std::shared_ptr<OrderManager> orderManager,
                    std::shared_ptr<PositionManager> positionManager = nullptr);

    /**
     * @brief Destructor
     */
    ~ExecutionEngine();

    // Non-copyable
    ExecutionEngine(const ExecutionEngine&) = delete;
    ExecutionEngine& operator=(const ExecutionEngine&) = delete;

    // ===== Execution =====

    /**
     * @brief Submit execution request
     * @return Request ID
     */
    uint64_t execute(const ExecutionRequest& request);

    /**
     * @brief Cancel execution
     */
    bool cancelExecution(uint64_t requestId);

    /**
     * @brief Cancel all active executions
     */
    size_t cancelAllExecutions();

    // ===== Market Data =====

    /**
     * @brief Update market data for routing decisions
     */
    void updateMarketData(const MarketSnapshot& snapshot);

    /**
     * @brief Update exchange metrics
     */
    void updateExchangeMetrics(const ExchangeMetrics& metrics);

    // ===== Queries =====

    /**
     * @brief Get execution status
     */
    [[nodiscard]] std::optional<ExecutionResult> getExecutionResult(uint64_t requestId) const;

    /**
     * @brief Get all active executions
     */
    [[nodiscard]] std::vector<uint64_t> getActiveExecutions() const;

    /**
     * @brief Get smart order router
     */
    [[nodiscard]] SmartOrderRouter& getRouter() { return router_; }

    // ===== Callbacks =====

    /**
     * @brief Set callback for execution completion
     */
    using ExecutionCallback = std::function<void(const ExecutionResult&)>;
    void setExecutionCallback(ExecutionCallback callback);

    // ===== Processing =====

    /**
     * @brief Process pending algorithm steps (call periodically)
     */
    void process();

    /**
     * @brief Handle order update from OMS
     */
    void onOrderUpdate(const Order& order);

    /**
     * @brief Handle fill from OMS
     */
    void onFill(uint64_t orderId, Price price, Quantity qty);

    // ===== Routing =====

    /**
     * @brief Get best routing for order
     */
    [[nodiscard]] std::vector<RoutingDecision> getBestRouting(
        const Symbol& symbol,
        Side side,
        Quantity quantity,
        std::optional<Price> limitPrice = std::nullopt
    );

private:
    std::shared_ptr<OrderManager> orderManager_;
    std::shared_ptr<PositionManager> positionManager_;
    SmartOrderRouter router_;

    // Active executions
    mutable std::shared_mutex mutex_;
    std::unordered_map<uint64_t, std::unique_ptr<ExecutionAlgo>> activeAlgos_;
    std::unordered_map<uint64_t, ExecutionResult> completedExecutions_;

    // Order to execution mapping
    std::unordered_map<uint64_t, uint64_t> orderToExecution_;

    ExecutionCallback executionCallback_;

    std::atomic<uint64_t> nextRequestId_{1};

    // Helper methods
    std::unique_ptr<ExecutionAlgo> createAlgorithm(ExecutionAlgorithm type);
    void completeExecution(uint64_t requestId, ExecutionResult result);
};

/**
 * @brief Builder for execution requests
 */
class ExecutionRequestBuilder {
public:
    ExecutionRequestBuilder& symbol(const Symbol& s);
    ExecutionRequestBuilder& side(Side s);
    ExecutionRequestBuilder& quantity(Quantity q);
    ExecutionRequestBuilder& limitPrice(Price p);
    ExecutionRequestBuilder& algorithm(ExecutionAlgorithm algo);
    ExecutionRequestBuilder& preferredExchange(Exchange e);
    ExecutionRequestBuilder& duration(uint32_t seconds);
    ExecutionRequestBuilder& slices(uint32_t n);
    ExecutionRequestBuilder& participationRate(double rate);
    ExecutionRequestBuilder& postOnly(bool v = true);
    ExecutionRequestBuilder& reduceOnly(bool v = true);
    ExecutionRequestBuilder& maxPrice(Price p);
    ExecutionRequestBuilder& minPrice(Price p);
    ExecutionRequestBuilder& deadline(Timestamp t);
    ExecutionRequestBuilder& strategyId(const std::string& id);

    [[nodiscard]] ExecutionRequest build();

private:
    ExecutionRequest request_;
};

} // namespace hft::oms
