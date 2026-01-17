#pragma once

#include "core/types.hpp"
#include <memory>
#include <unordered_map>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>

namespace hft {

// ============================================================================
// Exchange Configuration
// ============================================================================

struct ExchangeConfig {
    ExchangeId id;
    std::string name;
    std::string rest_url;
    std::string ws_url;
    std::string api_key;
    std::string api_secret;
    std::string passphrase;

    uint32_t connect_timeout_ms = 5000;
    uint32_t read_timeout_ms = 1000;
    uint32_t max_requests_per_second = 10;
    uint32_t max_orders_per_second = 10;

    bool enabled = true;
    int priority = 0;  // Higher = preferred for execution
};

// ============================================================================
// Exchange Callbacks
// ============================================================================

struct ExchangeCallbacks {
    std::function<void(ExchangeId, const Tick&)> on_tick;
    std::function<void(ExchangeId, const Order&)> on_order_update;
    std::function<void(ExchangeId, const Trade&)> on_trade;
    std::function<void(ExchangeId, const std::string&)> on_error;
    std::function<void(ExchangeId)> on_connected;
    std::function<void(ExchangeId)> on_disconnected;
};

// ============================================================================
// Exchange Client Interface
// ============================================================================

class IExchangeClient {
public:
    virtual ~IExchangeClient() = default;

    // Identity
    [[nodiscard]] virtual ExchangeId id() const = 0;
    [[nodiscard]] virtual const std::string& name() const = 0;

    // Connection
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    [[nodiscard]] virtual bool is_connected() const = 0;

    // Market data
    virtual bool subscribe_ticker(const Symbol& symbol) = 0;
    virtual bool subscribe_orderbook(const Symbol& symbol, int depth = 20) = 0;
    virtual bool subscribe_trades(const Symbol& symbol) = 0;
    virtual bool unsubscribe(const Symbol& symbol) = 0;

    // Orders
    virtual OrderId send_order(const Order& order) = 0;
    virtual bool cancel_order(OrderId order_id, const Symbol& symbol) = 0;
    virtual bool cancel_all_orders(const Symbol& symbol) = 0;

    // Account
    virtual double get_balance(const std::string& asset) = 0;
    virtual std::vector<Order> get_open_orders(const Symbol& symbol) = 0;

    // Callbacks
    virtual void set_callbacks(const ExchangeCallbacks& callbacks) = 0;

    // Latency tracking
    [[nodiscard]] virtual Timestamp get_latency_ns() const = 0;
    [[nodiscard]] virtual Timestamp server_time() const = 0;
};

// ============================================================================
// Exchange Manager - Manages Multiple Exchange Connections
// ============================================================================

class ExchangeManager {
public:
    ExchangeManager();
    ~ExchangeManager();

    // Exchange registration
    void add_exchange(const ExchangeConfig& config);
    void remove_exchange(ExchangeId id);
    IExchangeClient* get_exchange(ExchangeId id);
    const IExchangeClient* get_exchange(ExchangeId id) const;

    // Batch operations
    void connect_all();
    void disconnect_all();
    bool subscribe_all(const Symbol& symbol);
    bool unsubscribe_all(const Symbol& symbol);

    // Order routing
    OrderId send_order(const Order& order);  // Uses order.exchange
    OrderId send_order_best_price(const Order& order);  // Routes to best price
    bool cancel_order(ExchangeId exchange, OrderId order_id, const Symbol& symbol);
    void cancel_all_orders(const Symbol& symbol);

    // Callbacks
    void set_callbacks(const ExchangeCallbacks& callbacks);

    // State queries
    [[nodiscard]] std::vector<ExchangeId> connected_exchanges() const;
    [[nodiscard]] size_t exchange_count() const;
    [[nodiscard]] bool all_connected() const;

    // Latency
    [[nodiscard]] Timestamp get_latency(ExchangeId id) const;
    [[nodiscard]] ExchangeId fastest_exchange() const;

    // Balance aggregation
    [[nodiscard]] double get_total_balance(const std::string& asset) const;
    [[nodiscard]] std::unordered_map<ExchangeId, double> get_balances(const std::string& asset) const;

private:
    std::unique_ptr<IExchangeClient> create_client(const ExchangeConfig& config);
    void setup_client_callbacks(IExchangeClient* client);

    std::unordered_map<ExchangeId, std::unique_ptr<IExchangeClient>> clients_;
    std::unordered_map<ExchangeId, ExchangeConfig> configs_;
    ExchangeCallbacks callbacks_;
    mutable std::mutex mutex_;
};

// ============================================================================
// Exchange Router - Smart Order Routing
// ============================================================================

class ExchangeRouter {
public:
    enum class RoutingStrategy {
        BEST_PRICE,      // Route to best bid/ask
        LOWEST_LATENCY,  // Route to fastest exchange
        ROUND_ROBIN,     // Distribute evenly
        PRIORITY,        // Use configured priority
        SPLIT_ORDER      // Split across exchanges
    };

    explicit ExchangeRouter(ExchangeManager& manager);

    void set_strategy(RoutingStrategy strategy);

    // Route order based on strategy
    ExchangeId select_exchange(
        const Order& order,
        const std::unordered_map<ExchangeId, ExchangeQuote>& quotes
    );

    // Split order across exchanges
    std::vector<std::pair<ExchangeId, Quantity>> split_order(
        const Order& order,
        const std::unordered_map<ExchangeId, ExchangeQuote>& quotes
    );

private:
    ExchangeManager& manager_;
    RoutingStrategy strategy_ = RoutingStrategy::BEST_PRICE;
    std::atomic<size_t> round_robin_idx_{0};
};

// ============================================================================
// Exchange Health Monitor
// ============================================================================

class ExchangeHealthMonitor {
public:
    struct ExchangeHealth {
        ExchangeId id;
        bool connected;
        Timestamp latency_ns;
        uint32_t error_count;
        uint32_t timeout_count;
        Timestamp last_message;
        double uptime_percent;
    };

    explicit ExchangeHealthMonitor(ExchangeManager& manager);

    void update();
    [[nodiscard]] ExchangeHealth get_health(ExchangeId id) const;
    [[nodiscard]] std::vector<ExchangeHealth> get_all_health() const;
    [[nodiscard]] bool is_healthy(ExchangeId id) const;

    void set_latency_threshold(Timestamp threshold_ns);
    void set_error_threshold(uint32_t threshold);

private:
    ExchangeManager& manager_;
    std::unordered_map<ExchangeId, ExchangeHealth> health_;
    Timestamp latency_threshold_ = 100000000;  // 100ms
    uint32_t error_threshold_ = 10;
    mutable std::mutex mutex_;
};

} // namespace hft
