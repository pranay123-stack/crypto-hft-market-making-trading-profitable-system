#pragma once

#include "core/types.hpp"
#include "core/lock_free_queue.hpp"
#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <thread>

namespace hft {

// ============================================================================
// Exchange Configuration
// ============================================================================

struct ExchangeConfig {
    std::string name;
    std::string rest_url;
    std::string ws_url;
    std::string api_key;
    std::string api_secret;
    std::string passphrase;  // For exchanges that require it

    // Connection settings
    uint32_t connect_timeout_ms = 5000;
    uint32_t read_timeout_ms = 1000;
    uint32_t write_timeout_ms = 1000;
    uint32_t heartbeat_interval_ms = 30000;

    // Rate limits
    uint32_t max_requests_per_second = 10;
    uint32_t max_orders_per_second = 10;
};

// ============================================================================
// Exchange Callbacks
// ============================================================================

struct ExchangeCallbacks {
    std::function<void(const Tick&)> on_tick;
    std::function<void(const Order&)> on_order_update;
    std::function<void(const Trade&)> on_trade;
    std::function<void(const std::string&)> on_error;
    std::function<void()> on_connected;
    std::function<void()> on_disconnected;
};

// ============================================================================
// Order Request/Response
// ============================================================================

struct OrderRequest {
    Symbol symbol;
    Side side;
    OrderType type;
    TimeInForce tif;
    Price price;
    Quantity quantity;
    OrderId client_order_id;
};

struct OrderResponse {
    bool success;
    OrderId exchange_order_id;
    OrderId client_order_id;
    std::string error_message;
    Timestamp exchange_timestamp;
};

struct CancelRequest {
    Symbol symbol;
    OrderId exchange_order_id;
    OrderId client_order_id;
};

struct CancelResponse {
    bool success;
    OrderId exchange_order_id;
    std::string error_message;
};

// ============================================================================
// Exchange Client Interface
// ============================================================================

class ExchangeClient {
public:
    virtual ~ExchangeClient() = default;

    // Connection management
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    [[nodiscard]] virtual bool is_connected() const = 0;

    // Market data subscription
    virtual bool subscribe_ticker(const Symbol& symbol) = 0;
    virtual bool subscribe_orderbook(const Symbol& symbol, int depth = 20) = 0;
    virtual bool subscribe_trades(const Symbol& symbol) = 0;
    virtual bool unsubscribe(const Symbol& symbol) = 0;

    // Order management
    virtual OrderResponse send_order(const OrderRequest& request) = 0;
    virtual CancelResponse cancel_order(const CancelRequest& request) = 0;
    virtual CancelResponse cancel_all_orders(const Symbol& symbol) = 0;

    // Account queries
    virtual double get_balance(const std::string& asset) = 0;
    virtual std::vector<Order> get_open_orders(const Symbol& symbol) = 0;

    // Callbacks
    virtual void set_callbacks(const ExchangeCallbacks& callbacks) = 0;

    // Exchange info
    [[nodiscard]] virtual const std::string& name() const = 0;
    [[nodiscard]] virtual Timestamp server_time() const = 0;
};

// ============================================================================
// WebSocket Message Types
// ============================================================================

enum class WSMessageType : uint8_t {
    UNKNOWN = 0,
    TICKER,
    ORDERBOOK_SNAPSHOT,
    ORDERBOOK_UPDATE,
    TRADE,
    ORDER_UPDATE,
    ACCOUNT_UPDATE,
    PING,
    PONG,
    ERROR
};

struct WSMessage {
    WSMessageType type;
    std::string raw_data;
    Timestamp local_timestamp;
    Timestamp exchange_timestamp;
};

// ============================================================================
// Base WebSocket Client
// ============================================================================

class WebSocketClient {
public:
    using MessageCallback = std::function<void(const std::string&)>;
    using ConnectCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const std::string&)>;

    virtual ~WebSocketClient() = default;

    virtual bool connect(const std::string& url) = 0;
    virtual void disconnect() = 0;
    [[nodiscard]] virtual bool is_connected() const = 0;

    virtual bool send(const std::string& message) = 0;

    virtual void set_message_callback(MessageCallback cb) = 0;
    virtual void set_connect_callback(ConnectCallback cb) = 0;
    virtual void set_disconnect_callback(ConnectCallback cb) = 0;
    virtual void set_error_callback(ErrorCallback cb) = 0;

    // Event loop
    virtual void run() = 0;
    virtual void stop() = 0;
};

// ============================================================================
// REST Client
// ============================================================================

class RESTClient {
public:
    struct Response {
        int status_code;
        std::string body;
        std::string error;
        Timestamp latency_us;
    };

    virtual ~RESTClient() = default;

    virtual Response get(const std::string& endpoint,
                        const std::string& query_params = "") = 0;
    virtual Response post(const std::string& endpoint,
                         const std::string& body) = 0;
    virtual Response del(const std::string& endpoint,
                        const std::string& body = "") = 0;

    virtual void set_auth(const std::string& api_key,
                         const std::string& api_secret,
                         const std::string& passphrase = "") = 0;
};

// ============================================================================
// Factory
// ============================================================================

class ExchangeClientFactory {
public:
    static std::unique_ptr<ExchangeClient> create(
        const std::string& exchange_name,
        const ExchangeConfig& config
    );
};

} // namespace hft
