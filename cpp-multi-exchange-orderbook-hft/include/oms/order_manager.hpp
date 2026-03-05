#pragma once

/**
 * @file order_manager.hpp
 * @brief Central Order Management System (OMS) for HFT trading
 *
 * Provides centralized order management including:
 * - Order creation, submission, and tracking
 * - Order routing to exchanges
 * - Fill aggregation and position updates
 * - Order book management per symbol
 * - Thread-safe concurrent order operations
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

#include "core/types.hpp"
#include "core/lock_free_queue.hpp"
#include "oms/order.hpp"

namespace hft::oms {

using namespace hft::core;

// Forward declarations
class PositionManager;

/**
 * @brief Exchange connector interface for order routing
 */
class IExchangeConnector {
public:
    virtual ~IExchangeConnector() = default;

    /**
     * @brief Submit order to exchange
     * @return true if order submitted successfully
     */
    virtual bool submitOrder(const Order& order) = 0;

    /**
     * @brief Cancel order on exchange
     */
    virtual bool cancelOrder(uint64_t orderId, const std::string& exchangeOrderId) = 0;

    /**
     * @brief Modify order on exchange
     */
    virtual bool modifyOrder(const OrderModification& mod) = 0;

    /**
     * @brief Get exchange identifier
     */
    [[nodiscard]] virtual Exchange getExchange() const = 0;

    /**
     * @brief Check if exchange is connected
     */
    [[nodiscard]] virtual bool isConnected() const = 0;
};

/**
 * @brief Order book for tracking orders per symbol
 */
class SymbolOrderBook {
public:
    explicit SymbolOrderBook(const Symbol& symbol);

    /**
     * @brief Add order to book
     */
    void addOrder(const Order& order);

    /**
     * @brief Remove order from book
     */
    void removeOrder(uint64_t orderId);

    /**
     * @brief Update order in book
     */
    void updateOrder(const Order& order);

    /**
     * @brief Get all active buy orders
     */
    [[nodiscard]] std::vector<Order> getBuyOrders() const;

    /**
     * @brief Get all active sell orders
     */
    [[nodiscard]] std::vector<Order> getSellOrders() const;

    /**
     * @brief Get all active orders
     */
    [[nodiscard]] std::vector<Order> getAllOrders() const;

    /**
     * @brief Get order by ID
     */
    [[nodiscard]] std::optional<Order> getOrder(uint64_t orderId) const;

    /**
     * @brief Get total bid quantity at price level
     */
    [[nodiscard]] Quantity getBidQuantityAtPrice(Price price) const;

    /**
     * @brief Get total ask quantity at price level
     */
    [[nodiscard]] Quantity getAskQuantityAtPrice(Price price) const;

    /**
     * @brief Get best bid price
     */
    [[nodiscard]] std::optional<Price> getBestBid() const;

    /**
     * @brief Get best ask price
     */
    [[nodiscard]] std::optional<Price> getBestAsk() const;

    /**
     * @brief Get symbol
     */
    [[nodiscard]] const Symbol& getSymbol() const { return symbol_; }

    /**
     * @brief Get active order count
     */
    [[nodiscard]] size_t getActiveOrderCount() const;

private:
    Symbol symbol_;
    mutable std::shared_mutex mutex_;

    // Orders indexed by order ID
    std::unordered_map<uint64_t, Order> orders_;

    // Price levels for quick lookup
    std::map<uint64_t, std::vector<uint64_t>> bidsByPrice_; // price -> order IDs (descending)
    std::map<uint64_t, std::vector<uint64_t>> asksByPrice_; // price -> order IDs (ascending)
};

/**
 * @brief Order manager statistics
 */
struct OrderManagerStats {
    std::atomic<uint64_t> totalOrdersCreated{0};
    std::atomic<uint64_t> totalOrdersSubmitted{0};
    std::atomic<uint64_t> totalOrdersFilled{0};
    std::atomic<uint64_t> totalOrdersCancelled{0};
    std::atomic<uint64_t> totalOrdersRejected{0};
    std::atomic<uint64_t> totalFills{0};
    std::atomic<uint64_t> totalNotionalTraded{0};

    // Latency tracking (in microseconds)
    std::atomic<uint64_t> avgSubmitLatencyUs{0};
    std::atomic<uint64_t> maxSubmitLatencyUs{0};
    std::atomic<uint64_t> avgAckLatencyUs{0};
    std::atomic<uint64_t> maxAckLatencyUs{0};
};

/**
 * @brief Central Order Manager
 *
 * Thread-safe order management with support for:
 * - Multiple exchanges
 * - Concurrent order operations
 * - Fill aggregation
 * - Order state tracking
 */
class OrderManager {
public:
    /**
     * @brief Constructor
     */
    explicit OrderManager(std::shared_ptr<PositionManager> positionManager = nullptr);

    /**
     * @brief Destructor
     */
    ~OrderManager();

    // Non-copyable
    OrderManager(const OrderManager&) = delete;
    OrderManager& operator=(const OrderManager&) = delete;

    /**
     * @brief Register exchange connector
     */
    void registerExchange(Exchange exchange, std::shared_ptr<IExchangeConnector> connector);

    /**
     * @brief Unregister exchange connector
     */
    void unregisterExchange(Exchange exchange);

    // ===== Order Operations =====

    /**
     * @brief Create and submit a new order
     * @return Order ID if successful, 0 otherwise
     */
    uint64_t submitOrder(
        const Symbol& symbol,
        Exchange exchange,
        Side side,
        OrderType type,
        Price price,
        Quantity quantity,
        TimeInForce tif = TimeInForce::GTC,
        const std::string& strategyId = ""
    );

    /**
     * @brief Submit pre-created order
     * @return true if order submitted successfully
     */
    bool submitOrder(Order& order);

    /**
     * @brief Cancel order
     * @return true if cancel request sent
     */
    bool cancelOrder(uint64_t orderId);

    /**
     * @brief Cancel all orders for symbol
     * @return Number of cancel requests sent
     */
    size_t cancelAllOrders(const Symbol& symbol);

    /**
     * @brief Cancel all orders on exchange
     * @return Number of cancel requests sent
     */
    size_t cancelAllOrdersOnExchange(Exchange exchange);

    /**
     * @brief Cancel all active orders
     * @return Number of cancel requests sent
     */
    size_t cancelAllOrders();

    /**
     * @brief Modify existing order (price/quantity)
     * @return true if modify request sent
     */
    bool modifyOrder(uint64_t orderId, std::optional<Price> newPrice,
                     std::optional<Quantity> newQuantity);

    // ===== Order Queries =====

    /**
     * @brief Get order by ID
     */
    [[nodiscard]] std::optional<Order> getOrder(uint64_t orderId) const;

    /**
     * @brief Get all orders for symbol
     */
    [[nodiscard]] std::vector<Order> getOrdersForSymbol(const Symbol& symbol) const;

    /**
     * @brief Get all active orders
     */
    [[nodiscard]] std::vector<Order> getActiveOrders() const;

    /**
     * @brief Get all active orders for exchange
     */
    [[nodiscard]] std::vector<Order> getActiveOrdersForExchange(Exchange exchange) const;

    /**
     * @brief Get all filled orders
     */
    [[nodiscard]] std::vector<Order> getFilledOrders() const;

    /**
     * @brief Get order count by state
     */
    [[nodiscard]] size_t getOrderCountByState(OrderState state) const;

    /**
     * @brief Get total active order count
     */
    [[nodiscard]] size_t getActiveOrderCount() const;

    // ===== Execution Report Handling =====

    /**
     * @brief Process execution report from exchange
     */
    void onExecutionReport(const ExecutionReport& report);

    /**
     * @brief Process fill
     */
    void onFill(uint64_t orderId, Price fillPrice, Quantity fillQty,
                const std::string& executionId);

    /**
     * @brief Process order acknowledgement
     */
    void onOrderAcknowledged(uint64_t orderId, const std::string& exchangeOrderId);

    /**
     * @brief Process order rejection
     */
    void onOrderRejected(uint64_t orderId, const std::string& reason);

    /**
     * @brief Process order cancellation
     */
    void onOrderCancelled(uint64_t orderId);

    // ===== Callbacks =====

    /**
     * @brief Set callback for order state changes
     */
    void setOrderCallback(OrderCallback callback);

    /**
     * @brief Set callback for execution reports
     */
    void setExecutionCallback(ExecutionCallback callback);

    // ===== Order Book Access =====

    /**
     * @brief Get order book for symbol
     */
    [[nodiscard]] std::shared_ptr<SymbolOrderBook> getOrderBook(const Symbol& symbol);

    // ===== Statistics =====

    /**
     * @brief Get order manager statistics
     */
    [[nodiscard]] const OrderManagerStats& getStats() const { return stats_; }

    /**
     * @brief Reset statistics
     */
    void resetStats();

    // ===== Utility =====

    /**
     * @brief Generate unique client order ID
     */
    [[nodiscard]] uint64_t generateClientOrderId() const;

    /**
     * @brief Check if exchange is connected
     */
    [[nodiscard]] bool isExchangeConnected(Exchange exchange) const;

private:
    // Order storage
    mutable std::shared_mutex ordersMutex_;
    std::unordered_map<uint64_t, Order> orders_;
    std::unordered_map<uint64_t, Order> filledOrders_;
    std::unordered_map<uint64_t, Order> cancelledOrders_;

    // Order books per symbol
    mutable std::shared_mutex booksMutex_;
    std::unordered_map<std::string, std::shared_ptr<SymbolOrderBook>> orderBooks_;

    // Exchange connectors
    mutable std::shared_mutex exchangesMutex_;
    std::unordered_map<Exchange, std::shared_ptr<IExchangeConnector>> exchanges_;

    // Position manager (optional)
    std::shared_ptr<PositionManager> positionManager_;

    // Callbacks
    OrderCallback orderCallback_;
    ExecutionCallback executionCallback_;

    // Statistics
    OrderManagerStats stats_;

    // Order ID generation
    mutable std::atomic<uint64_t> nextOrderId_{1};
    mutable std::atomic<uint64_t> nextClientOrderId_{1};

    // Internal helpers
    void updateOrderFromExecution(Order& order, const ExecutionReport& report);
    void moveToFilled(uint64_t orderId);
    void moveToCancelled(uint64_t orderId);
    void notifyOrderUpdate(const Order& order);
    void notifyExecution(const ExecutionReport& report);
    std::shared_ptr<IExchangeConnector> getConnector(Exchange exchange);
};

/**
 * @brief Order manager builder for fluent configuration
 */
class OrderManagerBuilder {
public:
    OrderManagerBuilder& withPositionManager(std::shared_ptr<PositionManager> pm);
    OrderManagerBuilder& withExchange(Exchange exchange, std::shared_ptr<IExchangeConnector> connector);
    OrderManagerBuilder& withOrderCallback(OrderCallback callback);
    OrderManagerBuilder& withExecutionCallback(ExecutionCallback callback);

    std::unique_ptr<OrderManager> build();

private:
    std::shared_ptr<PositionManager> positionManager_;
    std::vector<std::pair<Exchange, std::shared_ptr<IExchangeConnector>>> exchanges_;
    OrderCallback orderCallback_;
    ExecutionCallback executionCallback_;
};

} // namespace hft::oms
