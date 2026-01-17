#pragma once

#include "exchange/exchange_client.hpp"
#include <openssl/hmac.h>
#include <memory>
#include <mutex>
#include <condition_variable>

namespace hft {

// ============================================================================
// Binance-specific Configuration
// ============================================================================

struct BinanceConfig : public ExchangeConfig {
    bool use_testnet = false;
    bool futures = false;              // Spot vs Futures
    std::string recv_window = "5000";  // Recv window for signed requests

    BinanceConfig() {
        name = "binance";
        rest_url = "https://api.binance.com";
        ws_url = "wss://stream.binance.com:9443/ws";
    }

    void set_testnet() {
        use_testnet = true;
        rest_url = "https://testnet.binance.vision";
        ws_url = "wss://testnet.binance.vision/ws";
    }

    void set_futures() {
        futures = true;
        if (use_testnet) {
            rest_url = "https://testnet.binancefuture.com";
            ws_url = "wss://stream.binancefuture.com/ws";
        } else {
            rest_url = "https://fapi.binance.com";
            ws_url = "wss://fstream.binance.com/ws";
        }
    }
};

// ============================================================================
// Binance Message Parser
// ============================================================================

class BinanceParser {
public:
    static Tick parse_ticker(const std::string& json);
    static std::pair<std::vector<PriceLevel>, std::vector<PriceLevel>>
        parse_depth_snapshot(const std::string& json);
    static void parse_depth_update(const std::string& json,
                                   std::vector<PriceLevel>& bids,
                                   std::vector<PriceLevel>& asks);
    static Trade parse_trade(const std::string& json);
    static Order parse_order_update(const std::string& json);
    static OrderResponse parse_order_response(const std::string& json);
};

// ============================================================================
// Binance WebSocket Streams
// ============================================================================

class BinanceWebSocket {
public:
    explicit BinanceWebSocket(const BinanceConfig& config);
    ~BinanceWebSocket();

    bool connect();
    void disconnect();
    [[nodiscard]] bool is_connected() const;

    // Public streams
    bool subscribe_ticker(const std::string& symbol);
    bool subscribe_depth(const std::string& symbol, int levels = 20);
    bool subscribe_trades(const std::string& symbol);
    bool subscribe_book_ticker(const std::string& symbol);

    // User data stream
    bool subscribe_user_data(const std::string& listen_key);

    void set_callbacks(const ExchangeCallbacks& callbacks);

    void run();
    void stop();

private:
    void handle_message(const std::string& message);
    void send_ping();
    void reconnect();
    std::string build_stream_url(const std::vector<std::string>& streams);

    BinanceConfig config_;
    std::unique_ptr<WebSocketClient> ws_client_;
    ExchangeCallbacks callbacks_;

    std::vector<std::string> subscribed_streams_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};

    std::thread ping_thread_;
    std::mutex mutex_;
};

// ============================================================================
// Binance REST API
// ============================================================================

class BinanceREST {
public:
    explicit BinanceREST(const BinanceConfig& config);
    ~BinanceREST() = default;

    // Public endpoints
    Timestamp get_server_time();
    std::string get_exchange_info();

    // Market data
    std::string get_depth(const std::string& symbol, int limit = 100);
    std::string get_recent_trades(const std::string& symbol, int limit = 500);
    std::string get_ticker_price(const std::string& symbol);

    // Account (requires auth)
    std::string get_account_info();
    double get_balance(const std::string& asset);

    // Trading (requires auth)
    OrderResponse new_order(const OrderRequest& request);
    CancelResponse cancel_order(const std::string& symbol,
                               OrderId order_id);
    CancelResponse cancel_all_orders(const std::string& symbol);
    std::vector<Order> get_open_orders(const std::string& symbol);

    // User data stream
    std::string create_listen_key();
    void keep_alive_listen_key(const std::string& listen_key);
    void delete_listen_key(const std::string& listen_key);

private:
    std::string sign_request(const std::string& query_string);
    std::string build_query_string(
        const std::vector<std::pair<std::string, std::string>>& params);

    BinanceConfig config_;
    std::unique_ptr<RESTClient> rest_client_;
};

// ============================================================================
// Binance Exchange Client (Full Implementation)
// ============================================================================

class BinanceClient : public ExchangeClient {
public:
    explicit BinanceClient(const BinanceConfig& config);
    ~BinanceClient() override;

    // Connection management
    bool connect() override;
    void disconnect() override;
    [[nodiscard]] bool is_connected() const override;

    // Market data subscription
    bool subscribe_ticker(const Symbol& symbol) override;
    bool subscribe_orderbook(const Symbol& symbol, int depth = 20) override;
    bool subscribe_trades(const Symbol& symbol) override;
    bool unsubscribe(const Symbol& symbol) override;

    // Order management
    OrderResponse send_order(const OrderRequest& request) override;
    CancelResponse cancel_order(const CancelRequest& request) override;
    CancelResponse cancel_all_orders(const Symbol& symbol) override;

    // Account queries
    double get_balance(const std::string& asset) override;
    std::vector<Order> get_open_orders(const Symbol& symbol) override;

    // Callbacks
    void set_callbacks(const ExchangeCallbacks& callbacks) override;

    // Exchange info
    [[nodiscard]] const std::string& name() const override { return config_.name; }
    [[nodiscard]] Timestamp server_time() const override;

private:
    void start_user_data_stream();
    void keep_alive_loop();

    BinanceConfig config_;
    std::unique_ptr<BinanceWebSocket> ws_;
    std::unique_ptr<BinanceREST> rest_;
    ExchangeCallbacks callbacks_;

    std::string listen_key_;
    std::thread keep_alive_thread_;
    std::atomic<bool> running_{false};
    mutable std::atomic<Timestamp> last_server_time_{0};
};

} // namespace hft
