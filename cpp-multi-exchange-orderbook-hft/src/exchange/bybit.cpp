#include "exchange/bybit.hpp"

#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/evp.h>

#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>

namespace hft {
namespace exchange {

// ============================================================================
// Constructors/Destructor
// ============================================================================

BybitExchange::BybitExchange(const ExchangeConfig& config)
    : ExchangeBase(config) {
    bybit_config_.api_key = config.api_key;
    bybit_config_.api_secret = config.api_secret;
    bybit_config_.testnet = config.testnet;

    switch (config.type) {
        case ExchangeType::Spot:
            bybit_config_.category = BybitCategory::Spot;
            break;
        case ExchangeType::Perpetual:
        case ExchangeType::Futures:
            bybit_config_.category = BybitCategory::Linear;
            break;
        default:
            bybit_config_.category = BybitCategory::Linear;
    }

    bybit_config_.order_rate_limit = config.orders_per_second;
    bybit_config_.request_rate_limit = config.requests_per_minute;

    // Initialize REST client
    network::RestClientConfig rest_config;
    rest_config.base_url = getRestUrl();
    rest_config.name = "bybit_rest";
    rest_config.request_timeout_ms = config.rest_timeout_ms;
    rest_config.max_retries = config.rest_max_retries;
    rest_config.rate_limit.requests_per_second = bybit_config_.order_rate_limit;
    rest_config.rate_limit.requests_per_minute = bybit_config_.request_rate_limit;

    rest_client_ = std::make_shared<network::RestClient>(rest_config);
}

BybitExchange::BybitExchange(const BybitConfig& bybit_config)
    : ExchangeBase(ExchangeConfig{
        .name = "Bybit",
        .api_key = bybit_config.api_key,
        .api_secret = bybit_config.api_secret,
        .type = bybit_config.category == BybitCategory::Spot ? ExchangeType::Spot : ExchangeType::Perpetual,
        .testnet = bybit_config.testnet
      }),
      bybit_config_(bybit_config) {

    // Initialize REST client
    network::RestClientConfig rest_config;
    rest_config.base_url = getRestUrl();
    rest_config.name = "bybit_rest";
    rest_config.rate_limit.requests_per_second = bybit_config_.order_rate_limit;
    rest_config.rate_limit.requests_per_minute = bybit_config_.request_rate_limit;

    rest_client_ = std::make_shared<network::RestClient>(rest_config);
}

BybitExchange::~BybitExchange() {
    disconnect();
}

// ============================================================================
// URL Builders
// ============================================================================

std::string BybitExchange::getRestUrl() const {
    return bybit_config_.testnet
        ? "https://api-testnet.bybit.com"
        : "https://api.bybit.com";
}

std::string BybitExchange::getPublicWsUrl() const {
    std::string base = bybit_config_.testnet
        ? "wss://stream-testnet.bybit.com/v5/public/"
        : "wss://stream.bybit.com/v5/public/";

    switch (bybit_config_.category) {
        case BybitCategory::Spot:
            return base + "spot";
        case BybitCategory::Linear:
            return base + "linear";
        case BybitCategory::Inverse:
            return base + "inverse";
        case BybitCategory::Option:
            return base + "option";
        default:
            return base + "linear";
    }
}

std::string BybitExchange::getPrivateWsUrl() const {
    return bybit_config_.testnet
        ? "wss://stream-testnet.bybit.com/v5/private"
        : "wss://stream.bybit.com/v5/private";
}

std::string BybitExchange::getCategoryString() const {
    switch (bybit_config_.category) {
        case BybitCategory::Spot: return "spot";
        case BybitCategory::Linear: return "linear";
        case BybitCategory::Inverse: return "inverse";
        case BybitCategory::Option: return "option";
        default: return "linear";
    }
}

// ============================================================================
// Connection Management
// ============================================================================

bool BybitExchange::connect() {
    if (public_ws_connected_.load()) {
        return true;
    }

    onConnectionStatus(ConnectionStatus::Connecting);

    // Initialize public WebSocket client
    network::WebSocketConfig ws_config;
    ws_config.url = getPublicWsUrl();
    ws_config.name = "bybit_public_ws";
    ws_config.ping_interval_ms = 20000;  // Bybit requires ping every 20s
    ws_config.auto_reconnect = true;
    ws_config.reconnect_initial_delay_ms = config_.ws_reconnect_delay_ms;

    public_ws_client_ = std::make_shared<network::WebSocketClient>(ws_config);

    public_ws_client_->setOnOpen([this]() {
        this->handleWsOpen(false);
    });

    public_ws_client_->setOnClose([this](int code, const std::string& reason) {
        this->handleWsClose(code, reason, false);
    });

    public_ws_client_->setOnMessage([this](const std::string& msg, network::MessageType type) {
        this->handlePublicWsMessage(msg, type);
    });

    public_ws_client_->setOnError([this](const std::string& error) {
        this->handleWsError(error, false);
    });

    return public_ws_client_->connect();
}

void BybitExchange::disconnect() {
    if (private_ws_client_) {
        private_ws_client_->disconnect();
        private_ws_client_.reset();
    }

    if (public_ws_client_) {
        public_ws_client_->disconnect();
        public_ws_client_.reset();
    }

    public_ws_connected_ = false;
    private_ws_connected_ = false;
    private_ws_authenticated_ = false;
    onConnectionStatus(ConnectionStatus::Disconnected);
}

bool BybitExchange::isConnected() const {
    return public_ws_connected_.load();
}

ConnectionStatus BybitExchange::connectionStatus() const {
    return connection_status_.load();
}

// ============================================================================
// WebSocket Handlers
// ============================================================================

void BybitExchange::handleWsOpen(bool is_private) {
    if (is_private) {
        private_ws_connected_ = true;
        authenticatePrivateWs();
    } else {
        public_ws_connected_ = true;
        onConnectionStatus(ConnectionStatus::Connected);
    }
}

void BybitExchange::handleWsClose(int code, const std::string& reason, bool is_private) {
    if (is_private) {
        private_ws_connected_ = false;
        private_ws_authenticated_ = false;
    } else {
        public_ws_connected_ = false;
        onConnectionStatus(ConnectionStatus::Reconnecting);
    }
}

void BybitExchange::handleWsError(const std::string& error, bool is_private) {
    onError("WebSocket error (" + std::string(is_private ? "private" : "public") + "): " + error);
}

void BybitExchange::handlePublicWsMessage(const std::string& message, network::MessageType type) {
    try {
        auto json = nlohmann::json::parse(message);

        // Check for subscription response
        if (json.contains("success")) {
            bool success = json["success"].get<bool>();
            if (!success) {
                onError("Subscription failed: " + json.value("ret_msg", "unknown"));
            }
            return;
        }

        // Handle data messages
        std::string topic = json.value("topic", "");
        auto data = json.value("data", nlohmann::json{});

        if (topic.find("orderbook") != std::string::npos) {
            std::string msg_type = json.value("type", "");
            if (msg_type == "snapshot") {
                handleOrderBookSnapshot(json);
            } else if (msg_type == "delta") {
                handleOrderBookDelta(json);
            }
        } else if (topic.find("publicTrade") != std::string::npos) {
            handleTradeUpdate(json);
        } else if (topic.find("tickers") != std::string::npos) {
            handleTickerUpdate(json);
        }

    } catch (const std::exception& e) {
        onError("Public WS message parse error: " + std::string(e.what()));
    }
}

void BybitExchange::handlePrivateWsMessage(const std::string& message, network::MessageType type) {
    try {
        auto json = nlohmann::json::parse(message);

        // Check for auth response
        if (json.contains("op") && json["op"] == "auth") {
            bool success = json.value("success", false);
            if (success) {
                private_ws_authenticated_ = true;
            } else {
                onError("Private WS authentication failed: " + json.value("ret_msg", "unknown"));
            }
            return;
        }

        // Check for subscription response
        if (json.contains("success")) {
            return;
        }

        // Handle data messages
        std::string topic = json.value("topic", "");
        auto data = json.value("data", nlohmann::json::array());

        if (topic == "order") {
            for (const auto& d : data) {
                handleOrderUpdate(d);
            }
        } else if (topic == "position") {
            for (const auto& d : data) {
                handlePositionUpdate(d);
            }
        } else if (topic == "wallet") {
            for (const auto& d : data) {
                handleWalletUpdate(d);
            }
        }

    } catch (const std::exception& e) {
        onError("Private WS message parse error: " + std::string(e.what()));
    }
}

void BybitExchange::handleOrderBookSnapshot(const nlohmann::json& json) {
    try {
        auto data = json["data"];
        std::string symbol = data.value("s", "");

        OrderBook ob;
        ob.symbol = symbol;
        ob.exchange = "bybit";
        ob.sequence = data.value("u", uint64_t(0));
        ob.timestamp = json.value("ts", uint64_t(0));
        ob.local_timestamp = currentTimeNs();

        // Parse bids
        auto bids = data.value("b", nlohmann::json::array());
        for (const auto& b : bids) {
            PriceLevel level;
            level.price = std::stod(b[0].get<std::string>());
            level.quantity = std::stod(b[1].get<std::string>());
            ob.bids.push_back(level);
        }

        // Parse asks
        auto asks = data.value("a", nlohmann::json::array());
        for (const auto& a : asks) {
            PriceLevel level;
            level.price = std::stod(a[0].get<std::string>());
            level.quantity = std::stod(a[1].get<std::string>());
            ob.asks.push_back(level);
        }

        // Update sequence
        {
            std::lock_guard<std::mutex> lock(orderbook_seq_mutex_);
            orderbook_seq_[symbol] = ob.sequence;
        }

        onOrderBook(ob);

    } catch (const std::exception& e) {
        onError("OrderBook snapshot error: " + std::string(e.what()));
    }
}

void BybitExchange::handleOrderBookDelta(const nlohmann::json& json) {
    try {
        auto data = json["data"];
        std::string symbol = data.value("s", "");
        uint64_t seq = data.value("u", uint64_t(0));

        // Check sequence
        {
            std::lock_guard<std::mutex> lock(orderbook_seq_mutex_);
            auto it = orderbook_seq_.find(symbol);
            if (it == orderbook_seq_.end()) {
                return;  // Need snapshot first
            }
            if (seq <= it->second) {
                return;  // Old update
            }
            orderbook_seq_[symbol] = seq;
        }

        // Get cached orderbook
        auto cached = getCachedOrderBook(symbol);
        if (!cached) {
            return;
        }

        OrderBook ob = *cached;
        ob.sequence = seq;
        ob.timestamp = json.value("ts", uint64_t(0));
        ob.local_timestamp = currentTimeNs();

        // Apply delta updates
        auto applyUpdates = [](std::vector<PriceLevel>& levels,
                              const nlohmann::json& updates,
                              bool ascending) {
            for (const auto& update : updates) {
                double price = std::stod(update[0].get<std::string>());
                double qty = std::stod(update[1].get<std::string>());

                auto it = std::find_if(levels.begin(), levels.end(),
                    [price](const PriceLevel& l) { return l.price == price; });

                if (qty == 0.0) {
                    if (it != levels.end()) {
                        levels.erase(it);
                    }
                } else if (it != levels.end()) {
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
        };

        if (data.contains("b")) {
            applyUpdates(ob.bids, data["b"], false);
        }
        if (data.contains("a")) {
            applyUpdates(ob.asks, data["a"], true);
        }

        onOrderBook(ob);

    } catch (const std::exception& e) {
        onError("OrderBook delta error: " + std::string(e.what()));
    }
}

void BybitExchange::handleTradeUpdate(const nlohmann::json& json) {
    try {
        auto data = json.value("data", nlohmann::json::array());

        for (const auto& t : data) {
            Trade trade;
            trade.exchange = "bybit";
            trade.symbol = t.value("s", "");
            trade.trade_id = t.value("i", "");
            trade.price = std::stod(t.value("p", "0"));
            trade.quantity = std::stod(t.value("v", "0"));
            trade.timestamp = t.value("T", uint64_t(0));
            trade.local_timestamp = currentTimeNs();

            std::string side = t.value("S", "Buy");
            trade.side = (side == "Buy") ? OrderSide::Buy : OrderSide::Sell;
            trade.is_maker = t.value("mK", false);

            onTrade(trade);
        }

    } catch (const std::exception& e) {
        onError("Trade update error: " + std::string(e.what()));
    }
}

void BybitExchange::handleTickerUpdate(const nlohmann::json& json) {
    try {
        auto data = json["data"];

        Ticker ticker;
        ticker.exchange = "bybit";
        ticker.symbol = data.value("symbol", "");
        ticker.last = std::stod(data.value("lastPrice", "0"));
        ticker.bid = std::stod(data.value("bid1Price", "0"));
        ticker.ask = std::stod(data.value("ask1Price", "0"));
        ticker.bid_qty = std::stod(data.value("bid1Size", "0"));
        ticker.ask_qty = std::stod(data.value("ask1Size", "0"));
        ticker.high_24h = std::stod(data.value("highPrice24h", "0"));
        ticker.low_24h = std::stod(data.value("lowPrice24h", "0"));
        ticker.volume_24h = std::stod(data.value("volume24h", "0"));
        ticker.volume_quote_24h = std::stod(data.value("turnover24h", "0"));
        ticker.change_24h_pct = std::stod(data.value("price24hPcnt", "0")) * 100;
        ticker.timestamp = json.value("ts", uint64_t(0));

        onTicker(ticker);

    } catch (const std::exception& e) {
        onError("Ticker update error: " + std::string(e.what()));
    }
}

void BybitExchange::handleOrderUpdate(const nlohmann::json& data) {
    try {
        Order order;
        order.exchange = "bybit";
        order.symbol = data.value("symbol", "");
        order.order_id = data.value("orderId", "");
        order.client_order_id = data.value("orderLinkId", "");
        order.side = parseOrderSide(data.value("side", "Buy"));
        order.type = parseOrderType(data.value("orderType", "Limit"));
        order.status = parseOrderStatus(data.value("orderStatus", "New"));
        order.time_in_force = parseTimeInForce(data.value("timeInForce", "GTC"));

        order.quantity = std::stod(data.value("qty", "0"));
        order.filled_quantity = std::stod(data.value("cumExecQty", "0"));
        order.remaining_quantity = std::stod(data.value("leavesQty", "0"));
        order.price = std::stod(data.value("price", "0"));
        order.average_price = std::stod(data.value("avgPrice", "0"));
        order.stop_price = std::stod(data.value("triggerPrice", "0"));
        order.commission = std::stod(data.value("cumExecFee", "0"));

        order.create_time = std::stoull(data.value("createdTime", "0"));
        order.update_time = std::stoull(data.value("updatedTime", "0"));
        order.reduce_only = data.value("reduceOnly", false);
        order.raw = data;

        onOrder(order);

    } catch (const std::exception& e) {
        onError("Order update error: " + std::string(e.what()));
    }
}

void BybitExchange::handlePositionUpdate(const nlohmann::json& data) {
    try {
        Position position;
        position.exchange = "bybit";
        position.symbol = data.value("symbol", "");

        std::string side = data.value("side", "Buy");
        double size = std::stod(data.value("size", "0"));

        position.side = (side == "Buy") ? OrderSide::Buy : OrderSide::Sell;
        position.quantity = size;
        position.entry_price = std::stod(data.value("entryPrice", "0"));
        position.mark_price = std::stod(data.value("markPrice", "0"));
        position.liquidation_price = std::stod(data.value("liqPrice", "0"));
        position.unrealized_pnl = std::stod(data.value("unrealisedPnl", "0"));
        position.realized_pnl = std::stod(data.value("cumRealisedPnl", "0"));
        position.leverage = std::stod(data.value("leverage", "1"));
        position.margin = std::stod(data.value("positionIM", "0"));
        position.update_time = std::stoull(data.value("updatedTime", "0"));

        onPosition(position);

    } catch (const std::exception& e) {
        onError("Position update error: " + std::string(e.what()));
    }
}

void BybitExchange::handleWalletUpdate(const nlohmann::json& data) {
    try {
        auto coins = data.value("coin", nlohmann::json::array());
        for (const auto& c : coins) {
            Balance balance;
            balance.asset = c.value("coin", "");
            balance.free = std::stod(c.value("availableToWithdraw", "0"));
            balance.locked = std::stod(c.value("walletBalance", "0")) - balance.free;
            onBalance(balance);
        }
    } catch (const std::exception& e) {
        onError("Wallet update error: " + std::string(e.what()));
    }
}

// ============================================================================
// Signature Generation
// ============================================================================

std::string BybitExchange::sign(const std::string& timestamp, const std::string& api_key,
                                uint32_t recv_window, const std::string& query_string) {
    std::string sign_str = timestamp + api_key + std::to_string(recv_window) + query_string;

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;

    HMAC(EVP_sha256(),
         bybit_config_.api_secret.c_str(),
         static_cast<int>(bybit_config_.api_secret.length()),
         reinterpret_cast<const unsigned char*>(sign_str.c_str()),
         sign_str.length(),
         hash, &hash_len);

    std::stringstream ss;
    for (unsigned int i = 0; i < hash_len; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }

    return ss.str();
}

void BybitExchange::addAuthHeaders(network::HttpRequest& request, const std::string& query_string) {
    std::string timestamp = std::to_string(currentTimeMs());
    std::string signature = sign(timestamp, bybit_config_.api_key,
                                 bybit_config_.recv_window, query_string);

    request.headers["X-BAPI-API-KEY"] = bybit_config_.api_key;
    request.headers["X-BAPI-TIMESTAMP"] = timestamp;
    request.headers["X-BAPI-RECV-WINDOW"] = std::to_string(bybit_config_.recv_window);
    request.headers["X-BAPI-SIGN"] = signature;
}

// ============================================================================
// Subscription Management
// ============================================================================

bool BybitExchange::subscribeOrderBook(const std::string& symbol, int depth) {
    std::string topic = "orderbook." + std::to_string(depth) + "." + symbol;
    sendSubscribe({topic}, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.insert(topic);
    return true;
}

bool BybitExchange::unsubscribeOrderBook(const std::string& symbol) {
    std::vector<std::string> to_unsub;

    {
        std::lock_guard<std::mutex> lock(subscription_mutex_);
        for (const auto& topic : public_subscriptions_) {
            if (topic.find("orderbook") != std::string::npos &&
                topic.find(symbol) != std::string::npos) {
                to_unsub.push_back(topic);
            }
        }
        for (const auto& topic : to_unsub) {
            public_subscriptions_.erase(topic);
        }
    }

    if (!to_unsub.empty()) {
        sendUnsubscribe(to_unsub, false);
    }

    clearOrderBookCache(symbol);
    return true;
}

bool BybitExchange::subscribeTrades(const std::string& symbol) {
    std::string topic = "publicTrade." + symbol;
    sendSubscribe({topic}, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.insert(topic);
    return true;
}

bool BybitExchange::unsubscribeTrades(const std::string& symbol) {
    std::string topic = "publicTrade." + symbol;
    sendUnsubscribe({topic}, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.erase(topic);
    return true;
}

bool BybitExchange::subscribeTicker(const std::string& symbol) {
    std::string topic = "tickers." + symbol;
    sendSubscribe({topic}, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.insert(topic);
    return true;
}

bool BybitExchange::unsubscribeTicker(const std::string& symbol) {
    std::string topic = "tickers." + symbol;
    sendUnsubscribe({topic}, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.erase(topic);
    return true;
}

bool BybitExchange::subscribeUserData() {
    // Initialize private WebSocket if needed
    if (!private_ws_client_) {
        network::WebSocketConfig ws_config;
        ws_config.url = getPrivateWsUrl();
        ws_config.name = "bybit_private_ws";
        ws_config.ping_interval_ms = 20000;
        ws_config.auto_reconnect = true;

        private_ws_client_ = std::make_shared<network::WebSocketClient>(ws_config);

        private_ws_client_->setOnOpen([this]() {
            this->handleWsOpen(true);
        });

        private_ws_client_->setOnClose([this](int code, const std::string& reason) {
            this->handleWsClose(code, reason, true);
        });

        private_ws_client_->setOnMessage([this](const std::string& msg, network::MessageType type) {
            this->handlePrivateWsMessage(msg, type);
        });

        private_ws_client_->setOnError([this](const std::string& error) {
            this->handleWsError(error, true);
        });

        private_ws_client_->connect();
    }

    // Subscribe to user data topics
    std::vector<std::string> topics = {"order", "position", "wallet"};
    sendSubscribe(topics, true);

    return true;
}

bool BybitExchange::unsubscribeUserData() {
    if (private_ws_client_) {
        private_ws_client_->disconnect();
        private_ws_client_.reset();
    }

    private_ws_connected_ = false;
    private_ws_authenticated_ = false;
    return true;
}

void BybitExchange::sendSubscribe(const std::vector<std::string>& topics, bool is_private) {
    auto client = is_private ? private_ws_client_ : public_ws_client_;
    if (!client) return;

    nlohmann::json msg;
    msg["op"] = "subscribe";
    msg["args"] = topics;
    msg["req_id"] = std::to_string(next_req_id_++);

    client->send(msg);
}

void BybitExchange::sendUnsubscribe(const std::vector<std::string>& topics, bool is_private) {
    auto client = is_private ? private_ws_client_ : public_ws_client_;
    if (!client) return;

    nlohmann::json msg;
    msg["op"] = "unsubscribe";
    msg["args"] = topics;
    msg["req_id"] = std::to_string(next_req_id_++);

    client->send(msg);
}

void BybitExchange::authenticatePrivateWs() {
    if (!private_ws_client_) return;

    std::string timestamp = std::to_string(currentTimeMs());
    std::string expires = std::to_string(std::stoull(timestamp) + 10000);
    std::string sign_str = "GET/realtime" + expires;

    std::string signature = sign(timestamp, bybit_config_.api_key, bybit_config_.recv_window, sign_str);

    nlohmann::json auth_msg;
    auth_msg["op"] = "auth";
    auth_msg["args"] = {bybit_config_.api_key, expires, signature};

    private_ws_client_->send(auth_msg);
}

// ============================================================================
// REST Helpers
// ============================================================================

network::HttpResponse BybitExchange::signedGet(
    const std::string& endpoint,
    const std::unordered_map<std::string, std::string>& params) {

    // Build query string
    std::stringstream ss;
    bool first = true;
    for (const auto& [key, value] : params) {
        if (!first) ss << "&";
        first = false;
        ss << key << "=" << value;
    }
    std::string query_string = ss.str();

    network::HttpRequest request;
    request.method = network::HttpMethod::GET;
    request.path = endpoint;
    request.query_params = params;
    addAuthHeaders(request, query_string);

    return rest_client_->execute(request);
}

network::HttpResponse BybitExchange::signedPost(
    const std::string& endpoint,
    const nlohmann::json& body) {

    std::string body_str = body.empty() ? "" : body.dump();

    network::HttpRequest request;
    request.method = network::HttpMethod::POST;
    request.path = endpoint;
    request.body = body_str;
    request.headers["Content-Type"] = "application/json";
    addAuthHeaders(request, body_str);

    return rest_client_->execute(request);
}

network::HttpResponse BybitExchange::publicGet(
    const std::string& endpoint,
    const std::unordered_map<std::string, std::string>& params) {

    return rest_client_->get(endpoint, params);
}

// ============================================================================
// Order Management
// ============================================================================

std::optional<Order> BybitExchange::placeOrder(const OrderRequest& request) {
    nlohmann::json body;
    body["category"] = getCategoryString();
    body["symbol"] = request.symbol;
    body["side"] = orderSideToString(request.side);
    body["orderType"] = orderTypeToString(request.type);
    body["qty"] = std::to_string(request.quantity);

    if (request.type != OrderType::Market) {
        body["price"] = std::to_string(request.price);
        body["timeInForce"] = timeInForceToString(request.time_in_force);
    }

    if (request.stop_price > 0.0) {
        body["triggerPrice"] = std::to_string(request.stop_price);
    }

    if (!request.client_order_id.empty()) {
        body["orderLinkId"] = request.client_order_id;
    }

    if (request.reduce_only) {
        body["reduceOnly"] = true;
    }

    auto response = signedPost("/v5/order/create", body);

    if (!response.success) {
        onError("Place order failed: " + response.error + " " + response.body);
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("retCode", -1) != 0) {
            onError("Place order failed: " + json.value("retMsg", "unknown"));
            return std::nullopt;
        }

        Order order;
        auto result = json["result"];
        order.order_id = result.value("orderId", "");
        order.client_order_id = result.value("orderLinkId", "");
        order.symbol = request.symbol;
        order.side = request.side;
        order.type = request.type;
        order.quantity = request.quantity;
        order.price = request.price;
        order.status = OrderStatus::New;

        return order;

    } catch (const std::exception& e) {
        onError("Parse order response failed: " + std::string(e.what()));
        return std::nullopt;
    }
}

std::optional<Order> BybitExchange::cancelOrder(const std::string& symbol,
                                                const std::string& order_id) {
    nlohmann::json body;
    body["category"] = getCategoryString();
    body["symbol"] = symbol;
    body["orderId"] = order_id;

    auto response = signedPost("/v5/order/cancel", body);

    if (!response.success) {
        onError("Cancel order failed: " + response.error);
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("retCode", -1) != 0) {
            onError("Cancel order failed: " + json.value("retMsg", "unknown"));
            return std::nullopt;
        }

        Order order;
        order.order_id = order_id;
        order.symbol = symbol;
        order.status = OrderStatus::Cancelled;
        return order;

    } catch (...) {
        return std::nullopt;
    }
}

std::optional<Order> BybitExchange::cancelOrder(const std::string& symbol,
                                                const std::string& order_id,
                                                const std::string& client_order_id) {
    nlohmann::json body;
    body["category"] = getCategoryString();
    body["symbol"] = symbol;

    if (!order_id.empty()) {
        body["orderId"] = order_id;
    }
    if (!client_order_id.empty()) {
        body["orderLinkId"] = client_order_id;
    }

    auto response = signedPost("/v5/order/cancel", body);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("retCode", -1) != 0) {
            return std::nullopt;
        }

        Order order;
        order.order_id = order_id;
        order.client_order_id = client_order_id;
        order.symbol = symbol;
        order.status = OrderStatus::Cancelled;
        return order;

    } catch (...) {
        return std::nullopt;
    }
}

bool BybitExchange::cancelAllOrders(const std::string& symbol) {
    nlohmann::json body;
    body["category"] = getCategoryString();
    body["symbol"] = symbol;

    auto response = signedPost("/v5/order/cancel-all", body);
    return response.success && response.json().value("retCode", -1) == 0;
}

std::optional<Order> BybitExchange::getOrder(const std::string& symbol,
                                             const std::string& order_id) {
    std::unordered_map<std::string, std::string> params;
    params["category"] = getCategoryString();
    params["symbol"] = symbol;
    params["orderId"] = order_id;

    auto response = signedGet("/v5/order/realtime", params);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("retCode", -1) != 0) {
            return std::nullopt;
        }

        auto list = json["result"]["list"];
        if (list.empty()) {
            return std::nullopt;
        }

        return parseOrder(list[0]);

    } catch (...) {
        return std::nullopt;
    }
}

std::vector<Order> BybitExchange::getOpenOrders(const std::string& symbol) {
    std::unordered_map<std::string, std::string> params;
    params["category"] = getCategoryString();
    if (!symbol.empty()) {
        params["symbol"] = symbol;
    }

    auto response = signedGet("/v5/order/realtime", params);

    std::vector<Order> orders;
    if (!response.success) {
        return orders;
    }

    try {
        auto json = response.json();
        if (json.value("retCode", -1) != 0) {
            return orders;
        }

        auto list = json["result"]["list"];
        for (const auto& item : list) {
            orders.push_back(parseOrder(item));
        }

    } catch (...) {
        // Return empty
    }

    return orders;
}

std::vector<Order> BybitExchange::getOrderHistory(const std::string& symbol,
                                                  uint64_t start_time,
                                                  uint64_t end_time,
                                                  int limit) {
    std::unordered_map<std::string, std::string> params;
    params["category"] = getCategoryString();
    params["symbol"] = symbol;
    params["limit"] = std::to_string(limit);

    if (start_time > 0) {
        params["startTime"] = std::to_string(start_time);
    }
    if (end_time > 0) {
        params["endTime"] = std::to_string(end_time);
    }

    auto response = signedGet("/v5/order/history", params);

    std::vector<Order> orders;
    if (!response.success) {
        return orders;
    }

    try {
        auto json = response.json();
        if (json.value("retCode", -1) != 0) {
            return orders;
        }

        auto list = json["result"]["list"];
        for (const auto& item : list) {
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

std::optional<Account> BybitExchange::getAccount() {
    std::unordered_map<std::string, std::string> params;
    params["accountType"] = "UNIFIED";

    auto response = signedGet("/v5/account/wallet-balance", params);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("retCode", -1) != 0) {
            return std::nullopt;
        }

        Account account;
        account.exchange = "bybit";
        account.update_time = currentTimeMs();

        auto list = json["result"]["list"];
        if (!list.empty()) {
            auto wallet = list[0];
            account.total_margin = std::stod(wallet.value("totalEquity", "0"));
            account.available_margin = std::stod(wallet.value("totalAvailableBalance", "0"));
            account.total_unrealized_pnl = std::stod(wallet.value("totalPerpUPL", "0"));

            auto coins = wallet.value("coin", nlohmann::json::array());
            for (const auto& c : coins) {
                Balance balance;
                balance.asset = c.value("coin", "");
                balance.free = std::stod(c.value("availableToWithdraw", "0"));
                balance.locked = std::stod(c.value("walletBalance", "0")) - balance.free;
                account.balances[balance.asset] = balance;
            }
        }

        // Get positions
        account.positions = getPositions();

        return account;

    } catch (const std::exception& e) {
        onError("Parse account failed: " + std::string(e.what()));
        return std::nullopt;
    }
}

std::optional<Balance> BybitExchange::getBalance(const std::string& asset) {
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

std::vector<Position> BybitExchange::getPositions(const std::string& symbol) {
    std::unordered_map<std::string, std::string> params;
    params["category"] = getCategoryString();
    if (!symbol.empty()) {
        params["symbol"] = symbol;
    }

    auto response = signedGet("/v5/position/list", params);

    std::vector<Position> positions;
    if (!response.success) {
        return positions;
    }

    try {
        auto json = response.json();
        if (json.value("retCode", -1) != 0) {
            return positions;
        }

        auto list = json["result"]["list"];
        for (const auto& p : list) {
            double size = std::stod(p.value("size", "0"));
            if (size == 0.0) continue;

            Position position;
            position.exchange = "bybit";
            position.symbol = p.value("symbol", "");

            std::string side = p.value("side", "Buy");
            position.side = (side == "Buy") ? OrderSide::Buy : OrderSide::Sell;
            position.quantity = size;
            position.entry_price = std::stod(p.value("avgPrice", "0"));
            position.mark_price = std::stod(p.value("markPrice", "0"));
            position.liquidation_price = std::stod(p.value("liqPrice", "0"));
            position.unrealized_pnl = std::stod(p.value("unrealisedPnl", "0"));
            position.realized_pnl = std::stod(p.value("cumRealisedPnl", "0"));
            position.leverage = std::stod(p.value("leverage", "1"));
            position.margin = std::stod(p.value("positionIM", "0"));
            position.update_time = std::stoull(p.value("updatedTime", "0"));

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

std::vector<SymbolInfo> BybitExchange::getSymbols() {
    std::unordered_map<std::string, std::string> params;
    params["category"] = getCategoryString();

    auto response = publicGet("/v5/market/instruments-info", params);

    std::vector<SymbolInfo> symbols;
    if (!response.success) {
        return symbols;
    }

    try {
        auto json = response.json();
        if (json.value("retCode", -1) != 0) {
            return symbols;
        }

        auto list = json["result"]["list"];
        for (const auto& item : list) {
            auto info = parseSymbolInfo(item);
            updateSymbolInfo(info);
            symbols.push_back(info);
        }

    } catch (...) {
        // Return empty
    }

    return symbols;
}

std::optional<SymbolInfo> BybitExchange::getSymbolInfo(const std::string& symbol) {
    // Check cache first
    {
        std::lock_guard<std::mutex> lock(symbol_info_mutex_);
        auto it = symbol_info_cache_.find(symbol);
        if (it != symbol_info_cache_.end()) {
            return it->second;
        }
    }

    std::unordered_map<std::string, std::string> params;
    params["category"] = getCategoryString();
    params["symbol"] = symbol;

    auto response = publicGet("/v5/market/instruments-info", params);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("retCode", -1) != 0) {
            return std::nullopt;
        }

        auto list = json["result"]["list"];
        if (!list.empty()) {
            return parseSymbolInfo(list[0]);
        }

    } catch (...) {
        // Fall through
    }

    return std::nullopt;
}

std::optional<OrderBook> BybitExchange::getOrderBookSnapshot(const std::string& symbol, int depth) {
    std::unordered_map<std::string, std::string> params;
    params["category"] = getCategoryString();
    params["symbol"] = symbol;
    params["limit"] = std::to_string(depth);

    auto response = publicGet("/v5/market/orderbook", params);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("retCode", -1) != 0) {
            return std::nullopt;
        }

        auto result = json["result"];

        OrderBook ob;
        ob.symbol = symbol;
        ob.exchange = "bybit";
        ob.sequence = result.value("u", uint64_t(0));
        ob.timestamp = result.value("ts", uint64_t(0));
        ob.local_timestamp = currentTimeNs();

        auto bids = result.value("b", nlohmann::json::array());
        for (const auto& b : bids) {
            PriceLevel level;
            level.price = std::stod(b[0].get<std::string>());
            level.quantity = std::stod(b[1].get<std::string>());
            ob.bids.push_back(level);
        }

        auto asks = result.value("a", nlohmann::json::array());
        for (const auto& a : asks) {
            PriceLevel level;
            level.price = std::stod(a[0].get<std::string>());
            level.quantity = std::stod(a[1].get<std::string>());
            ob.asks.push_back(level);
        }

        return ob;

    } catch (...) {
        return std::nullopt;
    }
}

std::vector<Trade> BybitExchange::getRecentTrades(const std::string& symbol, int limit) {
    std::unordered_map<std::string, std::string> params;
    params["category"] = getCategoryString();
    params["symbol"] = symbol;
    params["limit"] = std::to_string(limit);

    auto response = publicGet("/v5/market/recent-trade", params);

    std::vector<Trade> trades;
    if (!response.success) {
        return trades;
    }

    try {
        auto json = response.json();
        if (json.value("retCode", -1) != 0) {
            return trades;
        }

        auto list = json["result"]["list"];
        for (const auto& t : list) {
            Trade trade;
            trade.exchange = "bybit";
            trade.symbol = symbol;
            trade.trade_id = t.value("execId", "");
            trade.price = std::stod(t.value("price", "0"));
            trade.quantity = std::stod(t.value("size", "0"));
            trade.timestamp = std::stoull(t.value("time", "0"));
            trade.side = t.value("side", "Buy") == "Buy" ? OrderSide::Buy : OrderSide::Sell;
            trades.push_back(trade);
        }

    } catch (...) {
        // Return empty
    }

    return trades;
}

std::optional<Ticker> BybitExchange::getTicker(const std::string& symbol) {
    std::unordered_map<std::string, std::string> params;
    params["category"] = getCategoryString();
    params["symbol"] = symbol;

    auto response = publicGet("/v5/market/tickers", params);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("retCode", -1) != 0) {
            return std::nullopt;
        }

        auto list = json["result"]["list"];
        if (list.empty()) {
            return std::nullopt;
        }

        auto data = list[0];

        Ticker ticker;
        ticker.exchange = "bybit";
        ticker.symbol = symbol;
        ticker.last = std::stod(data.value("lastPrice", "0"));
        ticker.bid = std::stod(data.value("bid1Price", "0"));
        ticker.ask = std::stod(data.value("ask1Price", "0"));
        ticker.bid_qty = std::stod(data.value("bid1Size", "0"));
        ticker.ask_qty = std::stod(data.value("ask1Size", "0"));
        ticker.high_24h = std::stod(data.value("highPrice24h", "0"));
        ticker.low_24h = std::stod(data.value("lowPrice24h", "0"));
        ticker.volume_24h = std::stod(data.value("volume24h", "0"));
        ticker.volume_quote_24h = std::stod(data.value("turnover24h", "0"));
        ticker.change_24h_pct = std::stod(data.value("price24hPcnt", "0")) * 100;

        return ticker;

    } catch (...) {
        return std::nullopt;
    }
}

uint64_t BybitExchange::getServerTime() {
    auto response = publicGet("/v5/market/time");

    if (response.success) {
        auto json = response.json();
        return std::stoull(json["result"].value("timeSecond", "0")) * 1000;
    }

    return 0;
}

std::string BybitExchange::formatSymbol(const std::string& base, const std::string& quote) const {
    return base + quote;
}

std::pair<std::string, std::string> BybitExchange::parseSymbol(const std::string& symbol) const {
    static const std::vector<std::string> quote_assets = {"USDT", "USDC", "BTC", "ETH"};

    for (const auto& quote : quote_assets) {
        if (symbol.length() > quote.length() &&
            symbol.substr(symbol.length() - quote.length()) == quote) {
            return {symbol.substr(0, symbol.length() - quote.length()), quote};
        }
    }

    return {symbol, ""};
}

// ============================================================================
// Data Converters
// ============================================================================

Order BybitExchange::parseOrder(const nlohmann::json& data) {
    Order order;
    order.exchange = "bybit";
    order.symbol = data.value("symbol", "");
    order.order_id = data.value("orderId", "");
    order.client_order_id = data.value("orderLinkId", "");
    order.side = parseOrderSide(data.value("side", "Buy"));
    order.type = parseOrderType(data.value("orderType", "Limit"));
    order.status = parseOrderStatus(data.value("orderStatus", "New"));
    order.time_in_force = parseTimeInForce(data.value("timeInForce", "GTC"));

    order.quantity = std::stod(data.value("qty", "0"));
    order.filled_quantity = std::stod(data.value("cumExecQty", "0"));
    order.remaining_quantity = std::stod(data.value("leavesQty", "0"));
    order.price = std::stod(data.value("price", "0"));
    order.average_price = std::stod(data.value("avgPrice", "0"));
    order.stop_price = std::stod(data.value("triggerPrice", "0"));
    order.commission = std::stod(data.value("cumExecFee", "0"));

    order.create_time = std::stoull(data.value("createdTime", "0"));
    order.update_time = std::stoull(data.value("updatedTime", "0"));
    order.reduce_only = data.value("reduceOnly", false);
    order.raw = data;

    return order;
}

OrderStatus BybitExchange::parseOrderStatus(const std::string& status) {
    if (status == "New" || status == "Created") return OrderStatus::New;
    if (status == "PartiallyFilled") return OrderStatus::PartiallyFilled;
    if (status == "Filled") return OrderStatus::Filled;
    if (status == "Cancelled") return OrderStatus::Cancelled;
    if (status == "Rejected") return OrderStatus::Rejected;
    if (status == "Expired" || status == "Deactivated") return OrderStatus::Expired;
    if (status == "PendingCancel") return OrderStatus::PendingCancel;
    return OrderStatus::Failed;
}

OrderSide BybitExchange::parseOrderSide(const std::string& side) {
    return (side == "Buy") ? OrderSide::Buy : OrderSide::Sell;
}

OrderType BybitExchange::parseOrderType(const std::string& type) {
    if (type == "Market") return OrderType::Market;
    if (type == "Limit") return OrderType::Limit;
    return OrderType::Limit;
}

TimeInForce BybitExchange::parseTimeInForce(const std::string& tif) {
    if (tif == "GTC") return TimeInForce::GTC;
    if (tif == "IOC") return TimeInForce::IOC;
    if (tif == "FOK") return TimeInForce::FOK;
    if (tif == "PostOnly") return TimeInForce::PostOnly;
    return TimeInForce::GTC;
}

std::string BybitExchange::orderSideToString(OrderSide side) {
    return side == OrderSide::Buy ? "Buy" : "Sell";
}

std::string BybitExchange::orderTypeToString(OrderType type) {
    switch (type) {
        case OrderType::Market: return "Market";
        case OrderType::Limit: return "Limit";
        default: return "Limit";
    }
}

std::string BybitExchange::timeInForceToString(TimeInForce tif) {
    switch (tif) {
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
        case TimeInForce::PostOnly: return "PostOnly";
        default: return "GTC";
    }
}

SymbolInfo BybitExchange::parseSymbolInfo(const nlohmann::json& data) {
    SymbolInfo info;
    info.symbol = data.value("symbol", "");
    info.base_asset = data.value("baseCoin", "");
    info.quote_asset = data.value("quoteCoin", "");
    info.trading_enabled = data.value("status", "") == "Trading";

    auto lot_filter = data.value("lotSizeFilter", nlohmann::json{});
    info.min_qty = std::stod(lot_filter.value("minOrderQty", "0"));
    info.max_qty = std::stod(lot_filter.value("maxOrderQty", "0"));
    info.step_size = std::stod(lot_filter.value("qtyStep", "0"));

    auto price_filter = data.value("priceFilter", nlohmann::json{});
    info.tick_size = std::stod(price_filter.value("tickSize", "0"));

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
// Bybit-specific Methods
// ============================================================================

bool BybitExchange::setLeverage(const std::string& symbol, int buy_leverage, int sell_leverage) {
    nlohmann::json body;
    body["category"] = getCategoryString();
    body["symbol"] = symbol;
    body["buyLeverage"] = std::to_string(buy_leverage);
    body["sellLeverage"] = std::to_string(sell_leverage);

    auto response = signedPost("/v5/position/set-leverage", body);
    return response.success && response.json().value("retCode", -1) == 0;
}

bool BybitExchange::setPositionMode(bool hedge_mode) {
    nlohmann::json body;
    body["category"] = getCategoryString();
    body["mode"] = hedge_mode ? 3 : 0;  // 0: One-Way, 3: Hedge

    auto response = signedPost("/v5/position/switch-mode", body);
    return response.success && response.json().value("retCode", -1) == 0;
}

double BybitExchange::getFundingRate(const std::string& symbol) {
    std::unordered_map<std::string, std::string> params;
    params["category"] = getCategoryString();
    params["symbol"] = symbol;

    auto response = publicGet("/v5/market/tickers", params);

    if (response.success) {
        auto json = response.json();
        if (json.value("retCode", -1) == 0) {
            auto list = json["result"]["list"];
            if (!list.empty()) {
                return std::stod(list[0].value("fundingRate", "0"));
            }
        }
    }

    return 0.0;
}

// ============================================================================
// Factory Functions
// ============================================================================

std::shared_ptr<BybitExchange> createBybitExchange(const ExchangeConfig& config) {
    return std::make_shared<BybitExchange>(config);
}

std::shared_ptr<BybitExchange> createBybitExchange(const BybitConfig& config) {
    return std::make_shared<BybitExchange>(config);
}

} // namespace exchange
} // namespace hft
