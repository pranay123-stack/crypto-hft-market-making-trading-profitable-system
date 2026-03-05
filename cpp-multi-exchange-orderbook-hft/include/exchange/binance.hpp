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

// Binance-specific configuration
struct BinanceConfig {
    bool spot{true};                      // Spot or Futures
    bool margin{false};                   // Cross/Isolated margin for spot
    bool testnet{false};
    std::string api_key;
    std::string api_secret;
    uint32_t recv_window{5000};           // Receive window for signatures

    // WebSocket settings
    bool combine_streams{true};           // Use combined streams
    uint32_t orderbook_depth{20};         // Default orderbook depth (5, 10, 20)
    uint32_t orderbook_update_speed{100}; // Update speed (100ms or 1000ms)

    // Rate limiting
    uint32_t order_rate_limit{10};        // Orders per second
    uint32_t request_rate_limit{1200};    // Requests per minute
};

// Binance exchange implementation
class BinanceExchange : public ExchangeBase {
public:
    explicit BinanceExchange(const ExchangeConfig& config);
    explicit BinanceExchange(const BinanceConfig& binance_config);
    ~BinanceExchange() override;

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
                                                  int depth = 20) override;
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
    // Binance-specific Methods
    // ========================================================================

    // Get/refresh listen key for user data stream
    std::string getListenKey();
    bool keepAliveListenKey();

    // Change leverage (futures only)
    bool setLeverage(const std::string& symbol, int leverage);

    // Change margin type (futures only)
    bool setMarginType(const std::string& symbol, bool isolated);

    // Get funding rate (futures only)
    double getFundingRate(const std::string& symbol);

private:
    // ========================================================================
    // Internal Types
    // ========================================================================

    struct StreamSubscription {
        std::string stream_name;
        std::string symbol;
        int request_id;
        bool active{false};
    };

    // ========================================================================
    // URL Builders
    // ========================================================================

    std::string getSpotRestUrl() const;
    std::string getFuturesRestUrl() const;
    std::string getSpotWsUrl() const;
    std::string getFuturesWsUrl() const;
    std::string getRestUrl() const;
    std::string getWsUrl() const;

    // ========================================================================
    // Signature Generation
    // ========================================================================

    std::string sign(const std::string& query_string);
    std::string buildSignedQuery(const std::unordered_map<std::string, std::string>& params);

    // ========================================================================
    // WebSocket Handlers
    // ========================================================================

    void handleWsMessage(const std::string& message, network::MessageType type);
    void handleWsOpen();
    void handleWsClose(int code, const std::string& reason);
    void handleWsError(const std::string& error);

    // Message parsers
    void handleDepthUpdate(const nlohmann::json& data);
    void handleTradeUpdate(const nlohmann::json& data);
    void handleTickerUpdate(const nlohmann::json& data);
    void handleUserDataUpdate(const nlohmann::json& data);
    void handleOrderUpdate(const nlohmann::json& data);
    void handleAccountUpdate(const nlohmann::json& data);
    void handlePositionUpdate(const nlohmann::json& data);

    // ========================================================================
    // REST Helpers
    // ========================================================================

    network::HttpResponse signedGet(const std::string& endpoint,
                                    const std::unordered_map<std::string, std::string>& params = {});
    network::HttpResponse signedPost(const std::string& endpoint,
                                     const std::unordered_map<std::string, std::string>& params = {});
    network::HttpResponse signedDelete(const std::string& endpoint,
                                       const std::unordered_map<std::string, std::string>& params = {});
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
    // Stream Management
    // ========================================================================

    void sendSubscribe(const std::vector<std::string>& streams);
    void sendUnsubscribe(const std::vector<std::string>& streams);
    std::string buildStreamName(const std::string& symbol, const std::string& stream_type);

    // Listen key management
    void startListenKeyRefresh();
    void stopListenKeyRefresh();

    // ========================================================================
    // Orderbook Management
    // ========================================================================

    void initializeOrderBook(const std::string& symbol, int depth);
    void applyOrderBookDelta(const std::string& symbol, const nlohmann::json& data);

    // ========================================================================
    // Member Variables
    // ========================================================================

    // Binance-specific config
    BinanceConfig binance_config_;

    // Network clients
    std::shared_ptr<network::WebSocketClient> ws_client_;
    std::shared_ptr<network::RestClient> rest_client_;

    // User data stream
    std::string listen_key_;
    std::shared_ptr<network::WebSocketClient> user_ws_client_;
    std::unique_ptr<std::thread> listen_key_refresh_thread_;
    std::atomic<bool> refresh_running_{false};

    // Subscriptions
    std::unordered_map<std::string, StreamSubscription> subscriptions_;
    mutable std::mutex subscription_mutex_;
    std::atomic<int> next_request_id_{1};

    // Orderbook sequence tracking
    std::unordered_map<std::string, uint64_t> last_update_id_;
    std::unordered_map<std::string, bool> orderbook_initialized_;
    mutable std::mutex orderbook_init_mutex_;

    // Connection state
    std::atomic<bool> ws_connected_{false};
    std::atomic<bool> user_ws_connected_{false};
};

// Factory function
std::shared_ptr<BinanceExchange> createBinanceExchange(const ExchangeConfig& config);
std::shared_ptr<BinanceExchange> createBinanceExchange(const BinanceConfig& config);

} // namespace exchange
} // namespace hft
