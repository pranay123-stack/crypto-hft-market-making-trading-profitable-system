/**
 * @file order_manager.cpp
 * @brief Implementation of central Order Management System
 */

#include "oms/order_manager.hpp"
#include "oms/position_manager.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <chrono>

namespace hft::oms {

// ============================================================================
// SymbolOrderBook Implementation
// ============================================================================

SymbolOrderBook::SymbolOrderBook(const Symbol& symbol)
    : symbol_(symbol)
{}

void SymbolOrderBook::addOrder(const Order& order) {
    std::unique_lock lock(mutex_);

    orders_[order.orderId] = order;

    // Add to price level index
    if (order.side == Side::Buy) {
        bidsByPrice_[order.price.value].push_back(order.orderId);
    } else {
        asksByPrice_[order.price.value].push_back(order.orderId);
    }
}

void SymbolOrderBook::removeOrder(uint64_t orderId) {
    std::unique_lock lock(mutex_);

    auto it = orders_.find(orderId);
    if (it == orders_.end()) {
        return;
    }

    const Order& order = it->second;

    // Remove from price level index
    auto& priceMap = (order.side == Side::Buy) ? bidsByPrice_ : asksByPrice_;
    auto priceIt = priceMap.find(order.price.value);
    if (priceIt != priceMap.end()) {
        auto& orderIds = priceIt->second;
        orderIds.erase(std::remove(orderIds.begin(), orderIds.end(), orderId), orderIds.end());
        if (orderIds.empty()) {
            priceMap.erase(priceIt);
        }
    }

    orders_.erase(it);
}

void SymbolOrderBook::updateOrder(const Order& order) {
    std::unique_lock lock(mutex_);

    auto it = orders_.find(order.orderId);
    if (it != orders_.end()) {
        it->second = order;
    }
}

std::vector<Order> SymbolOrderBook::getBuyOrders() const {
    std::shared_lock lock(mutex_);

    std::vector<Order> result;
    for (const auto& [price, orderIds] : bidsByPrice_) {
        for (uint64_t orderId : orderIds) {
            auto it = orders_.find(orderId);
            if (it != orders_.end() && is_active_state(it->second.state)) {
                result.push_back(it->second);
            }
        }
    }
    return result;
}

std::vector<Order> SymbolOrderBook::getSellOrders() const {
    std::shared_lock lock(mutex_);

    std::vector<Order> result;
    for (const auto& [price, orderIds] : asksByPrice_) {
        for (uint64_t orderId : orderIds) {
            auto it = orders_.find(orderId);
            if (it != orders_.end() && is_active_state(it->second.state)) {
                result.push_back(it->second);
            }
        }
    }
    return result;
}

std::vector<Order> SymbolOrderBook::getAllOrders() const {
    std::shared_lock lock(mutex_);

    std::vector<Order> result;
    result.reserve(orders_.size());
    for (const auto& [id, order] : orders_) {
        if (is_active_state(order.state)) {
            result.push_back(order);
        }
    }
    return result;
}

std::optional<Order> SymbolOrderBook::getOrder(uint64_t orderId) const {
    std::shared_lock lock(mutex_);

    auto it = orders_.find(orderId);
    if (it != orders_.end()) {
        return it->second;
    }
    return std::nullopt;
}

Quantity SymbolOrderBook::getBidQuantityAtPrice(Price price) const {
    std::shared_lock lock(mutex_);

    Quantity total{0};
    auto it = bidsByPrice_.find(price.value);
    if (it != bidsByPrice_.end()) {
        for (uint64_t orderId : it->second) {
            auto orderIt = orders_.find(orderId);
            if (orderIt != orders_.end() && is_active_state(orderIt->second.state)) {
                total = total + orderIt->second.remainingQuantity;
            }
        }
    }
    return total;
}

Quantity SymbolOrderBook::getAskQuantityAtPrice(Price price) const {
    std::shared_lock lock(mutex_);

    Quantity total{0};
    auto it = asksByPrice_.find(price.value);
    if (it != asksByPrice_.end()) {
        for (uint64_t orderId : it->second) {
            auto orderIt = orders_.find(orderId);
            if (orderIt != orders_.end() && is_active_state(orderIt->second.state)) {
                total = total + orderIt->second.remainingQuantity;
            }
        }
    }
    return total;
}

std::optional<Price> SymbolOrderBook::getBestBid() const {
    std::shared_lock lock(mutex_);

    // Iterate from highest price to find active bid
    for (auto it = bidsByPrice_.rbegin(); it != bidsByPrice_.rend(); ++it) {
        for (uint64_t orderId : it->second) {
            auto orderIt = orders_.find(orderId);
            if (orderIt != orders_.end() && is_active_state(orderIt->second.state)) {
                return Price{it->first};
            }
        }
    }
    return std::nullopt;
}

std::optional<Price> SymbolOrderBook::getBestAsk() const {
    std::shared_lock lock(mutex_);

    // Iterate from lowest price to find active ask
    for (auto it = asksByPrice_.begin(); it != asksByPrice_.end(); ++it) {
        for (uint64_t orderId : it->second) {
            auto orderIt = orders_.find(orderId);
            if (orderIt != orders_.end() && is_active_state(orderIt->second.state)) {
                return Price{it->first};
            }
        }
    }
    return std::nullopt;
}

size_t SymbolOrderBook::getActiveOrderCount() const {
    std::shared_lock lock(mutex_);

    size_t count = 0;
    for (const auto& [id, order] : orders_) {
        if (is_active_state(order.state)) {
            ++count;
        }
    }
    return count;
}

// ============================================================================
// OrderManager Implementation
// ============================================================================

OrderManager::OrderManager(std::shared_ptr<PositionManager> positionManager)
    : positionManager_(std::move(positionManager))
{
    LOG_INFO("OrderManager initialized");
}

OrderManager::~OrderManager() {
    LOG_INFO("OrderManager shutting down - active orders: {}", getActiveOrderCount());
}

void OrderManager::registerExchange(Exchange exchange, std::shared_ptr<IExchangeConnector> connector) {
    std::unique_lock lock(exchangesMutex_);
    exchanges_[exchange] = std::move(connector);
    LOG_INFO("Registered exchange connector: {}", exchange_to_string(exchange));
}

void OrderManager::unregisterExchange(Exchange exchange) {
    std::unique_lock lock(exchangesMutex_);
    exchanges_.erase(exchange);
    LOG_INFO("Unregistered exchange connector: {}", exchange_to_string(exchange));
}

uint64_t OrderManager::submitOrder(
    const Symbol& symbol,
    Exchange exchange,
    Side side,
    OrderType type,
    Price price,
    Quantity quantity,
    TimeInForce tif,
    const std::string& strategyId
) {
    // Create order
    Order order = OrderFactory::createOrder(symbol, exchange, side, type, price, quantity);
    order.tif = tif;
    order.strategyId = strategyId;
    order.clientOrderId = generateClientOrderId();

    if (submitOrder(order)) {
        return order.orderId;
    }
    return 0;
}

bool OrderManager::submitOrder(Order& order) {
    // Validate order
    if (order.quantity.value == 0) {
        LOG_ERROR("Cannot submit order with zero quantity");
        return false;
    }

    // Get connector
    auto connector = getConnector(order.exchange);
    if (!connector) {
        LOG_ERROR("No connector for exchange: {}", exchange_to_string(order.exchange));
        order.state = OrderState::Rejected;
        order.rejectReason = "No exchange connector";
        return false;
    }

    if (!connector->isConnected()) {
        LOG_ERROR("Exchange not connected: {}", exchange_to_string(order.exchange));
        order.state = OrderState::Rejected;
        order.rejectReason = "Exchange disconnected";
        return false;
    }

    // Record send time
    order.sendTime = Timestamp{static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count())};

    // Transition to pending
    OrderStateMachine::transition(order, OrderState::PendingNew);

    // Store order
    {
        std::unique_lock lock(ordersMutex_);
        orders_[order.orderId] = order;
    }

    // Add to order book
    auto book = getOrderBook(order.symbol);
    book->addOrder(order);

    // Submit to exchange
    if (!connector->submitOrder(order)) {
        LOG_ERROR("Failed to submit order to exchange: {}", order.toString());

        // Update state
        {
            std::unique_lock lock(ordersMutex_);
            auto it = orders_.find(order.orderId);
            if (it != orders_.end()) {
                OrderStateMachine::transition(it->second, OrderState::Rejected);
                it->second.rejectReason = "Submit failed";
                order = it->second;
            }
        }

        book->removeOrder(order.orderId);
        notifyOrderUpdate(order);
        return false;
    }

    stats_.totalOrdersCreated.fetch_add(1, std::memory_order_relaxed);
    stats_.totalOrdersSubmitted.fetch_add(1, std::memory_order_relaxed);

    LOG_DEBUG("Order submitted: {}", order.toString());
    notifyOrderUpdate(order);

    return true;
}

bool OrderManager::cancelOrder(uint64_t orderId) {
    Order order;
    {
        std::shared_lock lock(ordersMutex_);
        auto it = orders_.find(orderId);
        if (it == orders_.end()) {
            LOG_WARN("Cancel request for unknown order: {}", orderId);
            return false;
        }
        order = it->second;
    }

    if (!is_active_state(order.state)) {
        LOG_WARN("Cannot cancel order in state: {}", state_to_string(order.state));
        return false;
    }

    auto connector = getConnector(order.exchange);
    if (!connector || !connector->isConnected()) {
        LOG_ERROR("Cannot cancel - exchange unavailable");
        return false;
    }

    // Transition to pending cancel
    {
        std::unique_lock lock(ordersMutex_);
        auto it = orders_.find(orderId);
        if (it != orders_.end()) {
            OrderStateMachine::transition(it->second, OrderState::PendingCancel);
            order = it->second;
        }
    }

    // Send cancel request
    if (!connector->cancelOrder(orderId, order.exchangeOrderId)) {
        LOG_ERROR("Failed to send cancel request for order: {}", orderId);
        return false;
    }

    LOG_DEBUG("Cancel request sent for order: {}", orderId);
    notifyOrderUpdate(order);

    return true;
}

size_t OrderManager::cancelAllOrders(const Symbol& symbol) {
    std::vector<uint64_t> orderIds;

    {
        std::shared_lock lock(ordersMutex_);
        for (const auto& [id, order] : orders_) {
            if (order.symbol == symbol && is_active_state(order.state)) {
                orderIds.push_back(id);
            }
        }
    }

    size_t count = 0;
    for (uint64_t orderId : orderIds) {
        if (cancelOrder(orderId)) {
            ++count;
        }
    }

    LOG_INFO("Cancelled {} orders for symbol {}", count, symbol.view());
    return count;
}

size_t OrderManager::cancelAllOrdersOnExchange(Exchange exchange) {
    std::vector<uint64_t> orderIds;

    {
        std::shared_lock lock(ordersMutex_);
        for (const auto& [id, order] : orders_) {
            if (order.exchange == exchange && is_active_state(order.state)) {
                orderIds.push_back(id);
            }
        }
    }

    size_t count = 0;
    for (uint64_t orderId : orderIds) {
        if (cancelOrder(orderId)) {
            ++count;
        }
    }

    LOG_INFO("Cancelled {} orders on exchange {}", count, exchange_to_string(exchange));
    return count;
}

size_t OrderManager::cancelAllOrders() {
    std::vector<uint64_t> orderIds;

    {
        std::shared_lock lock(ordersMutex_);
        for (const auto& [id, order] : orders_) {
            if (is_active_state(order.state)) {
                orderIds.push_back(id);
            }
        }
    }

    size_t count = 0;
    for (uint64_t orderId : orderIds) {
        if (cancelOrder(orderId)) {
            ++count;
        }
    }

    LOG_INFO("Cancelled {} total orders", count);
    return count;
}

bool OrderManager::modifyOrder(uint64_t orderId, std::optional<Price> newPrice,
                               std::optional<Quantity> newQuantity) {
    Order order;
    {
        std::shared_lock lock(ordersMutex_);
        auto it = orders_.find(orderId);
        if (it == orders_.end()) {
            LOG_WARN("Modify request for unknown order: {}", orderId);
            return false;
        }
        order = it->second;
    }

    if (!is_active_state(order.state)) {
        LOG_WARN("Cannot modify order in state: {}", state_to_string(order.state));
        return false;
    }

    auto connector = getConnector(order.exchange);
    if (!connector || !connector->isConnected()) {
        LOG_ERROR("Cannot modify - exchange unavailable");
        return false;
    }

    OrderModification mod;
    mod.orderId = orderId;
    mod.newPrice = newPrice;
    mod.newQuantity = newQuantity;
    mod.requestTime = Timestamp{static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count())};

    if (!mod.isValid()) {
        LOG_WARN("Invalid modification request for order: {}", orderId);
        return false;
    }

    // Transition to pending replace
    {
        std::unique_lock lock(ordersMutex_);
        auto it = orders_.find(orderId);
        if (it != orders_.end()) {
            OrderStateMachine::transition(it->second, OrderState::PendingReplace);
        }
    }

    // Send modify request
    if (!connector->modifyOrder(mod)) {
        LOG_ERROR("Failed to send modify request for order: {}", orderId);
        return false;
    }

    LOG_DEBUG("Modify request sent for order: {}", orderId);
    return true;
}

std::optional<Order> OrderManager::getOrder(uint64_t orderId) const {
    {
        std::shared_lock lock(ordersMutex_);
        auto it = orders_.find(orderId);
        if (it != orders_.end()) {
            return it->second;
        }

        // Check filled orders
        auto filledIt = filledOrders_.find(orderId);
        if (filledIt != filledOrders_.end()) {
            return filledIt->second;
        }

        // Check cancelled orders
        auto cancelledIt = cancelledOrders_.find(orderId);
        if (cancelledIt != cancelledOrders_.end()) {
            return cancelledIt->second;
        }
    }

    return std::nullopt;
}

std::vector<Order> OrderManager::getOrdersForSymbol(const Symbol& symbol) const {
    std::shared_lock lock(ordersMutex_);

    std::vector<Order> result;
    for (const auto& [id, order] : orders_) {
        if (order.symbol == symbol) {
            result.push_back(order);
        }
    }
    return result;
}

std::vector<Order> OrderManager::getActiveOrders() const {
    std::shared_lock lock(ordersMutex_);

    std::vector<Order> result;
    for (const auto& [id, order] : orders_) {
        if (is_active_state(order.state)) {
            result.push_back(order);
        }
    }
    return result;
}

std::vector<Order> OrderManager::getActiveOrdersForExchange(Exchange exchange) const {
    std::shared_lock lock(ordersMutex_);

    std::vector<Order> result;
    for (const auto& [id, order] : orders_) {
        if (order.exchange == exchange && is_active_state(order.state)) {
            result.push_back(order);
        }
    }
    return result;
}

std::vector<Order> OrderManager::getFilledOrders() const {
    std::shared_lock lock(ordersMutex_);

    std::vector<Order> result;
    result.reserve(filledOrders_.size());
    for (const auto& [id, order] : filledOrders_) {
        result.push_back(order);
    }
    return result;
}

size_t OrderManager::getOrderCountByState(OrderState state) const {
    std::shared_lock lock(ordersMutex_);

    size_t count = 0;
    for (const auto& [id, order] : orders_) {
        if (order.state == state) {
            ++count;
        }
    }
    return count;
}

size_t OrderManager::getActiveOrderCount() const {
    std::shared_lock lock(ordersMutex_);

    size_t count = 0;
    for (const auto& [id, order] : orders_) {
        if (is_active_state(order.state)) {
            ++count;
        }
    }
    return count;
}

void OrderManager::onExecutionReport(const ExecutionReport& report) {
    LOG_DEBUG("Received execution report: {}", report.toString());

    std::unique_lock lock(ordersMutex_);

    auto it = orders_.find(report.orderId);
    if (it == orders_.end()) {
        LOG_WARN("Execution report for unknown order: {}", report.orderId);
        return;
    }

    Order& order = it->second;
    updateOrderFromExecution(order, report);

    // Update order book
    auto book = getOrderBook(order.symbol);
    book->updateOrder(order);

    // Handle terminal states
    if (order.state == OrderState::Filled) {
        moveToFilled(order.orderId);
        stats_.totalOrdersFilled.fetch_add(1, std::memory_order_relaxed);
    } else if (order.state == OrderState::Cancelled) {
        moveToCancelled(order.orderId);
        stats_.totalOrdersCancelled.fetch_add(1, std::memory_order_relaxed);
    } else if (order.state == OrderState::Rejected) {
        moveToCancelled(order.orderId);
        stats_.totalOrdersRejected.fetch_add(1, std::memory_order_relaxed);
    }

    // Track fills
    if (report.isFill()) {
        stats_.totalFills.fetch_add(1, std::memory_order_relaxed);

        // Update position manager
        if (positionManager_) {
            positionManager_->onFill(
                order.symbol,
                order.exchange,
                order.side,
                report.lastPrice,
                report.lastQuantity
            );
        }
    }

    lock.unlock();

    notifyOrderUpdate(order);
    notifyExecution(report);
}

void OrderManager::onFill(uint64_t orderId, Price fillPrice, Quantity fillQty,
                          const std::string& executionId) {
    ExecutionReport report;
    report.orderId = orderId;
    report.executionId = executionId;
    report.execType = ExecutionType::Trade;
    report.lastPrice = fillPrice;
    report.lastQuantity = fillQty;
    report.receiveTime = Timestamp{static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count())};

    // Get current order state
    {
        std::shared_lock lock(ordersMutex_);
        auto it = orders_.find(orderId);
        if (it != orders_.end()) {
            report.symbol = it->second.symbol;
            report.exchange = it->second.exchange;
            report.side = it->second.side;
            report.price = it->second.price;
            report.quantity = it->second.quantity;

            // Calculate filled quantity
            Quantity newFilled = it->second.filledQuantity + fillQty;
            report.filledQuantity = newFilled;
            report.remainingQuantity = Quantity{it->second.quantity.value - newFilled.value};

            // Determine if fully filled
            if (report.remainingQuantity.value == 0) {
                report.orderState = OrderState::Filled;
            } else {
                report.orderState = OrderState::PartiallyFilled;
            }
        }
    }

    onExecutionReport(report);
}

void OrderManager::onOrderAcknowledged(uint64_t orderId, const std::string& exchangeOrderId) {
    std::unique_lock lock(ordersMutex_);

    auto it = orders_.find(orderId);
    if (it == orders_.end()) {
        LOG_WARN("Ack for unknown order: {}", orderId);
        return;
    }

    Order& order = it->second;
    order.exchangeOrderId = exchangeOrderId;
    order.ackTime = Timestamp{static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count())};

    OrderStateMachine::transition(order, OrderState::New);

    // Track latency
    uint64_t latencyUs = (order.ackTime.nanos - order.sendTime.nanos) / 1000;
    stats_.avgAckLatencyUs.store(latencyUs, std::memory_order_relaxed);
    if (latencyUs > stats_.maxAckLatencyUs.load(std::memory_order_relaxed)) {
        stats_.maxAckLatencyUs.store(latencyUs, std::memory_order_relaxed);
    }

    // Update order book
    auto book = getOrderBook(order.symbol);
    book->updateOrder(order);

    lock.unlock();

    LOG_DEBUG("Order acknowledged: {} -> {}", orderId, exchangeOrderId);
    notifyOrderUpdate(order);
}

void OrderManager::onOrderRejected(uint64_t orderId, const std::string& reason) {
    std::unique_lock lock(ordersMutex_);

    auto it = orders_.find(orderId);
    if (it == orders_.end()) {
        LOG_WARN("Rejection for unknown order: {}", orderId);
        return;
    }

    Order& order = it->second;
    order.rejectReason = reason;
    OrderStateMachine::transition(order, OrderState::Rejected);

    stats_.totalOrdersRejected.fetch_add(1, std::memory_order_relaxed);

    // Remove from order book
    auto book = getOrderBook(order.symbol);
    book->removeOrder(orderId);

    lock.unlock();

    LOG_WARN("Order rejected: {} - {}", orderId, reason);
    moveToCancelled(orderId);
    notifyOrderUpdate(order);
}

void OrderManager::onOrderCancelled(uint64_t orderId) {
    std::unique_lock lock(ordersMutex_);

    auto it = orders_.find(orderId);
    if (it == orders_.end()) {
        LOG_WARN("Cancel confirmation for unknown order: {}", orderId);
        return;
    }

    Order& order = it->second;
    OrderStateMachine::transition(order, OrderState::Cancelled);

    stats_.totalOrdersCancelled.fetch_add(1, std::memory_order_relaxed);

    // Remove from order book
    auto book = getOrderBook(order.symbol);
    book->removeOrder(orderId);

    lock.unlock();

    LOG_DEBUG("Order cancelled: {}", orderId);
    moveToCancelled(orderId);
    notifyOrderUpdate(order);
}

void OrderManager::setOrderCallback(OrderCallback callback) {
    orderCallback_ = std::move(callback);
}

void OrderManager::setExecutionCallback(ExecutionCallback callback) {
    executionCallback_ = std::move(callback);
}

std::shared_ptr<SymbolOrderBook> OrderManager::getOrderBook(const Symbol& symbol) {
    std::string key{symbol.view()};

    {
        std::shared_lock lock(booksMutex_);
        auto it = orderBooks_.find(key);
        if (it != orderBooks_.end()) {
            return it->second;
        }
    }

    // Create new order book
    std::unique_lock lock(booksMutex_);
    auto it = orderBooks_.find(key);
    if (it != orderBooks_.end()) {
        return it->second;
    }

    auto book = std::make_shared<SymbolOrderBook>(symbol);
    orderBooks_[key] = book;
    return book;
}

void OrderManager::resetStats() {
    stats_.totalOrdersCreated.store(0, std::memory_order_relaxed);
    stats_.totalOrdersSubmitted.store(0, std::memory_order_relaxed);
    stats_.totalOrdersFilled.store(0, std::memory_order_relaxed);
    stats_.totalOrdersCancelled.store(0, std::memory_order_relaxed);
    stats_.totalOrdersRejected.store(0, std::memory_order_relaxed);
    stats_.totalFills.store(0, std::memory_order_relaxed);
    stats_.totalNotionalTraded.store(0, std::memory_order_relaxed);
    stats_.avgSubmitLatencyUs.store(0, std::memory_order_relaxed);
    stats_.maxSubmitLatencyUs.store(0, std::memory_order_relaxed);
    stats_.avgAckLatencyUs.store(0, std::memory_order_relaxed);
    stats_.maxAckLatencyUs.store(0, std::memory_order_relaxed);
}

uint64_t OrderManager::generateClientOrderId() const {
    return nextClientOrderId_.fetch_add(1, std::memory_order_relaxed);
}

bool OrderManager::isExchangeConnected(Exchange exchange) const {
    std::shared_lock lock(exchangesMutex_);

    auto it = exchanges_.find(exchange);
    if (it != exchanges_.end() && it->second) {
        return it->second->isConnected();
    }
    return false;
}

void OrderManager::updateOrderFromExecution(Order& order, const ExecutionReport& report) {
    order.state = report.orderState;
    order.lastUpdateTime = report.receiveTime;

    if (report.isFill()) {
        // Update fill information
        order.filledQuantity = report.filledQuantity;
        order.remainingQuantity = report.remainingQuantity;
        order.fillCount++;
        order.lastFillTime = report.transactTime;

        // Update average fill price
        if (order.filledQuantity.value > 0) {
            // Weighted average
            double totalValue = order.avgFillPrice.to_double() *
                               (order.filledQuantity.value - report.lastQuantity.value);
            totalValue += report.lastPrice.to_double() * report.lastQuantity.to_double();
            order.avgFillPrice = Price::from_double(totalValue / order.filledQuantity.to_double());
        }

        // Update commission
        order.totalCommission += report.commission;
        if (!report.commissionAsset.empty()) {
            order.commissionAsset = report.commissionAsset;
        }
    }

    if (!report.exchangeOrderId.empty()) {
        order.exchangeOrderId = report.exchangeOrderId;
    }

    if (!report.text.empty() && order.rejectReason.empty()) {
        order.rejectReason = report.text;
    }
}

void OrderManager::moveToFilled(uint64_t orderId) {
    // Note: Caller must hold ordersMutex_ write lock
    auto it = orders_.find(orderId);
    if (it != orders_.end()) {
        filledOrders_[orderId] = std::move(it->second);
        orders_.erase(it);
    }
}

void OrderManager::moveToCancelled(uint64_t orderId) {
    std::unique_lock lock(ordersMutex_);

    auto it = orders_.find(orderId);
    if (it != orders_.end()) {
        cancelledOrders_[orderId] = std::move(it->second);
        orders_.erase(it);
    }
}

void OrderManager::notifyOrderUpdate(const Order& order) {
    if (orderCallback_) {
        orderCallback_(order);
    }
}

void OrderManager::notifyExecution(const ExecutionReport& report) {
    if (executionCallback_) {
        executionCallback_(report);
    }
}

std::shared_ptr<IExchangeConnector> OrderManager::getConnector(Exchange exchange) {
    std::shared_lock lock(exchangesMutex_);

    auto it = exchanges_.find(exchange);
    if (it != exchanges_.end()) {
        return it->second;
    }
    return nullptr;
}

// ============================================================================
// OrderManagerBuilder Implementation
// ============================================================================

OrderManagerBuilder& OrderManagerBuilder::withPositionManager(std::shared_ptr<PositionManager> pm) {
    positionManager_ = std::move(pm);
    return *this;
}

OrderManagerBuilder& OrderManagerBuilder::withExchange(
    Exchange exchange, std::shared_ptr<IExchangeConnector> connector) {
    exchanges_.emplace_back(exchange, std::move(connector));
    return *this;
}

OrderManagerBuilder& OrderManagerBuilder::withOrderCallback(OrderCallback callback) {
    orderCallback_ = std::move(callback);
    return *this;
}

OrderManagerBuilder& OrderManagerBuilder::withExecutionCallback(ExecutionCallback callback) {
    executionCallback_ = std::move(callback);
    return *this;
}

std::unique_ptr<OrderManager> OrderManagerBuilder::build() {
    auto manager = std::make_unique<OrderManager>(positionManager_);

    for (auto& [exchange, connector] : exchanges_) {
        manager->registerExchange(exchange, std::move(connector));
    }

    if (orderCallback_) {
        manager->setOrderCallback(std::move(orderCallback_));
    }

    if (executionCallback_) {
        manager->setExecutionCallback(std::move(executionCallback_));
    }

    return manager;
}

} // namespace hft::oms
