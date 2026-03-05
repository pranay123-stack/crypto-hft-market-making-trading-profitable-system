/**
 * @file order.cpp
 * @brief Implementation of Order structure and state machine
 */

#include "oms/order.hpp"

#include <sstream>
#include <iomanip>
#include <chrono>

namespace hft::oms {

// Initialize static member
std::atomic<uint64_t> OrderFactory::nextOrderId_{1};

std::string Order::toString() const {
    std::ostringstream oss;
    oss << "Order{"
        << "id=" << orderId
        << ", symbol=" << symbol.view()
        << ", exchange=" << exchange_to_string(exchange)
        << ", side=" << side_to_string(side)
        << ", type=" << order_type_to_string(type)
        << ", state=" << state_to_string(state)
        << ", price=" << std::fixed << std::setprecision(8) << price.to_double()
        << ", qty=" << quantity.to_double()
        << ", filled=" << filledQuantity.to_double()
        << ", remaining=" << remainingQuantity.to_double();

    if (!exchangeOrderId.empty()) {
        oss << ", exchId=" << exchangeOrderId;
    }
    if (avgFillPrice.value > 0) {
        oss << ", avgPrice=" << avgFillPrice.to_double();
    }
    if (!rejectReason.empty()) {
        oss << ", reason=" << rejectReason;
    }

    oss << "}";
    return oss.str();
}

// State transition matrix
// Rows: from state, Columns: to state
// 1 = valid transition, 0 = invalid
static constexpr uint8_t STATE_TRANSITIONS[11][11] = {
    // Created PendNew New PartFill Filled PendCan Cancel PendRepl Reject Expire Error
    {  0,      1,      0,  0,       0,     0,      1,     0,       1,     0,     1  }, // Created
    {  0,      0,      1,  0,       0,     1,      0,     0,       1,     0,     1  }, // PendingNew
    {  0,      0,      0,  1,       1,     1,      0,     1,       0,     1,     1  }, // New
    {  0,      0,      0,  0,       1,     1,      0,     1,       0,     1,     1  }, // PartiallyFilled
    {  0,      0,      0,  0,       0,     0,      0,     0,       0,     0,     0  }, // Filled (terminal)
    {  0,      0,      1,  0,       1,     0,      1,     0,       1,     0,     1  }, // PendingCancel
    {  0,      0,      0,  0,       0,     0,      0,     0,       0,     0,     0  }, // Cancelled (terminal)
    {  0,      0,      1,  1,       1,     1,      0,     0,       1,     0,     1  }, // PendingReplace
    {  0,      0,      0,  0,       0,     0,      0,     0,       0,     0,     0  }, // Rejected (terminal)
    {  0,      0,      0,  0,       0,     0,      0,     0,       0,     0,     0  }, // Expired (terminal)
    {  0,      0,      0,  0,       0,     0,      0,     0,       0,     0,     0  }, // Error (terminal)
};

bool OrderStateMachine::isValidTransition(OrderState from, OrderState to) noexcept {
    auto fromIdx = static_cast<size_t>(from);
    auto toIdx = static_cast<size_t>(to);

    if (fromIdx >= 11 || toIdx >= 11) {
        return false;
    }

    return STATE_TRANSITIONS[fromIdx][toIdx] == 1;
}

bool OrderStateMachine::transition(Order& order, OrderState newState) noexcept {
    if (!isValidTransition(order.state, newState)) {
        return false;
    }

    order.state = newState;
    order.lastUpdateTime = Timestamp{static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count())};

    return true;
}

std::array<OrderState, 4> OrderStateMachine::getAllowedTransitions(OrderState from) noexcept {
    std::array<OrderState, 4> allowed{};
    size_t count = 0;

    auto fromIdx = static_cast<size_t>(from);
    if (fromIdx >= 11) {
        return allowed;
    }

    for (size_t i = 0; i < 11 && count < 4; ++i) {
        if (STATE_TRANSITIONS[fromIdx][i] == 1) {
            allowed[count++] = static_cast<OrderState>(i);
        }
    }

    return allowed;
}

std::string ExecutionReport::toString() const {
    std::ostringstream oss;
    oss << "ExecutionReport{"
        << "orderId=" << orderId
        << ", execId=" << executionId
        << ", symbol=" << symbol.view()
        << ", exchange=" << exchange_to_string(exchange)
        << ", side=" << side_to_string(side)
        << ", execType=" << exec_type_to_string(execType)
        << ", orderState=" << state_to_string(orderState)
        << ", price=" << std::fixed << std::setprecision(8) << price.to_double()
        << ", qty=" << quantity.to_double();

    if (isFill()) {
        oss << ", lastPrice=" << lastPrice.to_double()
            << ", lastQty=" << lastQuantity.to_double()
            << ", filled=" << filledQuantity.to_double()
            << ", avgPrice=" << avgPrice.to_double()
            << ", commission=" << commission
            << ", commAsset=" << commissionAsset;
    }

    if (!text.empty()) {
        oss << ", text=" << text;
    }

    oss << "}";
    return oss.str();
}

// OrderFactory implementation
Order OrderFactory::createOrder(
    const Symbol& symbol,
    Exchange exchange,
    Side side,
    OrderType type,
    Price price,
    Quantity quantity
) {
    Order order;
    order.orderId = generateOrderId();
    order.clientOrderId = order.orderId;
    order.symbol = symbol;
    order.exchange = exchange;
    order.side = side;
    order.type = type;
    order.price = price;
    order.quantity = quantity;
    order.remainingQuantity = quantity;
    order.state = OrderState::Created;
    order.createTime = Timestamp{static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count())};
    order.lastUpdateTime = order.createTime;

    return order;
}

Order OrderFactory::createMarketOrder(
    const Symbol& symbol,
    Exchange exchange,
    Side side,
    Quantity quantity
) {
    Order order = createOrder(symbol, exchange, side, OrderType::Market, Price{0}, quantity);
    order.tif = TimeInForce::IOC;
    return order;
}

Order OrderFactory::createLimitOrder(
    const Symbol& symbol,
    Exchange exchange,
    Side side,
    Price price,
    Quantity quantity,
    TimeInForce tif
) {
    Order order = createOrder(symbol, exchange, side, OrderType::Limit, price, quantity);
    order.tif = tif;
    return order;
}

Order OrderFactory::createStopOrder(
    const Symbol& symbol,
    Exchange exchange,
    Side side,
    Price stopPrice,
    Quantity quantity
) {
    Order order = createOrder(symbol, exchange, side, OrderType::StopLoss, Price{0}, quantity);
    order.stopPrice = stopPrice;
    order.tif = TimeInForce::GTC;
    return order;
}

Order OrderFactory::createStopLimitOrder(
    const Symbol& symbol,
    Exchange exchange,
    Side side,
    Price price,
    Price stopPrice,
    Quantity quantity
) {
    Order order = createOrder(symbol, exchange, side, OrderType::StopLossLimit, price, quantity);
    order.stopPrice = stopPrice;
    order.tif = TimeInForce::GTC;
    return order;
}

uint64_t OrderFactory::generateOrderId() noexcept {
    // Generate unique ID with timestamp component and sequence
    // Format: [40 bits timestamp (ms)] [24 bits sequence]
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    uint64_t seq = nextOrderId_.fetch_add(1, std::memory_order_relaxed) & 0xFFFFFF;
    return (static_cast<uint64_t>(now) << 24) | seq;
}

void OrderFactory::resetSequence(uint64_t startId) noexcept {
    nextOrderId_.store(startId, std::memory_order_relaxed);
}

} // namespace hft::oms
