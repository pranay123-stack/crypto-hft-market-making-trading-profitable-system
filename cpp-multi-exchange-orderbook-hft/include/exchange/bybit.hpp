#pragma once

#include "exchange/exchange_base.hpp"
#include "network/websocket_client.hpp"
#include "network/rest_client.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <thread>

namespace hft {
namespace exchange {

// Bybit V5 API categories
enum class BybitCategory {
    Spot,
    Linear,     // USDT/USDC perpetual
    Inverse,    // Inverse perpetual
    Option
};

// Bybit-specific configuration
struct BybitConfig {
    BybitCategory category{BybitCategory::Linear};
    bool testnet{false};
    std::string api_key;
    std::string api_secret;
    uint32_t recv_window{5000};

    // WebSocket settings
    uint32_t orderbook_depth{50};  // 1, 50, 200, or 500

    // Rate limiting
    uint32_t order_rate_limit{10};
    uint32_t request_rate_limit{600};
};

// Bybit V5 API exchange implementation
class BybitExchange : public ExchangeBase {
public:
    explicit BybitExchange(const ExchangeConfig& config);
    explicit BybitExchange(const BybitConfig& bybit_config);
    ~BybitExchange() override;

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

    bool subscribeOrderBook(const std::string& symbol, int depth = 50) override;
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
    std::optional<Order> cancelOrder(const std::string& symbol,
                                     const std::string& order_id) override;
    std::optional<Order> cancelOrder(const std::string& symbol,
                                     const std::string& order_id,
                                     const std::string& client_order_id) override;
    bool cancelAllOrders(const std::string& symbol) override;

    std::optional<Order> getOrder(const std::string& symbol,
                                  const std::string& order_id) override;
    std::vector<Order> getOpenOrders(const std::string& symbol = "") override;
    std::vector<Order> getOrderHistory(const std::string& symbol,
                                       uint64_t start_time = 0,
                                       uint64_t end_time = 0,
                                       int limit = 100) override;

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
    std::optional<OrderBook> getOrderBookSnapshot(const std::string& symbol,
                                                  int depth = 50) override;
    std::vector<Trade> getRecentTrades(const std::string& symbol,
                                       int limit = 100) override;
    std::optional<Ticker> getTicker(const std::string& symbol) override;

    // ========================================================================
    // Utility Methods
    // ========================================================================

    uint64_t getServerTime() override;
    std::string formatSymbol(const std::string& base,
                            const std::string& quote) const override;
    std::pair<std::string, std::string> parseSymbol(
        const std::string& symbol) const override;

    // ========================================================================
    // Bybit-specific Methods
    // ========================================================================

    // Set leverage
    bool setLeverage(const std::string& symbol, int buy_leverage, int sell_leverage);

    // Set position mode (hedge/one-way)
    bool setPositionMode(bool hedge_mode);

    // Get funding rate
    double getFundingRate(const std::string& symbol);

private:
    // ========================================================================
    // URL Builders
    // ========================================================================

    std::string getRestUrl() const;
    std::string getPublicWsUrl() const;
    std::string getPrivateWsUrl() const;
    std::string getCategoryString() const;

    // ========================================================================
    // Signature Generation
    // ========================================================================

    std::string sign(const std::string& timestamp, const std::string& api_key,
                    uint32_t recv_window, const std::string& query_string);
    void addAuthHeaders(network::HttpRequest& request,
                       const std::string& query_string = "");

    // ========================================================================
    // WebSocket Handlers
    // ========================================================================

    void handlePublicWsMessage(const std::string& message, network::MessageType type);
    void handlePrivateWsMessage(const std::string& message, network::MessageType type);
    void handleWsOpen(bool is_private);
    void handleWsClose(int code, const std::string& reason, bool is_private);
    void handleWsError(const std::string& error, bool is_private);

    // Message parsers
    void handleOrderBookSnapshot(const nlohmann::json& data);
    void handleOrderBookDelta(const nlohmann::json& data);
    void handleTradeUpdate(const nlohmann::json& data);
    void handleTickerUpdate(const nlohmann::json& data);
    void handleOrderUpdate(const nlohmann::json& data);
    void handlePositionUpdate(const nlohmann::json& data);
    void handleWalletUpdate(const nlohmann::json& data);

    // ========================================================================
    // REST Helpers
    // ========================================================================

    network::HttpResponse signedGet(const std::string& endpoint,
                                    const std::unordered_map<std::string, std::string>& params = {});
    network::HttpResponse signedPost(const std::string& endpoint,
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

    std::string orderSideToString(OrderSide side);
    std::string orderTypeToString(OrderType type);
    std::string timeInForceToString(TimeInForce tif);

    SymbolInfo parseSymbolInfo(const nlohmann::json& data);

    // ========================================================================
    // Subscription Management
    // ========================================================================

    void sendSubscribe(const std::vector<std::string>& topics, bool is_private);
    void sendUnsubscribe(const std::vector<std::string>& topics, bool is_private);
    void authenticatePrivateWs();

    // ========================================================================
    // Member Variables
    // ========================================================================

    // Bybit-specific config
    BybitConfig bybit_config_;

    // Network clients
    std::shared_ptr<network::WebSocketClient> public_ws_client_;
    std::shared_ptr<network::WebSocketClient> private_ws_client_;
    std::shared_ptr<network::RestClient> rest_client_;

    // Subscriptions
    std::unordered_set<std::string> public_subscriptions_;
    std::unordered_set<std::string> private_subscriptions_;
    mutable std::mutex subscription_mutex_;
    std::atomic<int> next_req_id_{1};

    // Connection state
    std::atomic<bool> public_ws_connected_{false};
    std::atomic<bool> private_ws_connected_{false};
    std::atomic<bool> private_ws_authenticated_{false};

    // Orderbook state
    std::unordered_map<std::string, uint64_t> orderbook_seq_;
    mutable std::mutex orderbook_seq_mutex_;
};

// Factory function
std::shared_ptr<BybitExchange> createBybitExchange(const ExchangeConfig& config);
std::shared_ptr<BybitExchange> createBybitExchange(const BybitConfig& config);

} // namespace exchange
} // namespace hft
