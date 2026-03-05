#pragma once

#include "exchange/exchange_base.hpp"
#include "network/websocket_client.hpp"
#include "network/rest_client.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <mutex>
#include <thread>

namespace hft {
namespace exchange {

// Kraken-specific configuration
struct KrakenConfig {
    bool spot{true};                      // Spot or Futures
    bool testnet{false};
    std::string api_key;
    std::string api_secret;

    // Rate limiting (Kraken uses tier-based limits)
    uint32_t order_rate_limit{10};        // Orders per second
    uint32_t request_rate_limit{600};     // Requests per minute

    // WebSocket settings
    uint32_t orderbook_depth{25};         // Default orderbook depth (10, 25, 100, 500, 1000)

    // REST settings
    uint32_t recv_window{5000};           // Not used by Kraken but kept for consistency
};

// Kraken exchange implementation
class KrakenExchange : public ExchangeBase {
public:
    explicit KrakenExchange(const ExchangeConfig& config);
    explicit KrakenExchange(const KrakenConfig& kraken_config);
    ~KrakenExchange() override;

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

    bool subscribeOrderBook(const std::string& symbol, int depth = 25) override;
    bool unsubscribeOrderBook(const std::string& symbol) override;

    bool subscribeTrades(const std::string& symbol) override;
    bool unsubscribeTrades(const std::string& symbol) override;

    bool subscribeTicker(const std::string& symbol) override;
    bool unsubscribeTicker(const std::string& symbol) override;

    // OHLC subscription (Kraken-specific)
    bool subscribeOHLC(const std::string& symbol, int interval = 1);
    bool unsubscribeOHLC(const std::string& symbol);

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

    // Kraken-specific: Get trade balance (margin info)
    std::optional<Account> getTradeBalance(const std::string& asset = "ZUSD");

    // ========================================================================
    // Market Information
    // ========================================================================

    std::vector<SymbolInfo> getSymbols() override;
    std::optional<SymbolInfo> getSymbolInfo(const std::string& symbol) override;
    std::optional<OrderBook> getOrderBookSnapshot(const std::string& symbol,
                                                  int depth = 25) override;
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
    // Kraken-specific Methods
    // ========================================================================

    // Get WebSocket authentication token
    std::string getWebSocketsToken();

    // Convert symbol formats
    std::string toKrakenSymbol(const std::string& symbol) const;
    std::string fromKrakenSymbol(const std::string& kraken_symbol) const;

    // Get asset info
    std::unordered_map<std::string, nlohmann::json> getAssetInfo();

private:
    // ========================================================================
    // Internal Types
    // ========================================================================

    struct StreamSubscription {
        std::string channel;
        std::string symbol;
        int depth{0};
        int interval{0};
        bool active{false};
    };

    // ========================================================================
    // URL Builders
    // ========================================================================

    std::string getRestUrl() const;
    std::string getPublicWsUrl() const;
    std::string getPrivateWsUrl() const;

    // ========================================================================
    // Signature Generation (Kraken uses HMAC-SHA512 with nonce)
    // ========================================================================

    std::string generateNonce();
    std::string sign(const std::string& path, const std::string& nonce,
                    const std::string& post_data);
    void addAuthHeaders(network::HttpRequest& request, const std::string& path,
                       const std::string& nonce, const std::string& post_data);

    // ========================================================================
    // WebSocket Handlers
    // ========================================================================

    void handlePublicWsMessage(const std::string& message, network::MessageType type);
    void handlePrivateWsMessage(const std::string& message, network::MessageType type);
    void handleWsOpen(bool is_private);
    void handleWsClose(int code, const std::string& reason, bool is_private);
    void handleWsError(const std::string& error, bool is_private);

    // Message parsers for WebSocket v2
    void handleOrderBookSnapshot(const nlohmann::json& data);
    void handleOrderBookUpdate(const nlohmann::json& data);
    void handleTradeUpdate(const nlohmann::json& data);
    void handleTickerUpdate(const nlohmann::json& data);
    void handleOHLCUpdate(const nlohmann::json& data);
    void handleExecutionUpdate(const nlohmann::json& data);
    void handleBalanceUpdate(const nlohmann::json& data);

    // ========================================================================
    // REST Helpers
    // ========================================================================

    network::HttpResponse signedPost(const std::string& endpoint,
                                     const std::unordered_map<std::string, std::string>& params = {});
    network::HttpResponse publicGet(const std::string& endpoint,
                                    const std::unordered_map<std::string, std::string>& params = {});

    // ========================================================================
    // Data Converters
    // ========================================================================

    Order parseOrder(const std::string& order_id, const nlohmann::json& data);
    OrderStatus parseOrderStatus(const std::string& status);
    OrderSide parseOrderSide(const std::string& side);
    OrderType parseOrderType(const std::string& type);
    TimeInForce parseTimeInForce(const std::string& tif);

    std::string orderSideToString(OrderSide side);
    std::string orderTypeToString(OrderType type);
    std::string timeInForceToString(TimeInForce tif);

    SymbolInfo parseSymbolInfo(const std::string& symbol, const nlohmann::json& data);

    // ========================================================================
    // Stream Management
    // ========================================================================

    void sendSubscribe(const nlohmann::json& subscription, bool is_private);
    void sendUnsubscribe(const nlohmann::json& subscription, bool is_private);
    void resubscribeAll();

    // Token refresh management
    void startTokenRefresh();
    void stopTokenRefresh();

    // ========================================================================
    // Member Variables
    // ========================================================================

    // Kraken-specific config
    KrakenConfig kraken_config_;

    // Network clients
    std::shared_ptr<network::WebSocketClient> public_ws_client_;
    std::shared_ptr<network::WebSocketClient> private_ws_client_;
    std::shared_ptr<network::RestClient> rest_client_;

    // WebSocket authentication
    std::string ws_token_;
    std::unique_ptr<std::thread> token_refresh_thread_;
    std::atomic<bool> token_refresh_running_{false};
    mutable std::mutex token_mutex_;

    // Nonce management (for REST API)
    std::atomic<uint64_t> nonce_{0};

    // Subscriptions tracking
    std::unordered_set<std::string> public_subscriptions_;
    std::unordered_set<std::string> private_subscriptions_;
    mutable std::mutex subscription_mutex_;

    // Orderbook sequence tracking
    std::unordered_map<std::string, uint64_t> orderbook_seq_;
    mutable std::mutex orderbook_seq_mutex_;

    // Request ID tracking
    std::atomic<uint64_t> next_req_id_{1};

    // Connection state
    std::atomic<bool> public_ws_connected_{false};
    std::atomic<bool> private_ws_connected_{false};
    std::atomic<bool> private_ws_authenticated_{false};

    // Symbol mapping cache
    std::unordered_map<std::string, std::string> symbol_to_kraken_;
    std::unordered_map<std::string, std::string> kraken_to_symbol_;
    mutable std::mutex symbol_map_mutex_;
};

// Factory functions
std::shared_ptr<KrakenExchange> createKrakenExchange(const ExchangeConfig& config);
std::shared_ptr<KrakenExchange> createKrakenExchange(const KrakenConfig& config);

} // namespace exchange
} // namespace hft
