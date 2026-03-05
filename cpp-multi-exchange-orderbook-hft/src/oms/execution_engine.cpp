/**
 * @file execution_engine.cpp
 * @brief Implementation of Execution Engine and Smart Order Routing
 */

#include "oms/execution_engine.hpp"
#include "oms/position_manager.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>

namespace hft::oms {

// ============================================================================
// SmartOrderRouter Implementation
// ============================================================================

SmartOrderRouter::SmartOrderRouter() {
    LOG_INFO("SmartOrderRouter initialized");
}

SmartOrderRouter::~SmartOrderRouter() = default;

void SmartOrderRouter::updateMarketData(const MarketSnapshot& snapshot) {
    std::unique_lock lock(mutex_);

    std::string symbolKey(snapshot.symbol.view());
    marketData_[symbolKey][snapshot.exchange] = snapshot;
}

void SmartOrderRouter::updateExchangeMetrics(const ExchangeMetrics& metrics) {
    std::unique_lock lock(mutex_);
    exchangeMetrics_[metrics.exchange] = metrics;
}

std::vector<RoutingDecision> SmartOrderRouter::route(
    const Symbol& symbol,
    Side side,
    Quantity quantity,
    std::optional<Price> limitPrice
) {
    std::shared_lock lock(mutex_);

    std::vector<RoutingDecision> decisions;
    std::string symbolKey(symbol.view());

    auto symbolIt = marketData_.find(symbolKey);
    if (symbolIt == marketData_.end()) {
        LOG_WARN("No market data for symbol: {}", symbol.view());
        return decisions;
    }

    // Score each exchange
    std::vector<std::pair<Exchange, double>> exchangeScores;

    for (const auto& [exchange, snapshot] : symbolIt->second) {
        // Skip stale data (> 5 seconds)
        if (snapshot.isStale(5'000'000'000ULL)) {
            continue;
        }

        // Get exchange metrics (or use defaults)
        ExchangeMetrics metrics;
        auto metricsIt = exchangeMetrics_.find(exchange);
        if (metricsIt != exchangeMetrics_.end()) {
            metrics = metricsIt->second;
        } else {
            metrics.exchange = exchange;
            metrics.fillRate = 0.9;
            metrics.avgLatencyMs = 50.0;
        }

        Price targetPrice = limitPrice.value_or(
            side == Side::Buy ? snapshot.bestAsk : snapshot.bestBid);

        double score = scoreExchange(snapshot, metrics, side, targetPrice);
        exchangeScores.emplace_back(exchange, score);
    }

    // Sort by score (descending)
    std::sort(exchangeScores.begin(), exchangeScores.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    // Create routing decisions
    Quantity remainingQty = quantity;

    for (const auto& [exchange, score] : exchangeScores) {
        if (remainingQty.value == 0) break;

        const auto& snapshot = symbolIt->second.at(exchange);

        // Determine available quantity at this exchange
        Quantity availableQty = side == Side::Buy ? snapshot.askSize : snapshot.bidSize;
        Quantity routeQty{std::min(remainingQty.value, availableQty.value)};

        if (routeQty.value == 0) continue;

        RoutingDecision decision;
        decision.exchange = exchange;
        decision.quantity = routeQty;
        decision.price = side == Side::Buy ? snapshot.bestAsk : snapshot.bestBid;
        decision.orderType = OrderType::Limit;
        decision.score = score;
        decision.reason = "Best available price/liquidity";

        decisions.push_back(decision);
        remainingQty = Quantity{remainingQty.value - routeQty.value};
    }

    // If we couldn't route everything, add remaining to best exchange
    if (remainingQty.value > 0 && !exchangeScores.empty()) {
        auto& decision = decisions.front();
        decision.quantity = Quantity{decision.quantity.value + remainingQty.value};
    }

    LOG_DEBUG("Routed {} {} {} across {} exchanges",
              quantity.to_double(), side_to_string(side), symbol.view(),
              decisions.size());

    return decisions;
}

std::optional<Exchange> SmartOrderRouter::getBestExchange(
    const Symbol& symbol,
    Side side
) const {
    std::shared_lock lock(mutex_);
    std::string symbolKey(symbol.view());

    auto symbolIt = marketData_.find(symbolKey);
    if (symbolIt == marketData_.end()) {
        return std::nullopt;
    }

    Exchange bestExchange = Exchange::Unknown;
    double bestPrice = side == Side::Buy ?
        std::numeric_limits<double>::max() : 0.0;

    for (const auto& [exchange, snapshot] : symbolIt->second) {
        if (snapshot.isStale(5'000'000'000ULL)) continue;

        double price = side == Side::Buy ?
            snapshot.bestAsk.to_double() : snapshot.bestBid.to_double();

        if (side == Side::Buy) {
            if (price < bestPrice) {
                bestPrice = price;
                bestExchange = exchange;
            }
        } else {
            if (price > bestPrice) {
                bestPrice = price;
                bestExchange = exchange;
            }
        }
    }

    if (bestExchange == Exchange::Unknown) {
        return std::nullopt;
    }
    return bestExchange;
}

std::vector<std::pair<Price, Quantity>> SmartOrderRouter::getAggregatedBook(
    const Symbol& symbol,
    Side side,
    size_t depth
) const {
    std::shared_lock lock(mutex_);
    std::string symbolKey(symbol.view());

    std::map<uint64_t, uint64_t> aggregatedLevels; // price -> quantity

    auto symbolIt = marketData_.find(symbolKey);
    if (symbolIt != marketData_.end()) {
        for (const auto& [exchange, snapshot] : symbolIt->second) {
            if (snapshot.isStale(5'000'000'000ULL)) continue;

            const auto& prices = side == Side::Buy ?
                snapshot.askPrices : snapshot.bidPrices;
            const auto& sizes = side == Side::Buy ?
                snapshot.askSizes : snapshot.bidSizes;

            for (size_t i = 0; i < 5; ++i) {
                if (prices[i].value > 0) {
                    aggregatedLevels[prices[i].value] += sizes[i].value;
                }
            }
        }
    }

    std::vector<std::pair<Price, Quantity>> result;
    size_t count = 0;

    if (side == Side::Buy) {
        // Asks: ascending order
        for (const auto& [price, qty] : aggregatedLevels) {
            if (count >= depth) break;
            result.emplace_back(Price{price}, Quantity{qty});
            ++count;
        }
    } else {
        // Bids: descending order
        for (auto it = aggregatedLevels.rbegin(); it != aggregatedLevels.rend(); ++it) {
            if (count >= depth) break;
            result.emplace_back(Price{it->first}, Quantity{it->second});
            ++count;
        }
    }

    return result;
}

void SmartOrderRouter::setWeights(double priceWeight, double latencyWeight,
                                   double fillRateWeight, double feeWeight) {
    std::unique_lock lock(mutex_);
    priceWeight_ = priceWeight;
    latencyWeight_ = latencyWeight;
    fillRateWeight_ = fillRateWeight;
    feeWeight_ = feeWeight;
}

double SmartOrderRouter::scoreExchange(const MarketSnapshot& snapshot,
                                        const ExchangeMetrics& metrics,
                                        Side side, Price targetPrice) const {
    double score = 0.0;

    // Price score (higher is better for both sides when normalized)
    double currentPrice = side == Side::Buy ?
        snapshot.bestAsk.to_double() : snapshot.bestBid.to_double();
    double targetPriceD = targetPrice.to_double();

    double priceScore = 0.0;
    if (side == Side::Buy) {
        // Lower ask price is better
        priceScore = targetPriceD > 0 ? (1.0 - (currentPrice / targetPriceD - 1.0)) : 0.5;
    } else {
        // Higher bid price is better
        priceScore = targetPriceD > 0 ? (currentPrice / targetPriceD) : 0.5;
    }
    priceScore = std::clamp(priceScore, 0.0, 1.0);

    // Latency score (lower is better)
    double latencyScore = 1.0 / (1.0 + metrics.avgLatencyMs / 100.0);

    // Fill rate score
    double fillScore = metrics.fillRate;

    // Fee score (lower is better)
    double fee = side == Side::Buy ? snapshot.takerFee : snapshot.makerFee;
    double feeScore = 1.0 / (1.0 + fee * 100.0);

    // Weighted sum
    score = priceWeight_ * priceScore +
            latencyWeight_ * latencyScore +
            fillRateWeight_ * fillScore +
            feeWeight_ * feeScore;

    return score;
}

// ============================================================================
// TWAP Algorithm Implementation
// ============================================================================

void TWAPAlgo::initialize(const ExecutionRequest& request,
                           std::shared_ptr<OrderManager> orderManager) {
    request_ = request;
    orderManager_ = std::move(orderManager);

    result_.requestId = request.requestId;
    result_.targetQuantity = request.quantity;
    result_.remainingQuantity = request.quantity;
    result_.status = ExecutionStatus::Active;
    result_.startTime = Timestamp{static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count())};

    // Calculate slice parameters
    uint32_t numSlices = request.numSlices > 0 ? request.numSlices : 10;
    sliceQuantity_ = Quantity{request.quantity.value / numSlices};

    if (sliceQuantity_.value == 0) {
        sliceQuantity_ = request.quantity;
        numSlices = 1;
    }

    uint64_t durationMs = request.durationSeconds * 1000ULL;
    sliceInterval_ = std::chrono::milliseconds(durationMs / numSlices);

    startTime_ = result_.startTime;
    nextSliceTime_ = std::chrono::steady_clock::now();

    LOG_INFO("TWAP initialized: {} {} {} over {} seconds in {} slices",
             request.quantity.to_double(), side_to_string(request.side),
             request.symbol.view(), request.durationSeconds, numSlices);
}

bool TWAPAlgo::step() {
    if (cancelled_ || isComplete()) {
        return false;
    }

    auto now = std::chrono::steady_clock::now();

    // Check if it's time for next slice
    if (now < nextSliceTime_) {
        return true; // Continue but don't do anything yet
    }

    // Check remaining quantity
    if (result_.remainingQuantity.value == 0) {
        result_.status = ExecutionStatus::Filled;
        result_.endTime = Timestamp{static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::high_resolution_clock::now().time_since_epoch()).count())};
        return false;
    }

    // Calculate this slice quantity
    Quantity thisSlice{std::min(sliceQuantity_.value, result_.remainingQuantity.value)};

    // Submit order
    if (orderManager_) {
        uint64_t orderId = orderManager_->submitOrder(
            request_.symbol,
            request_.preferredExchange.value_or(Exchange::Binance),
            request_.side,
            request_.limitPrice.value > 0 ? OrderType::Limit : OrderType::Market,
            request_.limitPrice,
            thisSlice,
            TimeInForce::IOC, // IOC for TWAP slices
            request_.strategyId
        );

        if (orderId > 0) {
            activeOrders_.push_back(orderId);
            result_.childOrderIds.push_back(orderId);
            result_.childOrderCount++;
        }
    }

    ++currentSlice_;
    nextSliceTime_ = now + sliceInterval_;

    return true;
}

void TWAPAlgo::onOrderUpdate(const Order& order) {
    // Remove from active orders if terminal
    if (order.isTerminal()) {
        activeOrders_.erase(
            std::remove(activeOrders_.begin(), activeOrders_.end(), order.orderId),
            activeOrders_.end());
    }
}

void TWAPAlgo::onFill(uint64_t orderId, Price price, Quantity qty) {
    result_.filledQuantity = Quantity{result_.filledQuantity.value + qty.value};
    result_.remainingQuantity = Quantity{result_.targetQuantity.value - result_.filledQuantity.value};
    result_.fillCount++;

    // Update average fill price
    double totalValue = result_.avgFillPrice * (result_.filledQuantity.value - qty.value);
    totalValue += price.to_double() * qty.to_double();
    result_.avgFillPrice = totalValue / result_.filledQuantity.to_double();

    result_.totalNotional += price.to_double() * qty.to_double();

    if (result_.filledQuantity.value > 0) {
        result_.status = ExecutionStatus::PartiallyFilled;
    }

    if (result_.remainingQuantity.value == 0) {
        result_.status = ExecutionStatus::Filled;
        result_.endTime = Timestamp{static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::high_resolution_clock::now().time_since_epoch()).count())};
    }
}

void TWAPAlgo::cancel() {
    cancelled_ = true;

    // Cancel all active orders
    if (orderManager_) {
        for (uint64_t orderId : activeOrders_) {
            orderManager_->cancelOrder(orderId);
        }
    }

    result_.status = ExecutionStatus::Cancelled;
    result_.endTime = Timestamp{static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count())};
}

bool TWAPAlgo::isComplete() const {
    return result_.isComplete();
}

// ============================================================================
// VWAP Algorithm Implementation
// ============================================================================

void VWAPAlgo::initialize(const ExecutionRequest& request,
                           std::shared_ptr<OrderManager> orderManager) {
    request_ = request;
    orderManager_ = std::move(orderManager);

    result_.requestId = request.requestId;
    result_.targetQuantity = request.quantity;
    result_.remainingQuantity = request.quantity;
    result_.status = ExecutionStatus::Active;
    result_.startTime = Timestamp{static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count())};

    startTime_ = result_.startTime;

    // Default volume profile (uniform if not set)
    if (volumeProfile_.empty()) {
        uint32_t numBuckets = request.durationSeconds > 0 ? request.durationSeconds / 60 : 10;
        numBuckets = std::max(numBuckets, 1u);
        volumeProfile_.resize(numBuckets, 1.0 / numBuckets);
    }

    bucketDuration_ = std::chrono::milliseconds(request.durationSeconds * 1000 / volumeProfile_.size());

    LOG_INFO("VWAP initialized: {} {} {} over {} seconds with {} buckets",
             request.quantity.to_double(), side_to_string(request.side),
             request.symbol.view(), request.durationSeconds, volumeProfile_.size());
}

bool VWAPAlgo::step() {
    if (cancelled_ || isComplete()) {
        return false;
    }

    // Similar to TWAP but weighted by volume profile
    if (currentBucket_ >= volumeProfile_.size()) {
        result_.status = result_.filledQuantity.value > 0 ?
            ExecutionStatus::PartiallyFilled : ExecutionStatus::Expired;
        result_.endTime = Timestamp{static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::high_resolution_clock::now().time_since_epoch()).count())};
        return false;
    }

    // Calculate quantity for this bucket based on volume profile
    double bucketPct = volumeProfile_[currentBucket_];
    Quantity bucketQty{static_cast<uint64_t>(request_.quantity.value * bucketPct)};
    bucketQty = Quantity{std::min(bucketQty.value, result_.remainingQuantity.value)};

    if (bucketQty.value > 0 && orderManager_) {
        uint64_t orderId = orderManager_->submitOrder(
            request_.symbol,
            request_.preferredExchange.value_or(Exchange::Binance),
            request_.side,
            request_.limitPrice.value > 0 ? OrderType::Limit : OrderType::Market,
            request_.limitPrice,
            bucketQty,
            TimeInForce::IOC,
            request_.strategyId
        );

        if (orderId > 0) {
            activeOrders_.push_back(orderId);
            result_.childOrderIds.push_back(orderId);
            result_.childOrderCount++;
        }
    }

    ++currentBucket_;
    return true;
}

void VWAPAlgo::onOrderUpdate(const Order& order) {
    if (order.isTerminal()) {
        activeOrders_.erase(
            std::remove(activeOrders_.begin(), activeOrders_.end(), order.orderId),
            activeOrders_.end());
    }
}

void VWAPAlgo::onFill(uint64_t orderId, Price price, Quantity qty) {
    result_.filledQuantity = Quantity{result_.filledQuantity.value + qty.value};
    result_.remainingQuantity = Quantity{result_.targetQuantity.value - result_.filledQuantity.value};
    result_.fillCount++;

    double totalValue = result_.avgFillPrice * (result_.filledQuantity.value - qty.value);
    totalValue += price.to_double() * qty.to_double();
    result_.avgFillPrice = totalValue / result_.filledQuantity.to_double();

    result_.totalNotional += price.to_double() * qty.to_double();

    if (result_.filledQuantity.value > 0) {
        result_.status = ExecutionStatus::PartiallyFilled;
    }

    if (result_.remainingQuantity.value == 0) {
        result_.status = ExecutionStatus::Filled;
    }
}

void VWAPAlgo::cancel() {
    cancelled_ = true;
    if (orderManager_) {
        for (uint64_t orderId : activeOrders_) {
            orderManager_->cancelOrder(orderId);
        }
    }
    result_.status = ExecutionStatus::Cancelled;
}

bool VWAPAlgo::isComplete() const {
    return result_.isComplete();
}

void VWAPAlgo::updateVolumeProfile(const std::vector<double>& volumeProfile) {
    volumeProfile_ = volumeProfile;

    // Normalize
    double sum = 0.0;
    for (double v : volumeProfile_) sum += v;
    if (sum > 0) {
        for (double& v : volumeProfile_) v /= sum;
    }
}

// ============================================================================
// Iceberg Algorithm Implementation
// ============================================================================

void IcebergAlgo::initialize(const ExecutionRequest& request,
                              std::shared_ptr<OrderManager> orderManager) {
    request_ = request;
    orderManager_ = std::move(orderManager);

    result_.requestId = request.requestId;
    result_.targetQuantity = request.quantity;
    result_.remainingQuantity = request.quantity;
    result_.status = ExecutionStatus::Active;
    result_.startTime = Timestamp{static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count())};

    // Display quantity is total / number of slices
    uint32_t numSlices = request.numSlices > 0 ? request.numSlices : 10;
    displayQuantity_ = Quantity{request.quantity.value / numSlices};

    LOG_INFO("Iceberg initialized: {} {} {} with display qty {}",
             request.quantity.to_double(), side_to_string(request.side),
             request.symbol.view(), displayQuantity_.to_double());
}

bool IcebergAlgo::step() {
    if (cancelled_ || isComplete()) {
        return false;
    }

    if (result_.remainingQuantity.value == 0) {
        result_.status = ExecutionStatus::Filled;
        return false;
    }

    // If no active order, place one
    if (currentOrderId_ == 0 && orderManager_) {
        Quantity qty{std::min(displayQuantity_.value, result_.remainingQuantity.value)};

        currentOrderId_ = orderManager_->submitOrder(
            request_.symbol,
            request_.preferredExchange.value_or(Exchange::Binance),
            request_.side,
            OrderType::Limit,
            request_.limitPrice,
            qty,
            TimeInForce::GTC,
            request_.strategyId
        );

        if (currentOrderId_ > 0) {
            result_.childOrderIds.push_back(currentOrderId_);
            result_.childOrderCount++;
        }
    }

    return true;
}

void IcebergAlgo::onOrderUpdate(const Order& order) {
    if (order.orderId == currentOrderId_ && order.isTerminal()) {
        currentOrderId_ = 0; // Ready to place new order
    }
}

void IcebergAlgo::onFill(uint64_t orderId, Price price, Quantity qty) {
    result_.filledQuantity = Quantity{result_.filledQuantity.value + qty.value};
    result_.remainingQuantity = Quantity{result_.targetQuantity.value - result_.filledQuantity.value};
    result_.fillCount++;

    double totalValue = result_.avgFillPrice * (result_.filledQuantity.value - qty.value);
    totalValue += price.to_double() * qty.to_double();
    result_.avgFillPrice = totalValue / result_.filledQuantity.to_double();

    result_.totalNotional += price.to_double() * qty.to_double();

    if (result_.filledQuantity.value > 0) {
        result_.status = ExecutionStatus::PartiallyFilled;
    }
}

void IcebergAlgo::cancel() {
    cancelled_ = true;
    if (orderManager_ && currentOrderId_ > 0) {
        orderManager_->cancelOrder(currentOrderId_);
    }
    result_.status = ExecutionStatus::Cancelled;
}

bool IcebergAlgo::isComplete() const {
    return result_.isComplete();
}

// ============================================================================
// ExecutionEngine Implementation
// ============================================================================

ExecutionEngine::ExecutionEngine(std::shared_ptr<OrderManager> orderManager,
                                  std::shared_ptr<PositionManager> positionManager)
    : orderManager_(std::move(orderManager))
    , positionManager_(std::move(positionManager))
{
    LOG_INFO("ExecutionEngine initialized");
}

ExecutionEngine::~ExecutionEngine() {
    cancelAllExecutions();
    LOG_INFO("ExecutionEngine shut down");
}

uint64_t ExecutionEngine::execute(const ExecutionRequest& request) {
    uint64_t requestId = nextRequestId_.fetch_add(1, std::memory_order_relaxed);

    ExecutionRequest req = request;
    req.requestId = requestId;
    req.createTime = Timestamp{static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count())};

    auto algo = createAlgorithm(req.algorithm);
    if (!algo) {
        LOG_ERROR("Failed to create algorithm: {}", algo_to_string(req.algorithm));
        return 0;
    }

    algo->initialize(req, orderManager_);

    {
        std::unique_lock lock(mutex_);
        activeAlgos_[requestId] = std::move(algo);
    }

    LOG_INFO("Execution started: id={}, algo={}, symbol={}, side={}, qty={}",
             requestId, algo_to_string(req.algorithm), req.symbol.view(),
             side_to_string(req.side), req.quantity.to_double());

    return requestId;
}

bool ExecutionEngine::cancelExecution(uint64_t requestId) {
    std::unique_lock lock(mutex_);

    auto it = activeAlgos_.find(requestId);
    if (it == activeAlgos_.end()) {
        return false;
    }

    it->second->cancel();

    ExecutionResult result = it->second->getResult();
    completedExecutions_[requestId] = result;
    activeAlgos_.erase(it);

    lock.unlock();

    if (executionCallback_) {
        executionCallback_(result);
    }

    LOG_INFO("Execution cancelled: id={}", requestId);
    return true;
}

size_t ExecutionEngine::cancelAllExecutions() {
    std::vector<uint64_t> requestIds;

    {
        std::shared_lock lock(mutex_);
        for (const auto& [id, algo] : activeAlgos_) {
            requestIds.push_back(id);
        }
    }

    size_t count = 0;
    for (uint64_t id : requestIds) {
        if (cancelExecution(id)) {
            ++count;
        }
    }

    LOG_INFO("Cancelled {} executions", count);
    return count;
}

void ExecutionEngine::updateMarketData(const MarketSnapshot& snapshot) {
    router_.updateMarketData(snapshot);
}

void ExecutionEngine::updateExchangeMetrics(const ExchangeMetrics& metrics) {
    router_.updateExchangeMetrics(metrics);
}

std::optional<ExecutionResult> ExecutionEngine::getExecutionResult(uint64_t requestId) const {
    std::shared_lock lock(mutex_);

    // Check active
    auto activeIt = activeAlgos_.find(requestId);
    if (activeIt != activeAlgos_.end()) {
        return activeIt->second->getResult();
    }

    // Check completed
    auto compIt = completedExecutions_.find(requestId);
    if (compIt != completedExecutions_.end()) {
        return compIt->second;
    }

    return std::nullopt;
}

std::vector<uint64_t> ExecutionEngine::getActiveExecutions() const {
    std::shared_lock lock(mutex_);

    std::vector<uint64_t> result;
    result.reserve(activeAlgos_.size());

    for (const auto& [id, algo] : activeAlgos_) {
        result.push_back(id);
    }

    return result;
}

void ExecutionEngine::setExecutionCallback(ExecutionCallback callback) {
    executionCallback_ = std::move(callback);
}

void ExecutionEngine::process() {
    std::vector<std::pair<uint64_t, ExecutionResult>> completedList;

    {
        std::unique_lock lock(mutex_);

        for (auto it = activeAlgos_.begin(); it != activeAlgos_.end(); ) {
            auto& [id, algo] = *it;

            // Step the algorithm
            bool continueRunning = algo->step();

            if (!continueRunning || algo->isComplete()) {
                ExecutionResult result = algo->getResult();
                completedExecutions_[id] = result;
                completedList.emplace_back(id, result);
                it = activeAlgos_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Notify callbacks outside lock
    for (const auto& [id, result] : completedList) {
        LOG_INFO("Execution completed: id={}, status={}, filled={}",
                 id, static_cast<int>(result.status), result.filledQuantity.to_double());

        if (executionCallback_) {
            executionCallback_(result);
        }
    }
}

void ExecutionEngine::onOrderUpdate(const Order& order) {
    std::shared_lock lock(mutex_);

    // Find execution for this order
    for (auto& [id, algo] : activeAlgos_) {
        algo->onOrderUpdate(order);
    }
}

void ExecutionEngine::onFill(uint64_t orderId, Price price, Quantity qty) {
    std::shared_lock lock(mutex_);

    for (auto& [id, algo] : activeAlgos_) {
        algo->onFill(orderId, price, qty);
    }
}

std::vector<RoutingDecision> ExecutionEngine::getBestRouting(
    const Symbol& symbol,
    Side side,
    Quantity quantity,
    std::optional<Price> limitPrice
) {
    return router_.route(symbol, side, quantity, limitPrice);
}

std::unique_ptr<ExecutionAlgo> ExecutionEngine::createAlgorithm(ExecutionAlgorithm type) {
    switch (type) {
        case ExecutionAlgorithm::TWAP:
            return std::make_unique<TWAPAlgo>();
        case ExecutionAlgorithm::VWAP:
            return std::make_unique<VWAPAlgo>();
        case ExecutionAlgorithm::Iceberg:
            return std::make_unique<IcebergAlgo>();
        case ExecutionAlgorithm::Direct:
        case ExecutionAlgorithm::SOR:
        case ExecutionAlgorithm::Peg:
        case ExecutionAlgorithm::Sniper:
        case ExecutionAlgorithm::Maker:
            // For now, these use TWAP as placeholder
            return std::make_unique<TWAPAlgo>();
        default:
            return nullptr;
    }
}

void ExecutionEngine::completeExecution(uint64_t requestId, ExecutionResult result) {
    {
        std::unique_lock lock(mutex_);
        completedExecutions_[requestId] = result;
        activeAlgos_.erase(requestId);
    }

    if (executionCallback_) {
        executionCallback_(result);
    }
}

// ============================================================================
// ExecutionRequestBuilder Implementation
// ============================================================================

ExecutionRequestBuilder& ExecutionRequestBuilder::symbol(const Symbol& s) {
    request_.symbol = s;
    return *this;
}

ExecutionRequestBuilder& ExecutionRequestBuilder::side(Side s) {
    request_.side = s;
    return *this;
}

ExecutionRequestBuilder& ExecutionRequestBuilder::quantity(Quantity q) {
    request_.quantity = q;
    return *this;
}

ExecutionRequestBuilder& ExecutionRequestBuilder::limitPrice(Price p) {
    request_.limitPrice = p;
    return *this;
}

ExecutionRequestBuilder& ExecutionRequestBuilder::algorithm(ExecutionAlgorithm algo) {
    request_.algorithm = algo;
    return *this;
}

ExecutionRequestBuilder& ExecutionRequestBuilder::preferredExchange(Exchange e) {
    request_.preferredExchange = e;
    return *this;
}

ExecutionRequestBuilder& ExecutionRequestBuilder::duration(uint32_t seconds) {
    request_.durationSeconds = seconds;
    return *this;
}

ExecutionRequestBuilder& ExecutionRequestBuilder::slices(uint32_t n) {
    request_.numSlices = n;
    return *this;
}

ExecutionRequestBuilder& ExecutionRequestBuilder::participationRate(double rate) {
    request_.participationRate = rate;
    return *this;
}

ExecutionRequestBuilder& ExecutionRequestBuilder::postOnly(bool v) {
    request_.postOnly = v;
    return *this;
}

ExecutionRequestBuilder& ExecutionRequestBuilder::reduceOnly(bool v) {
    request_.reduceOnly = v;
    return *this;
}

ExecutionRequestBuilder& ExecutionRequestBuilder::maxPrice(Price p) {
    request_.maxPrice = p;
    return *this;
}

ExecutionRequestBuilder& ExecutionRequestBuilder::minPrice(Price p) {
    request_.minPrice = p;
    return *this;
}

ExecutionRequestBuilder& ExecutionRequestBuilder::deadline(Timestamp t) {
    request_.deadline = t;
    return *this;
}

ExecutionRequestBuilder& ExecutionRequestBuilder::strategyId(const std::string& id) {
    request_.strategyId = id;
    return *this;
}

ExecutionRequest ExecutionRequestBuilder::build() {
    return request_;
}

} // namespace hft::oms
