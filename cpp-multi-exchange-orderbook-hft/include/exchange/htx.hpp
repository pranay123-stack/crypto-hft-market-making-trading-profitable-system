#pragma once

#include "exchange/exchange_base.hpp"
#include "network/websocket_client.hpp"
#include "network/rest_client.hpp"

#include <memory>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <set>
#include <unordered_map>

namespace hft {
namespace exchange {

// HTX (formerly Huobi)-specific configuration
struct HTXConfig {
    bool spot{true};
    bool testnet{false};
    std::string api_key;
    std::string api_secret;

    uint32_t order_rate_limit{10};      // 10 orders per second
    uint32_t request_rate_limit{100};   // 100 requests per minute
    uint32_t recv_window{5000};         // Receive window in ms
    int orderbook_depth{20};            // Default orderbook depth
};

// HTX exchange implementation
class HTXExchange : public ExchangeBase {
public:
    explicit HTXExchange(const ExchangeConfig& config);
    explicit HTXExchange(const HTXConfig& htx_config);
    ~HTXExchange() override;

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

    // ========================================================================
    // HTX-specific Methods
    // ========================================================================
    bool setLeverage(const std::string& symbol, int leverage);
    bool setMarginType(const std::string& symbol, bool isolated);
    double getFundingRate(const std::string& symbol);
    std::string getAccountId();

private:
    // ========================================================================
    // URL Builders
    // ========================================================================
    std::string getSpotRestUrl() const;
    std::string getFuturesRestUrl() const;
    std::string getSpotWsUrl() const;
    std::string getFuturesWsUrl() const;
    std::string getRestUrl() const;
    std::string getWsUrl() const;
    std::string getPrivateWsUrl() const;

    // ========================================================================
    // WebSocket Handlers
    // ========================================================================
    void handleWsOpen(bool is_private);
    void handleWsClose(int code, const std::string& reason, bool is_private);
    void handleWsError(const std::string& error, bool is_private);
    void handlePublicWsMessage(const std::string& message, network::MessageType type);
    void handlePrivateWsMessage(const std::string& message, network::MessageType type);

    // Message handlers
    void handleDepthUpdate(const nlohmann::json& data, const std::string& channel);
    void handleTradeUpdate(const nlohmann::json& data);
    void handleTickerUpdate(const nlohmann::json& data);
    void handleOrderUpdate(const nlohmann::json& data);
    void handleAccountUpdate(const nlohmann::json& data);

    // ========================================================================
    // WebSocket Subscription Management
    // ========================================================================
    void sendSubscribe(const std::string& channel, bool is_private = false);
    void sendUnsubscribe(const std::string& channel, bool is_private = false);
    void sendPong(uint64_t ts, bool is_private = false);
    void authenticatePrivateWs();

    // ========================================================================
    // GZIP Decompression
    // ========================================================================
    std::string decompressGzip(const std::string& compressed);

    // ========================================================================
    // Signature Generation
    // ========================================================================
    std::string sign(const std::string& method, const std::string& host,
                     const std::string& path, const std::string& params);
    std::string generateTimestamp();
    std::string urlEncode(const std::string& value);
    std::string buildSignedQuery(const std::string& method, const std::string& path,
                                  std::unordered_map<std::string, std::string>& params);

    // ========================================================================
    // REST Helpers
    // ========================================================================
    network::HttpResponse signedGet(const std::string& endpoint,
                                    std::unordered_map<std::string, std::string> params = {});
    network::HttpResponse signedPost(const std::string& endpoint,
                                     const nlohmann::json& body);
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
    std::string orderSideToString(OrderSide side);
    std::string orderTypeToString(OrderType type);
    std::string timeInForceToString(TimeInForce tif);
    SymbolInfo parseSymbolInfo(const nlohmann::json& data);

    // ========================================================================
    // Orderbook Management
    // ========================================================================
    void initializeOrderBook(const std::string& symbol, int depth);
    void applyOrderBookSnapshot(const std::string& symbol, const nlohmann::json& data);

    // ========================================================================
    // Private Members
    // ========================================================================
    HTXConfig htx_config_;

    // REST client
    std::shared_ptr<network::RestClient> rest_client_;

    // WebSocket clients
    std::shared_ptr<network::WebSocketClient> public_ws_client_;
    std::shared_ptr<network::WebSocketClient> private_ws_client_;

    // Connection states
    std::atomic<bool> public_ws_connected_{false};
    std::atomic<bool> private_ws_connected_{false};
    std::atomic<bool> private_ws_authenticated_{false};

    // Account ID (needed for some API calls)
    std::string account_id_;
    std::mutex account_id_mutex_;

    // Subscription tracking
    std::set<std::string> public_subscriptions_;
    std::set<std::string> private_subscriptions_;
    std::mutex subscription_mutex_;

    // Orderbook sequence tracking
    std::unordered_map<std::string, uint64_t> orderbook_seq_;
    std::mutex orderbook_seq_mutex_;

    // Request ID counter
    std::atomic<uint64_t> next_req_id_{1};
};

// Factory functions
std::shared_ptr<HTXExchange> createHTXExchange(const ExchangeConfig& config);
std::shared_ptr<HTXExchange> createHTXExchange(const HTXConfig& config);

} // namespace exchange
} // namespace hft
