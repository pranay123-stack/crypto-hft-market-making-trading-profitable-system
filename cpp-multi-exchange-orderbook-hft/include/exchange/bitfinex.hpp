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

// Bitfinex wallet types
enum class BitfinexWalletType {
    Exchange,   // For spot trading
    Margin,     // For margin trading
    Funding     // For funding/lending
};

// Bitfinex orderbook precision levels
enum class BitfinexBookPrecision {
    P0,  // 5 significant figures
    P1,  // 4 significant figures
    P2,  // 3 significant figures
    P3,  // 2 significant figures
    P4,  // 1 significant figure (raw precision)
    R0   // Raw orderbook
};

// Bitfinex-specific configuration
struct BitfinexConfig {
    bool testnet{false};
    std::string api_key;
    std::string api_secret;

    // Trading settings
    BitfinexWalletType default_wallet{BitfinexWalletType::Exchange};
    BitfinexBookPrecision book_precision{BitfinexBookPrecision::P0};

    // WebSocket settings
    uint32_t orderbook_depth{25};  // 1, 25, 100, 250

    // Rate limiting (Bitfinex has tight limits)
    uint32_t order_rate_limit{10};       // 10 orders per second
    uint32_t request_rate_limit{90};     // 90 requests per minute
    uint32_t recv_window{5000};          // Nonce window
};

// Bitfinex exchange implementation
class BitfinexExchange : public ExchangeBase {
public:
    explicit BitfinexExchange(const ExchangeConfig& config);
    explicit BitfinexExchange(const BitfinexConfig& bitfinex_config);
    ~BitfinexExchange() override;

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
    // Bitfinex-specific Methods
    // ========================================================================

    // Wallet operations
    std::vector<Balance> getWallets(BitfinexWalletType type = BitfinexWalletType::Exchange);
    bool transferBetweenWallets(const std::string& currency, double amount,
                                BitfinexWalletType from, BitfinexWalletType to);

    // Margin trading
    bool setLeverage(const std::string& symbol, double leverage);
    double getMarginInfo(const std::string& symbol);

    // Order flags helpers
    static constexpr int FLAG_HIDDEN = 64;
    static constexpr int FLAG_CLOSE = 512;
    static constexpr int FLAG_REDUCE_ONLY = 1024;
    static constexpr int FLAG_POST_ONLY = 4096;
    static constexpr int FLAG_OCO = 16384;
    static constexpr int FLAG_NO_VAR_RATES = 524288;

private:
    // ========================================================================
    // Internal Types
    // ========================================================================

    struct ChannelSubscription {
        int channel_id{0};
        std::string channel_type;  // "book", "trades", "ticker"
        std::string symbol;
        bool active{false};
        std::string precision;
        int depth{25};
    };

    // ========================================================================
    // URL Builders
    // ========================================================================

    std::string getRestUrl() const;
    std::string getWsUrl() const;
    std::string getWalletTypeString(BitfinexWalletType type) const;
    std::string getPrecisionString(BitfinexBookPrecision prec) const;

    // ========================================================================
    // Signature Generation (HMAC-SHA384)
    // ========================================================================

    std::string sign(const std::string& payload);
    void addAuthHeaders(network::HttpRequest& request, const std::string& endpoint,
                       const std::string& body = "");
    std::string generateNonce();

    // ========================================================================
    // WebSocket Handlers
    // ========================================================================

    void handleWsMessage(const std::string& message, network::MessageType type);
    void handleWsOpen();
    void handleWsClose(int code, const std::string& reason);
    void handleWsError(const std::string& error);

    // Array-based message parsers
    void handleInfoMessage(const nlohmann::json& msg);
    void handleAuthMessage(const nlohmann::json& msg);
    void handleSubscribedMessage(const nlohmann::json& msg);
    void handleUnsubscribedMessage(const nlohmann::json& msg);
    void handleErrorMessage(const nlohmann::json& msg);
    void handleHeartbeat(int channel_id);
    void handleChannelData(int channel_id, const nlohmann::json& data);

    // Channel-specific handlers
    void handleBookUpdate(const ChannelSubscription& sub, const nlohmann::json& data);
    void handleBookSnapshot(const std::string& symbol, const nlohmann::json& data,
                           bool is_raw = false);
    void handleBookDelta(const std::string& symbol, const nlohmann::json& data,
                        bool is_raw = false);
    void handleTradeUpdate(const ChannelSubscription& sub, const nlohmann::json& data);
    void handleTickerUpdate(const ChannelSubscription& sub, const nlohmann::json& data);

    // Private channel handlers (channel 0)
    void handlePrivateData(const std::string& msg_type, const nlohmann::json& data);
    void handleWalletUpdate(const nlohmann::json& data);
    void handleOrderUpdate(const nlohmann::json& data);
    void handlePositionUpdate(const nlohmann::json& data);
    void handleTradeExecutionUpdate(const nlohmann::json& data);
    void handleNotification(const nlohmann::json& data);

    // ========================================================================
    // REST Helpers
    // ========================================================================

    network::HttpResponse signedPost(const std::string& endpoint,
                                     const nlohmann::json& body = {});
    network::HttpResponse publicGet(const std::string& endpoint,
                                    const std::unordered_map<std::string, std::string>& params = {});

    // ========================================================================
    // Data Converters
    // ========================================================================

    Order parseOrderFromArray(const nlohmann::json& arr);
    Position parsePositionFromArray(const nlohmann::json& arr);
    Balance parseWalletFromArray(const nlohmann::json& arr);
    Trade parseTradeFromArray(const nlohmann::json& arr, const std::string& symbol);

    OrderStatus parseOrderStatus(const std::string& status);
    OrderSide parseOrderSide(double amount);
    OrderType parseOrderType(const std::string& type);
    TimeInForce parseTimeInForce(const std::string& type);

    std::string orderTypeToString(OrderType type, bool post_only = false);
    std::string timeInForceToString(TimeInForce tif);

    SymbolInfo parseSymbolInfo(const nlohmann::json& data);

    // ========================================================================
    // Subscription Management
    // ========================================================================

    void sendSubscribe(const std::string& channel, const std::string& symbol,
                      const std::string& precision = "", int depth = 25);
    void sendUnsubscribe(int channel_id);
    void authenticateWebSocket();

    // Ping/pong management
    void startHeartbeat();
    void stopHeartbeat();

    // ========================================================================
    // Member Variables
    // ========================================================================

    // Bitfinex-specific config
    BitfinexConfig bitfinex_config_;

    // Network clients
    std::shared_ptr<network::WebSocketClient> ws_client_;
    std::shared_ptr<network::RestClient> rest_client_;

    // Channel management
    std::unordered_map<int, ChannelSubscription> channel_subscriptions_;  // channel_id -> subscription
    std::unordered_map<std::string, int> symbol_to_channel_;  // "book:tBTCUSD" -> channel_id
    mutable std::mutex channel_mutex_;

    // Nonce for authentication
    std::atomic<uint64_t> nonce_{0};

    // Connection state
    std::atomic<bool> ws_connected_{false};
    std::atomic<bool> ws_authenticated_{false};

    // Orderbook state tracking
    std::unordered_map<std::string, bool> orderbook_initialized_;
    mutable std::mutex orderbook_state_mutex_;

    // Heartbeat management
    std::unique_ptr<std::thread> heartbeat_thread_;
    std::atomic<bool> heartbeat_running_{false};
    std::chrono::steady_clock::time_point last_heartbeat_;
    mutable std::mutex heartbeat_mutex_;

    // Rate limiting
    std::chrono::steady_clock::time_point last_request_time_;
    mutable std::mutex rate_limit_mutex_;
};

// Factory function
std::shared_ptr<BitfinexExchange> createBitfinexExchange(const ExchangeConfig& config);
std::shared_ptr<BitfinexExchange> createBitfinexExchange(const BitfinexConfig& config);

} // namespace exchange
} // namespace hft
