#pragma once

#include "exchange/exchange_base.hpp"
#include "network/websocket_client.hpp"
#include "network/rest_client.hpp"

#include <memory>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace hft {
namespace exchange {

// Coinbase-specific configuration
struct CoinbaseConfig {
    bool testnet{false};          // Use sandbox environment
    std::string api_key;
    std::string api_secret;
    std::string passphrase;       // Required for legacy API (optional for new API)

    uint32_t order_rate_limit{10};      // 10 requests per second
    uint32_t request_rate_limit{600};   // 600 requests per minute
    uint32_t recv_window{5000};         // Receive window for signature

    // Advanced Trade API vs Legacy Exchange API
    bool use_advanced_api{true};  // Use newer Advanced Trade API (recommended)

    // WebSocket settings
    int orderbook_depth{50};      // Default orderbook depth
};

// Coinbase Advanced Trade API implementation
class CoinbaseExchange : public ExchangeBase {
public:
    explicit CoinbaseExchange(const ExchangeConfig& config);
    explicit CoinbaseExchange(const CoinbaseConfig& coinbase_config);
    ~CoinbaseExchange() override;

    // ========================================================================
    // Connection Management
    // ========================================================================

    bool connect() override;
    void disconnect() override;
    bool isConnected() const override;
    ConnectionStatus connectionStatus() const override;

    // ========================================================================
    // Market Data Subscriptions
    // ========================================================================

    bool subscribeOrderBook(const std::string& symbol, int depth = 20) override;
    bool unsubscribeOrderBook(const std::string& symbol) override;
    bool subscribeTrades(const std::string& symbol) override;
    bool unsubscribeTrades(const std::string& symbol) override;
    bool subscribeTicker(const std::string& symbol) override;
    bool unsubscribeTicker(const std::string& symbol) override;
    bool subscribeUserData() override;
    bool unsubscribeUserData() override;

    // ========================================================================
    // Order Management
    // ========================================================================

    std::optional<Order> placeOrder(const OrderRequest& request) override;
    std::optional<Order> cancelOrder(const std::string& symbol, const std::string& order_id) override;
    std::optional<Order> cancelOrder(const std::string& symbol, const std::string& order_id,
                                     const std::string& client_order_id) override;
    bool cancelAllOrders(const std::string& symbol) override;
    std::optional<Order> getOrder(const std::string& symbol, const std::string& order_id) override;
    std::vector<Order> getOpenOrders(const std::string& symbol = "") override;
    std::vector<Order> getOrderHistory(const std::string& symbol, uint64_t start_time = 0,
                                       uint64_t end_time = 0, int limit = 100) override;

    // ========================================================================
    // Account Information
    // ========================================================================

    std::optional<Account> getAccount() override;
    std::optional<Balance> getBalance(const std::string& asset) override;
    std::vector<Position> getPositions(const std::string& symbol = "") override;

    // ========================================================================
    // Market Information
    // ========================================================================

    std::vector<SymbolInfo> getSymbols() override;
    std::optional<SymbolInfo> getSymbolInfo(const std::string& symbol) override;
    std::optional<OrderBook> getOrderBookSnapshot(const std::string& symbol, int depth = 20) override;
    std::vector<Trade> getRecentTrades(const std::string& symbol, int limit = 100) override;
    std::optional<Ticker> getTicker(const std::string& symbol) override;

    // ========================================================================
    // Utility Methods
    // ========================================================================

    uint64_t getServerTime() override;
    std::string formatSymbol(const std::string& base, const std::string& quote) const override;
    std::pair<std::string, std::string> parseSymbol(const std::string& symbol) const override;

private:
    // ========================================================================
    // URL Builders
    // ========================================================================

    std::string getRestUrl() const;
    std::string getWsUrl() const;
    std::string getWsUserUrl() const;  // Separate URL for user order data

    // ========================================================================
    // WebSocket Handlers
    // ========================================================================

    void handleWsOpen();
    void handleWsClose(int code, const std::string& reason);
    void handleWsError(const std::string& error);
    void handleWsMessage(const std::string& message, network::MessageType type);

    void handleOrderBookSnapshot(const nlohmann::json& data);
    void handleOrderBookUpdate(const nlohmann::json& data);
    void handleTradeUpdate(const nlohmann::json& data);
    void handleTickerUpdate(const nlohmann::json& data);
    void handleUserUpdate(const nlohmann::json& data);
    void handleOrderUpdate(const nlohmann::json& data);
    void handleMatchUpdate(const nlohmann::json& data);

    // ========================================================================
    // Subscription Management
    // ========================================================================

    void sendSubscribe(const std::vector<std::string>& channels,
                       const std::vector<std::string>& product_ids);
    void sendUnsubscribe(const std::vector<std::string>& channels,
                         const std::vector<std::string>& product_ids);
    nlohmann::json buildAuthMessage();

    // ========================================================================
    // Orderbook Management
    // ========================================================================

    void initializeOrderBook(const std::string& symbol, int depth);
    void applyOrderBookDelta(const std::string& symbol, const nlohmann::json& changes);

    // ========================================================================
    // Signature Generation
    // ========================================================================

    std::string generateTimestamp() const;
    std::string sign(const std::string& timestamp, const std::string& method,
                     const std::string& request_path, const std::string& body = "") const;
    void addAuthHeaders(network::HttpRequest& request, const std::string& method,
                        const std::string& request_path, const std::string& body = "");
    std::string generateJwt() const;  // Generate JWT for WebSocket authentication

    // ========================================================================
    // REST Helpers
    // ========================================================================

    network::HttpResponse signedGet(const std::string& endpoint,
                                    const std::unordered_map<std::string, std::string>& params = {});
    network::HttpResponse signedPost(const std::string& endpoint,
                                     const nlohmann::json& body = {});
    network::HttpResponse signedDelete(const std::string& endpoint,
                                       const nlohmann::json& body = {});
    network::HttpResponse publicGet(const std::string& endpoint,
                                    const std::unordered_map<std::string, std::string>& params = {});

    // ========================================================================
    // Data Converters
    // ========================================================================

    Order parseOrder(const nlohmann::json& data);
    OrderStatus parseOrderStatus(const std::string& status);
    OrderSide parseOrderSide(const std::string& side);
    OrderType parseOrderType(const std::string& type);
    TimeInForce parseTimeInForce(const std::string& tif);
    SymbolInfo parseSymbolInfo(const nlohmann::json& data);

    std::string orderSideToString(OrderSide side) const;
    std::string orderTypeToString(OrderType type) const;
    std::string timeInForceToString(TimeInForce tif) const;

    // ========================================================================
    // Member Variables
    // ========================================================================

    CoinbaseConfig coinbase_config_;

    // Network clients
    std::shared_ptr<network::WebSocketClient> ws_client_;
    std::shared_ptr<network::RestClient> rest_client_;

    // Connection state
    std::atomic<bool> ws_connected_{false};
    std::atomic<bool> user_subscribed_{false};

    // Subscription tracking
    std::unordered_set<std::string> subscribed_channels_;
    std::unordered_set<std::string> subscribed_products_;
    std::mutex subscription_mutex_;

    // Orderbook state
    std::unordered_map<std::string, bool> orderbook_initialized_;
    std::unordered_map<std::string, uint64_t> orderbook_sequence_;
    std::mutex orderbook_init_mutex_;

    // Sequence number tracking for gap detection (per channel:product)
    std::unordered_map<std::string, uint64_t> channel_sequences_;
    std::mutex sequence_mutex_;

    // Request ID counter
    std::atomic<uint64_t> next_request_id_{1};
};

// Factory functions
std::shared_ptr<CoinbaseExchange> createCoinbaseExchange(const ExchangeConfig& config);
std::shared_ptr<CoinbaseExchange> createCoinbaseExchange(const CoinbaseConfig& config);

} // namespace exchange
} // namespace hft
