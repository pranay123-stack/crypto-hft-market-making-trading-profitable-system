/**
 * @file risk_manager.cpp
 * @brief Implementation of Risk Manager
 */

#include "risk/risk_manager.hpp"
#include "risk/circuit_breaker.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>

namespace hft::risk {

// ============================================================================
// OrderRateLimiter Implementation
// ============================================================================

OrderRateLimiter::OrderRateLimiter(uint32_t maxPerSecond, uint32_t maxPerMinute)
    : maxPerSecond_(maxPerSecond)
    , maxPerMinute_(maxPerMinute)
{}

bool OrderRateLimiter::checkAndIncrement() {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::steady_clock::now();

    // Remove old entries
    auto oneSecondAgo = now - std::chrono::seconds(1);
    auto oneMinuteAgo = now - std::chrono::minutes(1);

    while (!recentOrders_.empty() && recentOrders_.front() < oneMinuteAgo) {
        recentOrders_.pop_front();
    }

    // Count orders in last second and minute
    uint32_t ordersLastSecond = 0;
    uint32_t ordersLastMinute = static_cast<uint32_t>(recentOrders_.size());

    for (auto it = recentOrders_.rbegin(); it != recentOrders_.rend(); ++it) {
        if (*it >= oneSecondAgo) {
            ++ordersLastSecond;
        } else {
            break;
        }
    }

    // Check limits
    if (ordersLastSecond >= maxPerSecond_ || ordersLastMinute >= maxPerMinute_) {
        return false;
    }

    recentOrders_.push_back(now);
    return true;
}

uint32_t OrderRateLimiter::getCurrentRate() const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::steady_clock::now();
    auto oneSecondAgo = now - std::chrono::seconds(1);

    uint32_t count = 0;
    for (auto it = recentOrders_.rbegin(); it != recentOrders_.rend(); ++it) {
        if (*it >= oneSecondAgo) {
            ++count;
        } else {
            break;
        }
    }

    return count;
}

uint32_t OrderRateLimiter::getRatePerMinute() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint32_t>(recentOrders_.size());
}

void OrderRateLimiter::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    recentOrders_.clear();
}

// ============================================================================
// RiskManager Implementation
// ============================================================================

RiskManager::RiskManager(std::shared_ptr<PositionManager> positionManager)
    : positionManager_(std::move(positionManager))
{
    // Initialize rate limiters with default values
    orderRateLimiter_ = std::make_unique<OrderRateLimiter>(100, 1000);
    cancelRateLimiter_ = std::make_unique<OrderRateLimiter>(50, 500);

    LOG_INFO("RiskManager initialized");
}

RiskManager::~RiskManager() {
    LOG_INFO("RiskManager shut down");
}

RiskCheckResponse RiskManager::checkOrder(const Order& order) {
    // Check circuit breaker first
    if (isCircuitBreakerTriggered()) {
        RiskCheckResponse response;
        response.result = RiskCheckResult::RejectedCircuitBreaker;
        response.reason = "Circuit breaker is triggered";
        notifyRiskCheck(response);
        return response;
    }

    // Check if symbol is enabled
    if (!isSymbolEnabled(order.symbol)) {
        RiskCheckResponse response;
        response.result = RiskCheckResult::RejectedSymbolDisabled;
        response.reason = "Symbol is disabled for trading";
        notifyRiskCheck(response);
        return response;
    }

    // Check if exchange is enabled
    if (!isExchangeEnabled(order.exchange)) {
        RiskCheckResponse response;
        response.result = RiskCheckResult::RejectedExchangeDisabled;
        response.reason = "Exchange is disabled for trading";
        notifyRiskCheck(response);
        return response;
    }

    // Check rate limit
    auto rateCheck = checkRateLimit();
    if (!rateCheck.passed()) {
        notifyRiskCheck(rateCheck);
        return rateCheck;
    }

    // Calculate notional
    double notional = order.price.to_double() * order.quantity.to_double();

    // Check notional limit
    auto notionalCheck = checkNotionalLimit(notional);
    if (!notionalCheck.passed()) {
        notifyRiskCheck(notionalCheck);
        return notionalCheck;
    }

    // Check position limit
    auto positionCheck = checkPositionLimit(
        order.symbol,
        order.exchange,
        order.side,
        order.quantity.to_double(),
        order.price.to_double()
    );
    if (!positionCheck.passed()) {
        notifyRiskCheck(positionCheck);
        return positionCheck;
    }

    // Check price limit
    double marketPrice = 0.0;
    {
        std::shared_lock lock(mutex_);
        auto it = marketPrices_.find(std::string(order.symbol.view()));
        if (it != marketPrices_.end()) {
            marketPrice = it->second;
        }
    }

    if (marketPrice > 0 && order.price.to_double() > 0) {
        auto priceCheck = checkPriceLimit(order.symbol, order.price.to_double(), marketPrice);
        if (!priceCheck.passed()) {
            notifyRiskCheck(priceCheck);
            return priceCheck;
        }
    }

    // All checks passed
    RiskCheckResponse response;
    response.result = RiskCheckResult::Passed;
    return response;
}

RiskCheckResponse RiskManager::checkPositionLimit(
    const Symbol& symbol,
    Exchange exchange,
    Side side,
    double quantity,
    double price
) {
    RiskCheckResponse response;

    if (!positionManager_) {
        response.result = RiskCheckResult::Passed;
        return response;
    }

    std::shared_lock lock(mutex_);

    // Get current position
    auto position = positionManager_->getPosition(symbol, exchange);
    double currentQty = position ? position->quantity : 0.0;
    double currentValue = position ? position->notionalValue() : 0.0;

    // Calculate new position after order
    double orderValue = quantity * price;
    double newQty = currentQty;

    if (side == Side::Buy) {
        newQty += quantity;
    } else {
        newQty -= quantity;
    }

    double newValue = std::abs(newQty) * price;

    // Check per-symbol limit
    double symbolLimit = getSymbolPositionLimit(symbol);
    if (newValue > symbolLimit) {
        response.result = RiskCheckResult::RejectedPositionLimit;
        response.reason = "Exceeds symbol position limit";
        response.currentValue = newValue;
        response.limitValue = symbolLimit;
        response.utilizationPct = (newValue / symbolLimit) * 100.0;
        return response;
    }

    // Check total portfolio limit
    double totalPositionValue = positionManager_->getGrossExposure();
    double deltaValue = newValue - currentValue;
    double newTotalValue = totalPositionValue + deltaValue;

    if (newTotalValue > limits_.maxPositionTotal) {
        response.result = RiskCheckResult::RejectedPositionLimit;
        response.reason = "Exceeds total position limit";
        response.currentValue = newTotalValue;
        response.limitValue = limits_.maxPositionTotal;
        response.utilizationPct = (newTotalValue / limits_.maxPositionTotal) * 100.0;
        return response;
    }

    // Check per-exchange limit
    double exchangeValue = 0.0;
    auto exchangePositions = positionManager_->getPositionsForExchange(exchange);
    for (const auto& pos : exchangePositions) {
        exchangeValue += pos.notionalValue();
    }
    double newExchangeValue = exchangeValue + deltaValue;

    if (newExchangeValue > limits_.maxPositionPerExchange) {
        response.result = RiskCheckResult::RejectedExchangeLimit;
        response.reason = "Exceeds exchange position limit";
        response.currentValue = newExchangeValue;
        response.limitValue = limits_.maxPositionPerExchange;
        response.utilizationPct = (newExchangeValue / limits_.maxPositionPerExchange) * 100.0;
        return response;
    }

    response.result = RiskCheckResult::Passed;
    response.utilizationPct = (newTotalValue / limits_.maxPositionTotal) * 100.0;
    return response;
}

RiskCheckResponse RiskManager::checkNotionalLimit(double notional) {
    RiskCheckResponse response;
    std::shared_lock lock(mutex_);

    // Check single order limit
    if (notional > limits_.maxOrderNotional) {
        response.result = RiskCheckResult::RejectedNotionalLimit;
        response.reason = "Exceeds single order notional limit";
        response.currentValue = notional;
        response.limitValue = limits_.maxOrderNotional;
        response.utilizationPct = (notional / limits_.maxOrderNotional) * 100.0;
        return response;
    }

    // Check daily limit
    double dailyNotional = static_cast<double>(dailyNotionalCents_.load(std::memory_order_relaxed)) / 100.0;
    double newDailyNotional = dailyNotional + notional;

    if (newDailyNotional > limits_.maxDailyNotional) {
        response.result = RiskCheckResult::RejectedNotionalLimit;
        response.reason = "Exceeds daily notional limit";
        response.currentValue = newDailyNotional;
        response.limitValue = limits_.maxDailyNotional;
        response.utilizationPct = (newDailyNotional / limits_.maxDailyNotional) * 100.0;
        return response;
    }

    response.result = RiskCheckResult::Passed;
    response.utilizationPct = (newDailyNotional / limits_.maxDailyNotional) * 100.0;
    return response;
}

RiskCheckResponse RiskManager::checkRateLimit() {
    RiskCheckResponse response;

    if (!orderRateLimiter_->checkAndIncrement()) {
        response.result = RiskCheckResult::RejectedRateLimit;
        response.reason = "Order rate limit exceeded";
        response.currentValue = orderRateLimiter_->getCurrentRate();
        response.limitValue = limits_.maxOrdersPerSecond;
        response.utilizationPct = 100.0;
        return response;
    }

    response.result = RiskCheckResult::Passed;
    response.utilizationPct = (static_cast<double>(orderRateLimiter_->getCurrentRate()) /
                               limits_.maxOrdersPerSecond) * 100.0;
    return response;
}

RiskCheckResponse RiskManager::checkPriceLimit(
    const Symbol& symbol,
    double orderPrice,
    double marketPrice
) {
    RiskCheckResponse response;
    std::shared_lock lock(mutex_);

    if (marketPrice <= 0) {
        response.result = RiskCheckResult::Passed;
        return response;
    }

    double deviationPct = std::abs(orderPrice - marketPrice) / marketPrice * 100.0;

    if (deviationPct > limits_.maxPriceDeviationPct) {
        response.result = RiskCheckResult::RejectedPriceLimit;
        response.reason = "Price deviation exceeds limit";
        response.currentValue = deviationPct;
        response.limitValue = limits_.maxPriceDeviationPct;
        response.utilizationPct = (deviationPct / limits_.maxPriceDeviationPct) * 100.0;
        return response;
    }

    response.result = RiskCheckResult::Passed;
    return response;
}

bool RiskManager::isSymbolEnabled(const Symbol& symbol) const {
    std::shared_lock lock(mutex_);
    std::string symbolStr(symbol.view());

    return std::find(limits_.disabledSymbols.begin(),
                     limits_.disabledSymbols.end(),
                     symbolStr) == limits_.disabledSymbols.end();
}

bool RiskManager::isExchangeEnabled(Exchange exchange) const {
    std::shared_lock lock(mutex_);

    return std::find(limits_.disabledExchanges.begin(),
                     limits_.disabledExchanges.end(),
                     exchange) == limits_.disabledExchanges.end();
}

void RiskManager::setLimits(const RiskLimits& limits) {
    std::unique_lock lock(mutex_);
    limits_ = limits;

    // Update rate limiters
    orderRateLimiter_ = std::make_unique<OrderRateLimiter>(
        limits_.maxOrdersPerSecond, limits_.maxOrdersPerMinute);
    cancelRateLimiter_ = std::make_unique<OrderRateLimiter>(
        limits_.maxCancelsPerSecond, limits_.maxCancelsPerSecond * 10);

    LOG_INFO("Risk limits updated");
}

const RiskLimits& RiskManager::getLimits() const {
    std::shared_lock lock(mutex_);
    return limits_;
}

void RiskManager::setSymbolPositionLimit(const Symbol& symbol, double limit) {
    std::unique_lock lock(mutex_);
    limits_.symbolPositionLimits[std::string(symbol.view())] = limit;
    LOG_INFO("Symbol position limit set: {} = {}", symbol.view(), limit);
}

void RiskManager::disableSymbol(const Symbol& symbol) {
    std::unique_lock lock(mutex_);
    std::string symbolStr(symbol.view());

    if (std::find(limits_.disabledSymbols.begin(),
                  limits_.disabledSymbols.end(),
                  symbolStr) == limits_.disabledSymbols.end()) {
        limits_.disabledSymbols.push_back(symbolStr);
        LOG_WARN("Symbol disabled: {}", symbol.view());
    }
}

void RiskManager::enableSymbol(const Symbol& symbol) {
    std::unique_lock lock(mutex_);
    std::string symbolStr(symbol.view());

    limits_.disabledSymbols.erase(
        std::remove(limits_.disabledSymbols.begin(),
                    limits_.disabledSymbols.end(),
                    symbolStr),
        limits_.disabledSymbols.end());

    LOG_INFO("Symbol enabled: {}", symbol.view());
}

void RiskManager::disableExchange(Exchange exchange) {
    std::unique_lock lock(mutex_);

    if (std::find(limits_.disabledExchanges.begin(),
                  limits_.disabledExchanges.end(),
                  exchange) == limits_.disabledExchanges.end()) {
        limits_.disabledExchanges.push_back(exchange);
        LOG_WARN("Exchange disabled: {}", exchange_to_string(exchange));
    }
}

void RiskManager::enableExchange(Exchange exchange) {
    std::unique_lock lock(mutex_);

    limits_.disabledExchanges.erase(
        std::remove(limits_.disabledExchanges.begin(),
                    limits_.disabledExchanges.end(),
                    exchange),
        limits_.disabledExchanges.end());

    LOG_INFO("Exchange enabled: {}", exchange_to_string(exchange));
}

void RiskManager::updateMarketPrice(const Symbol& symbol, double price) {
    std::unique_lock lock(mutex_);
    marketPrices_[std::string(symbol.view())] = price;
}

RiskMetrics RiskManager::getMetrics() const {
    std::shared_lock lock(mutex_);

    RiskMetrics metrics = metrics_;

    // Update from position manager
    if (positionManager_) {
        metrics.totalPositionValue = positionManager_->getGrossExposure();
        metrics.grossExposure = positionManager_->getGrossExposure();
        metrics.netExposure = positionManager_->getNetExposure();
        metrics.openPositionCount = positionManager_->getOpenPositionCount();
        metrics.realizedPnL = positionManager_->getTotalRealizedPnL();
        metrics.unrealizedPnL = positionManager_->getTotalUnrealizedPnL();
        metrics.dailyPnL = metrics.realizedPnL + metrics.unrealizedPnL;
    }

    // Update from atomic counters
    metrics.ordersToday = dailyOrders_.load(std::memory_order_relaxed);
    metrics.fillsToday = dailyFills_.load(std::memory_order_relaxed);
    metrics.cancelsToday = dailyCancels_.load(std::memory_order_relaxed);
    metrics.rejectsToday = dailyRejects_.load(std::memory_order_relaxed);
    metrics.dailyNotionalTraded = static_cast<double>(
        dailyNotionalCents_.load(std::memory_order_relaxed)) / 100.0;

    // Update rate metrics
    if (orderRateLimiter_) {
        metrics.ordersLastSecond = orderRateLimiter_->getCurrentRate();
        metrics.ordersLastMinute = orderRateLimiter_->getRatePerMinute();
    }
    if (cancelRateLimiter_) {
        metrics.cancelsLastSecond = cancelRateLimiter_->getCurrentRate();
    }

    // Calculate utilizations
    metrics.positionUtilization = limits_.maxPositionTotal > 0 ?
        (metrics.grossExposure / limits_.maxPositionTotal) * 100.0 : 0.0;
    metrics.notionalUtilization = limits_.maxDailyNotional > 0 ?
        (metrics.dailyNotionalTraded / limits_.maxDailyNotional) * 100.0 : 0.0;

    // Loss utilization (drawdown)
    if (metrics.dailyPnL < 0 && limits_.maxDailyLoss > 0) {
        metrics.lossUtilization = (std::abs(metrics.dailyPnL) / limits_.maxDailyLoss) * 100.0;
    }

    // Rate utilization
    metrics.rateUtilization = limits_.maxOrdersPerSecond > 0 ?
        (static_cast<double>(metrics.ordersLastSecond) / limits_.maxOrdersPerSecond) * 100.0 : 0.0;

    metrics.lastUpdateTime = Timestamp{static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count())};

    return metrics;
}

void RiskManager::updateMetrics() {
    auto metrics = getMetrics();

    // Check for alerts
    if (metrics.positionUtilization > 80.0) {
        notifyAlert("Position utilization above 80%", RiskCheckResult::RejectedPositionLimit);
    }
    if (metrics.lossUtilization > 80.0) {
        notifyAlert("Loss utilization above 80%", RiskCheckResult::RejectedMaxLoss);
    }
    if (metrics.notionalUtilization > 80.0) {
        notifyAlert("Notional utilization above 80%", RiskCheckResult::RejectedNotionalLimit);
    }

    std::unique_lock lock(mutex_);
    metrics_ = metrics;
}

void RiskManager::recordOrderSubmit() {
    dailyOrders_.fetch_add(1, std::memory_order_relaxed);
}

void RiskManager::recordOrderCancel() {
    dailyCancels_.fetch_add(1, std::memory_order_relaxed);
}

void RiskManager::recordFill(double notional) {
    dailyFills_.fetch_add(1, std::memory_order_relaxed);
    int64_t notionalCents = static_cast<int64_t>(notional * 100);
    dailyNotionalCents_.fetch_add(notionalCents, std::memory_order_relaxed);
}

void RiskManager::recordReject() {
    dailyRejects_.fetch_add(1, std::memory_order_relaxed);
}

void RiskManager::setCircuitBreaker(std::shared_ptr<CircuitBreaker> circuitBreaker) {
    circuitBreaker_ = std::move(circuitBreaker);
}

bool RiskManager::isCircuitBreakerTriggered() const {
    if (circuitBreaker_) {
        return circuitBreaker_->isTriggered();
    }
    return false;
}

void RiskManager::setRiskCallback(RiskCallback callback) {
    riskCallback_ = std::move(callback);
}

void RiskManager::setAlertCallback(RiskAlertCallback callback) {
    alertCallback_ = std::move(callback);
}

void RiskManager::resetDailyStats() {
    dailyOrders_.store(0, std::memory_order_relaxed);
    dailyFills_.store(0, std::memory_order_relaxed);
    dailyCancels_.store(0, std::memory_order_relaxed);
    dailyRejects_.store(0, std::memory_order_relaxed);
    dailyNotionalCents_.store(0, std::memory_order_relaxed);

    if (orderRateLimiter_) orderRateLimiter_->reset();
    if (cancelRateLimiter_) cancelRateLimiter_->reset();

    LOG_INFO("Daily risk statistics reset");
}

void RiskManager::notifyRiskCheck(const RiskCheckResponse& response) {
    if (response.result != RiskCheckResult::Passed) {
        LOG_WARN("Risk check failed: {} - {}", risk_result_to_string(response.result), response.reason);

        if (riskCallback_) {
            riskCallback_(response);
        }
    }
}

void RiskManager::notifyAlert(const std::string& alert, RiskCheckResult severity) {
    LOG_WARN("Risk alert: {}", alert);

    if (alertCallback_) {
        alertCallback_(alert, severity);
    }
}

double RiskManager::getSymbolPositionLimit(const Symbol& symbol) const {
    std::string symbolStr(symbol.view());

    auto it = limits_.symbolPositionLimits.find(symbolStr);
    if (it != limits_.symbolPositionLimits.end()) {
        return it->second;
    }
    return limits_.maxPositionPerSymbol;
}

double RiskManager::getSymbolNotionalLimit(const Symbol& symbol) const {
    std::string symbolStr(symbol.view());

    auto it = limits_.symbolNotionalLimits.find(symbolStr);
    if (it != limits_.symbolNotionalLimits.end()) {
        return it->second;
    }
    return limits_.maxOrderNotional;
}

// ============================================================================
// RiskManagerBuilder Implementation
// ============================================================================

RiskManagerBuilder& RiskManagerBuilder::withPositionManager(std::shared_ptr<PositionManager> pm) {
    positionManager_ = std::move(pm);
    return *this;
}

RiskManagerBuilder& RiskManagerBuilder::withLimits(const RiskLimits& limits) {
    limits_ = limits;
    return *this;
}

RiskManagerBuilder& RiskManagerBuilder::withCircuitBreaker(std::shared_ptr<CircuitBreaker> cb) {
    circuitBreaker_ = std::move(cb);
    return *this;
}

RiskManagerBuilder& RiskManagerBuilder::withRiskCallback(RiskCallback callback) {
    riskCallback_ = std::move(callback);
    return *this;
}

RiskManagerBuilder& RiskManagerBuilder::withAlertCallback(RiskAlertCallback callback) {
    alertCallback_ = std::move(callback);
    return *this;
}

std::unique_ptr<RiskManager> RiskManagerBuilder::build() {
    auto manager = std::make_unique<RiskManager>(positionManager_);

    manager->setLimits(limits_);

    if (circuitBreaker_) {
        manager->setCircuitBreaker(circuitBreaker_);
    }

    if (riskCallback_) {
        manager->setRiskCallback(std::move(riskCallback_));
    }

    if (alertCallback_) {
        manager->setAlertCallback(std::move(alertCallback_));
    }

    return manager;
}

} // namespace hft::risk
