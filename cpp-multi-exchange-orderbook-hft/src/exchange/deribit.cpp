#include "exchange/deribit.hpp"

#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/evp.h>

#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <random>

namespace hft {
namespace exchange {

// ============================================================================
// Constructors/Destructor
// ============================================================================

DeribitExchange::DeribitExchange(const ExchangeConfig& config)
    : ExchangeBase(config) {
    deribit_config_.api_key = config.api_key;
    deribit_config_.api_secret = config.api_secret;
    deribit_config_.testnet = config.testnet;
    deribit_config_.order_rate_limit = config.orders_per_second;
    deribit_config_.request_rate_limit = config.requests_per_minute;

    // Initialize REST client
    network::RestClientConfig rest_config;
    rest_config.base_url = getRestUrl();
    rest_config.name = "deribit_rest";
    rest_config.request_timeout_ms = config.rest_timeout_ms;
    rest_config.max_retries = config.rest_max_retries;
    rest_config.rate_limit.requests_per_second = deribit_config_.order_rate_limit;
    rest_config.rate_limit.requests_per_minute = deribit_config_.request_rate_limit;
    rest_config.default_headers["Content-Type"] = "application/json";

    rest_client_ = std::make_shared<network::RestClient>(rest_config);
}

DeribitExchange::DeribitExchange(const DeribitConfig& deribit_config)
    : ExchangeBase(ExchangeConfig{
        .name = "Deribit",
        .api_key = deribit_config.api_key,
        .api_secret = deribit_config.api_secret,
        .type = ExchangeType::Options,
        .testnet = deribit_config.testnet
      }),
      deribit_config_(deribit_config) {

    // Initialize REST client
    network::RestClientConfig rest_config;
    rest_config.base_url = getRestUrl();
    rest_config.name = "deribit_rest";
    rest_config.rate_limit.requests_per_second = deribit_config_.order_rate_limit;
    rest_config.rate_limit.requests_per_minute = deribit_config_.request_rate_limit;
    rest_config.default_headers["Content-Type"] = "application/json";

    rest_client_ = std::make_shared<network::RestClient>(rest_config);
}

DeribitExchange::~DeribitExchange() {
    disconnect();
}

// ============================================================================
// URL Builders
// ============================================================================

std::string DeribitExchange::getRestUrl() const {
    return deribit_config_.testnet
        ? "https://test.deribit.com/api/v2"
        : "https://www.deribit.com/api/v2";
}

std::string DeribitExchange::getWsUrl() const {
    return deribit_config_.testnet
        ? "wss://test.deribit.com/ws/api/v2"
        : "wss://www.deribit.com/ws/api/v2";
}

// ============================================================================
// Connection Management
// ============================================================================

bool DeribitExchange::connect() {
    if (ws_connected_.load()) {
        return true;
    }

    onConnectionStatus(ConnectionStatus::Connecting);

    // Initialize WebSocket client
    network::WebSocketConfig ws_config;
    ws_config.url = getWsUrl();
    ws_config.name = "deribit_ws";
    ws_config.ping_interval_ms = 0;  // Deribit uses JSON-RPC heartbeats
    ws_config.auto_reconnect = true;
    ws_config.reconnect_initial_delay_ms = config_.ws_reconnect_delay_ms;

    ws_client_ = std::make_shared<network::WebSocketClient>(ws_config);

    ws_client_->setOnOpen([this]() {
        this->handleWsOpen();
    });

    ws_client_->setOnClose([this](int code, const std::string& reason) {
        this->handleWsClose(code, reason);
    });

    ws_client_->setOnMessage([this](const std::string& msg, network::MessageType type) {
        this->handleWsMessage(msg, type);
    });

    ws_client_->setOnError([this](const std::string& error) {
        this->handleWsError(error);
    });

    return ws_client_->connect();
}

void DeribitExchange::disconnect() {
    stopTokenRefresh();

    if (ws_client_) {
        ws_client_->disconnect();
        ws_client_.reset();
    }

    ws_connected_ = false;
    authenticated_ = false;
    access_token_.clear();
    refresh_token_.clear();
    token_expiry_ = 0;

    onConnectionStatus(ConnectionStatus::Disconnected);
}

bool DeribitExchange::isConnected() const {
    return ws_connected_.load();
}

ConnectionStatus DeribitExchange::connectionStatus() const {
    return connection_status_.load();
}

// ============================================================================
// WebSocket Handlers
// ============================================================================

void DeribitExchange::handleWsOpen() {
    ws_connected_ = true;

    // First, set up heartbeat
    nlohmann::json heartbeat_params;
    heartbeat_params["interval"] = 30;  // 30 seconds

    auto req = buildJsonRpcRequest("public/set_heartbeat", heartbeat_params);
    ws_client_->send(req);

    // Then authenticate if credentials provided
    if (!deribit_config_.api_key.empty() && !deribit_config_.api_secret.empty()) {
        if (authenticate()) {
            onConnectionStatus(ConnectionStatus::Connected);
        } else {
            onError("Failed to authenticate with Deribit");
            onConnectionStatus(ConnectionStatus::Failed);
        }
    } else {
        onConnectionStatus(ConnectionStatus::Connected);
    }
}

void DeribitExchange::handleWsClose(int code, const std::string& reason) {
    ws_connected_ = false;
    authenticated_ = false;
    onConnectionStatus(ConnectionStatus::Reconnecting);
}

void DeribitExchange::handleWsError(const std::string& error) {
    onError("WebSocket error: " + error);
}

void DeribitExchange::handleWsMessage(const std::string& message, network::MessageType type) {
    try {
        auto json = nlohmann::json::parse(message);

        // Check for JSON-RPC response
        if (json.contains("id")) {
            uint64_t request_id = json["id"].get<uint64_t>();

            // Check if this is a heartbeat response
            if (json.contains("result") && json["result"].is_string()) {
                std::string result = json["result"].get<std::string>();
                if (result == "ok") {
                    // Heartbeat acknowledgment, ignore
                    return;
                }
            }

            // Check for error
            if (json.contains("error")) {
                auto error = json["error"];
                std::string error_msg = error.value("message", "Unknown error");
                int error_code = error.value("code", 0);
                onError("Deribit error " + std::to_string(error_code) + ": " + error_msg);
                return;
            }

            // Check pending request type
            std::string request_method;
            {
                std::lock_guard<std::mutex> lock(pending_requests_mutex_);
                auto it = pending_requests_.find(request_id);
                if (it != pending_requests_.end()) {
                    request_method = it->second;
                    pending_requests_.erase(it);
                }
            }

            // Handle auth response
            if (request_method == "public/auth" && json.contains("result")) {
                auto result = json["result"];
                {
                    std::lock_guard<std::mutex> lock(auth_mutex_);
                    access_token_ = result.value("access_token", "");
                    refresh_token_ = result.value("refresh_token", "");
                    token_expiry_ = currentTimeMs() + (result.value("expires_in", 900) * 1000);
                }
                authenticated_ = true;
                startTokenRefresh();
            }

            return;
        }

        // Check for test_request (heartbeat request from server)
        if (json.contains("method") && json["method"] == "heartbeat") {
            handleHeartbeat(json);
            return;
        }

        // Check for subscription message
        if (json.contains("method") && json["method"] == "subscription") {
            auto params = json["params"];
            std::string channel = params.value("channel", "");
            auto data = params["data"];

            // Route to appropriate handler based on channel
            if (channel.find("book.") != std::string::npos) {
                handleOrderBookUpdate(data, channel);
            } else if (channel.find("trades.") != std::string::npos) {
                handleTradeUpdate(data);
            } else if (channel.find("ticker.") != std::string::npos) {
                handleTickerUpdate(data);
            } else if (channel.find("user.orders.") != std::string::npos) {
                handleOrderUpdate(data);
            } else if (channel.find("user.trades.") != std::string::npos) {
                // User trade update
                handleTradeUpdate(data);
            } else if (channel.find("user.portfolio.") != std::string::npos) {
                handleAccountUpdate(data);
            } else if (channel.find("user.changes.") != std::string::npos) {
                // Changes include orders, positions, trades
                if (data.contains("orders")) {
                    for (const auto& order : data["orders"]) {
                        handleOrderUpdate(order);
                    }
                }
                if (data.contains("positions")) {
                    for (const auto& position : data["positions"]) {
                        handlePositionUpdate(position);
                    }
                }
            }
        }

    } catch (const std::exception& e) {
        onError("Message parse error: " + std::string(e.what()));
    }
}

void DeribitExchange::handleHeartbeat(const nlohmann::json& json) {
    auto params = json.value("params", nlohmann::json::object());
    std::string type = params.value("type", "");

    if (type == "test_request") {
        // Respond to test_request with public/test
        sendHeartbeat();
    }
}

void DeribitExchange::sendHeartbeat() {
    auto req = buildJsonRpcRequest("public/test");
    ws_client_->send(req);
}

// ============================================================================
// Authentication
// ============================================================================

bool DeribitExchange::authenticate() {
    nlohmann::json params;
    params["grant_type"] = "client_credentials";
    params["client_id"] = deribit_config_.api_key;
    params["client_secret"] = deribit_config_.api_secret;

    auto req = buildJsonRpcRequest("public/auth", params);

    // Track this request
    {
        std::lock_guard<std::mutex> lock(pending_requests_mutex_);
        pending_requests_[req["id"].get<uint64_t>()] = "public/auth";
    }

    ws_client_->send(req);

    // Wait for authentication response (with timeout)
    auto start = std::chrono::steady_clock::now();
    while (!authenticated_.load()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start
        );
        if (elapsed.count() > 5000) {  // 5 second timeout
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return true;
}

void DeribitExchange::startTokenRefresh() {
    if (refresh_running_.load()) {
        return;
    }

    refresh_running_ = true;
    token_refresh_thread_ = std::make_unique<std::thread>([this]() {
        while (refresh_running_.load()) {
            // Check if token needs refresh (5 minutes before expiry)
            uint64_t now = currentTimeMs();
            uint64_t expiry;
            {
                std::lock_guard<std::mutex> lock(auth_mutex_);
                expiry = token_expiry_;
            }

            if (now + 300000 >= expiry) {  // 5 minutes before expiry
                // Refresh token
                nlohmann::json params;
                params["grant_type"] = "refresh_token";
                {
                    std::lock_guard<std::mutex> lock(auth_mutex_);
                    params["refresh_token"] = refresh_token_;
                }

                auto req = buildJsonRpcRequest("public/auth", params);
                {
                    std::lock_guard<std::mutex> lock(pending_requests_mutex_);
                    pending_requests_[req["id"].get<uint64_t>()] = "public/auth";
                }

                if (ws_client_) {
                    ws_client_->send(req);
                }
            }

            // Sleep for 1 minute before checking again
            for (int i = 0; i < 60 && refresh_running_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    });
}

void DeribitExchange::stopTokenRefresh() {
    refresh_running_ = false;
    if (token_refresh_thread_ && token_refresh_thread_->joinable()) {
        token_refresh_thread_->join();
    }
    token_refresh_thread_.reset();
}

// ============================================================================
// Orderbook Handler
// ============================================================================

void DeribitExchange::handleOrderBookUpdate(const nlohmann::json& data, const std::string& channel) {
    try {
        std::string instrument = data.value("instrument_name", "");
        if (instrument.empty()) return;

        std::string type = data.value("type", "snapshot");
        uint64_t change_id = data.value("change_id", uint64_t(0));
        uint64_t prev_change_id = data.value("prev_change_id", uint64_t(0));

        // Check sequence for deltas
        if (type == "change") {
            std::lock_guard<std::mutex> lock(orderbook_seq_mutex_);
            auto it = orderbook_change_id_.find(instrument);
            if (it != orderbook_change_id_.end()) {
                if (prev_change_id != it->second) {
                    // Sequence gap, need to resubscribe
                    onError("Orderbook sequence gap for " + instrument);
                    return;
                }
            }
            orderbook_change_id_[instrument] = change_id;
        } else {
            // Snapshot - reset sequence
            std::lock_guard<std::mutex> lock(orderbook_seq_mutex_);
            orderbook_change_id_[instrument] = change_id;
        }

        OrderBook ob;
        ob.symbol = instrument;
        ob.exchange = "deribit";
        ob.sequence = change_id;
        ob.timestamp = data.value("timestamp", uint64_t(0));
        ob.local_timestamp = currentTimeNs();

        if (type == "snapshot") {
            // Full snapshot
            auto bids = data.value("bids", nlohmann::json::array());
            for (const auto& b : bids) {
                PriceLevel level;
                // Deribit format: [action, price, amount] or [price, amount]
                if (b.is_array()) {
                    if (b.size() >= 2) {
                        if (b[0].is_string()) {
                            // [action, price, amount]
                            level.price = b[1].get<double>();
                            level.quantity = b[2].get<double>();
                        } else {
                            // [price, amount]
                            level.price = b[0].get<double>();
                            level.quantity = b[1].get<double>();
                        }
                        ob.bids.push_back(level);
                    }
                }
            }

            auto asks = data.value("asks", nlohmann::json::array());
            for (const auto& a : asks) {
                PriceLevel level;
                if (a.is_array()) {
                    if (a.size() >= 2) {
                        if (a[0].is_string()) {
                            level.price = a[1].get<double>();
                            level.quantity = a[2].get<double>();
                        } else {
                            level.price = a[0].get<double>();
                            level.quantity = a[1].get<double>();
                        }
                        ob.asks.push_back(level);
                    }
                }
            }

            onOrderBook(ob);

        } else {
            // Delta update
            auto cached = getCachedOrderBook(instrument);
            if (!cached) {
                return;
            }

            ob = *cached;
            ob.sequence = change_id;
            ob.timestamp = data.value("timestamp", uint64_t(0));
            ob.local_timestamp = currentTimeNs();

            auto applyDelta = [](std::vector<PriceLevel>& levels,
                                const nlohmann::json& updates,
                                bool ascending) {
                for (const auto& update : updates) {
                    if (!update.is_array() || update.size() < 3) continue;

                    std::string action = update[0].get<std::string>();
                    double price = update[1].get<double>();
                    double qty = update[2].get<double>();

                    auto it = std::find_if(levels.begin(), levels.end(),
                        [price](const PriceLevel& l) { return l.price == price; });

                    if (action == "delete") {
                        if (it != levels.end()) {
                            levels.erase(it);
                        }
                    } else if (action == "new" || action == "change") {
                        if (it != levels.end()) {
                            it->quantity = qty;
                        } else {
                            PriceLevel level{price, qty, 0};
                            if (ascending) {
                                auto pos = std::lower_bound(levels.begin(), levels.end(), level);
                                levels.insert(pos, level);
                            } else {
                                auto pos = std::lower_bound(levels.begin(), levels.end(), level,
                                    [](const PriceLevel& a, const PriceLevel& b) {
                                        return a.price > b.price;
                                    });
                                levels.insert(pos, level);
                            }
                        }
                    }
                }
            };

            if (data.contains("bids")) {
                applyDelta(ob.bids, data["bids"], false);
            }
            if (data.contains("asks")) {
                applyDelta(ob.asks, data["asks"], true);
            }

            onOrderBook(ob);
        }

    } catch (const std::exception& e) {
        onError("Orderbook update error: " + std::string(e.what()));
    }
}

void DeribitExchange::handleTradeUpdate(const nlohmann::json& data) {
    try {
        // Data can be array of trades or single trade
        std::vector<nlohmann::json> trades_data;
        if (data.is_array()) {
            for (const auto& t : data) {
                trades_data.push_back(t);
            }
        } else {
            trades_data.push_back(data);
        }

        for (const auto& t : trades_data) {
            Trade trade;
            trade.exchange = "deribit";
            trade.symbol = t.value("instrument_name", "");
            trade.trade_id = t.value("trade_id", "");
            trade.price = t.value("price", 0.0);
            trade.quantity = t.value("amount", 0.0);
            trade.timestamp = t.value("timestamp", uint64_t(0));
            trade.local_timestamp = currentTimeNs();

            std::string direction = t.value("direction", "buy");
            trade.side = (direction == "buy") ? OrderSide::Buy : OrderSide::Sell;

            onTrade(trade);
        }

    } catch (const std::exception& e) {
        onError("Trade update error: " + std::string(e.what()));
    }
}

void DeribitExchange::handleTickerUpdate(const nlohmann::json& data) {
    try {
        Ticker ticker;
        ticker.exchange = "deribit";
        ticker.symbol = data.value("instrument_name", "");
        ticker.last = data.value("last_price", 0.0);
        ticker.bid = data.value("best_bid_price", 0.0);
        ticker.ask = data.value("best_ask_price", 0.0);
        ticker.bid_qty = data.value("best_bid_amount", 0.0);
        ticker.ask_qty = data.value("best_ask_amount", 0.0);

        // Per Deribit API docs: 24h stats are in the "stats" object with "high" and "low" fields
        // Note: max_price/min_price are order price bounds, NOT 24h high/low
        auto stats = data.value("stats", nlohmann::json::object());
        ticker.high_24h = stats.value("high", 0.0);
        ticker.low_24h = stats.value("low", 0.0);
        ticker.volume_24h = stats.value("volume", 0.0);
        ticker.change_24h_pct = stats.value("price_change", 0.0);
        ticker.timestamp = data.value("timestamp", uint64_t(0));

        onTicker(ticker);

    } catch (const std::exception& e) {
        onError("Ticker update error: " + std::string(e.what()));
    }
}

void DeribitExchange::handleOrderUpdate(const nlohmann::json& data) {
    try {
        onOrder(parseOrder(data));
    } catch (const std::exception& e) {
        onError("Order update error: " + std::string(e.what()));
    }
}

void DeribitExchange::handlePositionUpdate(const nlohmann::json& data) {
    try {
        Position position;
        position.exchange = "deribit";
        position.symbol = data.value("instrument_name", "");

        double size = data.value("size", 0.0);
        std::string direction = data.value("direction", "buy");

        if (size == 0.0) {
            position.side = OrderSide::Buy;
            position.quantity = 0.0;
        } else {
            position.side = (direction == "buy") ? OrderSide::Buy : OrderSide::Sell;
            position.quantity = std::abs(size);
        }

        position.entry_price = data.value("average_price", 0.0);
        position.mark_price = data.value("mark_price", 0.0);
        position.liquidation_price = data.value("estimated_liquidation_price", 0.0);
        position.unrealized_pnl = data.value("floating_profit_loss", 0.0);
        position.realized_pnl = data.value("realized_profit_loss", 0.0);
        position.leverage = data.value("leverage", 1.0);
        position.margin = data.value("initial_margin", 0.0);
        position.maintenance_margin = data.value("maintenance_margin", 0.0);

        onPosition(position);

    } catch (const std::exception& e) {
        onError("Position update error: " + std::string(e.what()));
    }
}

void DeribitExchange::handleAccountUpdate(const nlohmann::json& data) {
    try {
        std::string currency = data.value("currency", "");

        Balance balance;
        balance.asset = currency;
        balance.free = data.value("available_funds", 0.0);
        balance.locked = data.value("balance", 0.0) - balance.free;

        onBalance(balance);

    } catch (const std::exception& e) {
        onError("Account update error: " + std::string(e.what()));
    }
}

// ============================================================================
// Subscription Management
// ============================================================================

bool DeribitExchange::subscribeOrderBook(const std::string& symbol, int depth) {
    // Deribit orderbook channels: book.{instrument_name}.{interval}
    // interval: 100ms, raw, none (snapshot only)
    std::string channel = "book." + symbol + ".100ms";

    sendSubscribe({channel});

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    subscriptions_.insert(channel);
    return true;
}

bool DeribitExchange::unsubscribeOrderBook(const std::string& symbol) {
    std::vector<std::string> to_unsub;

    {
        std::lock_guard<std::mutex> lock(subscription_mutex_);
        for (const auto& sub : subscriptions_) {
            if (sub.find("book." + symbol) != std::string::npos) {
                to_unsub.push_back(sub);
            }
        }
        for (const auto& sub : to_unsub) {
            subscriptions_.erase(sub);
        }
    }

    if (!to_unsub.empty()) {
        sendUnsubscribe(to_unsub);
    }

    clearOrderBookCache(symbol);
    return true;
}

bool DeribitExchange::subscribeTrades(const std::string& symbol) {
    // trades.{instrument_name}.{interval}
    std::string channel = "trades." + symbol + ".100ms";

    sendSubscribe({channel});

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    subscriptions_.insert(channel);
    return true;
}

bool DeribitExchange::unsubscribeTrades(const std::string& symbol) {
    std::string channel = "trades." + symbol + ".100ms";
    sendUnsubscribe({channel});

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    subscriptions_.erase(channel);
    return true;
}

bool DeribitExchange::subscribeTicker(const std::string& symbol) {
    // ticker.{instrument_name}.{interval}
    std::string channel = "ticker." + symbol + ".100ms";

    sendSubscribe({channel});

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    subscriptions_.insert(channel);
    return true;
}

bool DeribitExchange::unsubscribeTicker(const std::string& symbol) {
    std::string channel = "ticker." + symbol + ".100ms";
    sendUnsubscribe({channel});

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    subscriptions_.erase(channel);
    return true;
}

bool DeribitExchange::subscribeUserData() {
    if (!authenticated_.load()) {
        onError("Not authenticated, cannot subscribe to user data");
        return false;
    }

    // Subscribe to user channels
    std::vector<std::string> channels;

    // User orders for all instruments
    channels.push_back("user.orders." + deribit_config_.currency + ".raw");

    // User trades
    channels.push_back("user.trades." + deribit_config_.currency + ".raw");

    // User portfolio (account updates)
    channels.push_back("user.portfolio." + deribit_config_.currency);

    // User changes (combined orders, positions, trades)
    channels.push_back("user.changes." + deribit_config_.currency + ".raw");

    // Use private/subscribe for authenticated channels
    nlohmann::json params;
    params["channels"] = channels;

    auto req = buildJsonRpcRequest("private/subscribe", params);
    ws_client_->send(req);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    for (const auto& ch : channels) {
        subscriptions_.insert(ch);
    }

    return true;
}

bool DeribitExchange::unsubscribeUserData() {
    std::vector<std::string> user_channels;

    {
        std::lock_guard<std::mutex> lock(subscription_mutex_);
        for (const auto& sub : subscriptions_) {
            if (sub.find("user.") != std::string::npos) {
                user_channels.push_back(sub);
            }
        }
        for (const auto& ch : user_channels) {
            subscriptions_.erase(ch);
        }
    }

    if (!user_channels.empty()) {
        nlohmann::json params;
        params["channels"] = user_channels;
        auto req = buildJsonRpcRequest("private/unsubscribe", params);
        ws_client_->send(req);
    }

    return true;
}

void DeribitExchange::sendSubscribe(const std::vector<std::string>& channels) {
    if (!ws_client_ || !ws_connected_.load()) {
        return;
    }

    nlohmann::json params;
    params["channels"] = channels;

    auto req = buildJsonRpcRequest("public/subscribe", params);
    ws_client_->send(req);
}

void DeribitExchange::sendUnsubscribe(const std::vector<std::string>& channels) {
    if (!ws_client_ || !ws_connected_.load()) {
        return;
    }

    nlohmann::json params;
    params["channels"] = channels;

    auto req = buildJsonRpcRequest("public/unsubscribe", params);
    ws_client_->send(req);
}

// ============================================================================
// JSON-RPC Helpers
// ============================================================================

nlohmann::json DeribitExchange::buildJsonRpcRequest(const std::string& method,
                                                    const nlohmann::json& params) {
    nlohmann::json req;
    req["jsonrpc"] = "2.0";
    req["id"] = getNextRequestId();
    req["method"] = method;

    if (!params.empty()) {
        req["params"] = params;
    }

    // Add access token for private methods
    if (method.find("private/") != std::string::npos && !access_token_.empty()) {
        req["params"]["access_token"] = access_token_;
    }

    return req;
}

uint64_t DeribitExchange::getNextRequestId() {
    return next_request_id_++;
}

// ============================================================================
// Order Management
// ============================================================================

std::optional<Order> DeribitExchange::placeOrder(const OrderRequest& request) {
    if (!authenticated_.load()) {
        onError("Not authenticated, cannot place order");
        return std::nullopt;
    }

    nlohmann::json params;
    params["instrument_name"] = request.symbol;
    params["amount"] = request.quantity;
    params["type"] = orderTypeToString(request.type);

    if (request.type != OrderType::Market) {
        params["price"] = request.price;
    }

    if (!request.client_order_id.empty()) {
        params["label"] = request.client_order_id;
    }

    if (request.reduce_only) {
        params["reduce_only"] = true;
    }

    if (request.post_only) {
        params["post_only"] = true;
    }

    params["time_in_force"] = timeInForceToString(request.time_in_force);

    // Determine endpoint based on order side
    std::string method = (request.side == OrderSide::Buy)
        ? "private/buy"
        : "private/sell";

    // Make REST call (synchronous for order placement)
    nlohmann::json rpc_request;
    rpc_request["jsonrpc"] = "2.0";
    rpc_request["id"] = getNextRequestId();
    rpc_request["method"] = method;
    rpc_request["params"] = params;

    // Add authorization header
    // Note: base_url already includes /api/v2, so path should be relative to that
    network::HttpRequest http_req;
    http_req.method = network::HttpMethod::POST;
    http_req.path = "/" + method;
    http_req.body = rpc_request.dump();
    http_req.headers["Content-Type"] = "application/json";
    http_req.headers["Authorization"] = "Bearer " + access_token_;

    auto response = rest_client_->execute(http_req);

    if (!response.success) {
        onError("Place order failed: " + response.error + " " + response.body);
        return std::nullopt;
    }

    try {
        auto json = response.json();

        if (json.contains("error")) {
            onError("Place order failed: " + json["error"].value("message", "unknown"));
            return std::nullopt;
        }

        auto result = json["result"];
        auto order_data = result["order"];
        return parseOrder(order_data);

    } catch (const std::exception& e) {
        onError("Parse order response failed: " + std::string(e.what()));
        return std::nullopt;
    }
}

std::optional<Order> DeribitExchange::cancelOrder(const std::string& symbol,
                                                   const std::string& order_id) {
    if (!authenticated_.load()) {
        onError("Not authenticated, cannot cancel order");
        return std::nullopt;
    }

    nlohmann::json params;
    params["order_id"] = order_id;

    nlohmann::json rpc_request;
    rpc_request["jsonrpc"] = "2.0";
    rpc_request["id"] = getNextRequestId();
    rpc_request["method"] = "private/cancel";
    rpc_request["params"] = params;

    network::HttpRequest http_req;
    http_req.method = network::HttpMethod::POST;
    http_req.path = "/private/cancel";
    http_req.body = rpc_request.dump();
    http_req.headers["Content-Type"] = "application/json";
    http_req.headers["Authorization"] = "Bearer " + access_token_;

    auto response = rest_client_->execute(http_req);

    if (!response.success) {
        onError("Cancel order failed: " + response.error);
        return std::nullopt;
    }

    try {
        auto json = response.json();

        if (json.contains("error")) {
            onError("Cancel order failed: " + json["error"].value("message", "unknown"));
            return std::nullopt;
        }

        auto result = json["result"];
        return parseOrder(result);

    } catch (...) {
        return std::nullopt;
    }
}

std::optional<Order> DeribitExchange::cancelOrder(const std::string& symbol,
                                                   const std::string& order_id,
                                                   const std::string& client_order_id) {
    // Deribit supports canceling by order_id or label (client_order_id)
    if (!order_id.empty()) {
        return cancelOrder(symbol, order_id);
    }

    if (!authenticated_.load() || client_order_id.empty()) {
        return std::nullopt;
    }

    nlohmann::json params;
    params["label"] = client_order_id;

    nlohmann::json rpc_request;
    rpc_request["jsonrpc"] = "2.0";
    rpc_request["id"] = getNextRequestId();
    rpc_request["method"] = "private/cancel_by_label";
    rpc_request["params"] = params;

    network::HttpRequest http_req;
    http_req.method = network::HttpMethod::POST;
    http_req.path = "/private/cancel_by_label";
    http_req.body = rpc_request.dump();
    http_req.headers["Content-Type"] = "application/json";
    http_req.headers["Authorization"] = "Bearer " + access_token_;

    auto response = rest_client_->execute(http_req);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.contains("error")) {
            return std::nullopt;
        }

        Order order;
        order.client_order_id = client_order_id;
        order.symbol = symbol;
        order.status = OrderStatus::Cancelled;
        return order;

    } catch (...) {
        return std::nullopt;
    }
}

bool DeribitExchange::cancelAllOrders(const std::string& symbol) {
    if (!authenticated_.load()) {
        onError("Not authenticated, cannot cancel orders");
        return false;
    }

    nlohmann::json params;
    if (!symbol.empty()) {
        params["instrument_name"] = symbol;
    } else {
        params["currency"] = deribit_config_.currency;
    }

    nlohmann::json rpc_request;
    rpc_request["jsonrpc"] = "2.0";
    rpc_request["id"] = getNextRequestId();
    rpc_request["method"] = "private/cancel_all";
    rpc_request["params"] = params;

    network::HttpRequest http_req;
    http_req.method = network::HttpMethod::POST;
    http_req.path = "/private/cancel_all";
    http_req.body = rpc_request.dump();
    http_req.headers["Content-Type"] = "application/json";
    http_req.headers["Authorization"] = "Bearer " + access_token_;

    auto response = rest_client_->execute(http_req);
    return response.success && !response.json().contains("error");
}

std::optional<Order> DeribitExchange::getOrder(const std::string& symbol,
                                                const std::string& order_id) {
    if (!authenticated_.load()) {
        return std::nullopt;
    }

    nlohmann::json params;
    params["order_id"] = order_id;

    nlohmann::json rpc_request;
    rpc_request["jsonrpc"] = "2.0";
    rpc_request["id"] = getNextRequestId();
    rpc_request["method"] = "private/get_order_state";
    rpc_request["params"] = params;

    network::HttpRequest http_req;
    http_req.method = network::HttpMethod::POST;
    http_req.path = "/private/get_order_state";
    http_req.body = rpc_request.dump();
    http_req.headers["Content-Type"] = "application/json";
    http_req.headers["Authorization"] = "Bearer " + access_token_;

    auto response = rest_client_->execute(http_req);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.contains("error")) {
            return std::nullopt;
        }

        return parseOrder(json["result"]);

    } catch (...) {
        return std::nullopt;
    }
}

std::vector<Order> DeribitExchange::getOpenOrders(const std::string& symbol) {
    std::vector<Order> orders;

    if (!authenticated_.load()) {
        return orders;
    }

    nlohmann::json params;
    if (!symbol.empty()) {
        params["instrument_name"] = symbol;
    } else {
        params["currency"] = deribit_config_.currency;
    }

    nlohmann::json rpc_request;
    rpc_request["jsonrpc"] = "2.0";
    rpc_request["id"] = getNextRequestId();
    rpc_request["method"] = "private/get_open_orders_by_currency";
    rpc_request["params"] = params;

    network::HttpRequest http_req;
    http_req.method = network::HttpMethod::POST;
    http_req.path = "/private/get_open_orders_by_currency";
    http_req.body = rpc_request.dump();
    http_req.headers["Content-Type"] = "application/json";
    http_req.headers["Authorization"] = "Bearer " + access_token_;

    auto response = rest_client_->execute(http_req);

    if (!response.success) {
        return orders;
    }

    try {
        auto json = response.json();
        if (json.contains("error")) {
            return orders;
        }

        auto result = json["result"];
        for (const auto& item : result) {
            orders.push_back(parseOrder(item));
        }

    } catch (...) {
        // Return empty
    }

    return orders;
}

std::vector<Order> DeribitExchange::getOrderHistory(const std::string& symbol,
                                                     uint64_t start_time,
                                                     uint64_t end_time,
                                                     int limit) {
    std::vector<Order> orders;

    if (!authenticated_.load()) {
        return orders;
    }

    nlohmann::json params;
    if (!symbol.empty()) {
        params["instrument_name"] = symbol;
    } else {
        params["currency"] = deribit_config_.currency;
    }
    params["count"] = limit;

    if (start_time > 0) {
        params["start_timestamp"] = start_time;
    }
    if (end_time > 0) {
        params["end_timestamp"] = end_time;
    }

    nlohmann::json rpc_request;
    rpc_request["jsonrpc"] = "2.0";
    rpc_request["id"] = getNextRequestId();
    rpc_request["method"] = "private/get_order_history_by_currency";
    rpc_request["params"] = params;

    network::HttpRequest http_req;
    http_req.method = network::HttpMethod::POST;
    http_req.path = "/private/get_order_history_by_currency";
    http_req.body = rpc_request.dump();
    http_req.headers["Content-Type"] = "application/json";
    http_req.headers["Authorization"] = "Bearer " + access_token_;

    auto response = rest_client_->execute(http_req);

    if (!response.success) {
        return orders;
    }

    try {
        auto json = response.json();
        if (json.contains("error")) {
            return orders;
        }

        auto result = json["result"];
        for (const auto& item : result) {
            orders.push_back(parseOrder(item));
        }

    } catch (...) {
        // Return empty
    }

    return orders;
}

// ============================================================================
// Account Information
// ============================================================================

std::optional<Account> DeribitExchange::getAccount() {
    if (!authenticated_.load()) {
        return std::nullopt;
    }

    nlohmann::json params;
    params["currency"] = deribit_config_.currency;
    params["extended"] = true;

    nlohmann::json rpc_request;
    rpc_request["jsonrpc"] = "2.0";
    rpc_request["id"] = getNextRequestId();
    rpc_request["method"] = "private/get_account_summary";
    rpc_request["params"] = params;

    network::HttpRequest http_req;
    http_req.method = network::HttpMethod::POST;
    http_req.path = "/private/get_account_summary";
    http_req.body = rpc_request.dump();
    http_req.headers["Content-Type"] = "application/json";
    http_req.headers["Authorization"] = "Bearer " + access_token_;

    auto response = rest_client_->execute(http_req);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.contains("error")) {
            return std::nullopt;
        }

        auto result = json["result"];

        Account account;
        account.exchange = "deribit";
        account.update_time = currentTimeMs();

        // Parse balance
        Balance balance;
        balance.asset = result.value("currency", deribit_config_.currency);
        balance.free = result.value("available_funds", 0.0);
        balance.locked = result.value("balance", 0.0) - balance.free;
        account.balances[balance.asset] = balance;

        account.total_margin = result.value("equity", 0.0);
        account.available_margin = result.value("available_funds", 0.0);
        account.total_unrealized_pnl = result.value("futures_pl", 0.0) +
                                       result.value("options_pl", 0.0);

        // Get positions
        account.positions = getPositions();

        return account;

    } catch (const std::exception& e) {
        onError("Parse account failed: " + std::string(e.what()));
        return std::nullopt;
    }
}

std::optional<Balance> DeribitExchange::getBalance(const std::string& asset) {
    auto account = getAccount();
    if (!account) {
        return std::nullopt;
    }

    auto it = account->balances.find(asset);
    if (it != account->balances.end()) {
        return it->second;
    }

    return std::nullopt;
}

std::vector<Position> DeribitExchange::getPositions(const std::string& symbol) {
    std::vector<Position> positions;

    if (!authenticated_.load()) {
        return positions;
    }

    nlohmann::json params;
    params["currency"] = deribit_config_.currency;

    nlohmann::json rpc_request;
    rpc_request["jsonrpc"] = "2.0";
    rpc_request["id"] = getNextRequestId();
    rpc_request["method"] = "private/get_positions";
    rpc_request["params"] = params;

    network::HttpRequest http_req;
    http_req.method = network::HttpMethod::POST;
    http_req.path = "/private/get_positions";
    http_req.body = rpc_request.dump();
    http_req.headers["Content-Type"] = "application/json";
    http_req.headers["Authorization"] = "Bearer " + access_token_;

    auto response = rest_client_->execute(http_req);

    if (!response.success) {
        return positions;
    }

    try {
        auto json = response.json();
        if (json.contains("error")) {
            return positions;
        }

        auto result = json["result"];
        for (const auto& p : result) {
            double size = p.value("size", 0.0);
            if (size == 0.0 && symbol.empty()) continue;

            // Filter by symbol if specified
            std::string inst_name = p.value("instrument_name", "");
            if (!symbol.empty() && inst_name != symbol) {
                continue;
            }

            Position position;
            position.exchange = "deribit";
            position.symbol = inst_name;

            std::string direction = p.value("direction", "buy");
            position.side = (direction == "buy") ? OrderSide::Buy : OrderSide::Sell;
            position.quantity = std::abs(size);
            position.entry_price = p.value("average_price", 0.0);
            position.mark_price = p.value("mark_price", 0.0);
            position.liquidation_price = p.value("estimated_liquidation_price", 0.0);
            position.unrealized_pnl = p.value("floating_profit_loss", 0.0);
            position.realized_pnl = p.value("realized_profit_loss", 0.0);
            position.leverage = p.value("leverage", 1.0);
            position.margin = p.value("initial_margin", 0.0);
            position.maintenance_margin = p.value("maintenance_margin", 0.0);

            positions.push_back(position);
        }

    } catch (...) {
        // Return empty
    }

    return positions;
}

// ============================================================================
// Market Information
// ============================================================================

std::vector<SymbolInfo> DeribitExchange::getSymbols() {
    std::vector<SymbolInfo> symbols;

    nlohmann::json params;
    params["currency"] = deribit_config_.currency;
    params["kind"] = "future";  // or "option"

    nlohmann::json rpc_request;
    rpc_request["jsonrpc"] = "2.0";
    rpc_request["id"] = getNextRequestId();
    rpc_request["method"] = "public/get_instruments";
    rpc_request["params"] = params;

    network::HttpRequest http_req;
    http_req.method = network::HttpMethod::POST;
    http_req.path = "/public/get_instruments";
    http_req.body = rpc_request.dump();
    http_req.headers["Content-Type"] = "application/json";

    auto response = rest_client_->execute(http_req);

    if (!response.success) {
        return symbols;
    }

    try {
        auto json = response.json();
        if (json.contains("error")) {
            return symbols;
        }

        auto result = json["result"];
        for (const auto& item : result) {
            auto info = parseSymbolInfo(item);
            updateSymbolInfo(info);
            symbols.push_back(info);
        }

        // Also fetch options
        params["kind"] = "option";
        rpc_request["params"] = params;
        rpc_request["id"] = getNextRequestId();
        http_req.body = rpc_request.dump();

        response = rest_client_->execute(http_req);
        if (response.success) {
            json = response.json();
            if (!json.contains("error")) {
                result = json["result"];
                for (const auto& item : result) {
                    auto info = parseSymbolInfo(item);
                    updateSymbolInfo(info);
                    symbols.push_back(info);
                }
            }
        }

    } catch (...) {
        // Return what we have
    }

    return symbols;
}

std::optional<SymbolInfo> DeribitExchange::getSymbolInfo(const std::string& symbol) {
    // Check cache first
    {
        std::lock_guard<std::mutex> lock(symbol_info_mutex_);
        auto it = symbol_info_cache_.find(symbol);
        if (it != symbol_info_cache_.end()) {
            return it->second;
        }
    }

    nlohmann::json params;
    params["instrument_name"] = symbol;

    nlohmann::json rpc_request;
    rpc_request["jsonrpc"] = "2.0";
    rpc_request["id"] = getNextRequestId();
    rpc_request["method"] = "public/get_instrument";
    rpc_request["params"] = params;

    network::HttpRequest http_req;
    http_req.method = network::HttpMethod::POST;
    http_req.path = "/public/get_instrument";
    http_req.body = rpc_request.dump();
    http_req.headers["Content-Type"] = "application/json";

    auto response = rest_client_->execute(http_req);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.contains("error")) {
            return std::nullopt;
        }

        return parseSymbolInfo(json["result"]);

    } catch (...) {
        return std::nullopt;
    }
}

std::optional<OrderBook> DeribitExchange::getOrderBookSnapshot(const std::string& symbol, int depth) {
    nlohmann::json params;
    params["instrument_name"] = symbol;
    params["depth"] = depth;

    nlohmann::json rpc_request;
    rpc_request["jsonrpc"] = "2.0";
    rpc_request["id"] = getNextRequestId();
    rpc_request["method"] = "public/get_order_book";
    rpc_request["params"] = params;

    network::HttpRequest http_req;
    http_req.method = network::HttpMethod::POST;
    http_req.path = "/public/get_order_book";
    http_req.body = rpc_request.dump();
    http_req.headers["Content-Type"] = "application/json";

    auto response = rest_client_->execute(http_req);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.contains("error")) {
            return std::nullopt;
        }

        auto result = json["result"];

        OrderBook ob;
        ob.symbol = symbol;
        ob.exchange = "deribit";
        ob.sequence = result.value("change_id", uint64_t(0));
        ob.timestamp = result.value("timestamp", uint64_t(0));
        ob.local_timestamp = currentTimeNs();

        auto bids = result.value("bids", nlohmann::json::array());
        for (const auto& b : bids) {
            if (b.is_array() && b.size() >= 2) {
                PriceLevel level;
                level.price = b[0].get<double>();
                level.quantity = b[1].get<double>();
                ob.bids.push_back(level);
            }
        }

        auto asks = result.value("asks", nlohmann::json::array());
        for (const auto& a : asks) {
            if (a.is_array() && a.size() >= 2) {
                PriceLevel level;
                level.price = a[0].get<double>();
                level.quantity = a[1].get<double>();
                ob.asks.push_back(level);
            }
        }

        return ob;

    } catch (...) {
        return std::nullopt;
    }
}

std::vector<Trade> DeribitExchange::getRecentTrades(const std::string& symbol, int limit) {
    std::vector<Trade> trades;

    nlohmann::json params;
    params["instrument_name"] = symbol;
    params["count"] = limit;

    nlohmann::json rpc_request;
    rpc_request["jsonrpc"] = "2.0";
    rpc_request["id"] = getNextRequestId();
    rpc_request["method"] = "public/get_last_trades_by_instrument";
    rpc_request["params"] = params;

    network::HttpRequest http_req;
    http_req.method = network::HttpMethod::POST;
    http_req.path = "/public/get_last_trades_by_instrument";
    http_req.body = rpc_request.dump();
    http_req.headers["Content-Type"] = "application/json";

    auto response = rest_client_->execute(http_req);

    if (!response.success) {
        return trades;
    }

    try {
        auto json = response.json();
        if (json.contains("error")) {
            return trades;
        }

        auto result = json["result"]["trades"];
        for (const auto& t : result) {
            Trade trade;
            trade.exchange = "deribit";
            trade.symbol = symbol;
            trade.trade_id = t.value("trade_id", "");
            trade.price = t.value("price", 0.0);
            trade.quantity = t.value("amount", 0.0);
            trade.timestamp = t.value("timestamp", uint64_t(0));
            trade.side = t.value("direction", "buy") == "buy" ? OrderSide::Buy : OrderSide::Sell;
            trades.push_back(trade);
        }

    } catch (...) {
        // Return empty
    }

    return trades;
}

std::optional<Ticker> DeribitExchange::getTicker(const std::string& symbol) {
    nlohmann::json params;
    params["instrument_name"] = symbol;

    nlohmann::json rpc_request;
    rpc_request["jsonrpc"] = "2.0";
    rpc_request["id"] = getNextRequestId();
    rpc_request["method"] = "public/ticker";
    rpc_request["params"] = params;

    network::HttpRequest http_req;
    http_req.method = network::HttpMethod::POST;
    http_req.path = "/public/ticker";
    http_req.body = rpc_request.dump();
    http_req.headers["Content-Type"] = "application/json";

    auto response = rest_client_->execute(http_req);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.contains("error")) {
            return std::nullopt;
        }

        auto result = json["result"];

        Ticker ticker;
        ticker.exchange = "deribit";
        ticker.symbol = symbol;
        ticker.last = result.value("last_price", 0.0);
        ticker.bid = result.value("best_bid_price", 0.0);
        ticker.ask = result.value("best_ask_price", 0.0);
        ticker.bid_qty = result.value("best_bid_amount", 0.0);
        ticker.ask_qty = result.value("best_ask_amount", 0.0);

        // Per Deribit API docs: 24h stats are in the "stats" object
        auto stats = result.value("stats", nlohmann::json::object());
        ticker.high_24h = stats.value("high", 0.0);
        ticker.low_24h = stats.value("low", 0.0);
        ticker.volume_24h = stats.value("volume", 0.0);
        ticker.change_24h_pct = stats.value("price_change", 0.0);
        ticker.timestamp = result.value("timestamp", uint64_t(0));

        return ticker;

    } catch (...) {
        return std::nullopt;
    }
}

uint64_t DeribitExchange::getServerTime() {
    nlohmann::json rpc_request;
    rpc_request["jsonrpc"] = "2.0";
    rpc_request["id"] = getNextRequestId();
    rpc_request["method"] = "public/get_time";

    network::HttpRequest http_req;
    http_req.method = network::HttpMethod::POST;
    http_req.path = "/public/get_time";
    http_req.body = rpc_request.dump();
    http_req.headers["Content-Type"] = "application/json";

    auto response = rest_client_->execute(http_req);

    if (response.success) {
        auto json = response.json();
        return json.value("result", uint64_t(0));
    }

    return 0;
}

std::string DeribitExchange::formatSymbol(const std::string& base, const std::string& quote) const {
    // Deribit format: BTC-PERPETUAL, BTC-25DEC20, BTC-25DEC20-36000-C (options)
    if (quote == "PERPETUAL") {
        return base + "-PERPETUAL";
    }
    return base + "-" + quote;
}

std::pair<std::string, std::string> DeribitExchange::parseSymbol(const std::string& symbol) const {
    // Parse BTC-PERPETUAL, BTC-25DEC20, etc.
    auto pos = symbol.find('-');
    if (pos != std::string::npos) {
        return {symbol.substr(0, pos), symbol.substr(pos + 1)};
    }
    return {symbol, ""};
}

std::string DeribitExchange::getCurrency(const std::string& symbol) const {
    auto parsed = parseSymbol(symbol);
    return parsed.first;  // BTC or ETH
}

// ============================================================================
// Data Converters
// ============================================================================

Order DeribitExchange::parseOrder(const nlohmann::json& data) {
    Order order;
    order.exchange = "deribit";
    order.symbol = data.value("instrument_name", "");
    order.order_id = data.value("order_id", "");
    order.client_order_id = data.value("label", "");
    order.side = parseOrderSide(data.value("direction", "buy"));
    order.type = parseOrderType(data.value("order_type", "limit"));
    order.status = parseOrderStatus(data.value("order_state", "open"));
    order.time_in_force = parseTimeInForce(data.value("time_in_force", "good_til_cancelled"));

    order.quantity = data.value("amount", 0.0);
    order.filled_quantity = data.value("filled_amount", 0.0);
    order.remaining_quantity = order.quantity - order.filled_quantity;
    order.price = data.value("price", 0.0);
    order.average_price = data.value("average_price", 0.0);
    order.commission = data.value("commission", 0.0);

    order.create_time = data.value("creation_timestamp", uint64_t(0));
    order.update_time = data.value("last_update_timestamp", uint64_t(0));
    order.reduce_only = data.value("reduce_only", false);
    order.raw = data;

    return order;
}

OrderStatus DeribitExchange::parseOrderStatus(const std::string& status) {
    // Per Deribit API docs, valid order_state values are:
    // open, filled, rejected, cancelled, untriggered, triggered
    if (status == "open") return OrderStatus::New;
    if (status == "filled") return OrderStatus::Filled;
    if (status == "cancelled") return OrderStatus::Cancelled;
    if (status == "rejected") return OrderStatus::Rejected;
    if (status == "untriggered") return OrderStatus::Pending;
    if (status == "triggered") return OrderStatus::New;  // Triggered orders become active
    return OrderStatus::Failed;
}

OrderSide DeribitExchange::parseOrderSide(const std::string& side) {
    return (side == "buy") ? OrderSide::Buy : OrderSide::Sell;
}

OrderType DeribitExchange::parseOrderType(const std::string& type) {
    if (type == "market") return OrderType::Market;
    if (type == "limit") return OrderType::Limit;
    if (type == "stop_market") return OrderType::StopMarket;
    if (type == "stop_limit") return OrderType::StopLimit;
    return OrderType::Limit;
}

TimeInForce DeribitExchange::parseTimeInForce(const std::string& tif) {
    if (tif == "good_til_cancelled") return TimeInForce::GTC;
    if (tif == "immediate_or_cancel") return TimeInForce::IOC;
    if (tif == "fill_or_kill") return TimeInForce::FOK;
    return TimeInForce::GTC;
}

std::string DeribitExchange::orderSideToString(OrderSide side) {
    return side == OrderSide::Buy ? "buy" : "sell";
}

std::string DeribitExchange::orderTypeToString(OrderType type) {
    switch (type) {
        case OrderType::Market: return "market";
        case OrderType::Limit: return "limit";
        case OrderType::StopMarket: return "stop_market";
        case OrderType::StopLimit: return "stop_limit";
        default: return "limit";
    }
}

std::string DeribitExchange::timeInForceToString(TimeInForce tif) {
    switch (tif) {
        case TimeInForce::GTC: return "good_til_cancelled";
        case TimeInForce::IOC: return "immediate_or_cancel";
        case TimeInForce::FOK: return "fill_or_kill";
        default: return "good_til_cancelled";
    }
}

SymbolInfo DeribitExchange::parseSymbolInfo(const nlohmann::json& data) {
    SymbolInfo info;
    info.symbol = data.value("instrument_name", "");
    info.base_asset = data.value("base_currency", "");
    info.quote_asset = data.value("quote_currency", "USD");

    std::string kind = data.value("kind", "future");
    if (kind == "future") {
        info.type = ExchangeType::Futures;
    } else if (kind == "option") {
        info.type = ExchangeType::Options;
    } else {
        info.type = ExchangeType::Perpetual;
    }

    info.min_qty = data.value("min_trade_amount", 0.0);
    info.tick_size = data.value("tick_size", 0.0);
    info.step_size = data.value("min_trade_amount", 0.0);
    info.trading_enabled = data.value("is_active", true);

    // Calculate precision from tick size
    auto calcPrecision = [](double step) -> int {
        if (step == 0.0) return 8;
        int precision = 0;
        while (step < 1.0 && precision < 16) {
            step *= 10.0;
            precision++;
        }
        return precision;
    };

    info.price_precision = calcPrecision(info.tick_size);
    info.qty_precision = calcPrecision(info.step_size);

    return info;
}

// ============================================================================
// Deribit-specific Methods
// ============================================================================

bool DeribitExchange::setLeverage(const std::string& symbol, double leverage) {
    if (!authenticated_.load()) {
        return false;
    }

    nlohmann::json params;
    params["instrument_name"] = symbol;
    params["leverage"] = leverage;

    nlohmann::json rpc_request;
    rpc_request["jsonrpc"] = "2.0";
    rpc_request["id"] = getNextRequestId();
    rpc_request["method"] = "private/set_leverage";
    rpc_request["params"] = params;

    network::HttpRequest http_req;
    http_req.method = network::HttpMethod::POST;
    http_req.path = "/private/set_leverage";
    http_req.body = rpc_request.dump();
    http_req.headers["Content-Type"] = "application/json";
    http_req.headers["Authorization"] = "Bearer " + access_token_;

    auto response = rest_client_->execute(http_req);
    return response.success && !response.json().contains("error");
}

double DeribitExchange::getFundingRate(const std::string& symbol) {
    nlohmann::json params;
    params["instrument_name"] = symbol;

    nlohmann::json rpc_request;
    rpc_request["jsonrpc"] = "2.0";
    rpc_request["id"] = getNextRequestId();
    rpc_request["method"] = "public/get_funding_rate_value";
    rpc_request["params"] = params;

    network::HttpRequest http_req;
    http_req.method = network::HttpMethod::POST;
    http_req.path = "/public/get_funding_rate_value";
    http_req.body = rpc_request.dump();
    http_req.headers["Content-Type"] = "application/json";

    auto response = rest_client_->execute(http_req);

    if (response.success) {
        auto json = response.json();
        if (!json.contains("error")) {
            return json.value("result", 0.0);
        }
    }

    return 0.0;
}

// ============================================================================
// Factory Functions
// ============================================================================

std::shared_ptr<DeribitExchange> createDeribitExchange(const ExchangeConfig& config) {
    return std::make_shared<DeribitExchange>(config);
}

std::shared_ptr<DeribitExchange> createDeribitExchange(const DeribitConfig& config) {
    return std::make_shared<DeribitExchange>(config);
}

} // namespace exchange
} // namespace hft
