/**
 * @file circuit_breaker.cpp
 * @brief Implementation of Circuit Breaker
 */

#include "risk/circuit_breaker.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <chrono>

namespace hft::risk {

// ============================================================================
// CircuitBreaker Implementation
// ============================================================================

CircuitBreaker::CircuitBreaker() {
    LOG_INFO("CircuitBreaker initialized");
}

CircuitBreaker::CircuitBreaker(const CircuitBreakerConfig& config)
    : config_(config)
{
    LOG_INFO("CircuitBreaker initialized with config - maxDailyLoss: {}, maxDrawdown: {}",
             config.maxDailyLoss, config.maxDrawdown);
}

CircuitBreaker::~CircuitBreaker() {
    LOG_INFO("CircuitBreaker destroyed - total triggers: {}", triggerCount_.load());
}

bool CircuitBreaker::isTriggered() const noexcept {
    CircuitState state = state_.load(std::memory_order_acquire);
    return state == CircuitState::Triggered || state == CircuitState::Halted;
}

bool CircuitBreaker::canTrade() const noexcept {
    if (!enabled_.load(std::memory_order_relaxed)) {
        return true; // If disabled, allow trading
    }

    CircuitState state = state_.load(std::memory_order_acquire);
    return state == CircuitState::Normal || state == CircuitState::Warning;
}

CircuitState CircuitBreaker::getState() const noexcept {
    return state_.load(std::memory_order_acquire);
}

CircuitBreakerStatus CircuitBreaker::getStatus() const {
    std::shared_lock lock(mutex_);

    CircuitBreakerStatus status;
    status.state = state_.load(std::memory_order_relaxed);
    status.lastTriggerReason = lastTriggerReason_;
    status.lastTriggerMessage = lastTriggerMessage_;
    status.lastTriggerTime = lastTriggerTime_;
    status.cooldownEndTime = cooldownEndTime_;
    status.triggerCount = triggerCount_.load(std::memory_order_relaxed);
    status.todayTriggerCount = todayTriggerCount_.load(std::memory_order_relaxed);

    // Copy recent events
    size_t count = std::min(eventHistory_.size(), size_t(10));
    status.recentEvents.reserve(count);
    auto it = eventHistory_.rbegin();
    for (size_t i = 0; i < count && it != eventHistory_.rend(); ++i, ++it) {
        status.recentEvents.push_back(*it);
    }

    return status;
}

void CircuitBreaker::trigger(TriggerReason reason, const std::string& message) {
    if (!enabled_.load(std::memory_order_relaxed)) {
        return;
    }

    std::unique_lock lock(mutex_);

    // Already triggered?
    CircuitState currentState = state_.load(std::memory_order_relaxed);
    if (currentState == CircuitState::Triggered || currentState == CircuitState::Halted) {
        LOG_WARN("Circuit breaker already triggered, ignoring new trigger: {}",
                 trigger_reason_to_string(reason));
        return;
    }

    LOG_ERROR("CIRCUIT BREAKER TRIGGERED: {} - {}", trigger_reason_to_string(reason), message);

    // Record trigger
    lastTriggerReason_ = reason;
    lastTriggerMessage_ = message;
    lastTriggerTime_ = Timestamp{static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count())};

    triggerCount_.fetch_add(1, std::memory_order_relaxed);
    todayTriggerCount_.fetch_add(1, std::memory_order_relaxed);

    recordEvent(reason, message);

    // Transition state
    transitionState(CircuitState::Triggered);

    lock.unlock();

    // Execute emergency actions
    executeEmergencyActions();

    // Notify
    notifyTrigger(reason, message);
}

void CircuitBreaker::checkPnL(double dailyPnL, double drawdown) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    if (!canTrade()) return;

    // Check daily loss
    if (dailyPnL < -config_.maxDailyLoss) {
        trigger(TriggerReason::MaxLoss,
                "Daily loss limit exceeded: " + std::to_string(dailyPnL) +
                " > " + std::to_string(-config_.maxDailyLoss));
        return;
    }

    // Warning at 80% of limit
    if (dailyPnL < -config_.maxDailyLoss * 0.8) {
        enterWarningState("Approaching daily loss limit");
    }

    // Check drawdown
    if (drawdown > config_.maxDrawdown) {
        trigger(TriggerReason::MaxDrawdown,
                "Max drawdown exceeded: " + std::to_string(drawdown) +
                " > " + std::to_string(config_.maxDrawdown));
        return;
    }

    // Warning at 80% of limit
    if (drawdown > config_.maxDrawdown * 0.8) {
        enterWarningState("Approaching max drawdown");
    }
}

void CircuitBreaker::checkRapidLoss(double recentLoss, std::chrono::seconds window) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    if (!canTrade()) return;

    if (recentLoss > config_.rapidLossThreshold) {
        trigger(TriggerReason::RapidLoss,
                "Rapid loss detected: " + std::to_string(recentLoss) +
                " over " + std::to_string(window.count()) + " seconds");
    }
}

void CircuitBreaker::recordOrderReject() {
    std::unique_lock lock(mutex_);

    auto now = std::chrono::steady_clock::now();
    recentOrders_.emplace_back(now, false);

    // Increment consecutive rejects
    consecutiveRejects_.fetch_add(1, std::memory_order_relaxed);

    // Check consecutive rejects
    if (consecutiveRejects_.load(std::memory_order_relaxed) >= config_.maxConsecutiveRejects) {
        lock.unlock();
        trigger(TriggerReason::HighOrderRejectRate,
                "Maximum consecutive rejects reached: " +
                std::to_string(config_.maxConsecutiveRejects));
        return;
    }

    // Cleanup old orders
    auto cutoff = now - config_.rejectRateWindow;
    while (!recentOrders_.empty() && recentOrders_.front().first < cutoff) {
        recentOrders_.pop_front();
    }

    // Check reject rate
    double rejectRate = calculateRejectRate();
    if (rejectRate > config_.maxOrderRejectRate) {
        lock.unlock();
        trigger(TriggerReason::HighOrderRejectRate,
                "Order reject rate exceeded: " + std::to_string(rejectRate * 100) + "%");
    }
}

void CircuitBreaker::recordOrderSuccess() {
    std::unique_lock lock(mutex_);

    auto now = std::chrono::steady_clock::now();
    recentOrders_.emplace_back(now, true);

    // Reset consecutive rejects
    consecutiveRejects_.store(0, std::memory_order_relaxed);

    // Cleanup old orders
    auto cutoff = now - config_.rejectRateWindow;
    while (!recentOrders_.empty() && recentOrders_.front().first < cutoff) {
        recentOrders_.pop_front();
    }
}

void CircuitBreaker::checkExchangeStatus(Exchange exchange, bool connected) {
    std::unique_lock lock(mutex_);

    bool wasConnected = true;
    auto it = exchangeStatus_.find(exchange);
    if (it != exchangeStatus_.end()) {
        wasConnected = it->second;
    }

    exchangeStatus_[exchange] = connected;

    // Trigger if exchange went down
    if (wasConnected && !connected) {
        lock.unlock();
        enterWarningState("Exchange disconnected: " + std::string(exchange_to_string(exchange)));

        // Count disconnected exchanges
        size_t disconnectedCount = 0;
        for (const auto& [ex, status] : exchangeStatus_) {
            if (!status) ++disconnectedCount;
        }

        // Trigger if too many exchanges are down
        if (disconnectedCount >= 2) {
            trigger(TriggerReason::ExchangeIssue,
                    "Multiple exchanges disconnected: " + std::to_string(disconnectedCount));
        }
    }
}

void CircuitBreaker::checkDataFreshness(const Symbol& symbol, std::chrono::milliseconds dataAge) {
    if (!enabled_.load(std::memory_order_relaxed)) return;

    auto maxStale = std::chrono::duration_cast<std::chrono::milliseconds>(config_.maxDataStaleTime);

    if (dataAge > maxStale) {
        enterWarningState("Stale market data for " + std::string(symbol.view()));

        // Trigger if data is very stale (5x threshold)
        if (dataAge > maxStale * 5) {
            trigger(TriggerReason::DataFeedIssue,
                    "Market data critically stale for " + std::string(symbol.view()) +
                    ": " + std::to_string(dataAge.count()) + "ms");
        }
    }
}

void CircuitBreaker::checkLatency(double latencyMs) {
    if (!enabled_.load(std::memory_order_relaxed)) return;

    std::unique_lock lock(mutex_);

    auto now = std::chrono::steady_clock::now();
    recentLatencies_.emplace_back(now, latencyMs);

    // Cleanup old entries (last 60 seconds)
    auto cutoff = now - std::chrono::seconds(60);
    while (!recentLatencies_.empty() && recentLatencies_.front().first < cutoff) {
        recentLatencies_.pop_front();
    }

    // Count spikes
    uint32_t spikeCount = 0;
    for (const auto& [time, lat] : recentLatencies_) {
        if (lat > config_.maxLatencyMs) {
            ++spikeCount;
        }
    }

    if (spikeCount >= config_.maxLatencySpikes) {
        lock.unlock();
        trigger(TriggerReason::HighLatency,
                "High latency detected: " + std::to_string(spikeCount) +
                " spikes in last 60 seconds");
    }
}

void CircuitBreaker::externalTrigger(const std::string& source, const std::string& message) {
    trigger(TriggerReason::ExternalSignal,
            "[" + source + "] " + message);
}

void CircuitBreaker::reset() {
    std::unique_lock lock(mutex_);

    CircuitState oldState = state_.load(std::memory_order_relaxed);

    state_.store(CircuitState::Normal, std::memory_order_release);
    consecutiveRejects_.store(0, std::memory_order_relaxed);

    LOG_INFO("Circuit breaker reset from state: {}", circuit_state_to_string(oldState));

    lock.unlock();

    if (stateChangeCallback_) {
        stateChangeCallback_(oldState, CircuitState::Normal);
    }

    if (recoveryCallback_) {
        recoveryCallback_();
    }
}

void CircuitBreaker::startCooldown() {
    std::unique_lock lock(mutex_);

    auto now = std::chrono::high_resolution_clock::now();
    auto cooldownEnd = now + config_.cooldownPeriod;

    cooldownEndTime_ = Timestamp{static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            cooldownEnd.time_since_epoch()).count())};

    transitionState(CircuitState::Cooldown);

    LOG_INFO("Circuit breaker entering cooldown for {} seconds",
             config_.cooldownPeriod.count());
}

void CircuitBreaker::processCooldown() {
    if (state_.load(std::memory_order_relaxed) != CircuitState::Cooldown) {
        return;
    }

    auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();

    if (static_cast<uint64_t>(now) >= cooldownEndTime_.nanos) {
        if (config_.autoRecoverEnabled) {
            returnToNormal();
        } else {
            // Stay in cooldown until manual reset
            LOG_INFO("Cooldown period ended - waiting for manual reset");
        }
    }
}

void CircuitBreaker::enterWarningState(const std::string& reason) {
    CircuitState currentState = state_.load(std::memory_order_relaxed);

    if (currentState == CircuitState::Normal) {
        transitionState(CircuitState::Warning);
        LOG_WARN("Circuit breaker entering warning state: {}", reason);
    }
}

void CircuitBreaker::returnToNormal() {
    std::unique_lock lock(mutex_);

    CircuitState oldState = state_.load(std::memory_order_relaxed);
    state_.store(CircuitState::Normal, std::memory_order_release);

    LOG_INFO("Circuit breaker returning to normal from state: {}",
             circuit_state_to_string(oldState));

    lock.unlock();

    if (stateChangeCallback_) {
        stateChangeCallback_(oldState, CircuitState::Normal);
    }

    if (recoveryCallback_) {
        recoveryCallback_();
    }
}

void CircuitBreaker::cancelAllOrders() {
    if (!orderManager_) {
        LOG_WARN("Cannot cancel orders - no order manager set");
        return;
    }

    LOG_WARN("Circuit breaker cancelling all orders");
    size_t cancelled = orderManager_->cancelAllOrders();
    LOG_INFO("Cancelled {} orders", cancelled);
}

void CircuitBreaker::flattenAllPositions() {
    if (!positionManager_) {
        LOG_WARN("Cannot flatten positions - no position manager set");
        return;
    }

    LOG_WARN("Circuit breaker flattening all positions");

    // Get all open positions
    auto positions = positionManager_->getAllOpenPositions();

    // For each position, we would need to place closing orders
    // This requires order manager integration
    if (orderManager_) {
        for (const auto& pos : positions) {
            if (pos.isFlat()) continue;

            // Determine closing side
            Side closeSide = pos.isLong() ? Side::Sell : Side::Buy;
            double closeQty = pos.absQuantity();

            LOG_INFO("Flattening position: {} {} {} on {}",
                     closeQty, side_to_string(closeSide),
                     pos.symbol.view(), exchange_to_string(pos.exchange));

            // Submit market order to close
            orderManager_->submitOrder(
                pos.symbol,
                pos.exchange,
                closeSide,
                OrderType::Market,
                Price{0}, // Market order, no price
                Quantity::from_double(closeQty),
                TimeInForce::IOC,
                "CIRCUIT_BREAKER_FLATTEN"
            );
        }
    } else {
        // Without order manager, just mark positions as flat
        positionManager_->flattenAllPositions();
    }
}

void CircuitBreaker::emergencyStop(TriggerReason reason, const std::string& message) {
    LOG_FATAL("EMERGENCY STOP: {} - {}", trigger_reason_to_string(reason), message);

    // Set halted state (permanent until manual intervention)
    {
        std::unique_lock lock(mutex_);
        lastTriggerReason_ = reason;
        lastTriggerMessage_ = message;
        lastTriggerTime_ = Timestamp{static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::high_resolution_clock::now().time_since_epoch()).count())};
        recordEvent(reason, message);
        transitionState(CircuitState::Halted);
    }

    // Emergency actions
    cancelAllOrders();
    flattenAllPositions();

    notifyTrigger(reason, "EMERGENCY STOP: " + message);
}

void CircuitBreaker::setConfig(const CircuitBreakerConfig& config) {
    std::unique_lock lock(mutex_);
    config_ = config;
    LOG_INFO("Circuit breaker config updated");
}

const CircuitBreakerConfig& CircuitBreaker::getConfig() const {
    std::shared_lock lock(mutex_);
    return config_;
}

void CircuitBreaker::setEnabled(bool enabled) {
    enabled_.store(enabled, std::memory_order_release);
    LOG_INFO("Circuit breaker {}", enabled ? "enabled" : "disabled");
}

bool CircuitBreaker::isEnabled() const noexcept {
    return enabled_.load(std::memory_order_relaxed);
}

void CircuitBreaker::setOrderManager(std::shared_ptr<OrderManager> orderManager) {
    orderManager_ = std::move(orderManager);
}

void CircuitBreaker::setPositionManager(std::shared_ptr<PositionManager> positionManager) {
    positionManager_ = std::move(positionManager);
}

void CircuitBreaker::setTriggerCallback(TriggerCallback callback) {
    triggerCallback_ = std::move(callback);
}

void CircuitBreaker::setStateChangeCallback(StateChangeCallback callback) {
    stateChangeCallback_ = std::move(callback);
}

void CircuitBreaker::setRecoveryCallback(RecoveryCallback callback) {
    recoveryCallback_ = std::move(callback);
}

uint32_t CircuitBreaker::getTriggerCount() const noexcept {
    return triggerCount_.load(std::memory_order_relaxed);
}

uint32_t CircuitBreaker::getTodayTriggerCount() const noexcept {
    return todayTriggerCount_.load(std::memory_order_relaxed);
}

std::vector<TriggerEvent> CircuitBreaker::getRecentEvents(size_t count) const {
    std::shared_lock lock(mutex_);

    std::vector<TriggerEvent> result;
    size_t eventCount = std::min(count, eventHistory_.size());
    result.reserve(eventCount);

    auto it = eventHistory_.rbegin();
    for (size_t i = 0; i < eventCount && it != eventHistory_.rend(); ++i, ++it) {
        result.push_back(*it);
    }

    return result;
}

void CircuitBreaker::resetDailyStats() {
    todayTriggerCount_.store(0, std::memory_order_relaxed);
    LOG_INFO("Circuit breaker daily stats reset");
}

void CircuitBreaker::transitionState(CircuitState newState) {
    CircuitState oldState = state_.load(std::memory_order_relaxed);
    state_.store(newState, std::memory_order_release);

    LOG_INFO("Circuit breaker state: {} -> {}",
             circuit_state_to_string(oldState), circuit_state_to_string(newState));

    if (stateChangeCallback_) {
        stateChangeCallback_(oldState, newState);
    }
}

void CircuitBreaker::recordEvent(TriggerReason reason, const std::string& description,
                                  double value, double threshold) {
    TriggerEvent event;
    event.reason = reason;
    event.description = description;
    event.value = value;
    event.threshold = threshold;
    event.timestamp = Timestamp{static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count())};

    eventHistory_.push_back(event);
    if (eventHistory_.size() > MAX_EVENT_HISTORY) {
        eventHistory_.pop_front();
    }
}

void CircuitBreaker::notifyTrigger(TriggerReason reason, const std::string& message) {
    if (triggerCallback_) {
        triggerCallback_(reason, message);
    }
}

void CircuitBreaker::executeEmergencyActions() {
    if (config_.cancelOrdersOnTrigger) {
        cancelAllOrders();
    }

    if (config_.flattenPositionsOnTrigger) {
        flattenAllPositions();
    }

    // Start cooldown if configured
    if (config_.cooldownPeriod.count() > 0) {
        startCooldown();
    }
}

double CircuitBreaker::calculateRejectRate() const {
    if (recentOrders_.empty()) return 0.0;

    size_t rejectCount = 0;
    for (const auto& [time, success] : recentOrders_) {
        if (!success) ++rejectCount;
    }

    return static_cast<double>(rejectCount) / recentOrders_.size();
}

// ============================================================================
// CircuitBreakerBuilder Implementation
// ============================================================================

CircuitBreakerBuilder& CircuitBreakerBuilder::withConfig(const CircuitBreakerConfig& config) {
    config_ = config;
    return *this;
}

CircuitBreakerBuilder& CircuitBreakerBuilder::withMaxDailyLoss(double limit) {
    config_.maxDailyLoss = limit;
    return *this;
}

CircuitBreakerBuilder& CircuitBreakerBuilder::withMaxDrawdown(double limit) {
    config_.maxDrawdown = limit;
    return *this;
}

CircuitBreakerBuilder& CircuitBreakerBuilder::withCooldownPeriod(std::chrono::seconds period) {
    config_.cooldownPeriod = period;
    return *this;
}

CircuitBreakerBuilder& CircuitBreakerBuilder::withOrderManager(std::shared_ptr<OrderManager> om) {
    orderManager_ = std::move(om);
    return *this;
}

CircuitBreakerBuilder& CircuitBreakerBuilder::withPositionManager(std::shared_ptr<PositionManager> pm) {
    positionManager_ = std::move(pm);
    return *this;
}

CircuitBreakerBuilder& CircuitBreakerBuilder::withTriggerCallback(TriggerCallback callback) {
    triggerCallback_ = std::move(callback);
    return *this;
}

CircuitBreakerBuilder& CircuitBreakerBuilder::withStateChangeCallback(StateChangeCallback callback) {
    stateChangeCallback_ = std::move(callback);
    return *this;
}

CircuitBreakerBuilder& CircuitBreakerBuilder::withRecoveryCallback(RecoveryCallback callback) {
    recoveryCallback_ = std::move(callback);
    return *this;
}

CircuitBreakerBuilder& CircuitBreakerBuilder::enableAutoRecovery(bool enable) {
    config_.autoRecoverEnabled = enable;
    return *this;
}

CircuitBreakerBuilder& CircuitBreakerBuilder::enablePositionFlattening(bool enable) {
    config_.flattenPositionsOnTrigger = enable;
    return *this;
}

std::unique_ptr<CircuitBreaker> CircuitBreakerBuilder::build() {
    auto breaker = std::make_unique<CircuitBreaker>(config_);

    if (orderManager_) {
        breaker->setOrderManager(orderManager_);
    }

    if (positionManager_) {
        breaker->setPositionManager(positionManager_);
    }

    if (triggerCallback_) {
        breaker->setTriggerCallback(std::move(triggerCallback_));
    }

    if (stateChangeCallback_) {
        breaker->setStateChangeCallback(std::move(stateChangeCallback_));
    }

    if (recoveryCallback_) {
        breaker->setRecoveryCallback(std::move(recoveryCallback_));
    }

    return breaker;
}

} // namespace hft::risk
