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

// KuCoin-specific configuration
struct KuCoinConfig {
    bool spot{true};
    bool testnet{false};
    std::string api_key;
    std::string api_secret;
    std::string passphrase;

    uint32_t order_rate_limit{30};       // 30 requests per second
    uint32_t request_rate_limit{1800};   // 1800 requests per minute
    uint32_t recv_window{5000};          // Receive window in ms
    int orderbook_depth{20};
};

// KuCoin exchange implementation
class KuCoinExchange : public ExchangeBase {
public:
    explicit KuCoinExchange(const ExchangeConfig& config);
    explicit KuCoinExchange(const KuCoinConfig& kucoin_config);
    ~KuCoinExchange() override;

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

private:
    // ========================================================================
    // URL Builders
    // ========================================================================
    std::string getRestUrl() const;
    std::string getPublicWsEndpoint();
    std::string getPrivateWsEndpoint();

    // ========================================================================
    // WebSocket Token Management
    // ========================================================================
    struct WsTokenInfo {
        std::string token;
        std::string endpoint;
        int ping_interval_ms{18000};
        int ping_timeout_ms{10000};
    };

    WsTokenInfo getPublicWsToken();
    WsTokenInfo getPrivateWsToken();

    // ========================================================================
    // Signature Generation
    // ========================================================================
    std::string sign(const std::string& timestamp, const std::string& method,
                     const std::string& request_path, const std::string& body);
    std::string encryptPassphrase();
    void addAuthHeaders(network::HttpRequest& request, const std::string& method,
                        const std::string& request_path, const std::string& body);

    // ========================================================================
    // REST Helpers
    // ========================================================================
    network::HttpResponse signedGet(const std::string& endpoint,
                                    const std::unordered_map<std::string, std::string>& params = {});
    network::HttpResponse signedPost(const std::string& endpoint,
                                     const nlohmann::json& body = {});
    network::HttpResponse signedDelete(const std::string& endpoint,
                                       const std::unordered_map<std::string, std::string>& params = {});
    network::HttpResponse publicGet(const std::string& endpoint,
                                    const std::unordered_map<std::string, std::string>& params = {});

    // ========================================================================
    // WebSocket Handlers
    // ========================================================================
    void handleWsOpen(bool is_private);
    void handleWsClose(int code, const std::string& reason, bool is_private);
    void handleWsError(const std::string& error, bool is_private);
    void handlePublicWsMessage(const std::string& message, network::MessageType type);
    void handlePrivateWsMessage(const std::string& message, network::MessageType type);

    // Data handlers
    void handleOrderBookUpdate(const nlohmann::json& data, const std::string& symbol);
    void handleTradeUpdate(const nlohmann::json& data, const std::string& symbol);
    void handleTickerUpdate(const nlohmann::json& data, const std::string& symbol);
    void handleOrderUpdate(const nlohmann::json& data);
    void handleBalanceUpdate(const nlohmann::json& data);

    // ========================================================================
    // Subscription Management
    // ========================================================================
    void sendSubscribe(const std::string& topic, bool is_private);
    void sendUnsubscribe(const std::string& topic, bool is_private);
    void startPingLoop();
    void stopPingLoop();

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
    // Private Members
    // ========================================================================
    KuCoinConfig kucoin_config_;

    // REST client
    std::shared_ptr<network::RestClient> rest_client_;

    // WebSocket clients
    std::shared_ptr<network::WebSocketClient> public_ws_client_;
    std::shared_ptr<network::WebSocketClient> private_ws_client_;

    // Connection state
    std::atomic<bool> public_ws_connected_{false};
    std::atomic<bool> private_ws_connected_{false};

    // WebSocket token info
    WsTokenInfo public_ws_token_;
    WsTokenInfo private_ws_token_;

    // Subscription tracking
    std::set<std::string> public_subscriptions_;
    std::set<std::string> private_subscriptions_;
    std::mutex subscription_mutex_;

    // Orderbook sequence tracking
    std::unordered_map<std::string, uint64_t> orderbook_seq_;
    std::mutex orderbook_seq_mutex_;

    // Ping management
    std::atomic<bool> ping_running_{false};
    std::unique_ptr<std::thread> ping_thread_;
    std::string connect_id_;

    // Request ID tracking
    std::atomic<uint64_t> next_req_id_{1};
};

std::shared_ptr<KuCoinExchange> createKuCoinExchange(const ExchangeConfig& config);
std::shared_ptr<KuCoinExchange> createKuCoinExchange(const KuCoinConfig& config);

} // namespace exchange
} // namespace hft
