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

// Gate.io-specific configuration
struct GateIOConfig {
    bool spot{true};
    bool testnet{false};
    std::string api_key;
    std::string api_secret;

    uint32_t order_rate_limit{10};       // 10 requests per second
    uint32_t request_rate_limit{900};    // 900 requests per minute
    uint32_t recv_window{5000};          // Receive window in ms
    int orderbook_depth{20};
    int orderbook_update_speed{100};     // 100ms update speed
};

// Gate.io exchange implementation
class GateIOExchange : public ExchangeBase {
public:
    explicit GateIOExchange(const ExchangeConfig& config);
    explicit GateIOExchange(const GateIOConfig& gateio_config);
    ~GateIOExchange() override;

    // Connection Management
    bool connect() override;
    void disconnect() override;
    bool isConnected() const override;
    ConnectionStatus connectionStatus() const override;

    // Market Data Subscriptions
    bool subscribeOrderBook(const std::string& symbol, int depth = 20) override;
    bool unsubscribeOrderBook(const std::string& symbol) override;
    bool subscribeTrades(const std::string& symbol) override;
    bool unsubscribeTrades(const std::string& symbol) override;
    bool subscribeTicker(const std::string& symbol) override;
    bool unsubscribeTicker(const std::string& symbol) override;
    bool subscribeUserData() override;
    bool unsubscribeUserData() override;

    // Order Management
    std::optional<Order> placeOrder(const OrderRequest& request) override;
    std::optional<Order> cancelOrder(const std::string& symbol, const std::string& order_id) override;
    std::optional<Order> cancelOrder(const std::string& symbol, const std::string& order_id,
                                     const std::string& client_order_id) override;
    bool cancelAllOrders(const std::string& symbol) override;
    std::optional<Order> getOrder(const std::string& symbol, const std::string& order_id) override;
    std::vector<Order> getOpenOrders(const std::string& symbol = "") override;
    std::vector<Order> getOrderHistory(const std::string& symbol, uint64_t start_time = 0,
                                       uint64_t end_time = 0, int limit = 100) override;

    // Account Information
    std::optional<Account> getAccount() override;
    std::optional<Balance> getBalance(const std::string& asset) override;
    std::vector<Position> getPositions(const std::string& symbol = "") override;

    // Market Information
    std::vector<SymbolInfo> getSymbols() override;
    std::optional<SymbolInfo> getSymbolInfo(const std::string& symbol) override;
    std::optional<OrderBook> getOrderBookSnapshot(const std::string& symbol, int depth = 20) override;
    std::vector<Trade> getRecentTrades(const std::string& symbol, int limit = 100) override;
    std::optional<Ticker> getTicker(const std::string& symbol) override;

    // Utility
    uint64_t getServerTime() override;
    std::string formatSymbol(const std::string& base, const std::string& quote) const override;
    std::pair<std::string, std::string> parseSymbol(const std::string& symbol) const override;

    // Gate.io-specific methods
    bool setLeverage(const std::string& symbol, int leverage);
    double getFundingRate(const std::string& symbol);

private:
    // URL Builders
    std::string getSpotRestUrl() const;
    std::string getFuturesRestUrl() const;
    std::string getSpotWsUrl() const;
    std::string getFuturesWsUrl() const;
    std::string getRestUrl() const;
    std::string getWsUrl() const;

    // WebSocket Handlers
    void handleWsOpen(bool is_private);
    void handleWsClose(int code, const std::string& reason, bool is_private);
    void handleWsError(const std::string& error, bool is_private);
    void handlePublicWsMessage(const std::string& message, network::MessageType type);
    void handlePrivateWsMessage(const std::string& message, network::MessageType type);

    // Market Data Handlers
    void handleOrderBookSnapshot(const nlohmann::json& json);
    void handleOrderBookDelta(const nlohmann::json& json);
    void handleTradeUpdate(const nlohmann::json& json);
    void handleTickerUpdate(const nlohmann::json& json);

    // User Data Handlers
    void handleOrderUpdate(const nlohmann::json& data);
    void handlePositionUpdate(const nlohmann::json& data);
    void handleBalanceUpdate(const nlohmann::json& data);

    // Signature Generation (HMAC-SHA512)
    std::string sign(const std::string& method, const std::string& path,
                     const std::string& query_string, const std::string& body,
                     const std::string& timestamp);
    std::string generateWsSignature(const std::string& channel, const std::string& event,
                                     const std::string& timestamp);

    // Subscription Management
    void sendSubscribe(const std::vector<nlohmann::json>& channels, bool is_private);
    void sendUnsubscribe(const std::vector<nlohmann::json>& channels, bool is_private);
    void authenticatePrivateWs();

    // REST Helpers
    network::HttpResponse signedGet(const std::string& endpoint,
                                    const std::unordered_map<std::string, std::string>& params = {});
    network::HttpResponse signedPost(const std::string& endpoint,
                                     const nlohmann::json& body);
    network::HttpResponse signedDelete(const std::string& endpoint,
                                       const std::unordered_map<std::string, std::string>& params = {});
    network::HttpResponse publicGet(const std::string& endpoint,
                                    const std::unordered_map<std::string, std::string>& params = {});

    // Data Converters
    Order parseOrder(const nlohmann::json& data);
    OrderStatus parseOrderStatus(const std::string& status);
    OrderSide parseOrderSide(const std::string& side);
    OrderType parseOrderType(const std::string& type);
    TimeInForce parseTimeInForce(const std::string& tif);
    std::string orderSideToString(OrderSide side);
    std::string orderTypeToString(OrderType type);
    std::string timeInForceToString(TimeInForce tif);
    SymbolInfo parseSymbolInfo(const nlohmann::json& data);

    // Configuration
    GateIOConfig gateio_config_;

    // REST Client
    std::shared_ptr<network::RestClient> rest_client_;

    // WebSocket Clients
    std::shared_ptr<network::WebSocketClient> public_ws_client_;
    std::shared_ptr<network::WebSocketClient> private_ws_client_;

    // Connection State
    std::atomic<bool> public_ws_connected_{false};
    std::atomic<bool> private_ws_connected_{false};
    std::atomic<bool> private_ws_authenticated_{false};

    // Subscription Management
    std::set<std::string> public_subscriptions_;
    std::mutex subscription_mutex_;

    // Orderbook State
    std::unordered_map<std::string, uint64_t> orderbook_seq_;
    std::mutex orderbook_seq_mutex_;
    std::unordered_map<std::string, bool> orderbook_initialized_;
    std::mutex orderbook_init_mutex_;

    // Request ID counter
    std::atomic<uint64_t> next_req_id_{1};
};

std::shared_ptr<GateIOExchange> createGateIOExchange(const ExchangeConfig& config);
std::shared_ptr<GateIOExchange> createGateIOExchange(const GateIOConfig& config);

} // namespace exchange
} // namespace hft
