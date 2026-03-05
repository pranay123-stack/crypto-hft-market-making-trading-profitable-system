#pragma once

#include "network/websocket_client.hpp"
#include "network/rest_client.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <optional>
#include <chrono>

namespace hft {
namespace exchange {

// ============================================================================
// Enums
// ============================================================================

enum class ExchangeType {
    Spot,
    Futures,
    Perpetual,
    Options,
    Margin
};

enum class OrderSide {
    Buy,
    Sell
};

enum class OrderType {
    Market,
    Limit,
    StopMarket,
    StopLimit,
    TakeProfitMarket,
    TakeProfitLimit,
    TrailingStop,
    PostOnly,
    FillOrKill,
    ImmediateOrCancel
};

enum class OrderStatus {
    Pending,         // Order created locally, not yet sent
    New,             // Order accepted by exchange
    PartiallyFilled,
    Filled,
    Cancelled,
    Rejected,
    Expired,
    PendingCancel,
    Failed
};

enum class TimeInForce {
    GTC,  // Good Till Cancel
    IOC,  // Immediate or Cancel
    FOK,  // Fill or Kill
    GTD,  // Good Till Date
    PostOnly
};

enum class ConnectionStatus {
    Disconnected,
    Connecting,
    Connected,
    Reconnecting,
    Failed
};

// ============================================================================
// Structures
// ============================================================================

// Price level in orderbook
struct PriceLevel {
    double price{0.0};
    double quantity{0.0};
    uint32_t order_count{0};  // Number of orders at this level (if available)

    bool operator<(const PriceLevel& other) const { return price < other.price; }
    bool operator>(const PriceLevel& other) const { return price > other.price; }
};

// Orderbook snapshot
struct OrderBook {
    std::string symbol;
    std::string exchange;
    uint64_t sequence{0};            // Exchange sequence number
    uint64_t timestamp{0};           // Exchange timestamp (ms)
    uint64_t local_timestamp{0};     // Local receive timestamp (ns)

    std::vector<PriceLevel> bids;    // Sorted by price descending
    std::vector<PriceLevel> asks;    // Sorted by price ascending

    // Quick accessors
    double bestBid() const { return bids.empty() ? 0.0 : bids[0].price; }
    double bestAsk() const { return asks.empty() ? 0.0 : asks[0].price; }
    double bestBidQty() const { return bids.empty() ? 0.0 : bids[0].quantity; }
    double bestAskQty() const { return asks.empty() ? 0.0 : asks[0].quantity; }
    double midPrice() const {
        if (bids.empty() || asks.empty()) return 0.0;
        return (bids[0].price + asks[0].price) / 2.0;
    }
    double spread() const {
        if (bids.empty() || asks.empty()) return 0.0;
        return asks[0].price - bids[0].price;
    }
    double spreadBps() const {
        double mid = midPrice();
        if (mid == 0.0) return 0.0;
        return (spread() / mid) * 10000.0;
    }
};

// Trade (public trade data)
struct Trade {
    std::string symbol;
    std::string exchange;
    std::string trade_id;
    double price{0.0};
    double quantity{0.0};
    OrderSide side{OrderSide::Buy};
    uint64_t timestamp{0};           // Exchange timestamp (ms)
    uint64_t local_timestamp{0};     // Local receive timestamp (ns)
    bool is_maker{false};
};

// Ticker
struct Ticker {
    std::string symbol;
    std::string exchange;
    double bid{0.0};
    double ask{0.0};
    double bid_qty{0.0};
    double ask_qty{0.0};
    double last{0.0};
    double volume_24h{0.0};
    double volume_quote_24h{0.0};
    double high_24h{0.0};
    double low_24h{0.0};
    double change_24h{0.0};
    double change_24h_pct{0.0};
    uint64_t timestamp{0};
};

// Order request
struct OrderRequest {
    std::string symbol;
    OrderSide side{OrderSide::Buy};
    OrderType type{OrderType::Limit};
    TimeInForce time_in_force{TimeInForce::GTC};
    double quantity{0.0};
    double price{0.0};              // For limit orders
    double stop_price{0.0};         // For stop orders
    double trailing_delta{0.0};     // For trailing stop
    std::string client_order_id;
    bool reduce_only{false};
    bool post_only{false};

    // Extra parameters for specific exchanges
    nlohmann::json extra_params;
};

// Order (response/status)
struct Order {
    std::string exchange;
    std::string symbol;
    std::string order_id;
    std::string client_order_id;
    OrderSide side{OrderSide::Buy};
    OrderType type{OrderType::Limit};
    OrderStatus status{OrderStatus::Pending};
    TimeInForce time_in_force{TimeInForce::GTC};
    double quantity{0.0};
    double filled_quantity{0.0};
    double remaining_quantity{0.0};
    double price{0.0};
    double average_price{0.0};
    double stop_price{0.0};
    double commission{0.0};
    std::string commission_asset;
    uint64_t create_time{0};
    uint64_t update_time{0};
    bool reduce_only{false};
    bool is_working{false};

    nlohmann::json raw;  // Raw exchange response
};

// Position (for futures)
struct Position {
    std::string exchange;
    std::string symbol;
    OrderSide side{OrderSide::Buy};  // Long or Short
    double quantity{0.0};
    double entry_price{0.0};
    double mark_price{0.0};
    double liquidation_price{0.0};
    double unrealized_pnl{0.0};
    double realized_pnl{0.0};
    double leverage{1.0};
    double margin{0.0};
    double maintenance_margin{0.0};
    uint64_t update_time{0};
};

// Balance for a single asset
struct Balance {
    std::string asset;
    double free{0.0};
    double locked{0.0};
    double total() const { return free + locked; }
};

// Account information
struct Account {
    std::string exchange;
    std::unordered_map<std::string, Balance> balances;
    std::vector<Position> positions;  // For futures
    double total_margin{0.0};
    double available_margin{0.0};
    double total_unrealized_pnl{0.0};
    uint64_t update_time{0};
};

// Symbol information
struct SymbolInfo {
    std::string symbol;
    std::string base_asset;
    std::string quote_asset;
    ExchangeType type{ExchangeType::Spot};
    double min_qty{0.0};
    double max_qty{0.0};
    double step_size{0.0};
    double min_price{0.0};
    double max_price{0.0};
    double tick_size{0.0};
    double min_notional{0.0};
    int price_precision{8};
    int qty_precision{8};
    bool trading_enabled{true};
};

// Exchange configuration
struct ExchangeConfig {
    std::string name;
    std::string api_key;
    std::string api_secret;
    std::string passphrase;  // For OKX, etc.
    ExchangeType type{ExchangeType::Spot};
    bool testnet{false};
    bool enable_websocket{true};
    bool enable_rest{true};

    // Network settings
    uint32_t ws_reconnect_delay_ms{1000};
    uint32_t ws_max_reconnect_attempts{0};  // 0 = unlimited
    uint32_t rest_timeout_ms{10000};
    uint32_t rest_max_retries{3};

    // Rate limits
    uint32_t orders_per_second{10};
    uint32_t requests_per_minute{1200};

    // Extra parameters
    nlohmann::json extra_params;
};

// ============================================================================
// Callback Types
// ============================================================================

using OrderBookCallback = std::function<void(const OrderBook&)>;
using TradeCallback = std::function<void(const Trade&)>;
using TickerCallback = std::function<void(const Ticker&)>;
using OrderCallback = std::function<void(const Order&)>;
using PositionCallback = std::function<void(const Position&)>;
using BalanceCallback = std::function<void(const Balance&)>;
using ConnectionCallback = std::function<void(ConnectionStatus)>;
using ErrorCallback = std::function<void(const std::string&)>;

// ============================================================================
// Abstract Exchange Base Class
// ============================================================================

class ExchangeBase : public std::enable_shared_from_this<ExchangeBase> {
public:
    explicit ExchangeBase(const ExchangeConfig& config);
    virtual ~ExchangeBase();

    // Non-copyable
    ExchangeBase(const ExchangeBase&) = delete;
    ExchangeBase& operator=(const ExchangeBase&) = delete;

    // ========================================================================
    // Connection Management
    // ========================================================================

    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;
    virtual ConnectionStatus connectionStatus() const = 0;

    // ========================================================================
    // Market Data Subscriptions
    // ========================================================================

    virtual bool subscribeOrderBook(const std::string& symbol, int depth = 20) = 0;
    virtual bool unsubscribeOrderBook(const std::string& symbol) = 0;

    virtual bool subscribeTrades(const std::string& symbol) = 0;
    virtual bool unsubscribeTrades(const std::string& symbol) = 0;

    virtual bool subscribeTicker(const std::string& symbol) = 0;
    virtual bool unsubscribeTicker(const std::string& symbol) = 0;

    // Subscribe to user data (orders, positions, balances)
    virtual bool subscribeUserData() = 0;
    virtual bool unsubscribeUserData() = 0;

    // ========================================================================
    // Order Management
    // ========================================================================

    virtual std::optional<Order> placeOrder(const OrderRequest& request) = 0;
    virtual std::optional<Order> cancelOrder(const std::string& symbol,
                                             const std::string& order_id) = 0;
    virtual std::optional<Order> cancelOrder(const std::string& symbol,
                                             const std::string& order_id,
                                             const std::string& client_order_id) = 0;
    virtual bool cancelAllOrders(const std::string& symbol) = 0;

    virtual std::optional<Order> getOrder(const std::string& symbol,
                                          const std::string& order_id) = 0;
    virtual std::vector<Order> getOpenOrders(const std::string& symbol = "") = 0;
    virtual std::vector<Order> getOrderHistory(const std::string& symbol,
                                               uint64_t start_time = 0,
                                               uint64_t end_time = 0,
                                               int limit = 100) = 0;

    // ========================================================================
    // Account Information
    // ========================================================================

    virtual std::optional<Account> getAccount() = 0;
    virtual std::optional<Balance> getBalance(const std::string& asset) = 0;
    virtual std::vector<Position> getPositions(const std::string& symbol = "") = 0;

    // ========================================================================
    // Market Information
    // ========================================================================

    virtual std::vector<SymbolInfo> getSymbols() = 0;
    virtual std::optional<SymbolInfo> getSymbolInfo(const std::string& symbol) = 0;
    virtual std::optional<OrderBook> getOrderBookSnapshot(const std::string& symbol,
                                                          int depth = 20) = 0;
    virtual std::vector<Trade> getRecentTrades(const std::string& symbol,
                                               int limit = 100) = 0;
    virtual std::optional<Ticker> getTicker(const std::string& symbol) = 0;

    // ========================================================================
    // Utility Methods
    // ========================================================================

    // Get server time
    virtual uint64_t getServerTime() = 0;

    // Symbol formatting (exchange-specific)
    virtual std::string formatSymbol(const std::string& base,
                                    const std::string& quote) const = 0;
    virtual std::pair<std::string, std::string> parseSymbol(
        const std::string& symbol) const = 0;

    // Round price/quantity to valid tick/step size
    virtual double roundPrice(const std::string& symbol, double price) const;
    virtual double roundQuantity(const std::string& symbol, double qty) const;

    // ========================================================================
    // Callback Setters
    // ========================================================================

    void setOrderBookCallback(OrderBookCallback callback) {
        orderbook_callback_ = std::move(callback);
    }
    void setTradeCallback(TradeCallback callback) {
        trade_callback_ = std::move(callback);
    }
    void setTickerCallback(TickerCallback callback) {
        ticker_callback_ = std::move(callback);
    }
    void setOrderCallback(OrderCallback callback) {
        order_callback_ = std::move(callback);
    }
    void setPositionCallback(PositionCallback callback) {
        position_callback_ = std::move(callback);
    }
    void setBalanceCallback(BalanceCallback callback) {
        balance_callback_ = std::move(callback);
    }
    void setConnectionCallback(ConnectionCallback callback) {
        connection_callback_ = std::move(callback);
    }
    void setErrorCallback(ErrorCallback callback) {
        error_callback_ = std::move(callback);
    }

    // ========================================================================
    // Accessors
    // ========================================================================

    const ExchangeConfig& config() const { return config_; }
    const std::string& name() const { return config_.name; }
    ExchangeType type() const { return config_.type; }
    bool isTestnet() const { return config_.testnet; }

    // Get cached orderbook
    std::optional<OrderBook> getCachedOrderBook(const std::string& symbol) const;

    // Get all cached orderbooks
    std::unordered_map<std::string, OrderBook> getAllCachedOrderBooks() const;

protected:
    // ========================================================================
    // Helper Methods for Subclasses
    // ========================================================================

    // Invoke callbacks safely
    void onOrderBook(const OrderBook& ob);
    void onTrade(const Trade& trade);
    void onTicker(const Ticker& ticker);
    void onOrder(const Order& order);
    void onPosition(const Position& position);
    void onBalance(const Balance& balance);
    void onConnectionStatus(ConnectionStatus status);
    void onError(const std::string& error);

    // Update local orderbook cache
    void updateOrderBookCache(const std::string& symbol, const OrderBook& ob);
    void clearOrderBookCache(const std::string& symbol);

    // Update symbol info cache
    void updateSymbolInfo(const SymbolInfo& info);

    // Generate client order ID
    std::string generateClientOrderId() const;

    // Current timestamp in milliseconds
    uint64_t currentTimeMs() const;

    // Current timestamp in nanoseconds
    uint64_t currentTimeNs() const;

    // Configuration
    ExchangeConfig config_;

    // Callbacks
    OrderBookCallback orderbook_callback_;
    TradeCallback trade_callback_;
    TickerCallback ticker_callback_;
    OrderCallback order_callback_;
    PositionCallback position_callback_;
    BalanceCallback balance_callback_;
    ConnectionCallback connection_callback_;
    ErrorCallback error_callback_;

    // Cached data
    std::unordered_map<std::string, OrderBook> orderbook_cache_;
    mutable std::mutex orderbook_mutex_;

    std::unordered_map<std::string, SymbolInfo> symbol_info_cache_;
    mutable std::mutex symbol_info_mutex_;

    // Connection status
    std::atomic<ConnectionStatus> connection_status_{ConnectionStatus::Disconnected};
};

// Type alias for shared pointer
using ExchangePtr = std::shared_ptr<ExchangeBase>;

} // namespace exchange
} // namespace hft
