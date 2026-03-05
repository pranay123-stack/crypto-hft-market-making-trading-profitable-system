#pragma once

/**
 * @file risk_manager.hpp
 * @brief Comprehensive Risk Management System for HFT trading
 *
 * Provides real-time risk management including:
 * - Pre-trade risk checks
 * - Position limit validation
 * - Order rate limiting
 * - Notional value limits
 * - Real-time risk monitoring
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

#include "core/types.hpp"
#include "oms/order.hpp"
#include "oms/position_manager.hpp"

namespace hft::risk {

using namespace hft::core;
using namespace hft::oms;

// Forward declarations
class PositionLimits;
class PnLTracker;
class CircuitBreaker;

/**
 * @brief Risk check result
 */
enum class RiskCheckResult : uint8_t {
    Passed = 0,
    RejectedPositionLimit = 1,
    RejectedNotionalLimit = 2,
    RejectedRateLimit = 3,
    RejectedPriceLimit = 4,
    RejectedExchangeLimit = 5,
    RejectedMaxLoss = 6,
    RejectedCircuitBreaker = 7,
    RejectedSymbolDisabled = 8,
    RejectedExchangeDisabled = 9,
    RejectedInvalidOrder = 10,
    Error = 99
};

[[nodiscard]] constexpr std::string_view risk_result_to_string(RiskCheckResult result) noexcept {
    switch (result) {
        case RiskCheckResult::Passed: return "PASSED";
        case RiskCheckResult::RejectedPositionLimit: return "REJECTED_POSITION_LIMIT";
        case RiskCheckResult::RejectedNotionalLimit: return "REJECTED_NOTIONAL_LIMIT";
        case RiskCheckResult::RejectedRateLimit: return "REJECTED_RATE_LIMIT";
        case RiskCheckResult::RejectedPriceLimit: return "REJECTED_PRICE_LIMIT";
        case RiskCheckResult::RejectedExchangeLimit: return "REJECTED_EXCHANGE_LIMIT";
        case RiskCheckResult::RejectedMaxLoss: return "REJECTED_MAX_LOSS";
        case RiskCheckResult::RejectedCircuitBreaker: return "REJECTED_CIRCUIT_BREAKER";
        case RiskCheckResult::RejectedSymbolDisabled: return "REJECTED_SYMBOL_DISABLED";
        case RiskCheckResult::RejectedExchangeDisabled: return "REJECTED_EXCHANGE_DISABLED";
        case RiskCheckResult::RejectedInvalidOrder: return "REJECTED_INVALID_ORDER";
        case RiskCheckResult::Error: return "ERROR";
    }
    return "UNKNOWN";
}

/**
 * @brief Pre-trade risk check response
 */
struct RiskCheckResponse {
    RiskCheckResult result{RiskCheckResult::Passed};
    std::string reason;
    double currentValue{0.0};   // Current value of checked metric
    double limitValue{0.0};     // Limit that was checked
    double utilizationPct{0.0}; // Percentage of limit used

    [[nodiscard]] bool passed() const noexcept {
        return result == RiskCheckResult::Passed;
    }
};

/**
 * @brief Risk limits configuration
 */
struct RiskLimits {
    // Position limits
    double maxPositionPerSymbol{1000000.0};   // Max position value per symbol
    double maxPositionTotal{10000000.0};      // Max total position value
    double maxPositionPerExchange{5000000.0}; // Max position per exchange

    // Notional limits
    double maxOrderNotional{100000.0};        // Max single order value
    double maxDailyNotional{50000000.0};      // Max daily traded value

    // Rate limits
    uint32_t maxOrdersPerSecond{100};         // Max orders per second
    uint32_t maxOrdersPerMinute{1000};        // Max orders per minute
    uint32_t maxCancelsPerSecond{50};         // Max cancels per second

    // Loss limits
    double maxDailyLoss{100000.0};            // Max daily loss
    double maxDrawdown{50000.0};              // Max drawdown from peak

    // Price limits
    double maxPriceDeviationPct{5.0};         // Max % deviation from market

    // Per-symbol overrides
    std::unordered_map<std::string, double> symbolPositionLimits;
    std::unordered_map<std::string, double> symbolNotionalLimits;

    // Disabled symbols/exchanges
    std::vector<std::string> disabledSymbols;
    std::vector<Exchange> disabledExchanges;
};

/**
 * @brief Real-time risk metrics
 */
struct RiskMetrics {
    // Position metrics
    double totalPositionValue{0.0};
    double grossExposure{0.0};
    double netExposure{0.0};
    size_t openPositionCount{0};

    // PnL metrics
    double dailyPnL{0.0};
    double realizedPnL{0.0};
    double unrealizedPnL{0.0};
    double currentDrawdown{0.0};
    double peakPnL{0.0};

    // Order metrics
    uint64_t ordersToday{0};
    uint64_t fillsToday{0};
    uint64_t cancelsToday{0};
    uint64_t rejectsToday{0};
    double dailyNotionalTraded{0.0};

    // Rate metrics
    uint32_t ordersLastSecond{0};
    uint32_t ordersLastMinute{0};
    uint32_t cancelsLastSecond{0};

    // Utilization percentages
    double positionUtilization{0.0};
    double notionalUtilization{0.0};
    double lossUtilization{0.0};
    double rateUtilization{0.0};

    Timestamp lastUpdateTime;

    [[nodiscard]] bool isHighRisk() const noexcept {
        return positionUtilization > 80.0 ||
               notionalUtilization > 80.0 ||
               lossUtilization > 80.0;
    }
};

/**
 * @brief Order rate limiter
 */
class OrderRateLimiter {
public:
    OrderRateLimiter(uint32_t maxPerSecond, uint32_t maxPerMinute);

    /**
     * @brief Check if order can be sent
     */
    [[nodiscard]] bool checkAndIncrement();

    /**
     * @brief Get current rate
     */
    [[nodiscard]] uint32_t getCurrentRate() const;

    /**
     * @brief Get rate per minute
     */
    [[nodiscard]] uint32_t getRatePerMinute() const;

    /**
     * @brief Reset counters
     */
    void reset();

private:
    uint32_t maxPerSecond_;
    uint32_t maxPerMinute_;

    mutable std::mutex mutex_;
    std::deque<std::chrono::steady_clock::time_point> recentOrders_;
};

/**
 * @brief Risk event callback types
 */
using RiskCallback = std::function<void(const RiskCheckResponse&)>;
using RiskAlertCallback = std::function<void(const std::string& alert, RiskCheckResult severity)>;

/**
 * @brief Central Risk Manager
 *
 * Thread-safe risk management with support for:
 * - Pre-trade risk checks
 * - Real-time monitoring
 * - Configurable limits
 * - Circuit breaker integration
 */
class RiskManager {
public:
    /**
     * @brief Constructor
     */
    RiskManager(std::shared_ptr<PositionManager> positionManager);

    /**
     * @brief Destructor
     */
    ~RiskManager();

    // Non-copyable
    RiskManager(const RiskManager&) = delete;
    RiskManager& operator=(const RiskManager&) = delete;

    // ===== Pre-trade Risk Checks =====

    /**
     * @brief Perform full pre-trade risk check on order
     */
    [[nodiscard]] RiskCheckResponse checkOrder(const Order& order);

    /**
     * @brief Check position limits for potential order
     */
    [[nodiscard]] RiskCheckResponse checkPositionLimit(
        const Symbol& symbol,
        Exchange exchange,
        Side side,
        double quantity,
        double price
    );

    /**
     * @brief Check notional limits
     */
    [[nodiscard]] RiskCheckResponse checkNotionalLimit(double notional);

    /**
     * @brief Check order rate limits
     */
    [[nodiscard]] RiskCheckResponse checkRateLimit();

    /**
     * @brief Check price reasonability
     */
    [[nodiscard]] RiskCheckResponse checkPriceLimit(
        const Symbol& symbol,
        double orderPrice,
        double marketPrice
    );

    /**
     * @brief Check if symbol is tradeable
     */
    [[nodiscard]] bool isSymbolEnabled(const Symbol& symbol) const;

    /**
     * @brief Check if exchange is enabled
     */
    [[nodiscard]] bool isExchangeEnabled(Exchange exchange) const;

    // ===== Configuration =====

    /**
     * @brief Set risk limits
     */
    void setLimits(const RiskLimits& limits);

    /**
     * @brief Get current limits
     */
    [[nodiscard]] const RiskLimits& getLimits() const;

    /**
     * @brief Update limit for specific symbol
     */
    void setSymbolPositionLimit(const Symbol& symbol, double limit);

    /**
     * @brief Disable symbol trading
     */
    void disableSymbol(const Symbol& symbol);

    /**
     * @brief Enable symbol trading
     */
    void enableSymbol(const Symbol& symbol);

    /**
     * @brief Disable exchange trading
     */
    void disableExchange(Exchange exchange);

    /**
     * @brief Enable exchange trading
     */
    void enableExchange(Exchange exchange);

    // ===== Market Data Updates =====

    /**
     * @brief Update market price for symbol (for price checks)
     */
    void updateMarketPrice(const Symbol& symbol, double price);

    // ===== Real-time Monitoring =====

    /**
     * @brief Get current risk metrics
     */
    [[nodiscard]] RiskMetrics getMetrics() const;

    /**
     * @brief Update metrics (call periodically)
     */
    void updateMetrics();

    /**
     * @brief Record order submission
     */
    void recordOrderSubmit();

    /**
     * @brief Record order cancellation
     */
    void recordOrderCancel();

    /**
     * @brief Record fill
     */
    void recordFill(double notional);

    /**
     * @brief Record rejection
     */
    void recordReject();

    // ===== Circuit Breaker Integration =====

    /**
     * @brief Set circuit breaker
     */
    void setCircuitBreaker(std::shared_ptr<CircuitBreaker> circuitBreaker);

    /**
     * @brief Check if circuit breaker is triggered
     */
    [[nodiscard]] bool isCircuitBreakerTriggered() const;

    // ===== Callbacks =====

    /**
     * @brief Set callback for risk rejections
     */
    void setRiskCallback(RiskCallback callback);

    /**
     * @brief Set callback for risk alerts
     */
    void setAlertCallback(RiskAlertCallback callback);

    // ===== Statistics =====

    /**
     * @brief Reset daily statistics
     */
    void resetDailyStats();

    /**
     * @brief Get position manager
     */
    [[nodiscard]] std::shared_ptr<PositionManager> getPositionManager() const {
        return positionManager_;
    }

private:
    std::shared_ptr<PositionManager> positionManager_;
    std::shared_ptr<CircuitBreaker> circuitBreaker_;

    mutable std::shared_mutex mutex_;
    RiskLimits limits_;
    RiskMetrics metrics_;

    // Market prices for price checks
    std::unordered_map<std::string, double> marketPrices_;

    // Rate limiters
    std::unique_ptr<OrderRateLimiter> orderRateLimiter_;
    std::unique_ptr<OrderRateLimiter> cancelRateLimiter_;

    // Daily counters (atomic for thread safety)
    std::atomic<uint64_t> dailyOrders_{0};
    std::atomic<uint64_t> dailyFills_{0};
    std::atomic<uint64_t> dailyCancels_{0};
    std::atomic<uint64_t> dailyRejects_{0};
    std::atomic<int64_t> dailyNotionalCents_{0};

    // Callbacks
    RiskCallback riskCallback_;
    RiskAlertCallback alertCallback_;

    // Helper methods
    void notifyRiskCheck(const RiskCheckResponse& response);
    void notifyAlert(const std::string& alert, RiskCheckResult severity);
    double getSymbolPositionLimit(const Symbol& symbol) const;
    double getSymbolNotionalLimit(const Symbol& symbol) const;
};

/**
 * @brief Risk manager builder
 */
class RiskManagerBuilder {
public:
    RiskManagerBuilder& withPositionManager(std::shared_ptr<PositionManager> pm);
    RiskManagerBuilder& withLimits(const RiskLimits& limits);
    RiskManagerBuilder& withCircuitBreaker(std::shared_ptr<CircuitBreaker> cb);
    RiskManagerBuilder& withRiskCallback(RiskCallback callback);
    RiskManagerBuilder& withAlertCallback(RiskAlertCallback callback);

    std::unique_ptr<RiskManager> build();

private:
    std::shared_ptr<PositionManager> positionManager_;
    RiskLimits limits_;
    std::shared_ptr<CircuitBreaker> circuitBreaker_;
    RiskCallback riskCallback_;
    RiskAlertCallback alertCallback_;
};

} // namespace hft::risk
