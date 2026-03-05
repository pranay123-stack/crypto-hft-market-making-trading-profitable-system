#pragma once

/**
 * @file circuit_breaker.hpp
 * @brief Circuit Breaker for HFT trading system emergency stop
 *
 * Provides emergency stop functionality including:
 * - Trigger conditions: max loss, unusual activity, exchange issues
 * - Automatic position flattening
 * - Manual override support
 * - Cooldown periods
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
#include "oms/order_manager.hpp"
#include "oms/position_manager.hpp"

namespace hft::risk {

using namespace hft::core;
using namespace hft::oms;

/**
 * @brief Circuit breaker trigger reason
 */
enum class TriggerReason : uint8_t {
    None = 0,
    MaxLoss = 1,              // Maximum loss threshold reached
    MaxDrawdown = 2,          // Maximum drawdown reached
    RapidLoss = 3,            // Rapid loss over short period
    UnusualVolatility = 4,    // Market volatility spike
    ExchangeIssue = 5,        // Exchange connectivity or data issues
    DataFeedIssue = 6,        // Market data stale or invalid
    HighOrderRejectRate = 7,  // Too many order rejections
    HighLatency = 8,          // System latency spike
    ManualTrigger = 9,        // Manually triggered
    SystemError = 10,         // Internal system error
    RiskLimitBreach = 11,     // Risk limit exceeded
    PositionReconciliation = 12, // Position mismatch detected
    ExternalSignal = 13       // External kill switch signal
};

[[nodiscard]] constexpr std::string_view trigger_reason_to_string(TriggerReason reason) noexcept {
    switch (reason) {
        case TriggerReason::None: return "NONE";
        case TriggerReason::MaxLoss: return "MAX_LOSS";
        case TriggerReason::MaxDrawdown: return "MAX_DRAWDOWN";
        case TriggerReason::RapidLoss: return "RAPID_LOSS";
        case TriggerReason::UnusualVolatility: return "UNUSUAL_VOLATILITY";
        case TriggerReason::ExchangeIssue: return "EXCHANGE_ISSUE";
        case TriggerReason::DataFeedIssue: return "DATA_FEED_ISSUE";
        case TriggerReason::HighOrderRejectRate: return "HIGH_ORDER_REJECT_RATE";
        case TriggerReason::HighLatency: return "HIGH_LATENCY";
        case TriggerReason::ManualTrigger: return "MANUAL_TRIGGER";
        case TriggerReason::SystemError: return "SYSTEM_ERROR";
        case TriggerReason::RiskLimitBreach: return "RISK_LIMIT_BREACH";
        case TriggerReason::PositionReconciliation: return "POSITION_RECONCILIATION";
        case TriggerReason::ExternalSignal: return "EXTERNAL_SIGNAL";
    }
    return "UNKNOWN";
}

/**
 * @brief Circuit breaker state
 */
enum class CircuitState : uint8_t {
    Normal = 0,       // Normal operation
    Warning = 1,      // Warning state, monitoring closely
    Triggered = 2,    // Circuit breaker triggered, stopping trades
    Cooldown = 3,     // In cooldown period before resuming
    Halted = 4        // Permanently halted until manual reset
};

[[nodiscard]] constexpr std::string_view circuit_state_to_string(CircuitState state) noexcept {
    switch (state) {
        case CircuitState::Normal: return "NORMAL";
        case CircuitState::Warning: return "WARNING";
        case CircuitState::Triggered: return "TRIGGERED";
        case CircuitState::Cooldown: return "COOLDOWN";
        case CircuitState::Halted: return "HALTED";
    }
    return "UNKNOWN";
}

/**
 * @brief Circuit breaker trigger event
 */
struct TriggerEvent {
    TriggerReason reason{TriggerReason::None};
    std::string description;
    double value{0.0};          // Trigger value (e.g., loss amount)
    double threshold{0.0};      // Threshold that was exceeded
    Timestamp timestamp;
    std::string source;         // What component triggered it
};

/**
 * @brief Circuit breaker configuration
 */
struct CircuitBreakerConfig {
    // Loss triggers
    double maxDailyLoss{100000.0};
    double maxDrawdown{50000.0};
    double rapidLossThreshold{10000.0};
    std::chrono::seconds rapidLossWindow{60};

    // Order quality triggers
    double maxOrderRejectRate{0.1};     // 10%
    std::chrono::seconds rejectRateWindow{60};
    uint32_t maxConsecutiveRejects{5};

    // Latency triggers
    double maxLatencyMs{1000.0};
    uint32_t maxLatencySpikes{3};       // In window

    // Market data triggers
    std::chrono::seconds maxDataStaleTime{5};

    // Cooldown settings
    std::chrono::seconds cooldownPeriod{300}; // 5 minutes
    bool autoRecoverEnabled{false};     // Auto-recover after cooldown

    // Behavior settings
    bool cancelOrdersOnTrigger{true};
    bool flattenPositionsOnTrigger{false};
    bool blockNewOrdersOnly{true};      // vs. full halt
};

/**
 * @brief Circuit breaker status
 */
struct CircuitBreakerStatus {
    CircuitState state{CircuitState::Normal};
    TriggerReason lastTriggerReason{TriggerReason::None};
    std::string lastTriggerMessage;
    Timestamp lastTriggerTime;
    Timestamp cooldownEndTime;

    uint32_t triggerCount{0};
    uint32_t todayTriggerCount{0};

    std::vector<TriggerEvent> recentEvents;

    [[nodiscard]] bool canTrade() const noexcept {
        return state == CircuitState::Normal || state == CircuitState::Warning;
    }

    [[nodiscard]] uint64_t remainingCooldownMs() const noexcept {
        if (state != CircuitState::Cooldown) return 0;
        auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
        if (cooldownEndTime.nanos > static_cast<uint64_t>(now)) {
            return (cooldownEndTime.nanos - now) / 1000000;
        }
        return 0;
    }
};

/**
 * @brief Callback types for circuit breaker events
 */
using TriggerCallback = std::function<void(TriggerReason, const std::string&)>;
using StateChangeCallback = std::function<void(CircuitState oldState, CircuitState newState)>;
using RecoveryCallback = std::function<void()>;

/**
 * @brief Circuit Breaker
 *
 * Thread-safe emergency stop mechanism with support for:
 * - Multiple trigger conditions
 * - Position flattening
 * - Cooldown periods
 * - Manual override
 */
class CircuitBreaker {
public:
    /**
     * @brief Constructor
     */
    CircuitBreaker();

    /**
     * @brief Constructor with configuration
     */
    explicit CircuitBreaker(const CircuitBreakerConfig& config);

    /**
     * @brief Destructor
     */
    ~CircuitBreaker();

    // Non-copyable
    CircuitBreaker(const CircuitBreaker&) = delete;
    CircuitBreaker& operator=(const CircuitBreaker&) = delete;

    // ===== State Management =====

    /**
     * @brief Check if circuit breaker is triggered
     */
    [[nodiscard]] bool isTriggered() const noexcept;

    /**
     * @brief Check if trading is allowed
     */
    [[nodiscard]] bool canTrade() const noexcept;

    /**
     * @brief Get current state
     */
    [[nodiscard]] CircuitState getState() const noexcept;

    /**
     * @brief Get full status
     */
    [[nodiscard]] CircuitBreakerStatus getStatus() const;

    // ===== Trigger Conditions =====

    /**
     * @brief Trigger circuit breaker manually
     */
    void trigger(TriggerReason reason, const std::string& message = "");

    /**
     * @brief Check PnL-based triggers
     */
    void checkPnL(double dailyPnL, double drawdown);

    /**
     * @brief Check for rapid loss
     */
    void checkRapidLoss(double recentLoss, std::chrono::seconds window);

    /**
     * @brief Record order rejection
     */
    void recordOrderReject();

    /**
     * @brief Record order success
     */
    void recordOrderSuccess();

    /**
     * @brief Check exchange connectivity
     */
    void checkExchangeStatus(Exchange exchange, bool connected);

    /**
     * @brief Check market data freshness
     */
    void checkDataFreshness(const Symbol& symbol, std::chrono::milliseconds dataAge);

    /**
     * @brief Check latency
     */
    void checkLatency(double latencyMs);

    /**
     * @brief External trigger signal (e.g., from monitoring system)
     */
    void externalTrigger(const std::string& source, const std::string& message);

    // ===== Recovery =====

    /**
     * @brief Reset circuit breaker (manual recovery)
     */
    void reset();

    /**
     * @brief Start cooldown period
     */
    void startCooldown();

    /**
     * @brief Check and process cooldown expiration
     */
    void processCooldown();

    /**
     * @brief Enter warning state
     */
    void enterWarningState(const std::string& reason);

    /**
     * @brief Return to normal state
     */
    void returnToNormal();

    // ===== Emergency Actions =====

    /**
     * @brief Cancel all active orders
     */
    void cancelAllOrders();

    /**
     * @brief Flatten all positions
     */
    void flattenAllPositions();

    /**
     * @brief Execute emergency stop
     */
    void emergencyStop(TriggerReason reason, const std::string& message);

    // ===== Configuration =====

    /**
     * @brief Set configuration
     */
    void setConfig(const CircuitBreakerConfig& config);

    /**
     * @brief Get configuration
     */
    [[nodiscard]] const CircuitBreakerConfig& getConfig() const;

    /**
     * @brief Enable/disable circuit breaker
     */
    void setEnabled(bool enabled);

    /**
     * @brief Check if enabled
     */
    [[nodiscard]] bool isEnabled() const noexcept;

    // ===== Integration =====

    /**
     * @brief Set order manager for cancellation
     */
    void setOrderManager(std::shared_ptr<OrderManager> orderManager);

    /**
     * @brief Set position manager for flattening
     */
    void setPositionManager(std::shared_ptr<PositionManager> positionManager);

    // ===== Callbacks =====

    /**
     * @brief Set trigger callback
     */
    void setTriggerCallback(TriggerCallback callback);

    /**
     * @brief Set state change callback
     */
    void setStateChangeCallback(StateChangeCallback callback);

    /**
     * @brief Set recovery callback
     */
    void setRecoveryCallback(RecoveryCallback callback);

    // ===== Statistics =====

    /**
     * @brief Get trigger count
     */
    [[nodiscard]] uint32_t getTriggerCount() const noexcept;

    /**
     * @brief Get today's trigger count
     */
    [[nodiscard]] uint32_t getTodayTriggerCount() const noexcept;

    /**
     * @brief Get recent trigger events
     */
    [[nodiscard]] std::vector<TriggerEvent> getRecentEvents(size_t count = 10) const;

    /**
     * @brief Reset daily statistics
     */
    void resetDailyStats();

private:
    CircuitBreakerConfig config_;

    mutable std::shared_mutex mutex_;

    // State
    std::atomic<CircuitState> state_{CircuitState::Normal};
    std::atomic<bool> enabled_{true};

    TriggerReason lastTriggerReason_{TriggerReason::None};
    std::string lastTriggerMessage_;
    Timestamp lastTriggerTime_;
    Timestamp cooldownEndTime_;

    // Statistics
    std::atomic<uint32_t> triggerCount_{0};
    std::atomic<uint32_t> todayTriggerCount_{0};

    // Order tracking for reject rate
    std::deque<std::pair<std::chrono::steady_clock::time_point, bool>> recentOrders_;
    std::atomic<uint32_t> consecutiveRejects_{0};

    // Latency tracking
    std::deque<std::pair<std::chrono::steady_clock::time_point, double>> recentLatencies_;

    // Event history
    std::deque<TriggerEvent> eventHistory_;
    static constexpr size_t MAX_EVENT_HISTORY = 100;

    // Exchange status
    std::unordered_map<Exchange, bool> exchangeStatus_;

    // Integration
    std::shared_ptr<OrderManager> orderManager_;
    std::shared_ptr<PositionManager> positionManager_;

    // Callbacks
    TriggerCallback triggerCallback_;
    StateChangeCallback stateChangeCallback_;
    RecoveryCallback recoveryCallback_;

    // Helper methods
    void transitionState(CircuitState newState);
    void recordEvent(TriggerReason reason, const std::string& description,
                     double value = 0.0, double threshold = 0.0);
    void notifyTrigger(TriggerReason reason, const std::string& message);
    void executeEmergencyActions();
    double calculateRejectRate() const;
};

/**
 * @brief Circuit breaker builder
 */
class CircuitBreakerBuilder {
public:
    CircuitBreakerBuilder& withConfig(const CircuitBreakerConfig& config);
    CircuitBreakerBuilder& withMaxDailyLoss(double limit);
    CircuitBreakerBuilder& withMaxDrawdown(double limit);
    CircuitBreakerBuilder& withCooldownPeriod(std::chrono::seconds period);
    CircuitBreakerBuilder& withOrderManager(std::shared_ptr<OrderManager> om);
    CircuitBreakerBuilder& withPositionManager(std::shared_ptr<PositionManager> pm);
    CircuitBreakerBuilder& withTriggerCallback(TriggerCallback callback);
    CircuitBreakerBuilder& withStateChangeCallback(StateChangeCallback callback);
    CircuitBreakerBuilder& withRecoveryCallback(RecoveryCallback callback);
    CircuitBreakerBuilder& enableAutoRecovery(bool enable = true);
    CircuitBreakerBuilder& enablePositionFlattening(bool enable = true);

    std::unique_ptr<CircuitBreaker> build();

private:
    CircuitBreakerConfig config_;
    std::shared_ptr<OrderManager> orderManager_;
    std::shared_ptr<PositionManager> positionManager_;
    TriggerCallback triggerCallback_;
    StateChangeCallback stateChangeCallback_;
    RecoveryCallback recoveryCallback_;
};

} // namespace hft::risk
