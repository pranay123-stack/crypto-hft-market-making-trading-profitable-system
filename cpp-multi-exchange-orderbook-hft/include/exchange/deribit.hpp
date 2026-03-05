#pragma once

#include "exchange/exchange_base.hpp"
#include "network/websocket_client.hpp"
#include "network/rest_client.hpp"

#include <memory>
#include <string>
#include <atomic>
#include <mutex>
#include <set>
#include <thread>

namespace hft {
namespace exchange {

// Deribit-specific configuration
struct DeribitConfig {
    bool testnet{false};
    std::string api_key;      // client_id for Deribit
    std::string api_secret;   // client_secret for Deribit
    std::string currency{"BTC"};  // BTC or ETH

    uint32_t order_rate_limit{10};
    uint32_t request_rate_limit{100};
    uint32_t recv_window{5000};
    int orderbook_depth{20};
};

// Deribit exchange implementation - Derivatives focused (Options & Futures)
class DeribitExchange : public ExchangeBase {
public:
    explicit DeribitExchange(const ExchangeConfig& config);
    explicit DeribitExchange(const DeribitConfig& deribit_config);
    ~DeribitExchange() override;

    bool connect() override;
    void disconnect() override;
    bool isConnected() const override;
    ConnectionStatus connectionStatus() const override;

    bool subscribeOrderBook(const std::string& symbol, int depth = 20) override;
    bool unsubscribeOrderBook(const std::string& symbol) override;
    bool subscribeTrades(const std::string& symbol) override;
    bool unsubscribeTrades(const std::string& symbol) override;
    bool subscribeTicker(const std::string& symbol) override;
    bool unsubscribeTicker(const std::string& symbol) override;
    bool subscribeUserData() override;
    bool unsubscribeUserData() override;

    std::optional<Order> placeOrder(const OrderRequest& request) override;
    std::optional<Order> cancelOrder(const std::string& symbol, const std::string& order_id) override;
    std::optional<Order> cancelOrder(const std::string& symbol, const std::string& order_id,
                                     const std::string& client_order_id) override;
    bool cancelAllOrders(const std::string& symbol) override;
    std::optional<Order> getOrder(const std::string& symbol, const std::string& order_id) override;
    std::vector<Order> getOpenOrders(const std::string& symbol = "") override;
    std::vector<Order> getOrderHistory(const std::string& symbol, uint64_t start_time = 0,
                                       uint64_t end_time = 0, int limit = 100) override;

    std::optional<Account> getAccount() override;
    std::optional<Balance> getBalance(const std::string& asset) override;
    std::vector<Position> getPositions(const std::string& symbol = "") override;

    std::vector<SymbolInfo> getSymbols() override;
    std::optional<SymbolInfo> getSymbolInfo(const std::string& symbol) override;
    std::optional<OrderBook> getOrderBookSnapshot(const std::string& symbol, int depth = 20) override;
    std::vector<Trade> getRecentTrades(const std::string& symbol, int limit = 100) override;
    std::optional<Ticker> getTicker(const std::string& symbol) override;

    uint64_t getServerTime() override;
    std::string formatSymbol(const std::string& base, const std::string& quote) const override;
    std::pair<std::string, std::string> parseSymbol(const std::string& symbol) const override;

    // Deribit-specific methods
    bool setLeverage(const std::string& symbol, double leverage);
    double getFundingRate(const std::string& symbol);
    std::string getCurrency(const std::string& symbol) const;

private:
    // URL builders
    std::string getRestUrl() const;
    std::string getWsUrl() const;

    // WebSocket handlers
    void handleWsOpen();
    void handleWsClose(int code, const std::string& reason);
    void handleWsError(const std::string& error);
    void handleWsMessage(const std::string& message, network::MessageType type);

    // Message handlers
    void handleSubscriptionResponse(const nlohmann::json& json);
    void handleOrderBookUpdate(const nlohmann::json& data, const std::string& channel);
    void handleTradeUpdate(const nlohmann::json& data);
    void handleTickerUpdate(const nlohmann::json& data);
    void handleOrderUpdate(const nlohmann::json& data);
    void handlePositionUpdate(const nlohmann::json& data);
    void handleAccountUpdate(const nlohmann::json& data);

    // Authentication
    bool authenticate();
    void startTokenRefresh();
    void stopTokenRefresh();

    // Heartbeat handling
    void handleHeartbeat(const nlohmann::json& json);
    void sendHeartbeat();

    // Subscription management
    void sendSubscribe(const std::vector<std::string>& channels);
    void sendUnsubscribe(const std::vector<std::string>& channels);

    // JSON-RPC helpers
    nlohmann::json buildJsonRpcRequest(const std::string& method,
                                       const nlohmann::json& params = nlohmann::json::object());
    uint64_t getNextRequestId();

    // Data converters
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
    DeribitConfig deribit_config_;

    // Network clients
    std::shared_ptr<network::WebSocketClient> ws_client_;
    std::shared_ptr<network::RestClient> rest_client_;

    // Connection state
    std::atomic<bool> ws_connected_{false};
    std::atomic<bool> authenticated_{false};

    // Authentication tokens
    std::string access_token_;
    std::string refresh_token_;
    uint64_t token_expiry_{0};
    std::atomic<bool> refresh_running_{false};
    std::unique_ptr<std::thread> token_refresh_thread_;
    std::mutex auth_mutex_;

    // Request ID generator
    std::atomic<uint64_t> next_request_id_{1};

    // Pending requests
    std::unordered_map<uint64_t, std::string> pending_requests_;
    std::mutex pending_requests_mutex_;

    // Subscriptions
    std::set<std::string> subscriptions_;
    std::mutex subscription_mutex_;

    // Orderbook sequence tracking
    std::unordered_map<std::string, uint64_t> orderbook_change_id_;
    std::mutex orderbook_seq_mutex_;
};

std::shared_ptr<DeribitExchange> createDeribitExchange(const ExchangeConfig& config);
std::shared_ptr<DeribitExchange> createDeribitExchange(const DeribitConfig& config);

} // namespace exchange
} // namespace hft
