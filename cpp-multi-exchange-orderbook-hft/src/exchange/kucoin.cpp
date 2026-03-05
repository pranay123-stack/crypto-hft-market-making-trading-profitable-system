#include "exchange/kucoin.hpp"

#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <random>

namespace hft {
namespace exchange {

// ============================================================================
// Constructors/Destructor
// ============================================================================

KuCoinExchange::KuCoinExchange(const ExchangeConfig& config)
    : ExchangeBase(config) {
    kucoin_config_.api_key = config.api_key;
    kucoin_config_.api_secret = config.api_secret;
    kucoin_config_.passphrase = config.passphrase;
    kucoin_config_.testnet = config.testnet;
    kucoin_config_.spot = (config.type == ExchangeType::Spot);
    kucoin_config_.order_rate_limit = config.orders_per_second;
    kucoin_config_.request_rate_limit = config.requests_per_minute;

    // Initialize REST client
    network::RestClientConfig rest_config;
    rest_config.base_url = getRestUrl();
    rest_config.name = "kucoin_rest";
    rest_config.request_timeout_ms = config.rest_timeout_ms;
    rest_config.max_retries = config.rest_max_retries;
    rest_config.rate_limit.requests_per_second = kucoin_config_.order_rate_limit;
    rest_config.rate_limit.requests_per_minute = kucoin_config_.request_rate_limit;

    rest_client_ = std::make_shared<network::RestClient>(rest_config);
}

KuCoinExchange::KuCoinExchange(const KuCoinConfig& kucoin_config)
    : ExchangeBase(ExchangeConfig{
        .name = "KuCoin",
        .api_key = kucoin_config.api_key,
        .api_secret = kucoin_config.api_secret,
        .passphrase = kucoin_config.passphrase,
        .type = kucoin_config.spot ? ExchangeType::Spot : ExchangeType::Perpetual,
        .testnet = kucoin_config.testnet
      }),
      kucoin_config_(kucoin_config) {

    // Initialize REST client
    network::RestClientConfig rest_config;
    rest_config.base_url = getRestUrl();
    rest_config.name = "kucoin_rest";
    rest_config.rate_limit.requests_per_second = kucoin_config_.order_rate_limit;
    rest_config.rate_limit.requests_per_minute = kucoin_config_.request_rate_limit;

    rest_client_ = std::make_shared<network::RestClient>(rest_config);
}

KuCoinExchange::~KuCoinExchange() {
    disconnect();
}

// ============================================================================
// URL Builders
// ============================================================================

std::string KuCoinExchange::getRestUrl() const {
    if (kucoin_config_.spot) {
        return kucoin_config_.testnet
            ? "https://openapi-sandbox.kucoin.com"
            : "https://api.kucoin.com";
    } else {
        return kucoin_config_.testnet
            ? "https://api-sandbox-futures.kucoin.com"
            : "https://api-futures.kucoin.com";
    }
}

// ============================================================================
// WebSocket Token Management
// ============================================================================

KuCoinExchange::WsTokenInfo KuCoinExchange::getPublicWsToken() {
    WsTokenInfo info;
    std::string endpoint = kucoin_config_.spot
        ? "/api/v1/bullet-public"
        : "/api/v1/bullet-public";

    auto response = rest_client_->post(endpoint);

    if (!response.success) {
        onError("Failed to get public WS token: " + response.error);
        return info;
    }

    try {
        auto json = response.json();
        if (json.value("code", "") != "200000") {
            onError("Failed to get public WS token: " + json.value("msg", "unknown"));
            return info;
        }

        auto data = json["data"];
        info.token = data.value("token", "");

        auto servers = data.value("instanceServers", nlohmann::json::array());
        if (!servers.empty()) {
            info.endpoint = servers[0].value("endpoint", "");
            info.ping_interval_ms = servers[0].value("pingInterval", 18000);
            info.ping_timeout_ms = servers[0].value("pingTimeout", 10000);
        }

    } catch (const std::exception& e) {
        onError("Parse public WS token failed: " + std::string(e.what()));
    }

    return info;
}

KuCoinExchange::WsTokenInfo KuCoinExchange::getPrivateWsToken() {
    WsTokenInfo info;
    std::string endpoint = kucoin_config_.spot
        ? "/api/v1/bullet-private"
        : "/api/v1/bullet-private";

    auto response = signedPost(endpoint);

    if (!response.success) {
        onError("Failed to get private WS token: " + response.error);
        return info;
    }

    try {
        auto json = response.json();
        if (json.value("code", "") != "200000") {
            onError("Failed to get private WS token: " + json.value("msg", "unknown"));
            return info;
        }

        auto data = json["data"];
        info.token = data.value("token", "");

        auto servers = data.value("instanceServers", nlohmann::json::array());
        if (!servers.empty()) {
            info.endpoint = servers[0].value("endpoint", "");
            info.ping_interval_ms = servers[0].value("pingInterval", 18000);
            info.ping_timeout_ms = servers[0].value("pingTimeout", 10000);
        }

    } catch (const std::exception& e) {
        onError("Parse private WS token failed: " + std::string(e.what()));
    }

    return info;
}

std::string KuCoinExchange::getPublicWsEndpoint() {
    public_ws_token_ = getPublicWsToken();

    // Generate connect ID
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(100000000, 999999999);
    connect_id_ = std::to_string(dis(gen));

    return public_ws_token_.endpoint + "?token=" + public_ws_token_.token +
           "&connectId=" + connect_id_;
}

std::string KuCoinExchange::getPrivateWsEndpoint() {
    private_ws_token_ = getPrivateWsToken();

    // Generate connect ID
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(100000000, 999999999);
    std::string private_connect_id = std::to_string(dis(gen));

    return private_ws_token_.endpoint + "?token=" + private_ws_token_.token +
           "&connectId=" + private_connect_id;
}

// ============================================================================
// Connection Management
// ============================================================================

bool KuCoinExchange::connect() {
    if (public_ws_connected_.load()) {
        return true;
    }

    onConnectionStatus(ConnectionStatus::Connecting);

    // Get WebSocket endpoint with token
    std::string ws_url = getPublicWsEndpoint();
    if (ws_url.empty() || public_ws_token_.token.empty()) {
        onError("Failed to get WebSocket endpoint");
        onConnectionStatus(ConnectionStatus::Failed);
        return false;
    }

    // Initialize public WebSocket client
    network::WebSocketConfig ws_config;
    ws_config.url = ws_url;
    ws_config.name = "kucoin_public_ws";
    ws_config.ping_interval_ms = public_ws_token_.ping_interval_ms;
    ws_config.pong_timeout_ms = public_ws_token_.ping_timeout_ms;
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

    bool connected = public_ws_client_->connect();

    if (connected) {
        startPingLoop();
    }

    return connected;
}

void KuCoinExchange::disconnect() {
    stopPingLoop();

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
    onConnectionStatus(ConnectionStatus::Disconnected);
}

bool KuCoinExchange::isConnected() const {
    return public_ws_connected_.load();
}

ConnectionStatus KuCoinExchange::connectionStatus() const {
    return connection_status_.load();
}

// ============================================================================
// Ping Management
// ============================================================================

void KuCoinExchange::startPingLoop() {
    if (ping_running_.load()) {
        return;
    }

    ping_running_ = true;
    ping_thread_ = std::make_unique<std::thread>([this]() {
        while (ping_running_.load()) {
            // Sleep for ping interval
            std::this_thread::sleep_for(
                std::chrono::milliseconds(public_ws_token_.ping_interval_ms / 2));

            if (!ping_running_.load()) {
                break;
            }

            // Send ping to public WS
            if (public_ws_client_ && public_ws_connected_.load()) {
                nlohmann::json ping_msg;
                ping_msg["id"] = std::to_string(next_req_id_++);
                ping_msg["type"] = "ping";
                public_ws_client_->send(ping_msg);
            }

            // Send ping to private WS
            if (private_ws_client_ && private_ws_connected_.load()) {
                nlohmann::json ping_msg;
                ping_msg["id"] = std::to_string(next_req_id_++);
                ping_msg["type"] = "ping";
                private_ws_client_->send(ping_msg);
            }
        }
    });
}

void KuCoinExchange::stopPingLoop() {
    ping_running_ = false;
    if (ping_thread_ && ping_thread_->joinable()) {
        ping_thread_->join();
    }
    ping_thread_.reset();
}

// ============================================================================
// Signature Generation
// ============================================================================

std::string KuCoinExchange::sign(const std::string& timestamp, const std::string& method,
                                  const std::string& request_path, const std::string& body) {
    std::string pre_hash = timestamp + method + request_path + body;

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;

    HMAC(EVP_sha256(),
         kucoin_config_.api_secret.c_str(),
         static_cast<int>(kucoin_config_.api_secret.length()),
         reinterpret_cast<const unsigned char*>(pre_hash.c_str()),
         pre_hash.length(),
         hash, &hash_len);

    // Base64 encode
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* bio = BIO_new(BIO_s_mem());
    bio = BIO_push(b64, bio);

    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(bio, hash, hash_len);
    BIO_flush(bio);

    BUF_MEM* buffer_ptr;
    BIO_get_mem_ptr(bio, &buffer_ptr);

    std::string result(buffer_ptr->data, buffer_ptr->length);
    BIO_free_all(bio);

    return result;
}

std::string KuCoinExchange::encryptPassphrase() {
    // KuCoin requires the passphrase to be encrypted with HMAC-SHA256 and base64 encoded
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;

    HMAC(EVP_sha256(),
         kucoin_config_.api_secret.c_str(),
         static_cast<int>(kucoin_config_.api_secret.length()),
         reinterpret_cast<const unsigned char*>(kucoin_config_.passphrase.c_str()),
         kucoin_config_.passphrase.length(),
         hash, &hash_len);

    // Base64 encode
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* bio = BIO_new(BIO_s_mem());
    bio = BIO_push(b64, bio);

    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(bio, hash, hash_len);
    BIO_flush(bio);

    BUF_MEM* buffer_ptr;
    BIO_get_mem_ptr(bio, &buffer_ptr);

    std::string result(buffer_ptr->data, buffer_ptr->length);
    BIO_free_all(bio);

    return result;
}

void KuCoinExchange::addAuthHeaders(network::HttpRequest& request, const std::string& method,
                                     const std::string& request_path, const std::string& body) {
    std::string timestamp = std::to_string(currentTimeMs());
    std::string signature = sign(timestamp, method, request_path, body);
    std::string encrypted_passphrase = encryptPassphrase();

    request.headers["KC-API-KEY"] = kucoin_config_.api_key;
    request.headers["KC-API-SIGN"] = signature;
    request.headers["KC-API-TIMESTAMP"] = timestamp;
    request.headers["KC-API-PASSPHRASE"] = encrypted_passphrase;
    request.headers["KC-API-KEY-VERSION"] = "2";  // Version 2 uses encrypted passphrase
    request.headers["Content-Type"] = "application/json";
}

// ============================================================================
// REST Helpers
// ============================================================================

network::HttpResponse KuCoinExchange::signedGet(
    const std::string& endpoint,
    const std::unordered_map<std::string, std::string>& params) {

    std::string query_string;
    if (!params.empty()) {
        std::stringstream ss;
        bool first = true;
        for (const auto& [key, value] : params) {
            if (!first) ss << "&";
            first = false;
            ss << key << "=" << value;
        }
        query_string = "?" + ss.str();
    }

    std::string full_path = endpoint + query_string;

    network::HttpRequest request;
    request.method = network::HttpMethod::GET;
    request.path = endpoint;
    request.query_params = params;
    addAuthHeaders(request, "GET", full_path, "");

    return rest_client_->execute(request);
}

network::HttpResponse KuCoinExchange::signedPost(
    const std::string& endpoint,
    const nlohmann::json& body) {

    std::string body_str = body.empty() ? "" : body.dump();

    network::HttpRequest request;
    request.method = network::HttpMethod::POST;
    request.path = endpoint;
    request.body = body_str;
    addAuthHeaders(request, "POST", endpoint, body_str);

    return rest_client_->execute(request);
}

network::HttpResponse KuCoinExchange::signedDelete(
    const std::string& endpoint,
    const std::unordered_map<std::string, std::string>& params) {

    std::string query_string;
    if (!params.empty()) {
        std::stringstream ss;
        bool first = true;
        for (const auto& [key, value] : params) {
            if (!first) ss << "&";
            first = false;
            ss << key << "=" << value;
        }
        query_string = "?" + ss.str();
    }

    std::string full_path = endpoint + query_string;

    network::HttpRequest request;
    request.method = network::HttpMethod::DELETE;
    request.path = endpoint;
    request.query_params = params;
    addAuthHeaders(request, "DELETE", full_path, "");

    return rest_client_->execute(request);
}

network::HttpResponse KuCoinExchange::publicGet(
    const std::string& endpoint,
    const std::unordered_map<std::string, std::string>& params) {

    return rest_client_->get(endpoint, params);
}

// ============================================================================
// WebSocket Handlers
// ============================================================================

void KuCoinExchange::handleWsOpen(bool is_private) {
    if (is_private) {
        private_ws_connected_ = true;
    } else {
        public_ws_connected_ = true;
        onConnectionStatus(ConnectionStatus::Connected);
    }
}

void KuCoinExchange::handleWsClose(int code, const std::string& reason, bool is_private) {
    if (is_private) {
        private_ws_connected_ = false;
    } else {
        public_ws_connected_ = false;
        onConnectionStatus(ConnectionStatus::Reconnecting);
    }
}

void KuCoinExchange::handleWsError(const std::string& error, bool is_private) {
    onError("WebSocket error (" + std::string(is_private ? "private" : "public") + "): " + error);
}

void KuCoinExchange::handlePublicWsMessage(const std::string& message, network::MessageType type) {
    try {
        auto json = nlohmann::json::parse(message);

        // Check message type
        std::string msg_type = json.value("type", "");

        if (msg_type == "pong" || msg_type == "welcome" || msg_type == "ack") {
            return;  // Ignore control messages
        }

        if (msg_type == "error") {
            onError("WebSocket error: " + json.value("data", "unknown"));
            return;
        }

        if (msg_type != "message") {
            return;
        }

        // Parse topic
        std::string topic = json.value("topic", "");
        auto data = json.value("data", nlohmann::json{});
        std::string subject = json.value("subject", "");

        // Extract symbol from topic
        std::string symbol;
        size_t colon_pos = topic.find(':');
        if (colon_pos != std::string::npos) {
            symbol = topic.substr(colon_pos + 1);
        }

        // Route to appropriate handler
        if (topic.find("/market/level2") != std::string::npos) {
            handleOrderBookUpdate(data, symbol);
        } else if (topic.find("/market/match") != std::string::npos ||
                   subject == "trade.l3match") {
            handleTradeUpdate(data, symbol);
        } else if (topic.find("/market/ticker") != std::string::npos) {
            handleTickerUpdate(data, symbol);
        }

    } catch (const std::exception& e) {
        onError("Public WS message parse error: " + std::string(e.what()));
    }
}

void KuCoinExchange::handlePrivateWsMessage(const std::string& message, network::MessageType type) {
    try {
        auto json = nlohmann::json::parse(message);

        // Check message type
        std::string msg_type = json.value("type", "");

        if (msg_type == "pong" || msg_type == "welcome" || msg_type == "ack") {
            return;  // Ignore control messages
        }

        if (msg_type == "error") {
            onError("Private WebSocket error: " + json.value("data", "unknown"));
            return;
        }

        if (msg_type != "message") {
            return;
        }

        // Parse topic
        std::string topic = json.value("topic", "");
        auto data = json.value("data", nlohmann::json{});
        std::string subject = json.value("subject", "");

        // Route to appropriate handler
        if (topic.find("/spotMarket/tradeOrders") != std::string::npos ||
            subject == "orderChange") {
            handleOrderUpdate(data);
        } else if (topic.find("/account/balance") != std::string::npos) {
            handleBalanceUpdate(data);
        }

    } catch (const std::exception& e) {
        onError("Private WS message parse error: " + std::string(e.what()));
    }
}

void KuCoinExchange::handleOrderBookUpdate(const nlohmann::json& data, const std::string& symbol) {
    try {
        uint64_t seq = data.value("sequenceEnd", uint64_t(0));

        // Check sequence
        {
            std::lock_guard<std::mutex> lock(orderbook_seq_mutex_);
            auto it = orderbook_seq_.find(symbol);
            if (it != orderbook_seq_.end() && seq <= it->second) {
                return;  // Old update
            }
            orderbook_seq_[symbol] = seq;
        }

        // Get cached orderbook or create new
        auto cached = getCachedOrderBook(symbol);
        OrderBook ob;

        if (cached) {
            ob = *cached;
        } else {
            ob.symbol = symbol;
            ob.exchange = "kucoin";
        }

        ob.sequence = seq;
        ob.timestamp = currentTimeMs();
        ob.local_timestamp = currentTimeNs();

        // Apply changes
        auto applyChanges = [](std::vector<PriceLevel>& levels,
                              const nlohmann::json& changes,
                              bool ascending) {
            for (const auto& change : changes) {
                if (!change.is_array() || change.size() < 2) continue;

                double price = std::stod(change[0].get<std::string>());
                double qty = std::stod(change[1].get<std::string>());

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

        // KuCoin sends "changes" with "asks" and "bids"
        auto changes = data.value("changes", nlohmann::json{});
        if (changes.contains("asks")) {
            applyChanges(ob.asks, changes["asks"], true);
        }
        if (changes.contains("bids")) {
            applyChanges(ob.bids, changes["bids"], false);
        }

        onOrderBook(ob);

    } catch (const std::exception& e) {
        onError("OrderBook update error: " + std::string(e.what()));
    }
}

void KuCoinExchange::handleTradeUpdate(const nlohmann::json& data, const std::string& symbol) {
    try {
        Trade trade;
        trade.exchange = "kucoin";
        trade.symbol = symbol;
        trade.trade_id = data.value("tradeId", data.value("sequence", ""));
        trade.price = std::stod(data.value("price", "0"));
        trade.quantity = std::stod(data.value("size", "0"));

        // Handle timestamp - KuCoin uses nanoseconds
        std::string time_str = data.value("time", "0");
        if (time_str.length() > 13) {
            // Nanoseconds, convert to milliseconds
            trade.timestamp = std::stoull(time_str) / 1000000;
        } else {
            trade.timestamp = std::stoull(time_str);
        }

        trade.local_timestamp = currentTimeNs();

        std::string side = data.value("side", "buy");
        trade.side = (side == "buy") ? OrderSide::Buy : OrderSide::Sell;
        trade.is_maker = data.value("makerOrderId", "") != "";

        onTrade(trade);

    } catch (const std::exception& e) {
        onError("Trade update error: " + std::string(e.what()));
    }
}

void KuCoinExchange::handleTickerUpdate(const nlohmann::json& data, const std::string& symbol) {
    try {
        Ticker ticker;
        ticker.exchange = "kucoin";
        ticker.symbol = symbol;
        ticker.last = std::stod(data.value("price", data.value("last", "0")));
        ticker.bid = std::stod(data.value("bestBid", "0"));
        ticker.ask = std::stod(data.value("bestAsk", "0"));
        ticker.bid_qty = std::stod(data.value("bestBidSize", "0"));
        ticker.ask_qty = std::stod(data.value("bestAskSize", "0"));
        ticker.volume_24h = std::stod(data.value("vol", data.value("volume", "0")));
        ticker.volume_quote_24h = std::stod(data.value("volValue", "0"));
        ticker.change_24h_pct = std::stod(data.value("changeRate", "0")) * 100;
        ticker.high_24h = std::stod(data.value("high", "0"));
        ticker.low_24h = std::stod(data.value("low", "0"));
        ticker.timestamp = currentTimeMs();

        onTicker(ticker);

    } catch (const std::exception& e) {
        onError("Ticker update error: " + std::string(e.what()));
    }
}

void KuCoinExchange::handleOrderUpdate(const nlohmann::json& data) {
    try {
        Order order;
        order.exchange = "kucoin";
        order.symbol = data.value("symbol", "");
        order.order_id = data.value("orderId", "");
        order.client_order_id = data.value("clientOid", "");
        order.side = parseOrderSide(data.value("side", "buy"));
        order.type = parseOrderType(data.value("orderType", data.value("type", "limit")));
        order.status = parseOrderStatus(data.value("status", data.value("type", "")));

        order.quantity = std::stod(data.value("size", data.value("originSize", "0")));
        order.filled_quantity = std::stod(data.value("filledSize", "0"));
        order.remaining_quantity = std::stod(data.value("remainSize", "0"));
        order.price = std::stod(data.value("price", "0"));

        // Calculate average price from matchPrice or funds/filledSize
        if (data.contains("matchPrice")) {
            order.average_price = std::stod(data.value("matchPrice", "0"));
        } else if (order.filled_quantity > 0 && data.contains("funds")) {
            double funds = std::stod(data.value("funds", "0"));
            order.average_price = funds / order.filled_quantity;
        }

        order.update_time = std::stoull(data.value("ts", std::to_string(currentTimeMs())));
        order.raw = data;

        onOrder(order);

    } catch (const std::exception& e) {
        onError("Order update error: " + std::string(e.what()));
    }
}

void KuCoinExchange::handleBalanceUpdate(const nlohmann::json& data) {
    try {
        Balance balance;
        balance.asset = data.value("currency", "");
        balance.free = std::stod(data.value("available", "0"));
        balance.locked = std::stod(data.value("hold", "0"));

        onBalance(balance);

    } catch (const std::exception& e) {
        onError("Balance update error: " + std::string(e.what()));
    }
}

// ============================================================================
// Subscription Management
// ============================================================================

bool KuCoinExchange::subscribeOrderBook(const std::string& symbol, int depth) {
    // KuCoin uses level2 topic for orderbook
    std::string topic = "/market/level2:" + symbol;

    sendSubscribe(topic, false);

    // Initialize orderbook from REST snapshot
    auto snapshot = getOrderBookSnapshot(symbol, depth);
    if (snapshot) {
        std::lock_guard<std::mutex> lock(orderbook_seq_mutex_);
        orderbook_seq_[symbol] = snapshot->sequence;
        onOrderBook(*snapshot);
    }

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.insert(topic);
    return true;
}

bool KuCoinExchange::unsubscribeOrderBook(const std::string& symbol) {
    std::string topic = "/market/level2:" + symbol;
    sendUnsubscribe(topic, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.erase(topic);

    clearOrderBookCache(symbol);
    return true;
}

bool KuCoinExchange::subscribeTrades(const std::string& symbol) {
    std::string topic = "/market/match:" + symbol;
    sendSubscribe(topic, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.insert(topic);
    return true;
}

bool KuCoinExchange::unsubscribeTrades(const std::string& symbol) {
    std::string topic = "/market/match:" + symbol;
    sendUnsubscribe(topic, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.erase(topic);
    return true;
}

bool KuCoinExchange::subscribeTicker(const std::string& symbol) {
    std::string topic = "/market/ticker:" + symbol;
    sendSubscribe(topic, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.insert(topic);
    return true;
}

bool KuCoinExchange::unsubscribeTicker(const std::string& symbol) {
    std::string topic = "/market/ticker:" + symbol;
    sendUnsubscribe(topic, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.erase(topic);
    return true;
}

bool KuCoinExchange::subscribeUserData() {
    // Get private WebSocket endpoint
    std::string ws_url = getPrivateWsEndpoint();
    if (ws_url.empty() || private_ws_token_.token.empty()) {
        onError("Failed to get private WebSocket endpoint");
        return false;
    }

    // Initialize private WebSocket client
    network::WebSocketConfig ws_config;
    ws_config.url = ws_url;
    ws_config.name = "kucoin_private_ws";
    ws_config.ping_interval_ms = private_ws_token_.ping_interval_ms;
    ws_config.pong_timeout_ms = private_ws_token_.ping_timeout_ms;
    ws_config.auto_reconnect = true;

    private_ws_client_ = std::make_shared<network::WebSocketClient>(ws_config);

    private_ws_client_->setOnOpen([this]() {
        this->handleWsOpen(true);

        // Subscribe to user data channels after connection
        if (kucoin_config_.spot) {
            // Spot trade orders
            sendSubscribe("/spotMarket/tradeOrders", true);
            // Account balance changes
            sendSubscribe("/account/balance", true);
        } else {
            // Futures orders and positions
            sendSubscribe("/contractMarket/tradeOrders", true);
            sendSubscribe("/contractAccount/wallet", true);
        }
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

    return private_ws_client_->connect();
}

bool KuCoinExchange::unsubscribeUserData() {
    if (private_ws_client_) {
        private_ws_client_->disconnect();
        private_ws_client_.reset();
    }

    private_ws_connected_ = false;
    return true;
}

void KuCoinExchange::sendSubscribe(const std::string& topic, bool is_private) {
    auto client = is_private ? private_ws_client_ : public_ws_client_;
    if (!client) return;

    nlohmann::json msg;
    msg["id"] = std::to_string(next_req_id_++);
    msg["type"] = "subscribe";
    msg["topic"] = topic;
    msg["privateChannel"] = is_private;
    msg["response"] = true;

    client->send(msg);
}

void KuCoinExchange::sendUnsubscribe(const std::string& topic, bool is_private) {
    auto client = is_private ? private_ws_client_ : public_ws_client_;
    if (!client) return;

    nlohmann::json msg;
    msg["id"] = std::to_string(next_req_id_++);
    msg["type"] = "unsubscribe";
    msg["topic"] = topic;
    msg["privateChannel"] = is_private;
    msg["response"] = true;

    client->send(msg);
}

// ============================================================================
// Order Management
// ============================================================================

std::optional<Order> KuCoinExchange::placeOrder(const OrderRequest& request) {
    nlohmann::json body;
    body["clientOid"] = request.client_order_id.empty()
        ? generateClientOrderId()
        : request.client_order_id;
    body["symbol"] = request.symbol;
    body["side"] = orderSideToString(request.side);
    body["type"] = orderTypeToString(request.type);
    body["size"] = std::to_string(request.quantity);

    if (request.type != OrderType::Market) {
        body["price"] = std::to_string(request.price);
        body["timeInForce"] = timeInForceToString(request.time_in_force);
    }

    if (request.stop_price > 0.0) {
        body["stopPrice"] = std::to_string(request.stop_price);
        body["stop"] = request.side == OrderSide::Buy ? "loss" : "entry";
    }

    // Post-only order
    if (request.time_in_force == TimeInForce::PostOnly) {
        body["postOnly"] = true;
    }

    // Use HF (High-Frequency) orders endpoint for spot trading as recommended by KuCoin
    // The old /api/v1/orders endpoint is deprecated
    std::string endpoint = kucoin_config_.spot
        ? "/api/v1/hf/orders"
        : "/api/v1/orders";

    auto response = signedPost(endpoint, body);

    if (!response.success) {
        onError("Place order failed: " + response.error + " " + response.body);
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("code", "") != "200000") {
            onError("Place order failed: " + json.value("msg", "unknown"));
            return std::nullopt;
        }

        auto data = json["data"];
        Order order;
        order.exchange = "kucoin";
        order.order_id = data.value("orderId", "");
        order.client_order_id = body["clientOid"];
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

std::optional<Order> KuCoinExchange::cancelOrder(const std::string& symbol,
                                                  const std::string& order_id) {
    // Use HF orders endpoint for spot trading (deprecated /api/v1/orders/{orderId})
    std::string endpoint = kucoin_config_.spot
        ? "/api/v1/hf/orders/" + order_id
        : "/api/v1/orders/" + order_id;

    auto response = signedDelete(endpoint);

    if (!response.success) {
        onError("Cancel order failed: " + response.error);
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("code", "") != "200000") {
            onError("Cancel order failed: " + json.value("msg", "unknown"));
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

std::optional<Order> KuCoinExchange::cancelOrder(const std::string& symbol,
                                                  const std::string& order_id,
                                                  const std::string& client_order_id) {
    if (!order_id.empty()) {
        return cancelOrder(symbol, order_id);
    }

    // Cancel by client order ID using HF endpoint for spot
    std::string endpoint = kucoin_config_.spot
        ? "/api/v1/hf/orders/client-order/" + client_order_id
        : "/api/v1/orders/client-order/" + client_order_id;

    auto response = signedDelete(endpoint);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("code", "") != "200000") {
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

bool KuCoinExchange::cancelAllOrders(const std::string& symbol) {
    std::unordered_map<std::string, std::string> params;
    if (!symbol.empty()) {
        params["symbol"] = symbol;
    }

    // Use HF orders endpoint for spot trading
    std::string endpoint = kucoin_config_.spot
        ? "/api/v1/hf/orders"
        : "/api/v1/orders";

    auto response = signedDelete(endpoint, params);

    if (!response.success) {
        return false;
    }

    auto json = response.json();
    return json.value("code", "") == "200000";
}

std::optional<Order> KuCoinExchange::getOrder(const std::string& symbol,
                                               const std::string& order_id) {
    // Use HF orders endpoint for spot trading
    std::string endpoint = kucoin_config_.spot
        ? "/api/v1/hf/orders/" + order_id
        : "/api/v1/orders/" + order_id;

    auto response = signedGet(endpoint);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("code", "") != "200000") {
            return std::nullopt;
        }

        return parseOrder(json["data"]);

    } catch (...) {
        return std::nullopt;
    }
}

std::vector<Order> KuCoinExchange::getOpenOrders(const std::string& symbol) {
    std::unordered_map<std::string, std::string> params;
    if (!symbol.empty()) {
        params["symbol"] = symbol;
    }

    // Use HF orders endpoint for spot trading - note: HF endpoint requires symbol
    // and doesn't use status parameter the same way
    std::string endpoint = kucoin_config_.spot
        ? "/api/v1/hf/orders/active"
        : "/api/v1/orders";

    // For non-HF futures endpoint, we still need the status parameter
    if (!kucoin_config_.spot) {
        params["status"] = "active";
    }

    auto response = signedGet(endpoint, params);

    std::vector<Order> orders;
    if (!response.success) {
        return orders;
    }

    try {
        auto json = response.json();
        if (json.value("code", "") != "200000") {
            return orders;
        }

        auto items = json["data"]["items"];
        for (const auto& item : items) {
            orders.push_back(parseOrder(item));
        }

    } catch (...) {
        // Return empty
    }

    return orders;
}

std::vector<Order> KuCoinExchange::getOrderHistory(const std::string& symbol,
                                                    uint64_t start_time,
                                                    uint64_t end_time,
                                                    int limit) {
    std::unordered_map<std::string, std::string> params;
    if (!symbol.empty()) {
        params["symbol"] = symbol;
    }
    if (start_time > 0) {
        params["startAt"] = std::to_string(start_time);
    }
    if (end_time > 0) {
        params["endAt"] = std::to_string(end_time);
    }
    params["pageSize"] = std::to_string(limit);

    // Use HF orders endpoint for spot trading - done orders
    std::string endpoint = kucoin_config_.spot
        ? "/api/v1/hf/orders/done"
        : "/api/v1/orders";

    // For non-HF futures endpoint, we still need the status parameter
    if (!kucoin_config_.spot) {
        params["status"] = "done";
    }

    auto response = signedGet(endpoint, params);

    std::vector<Order> orders;
    if (!response.success) {
        return orders;
    }

    try {
        auto json = response.json();
        if (json.value("code", "") != "200000") {
            return orders;
        }

        auto items = json["data"]["items"];
        for (const auto& item : items) {
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

std::optional<Account> KuCoinExchange::getAccount() {
    std::string endpoint = kucoin_config_.spot
        ? "/api/v1/accounts"
        : "/api/v1/account-overview";

    auto response = signedGet(endpoint);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("code", "") != "200000") {
            return std::nullopt;
        }

        Account account;
        account.exchange = "kucoin";
        account.update_time = currentTimeMs();

        if (kucoin_config_.spot) {
            auto data = json["data"];
            for (const auto& acc : data) {
                std::string acc_type = acc.value("type", "");
                if (acc_type != "trade") continue;  // Only trade accounts

                Balance balance;
                balance.asset = acc.value("currency", "");
                balance.free = std::stod(acc.value("available", "0"));
                balance.locked = std::stod(acc.value("holds", "0"));
                account.balances[balance.asset] = balance;
            }
        } else {
            auto data = json["data"];
            account.total_margin = std::stod(data.value("accountEquity", "0"));
            account.available_margin = std::stod(data.value("availableBalance", "0"));
            account.total_unrealized_pnl = std::stod(data.value("unrealisedPNL", "0"));

            Balance balance;
            balance.asset = data.value("currency", "USDT");
            balance.free = std::stod(data.value("availableBalance", "0"));
            balance.locked = std::stod(data.value("frozenFunds", "0"));
            account.balances[balance.asset] = balance;

            // Get positions
            account.positions = getPositions();
        }

        return account;

    } catch (const std::exception& e) {
        onError("Parse account failed: " + std::string(e.what()));
        return std::nullopt;
    }
}

std::optional<Balance> KuCoinExchange::getBalance(const std::string& asset) {
    if (kucoin_config_.spot) {
        std::unordered_map<std::string, std::string> params;
        params["currency"] = asset;
        params["type"] = "trade";

        auto response = signedGet("/api/v1/accounts", params);

        if (!response.success) {
            return std::nullopt;
        }

        try {
            auto json = response.json();
            if (json.value("code", "") != "200000") {
                return std::nullopt;
            }

            auto data = json["data"];
            if (data.empty()) {
                return std::nullopt;
            }

            Balance balance;
            balance.asset = asset;
            balance.free = std::stod(data[0].value("available", "0"));
            balance.locked = std::stod(data[0].value("holds", "0"));
            return balance;

        } catch (...) {
            return std::nullopt;
        }
    } else {
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
}

std::vector<Position> KuCoinExchange::getPositions(const std::string& symbol) {
    std::vector<Position> positions;

    if (kucoin_config_.spot) {
        return positions;  // No positions for spot
    }

    std::unordered_map<std::string, std::string> params;
    if (!symbol.empty()) {
        params["symbol"] = symbol;
    }

    auto response = signedGet("/api/v1/positions", params);

    if (!response.success) {
        return positions;
    }

    try {
        auto json = response.json();
        if (json.value("code", "") != "200000") {
            return positions;
        }

        auto data = json["data"];
        for (const auto& p : data) {
            double qty = std::stod(p.value("currentQty", "0"));
            if (qty == 0.0) continue;

            Position position;
            position.exchange = "kucoin";
            position.symbol = p.value("symbol", "");
            position.side = qty > 0 ? OrderSide::Buy : OrderSide::Sell;
            position.quantity = std::abs(qty);
            position.entry_price = std::stod(p.value("avgEntryPrice", "0"));
            position.mark_price = std::stod(p.value("markPrice", "0"));
            position.liquidation_price = std::stod(p.value("liquidationPrice", "0"));
            position.unrealized_pnl = std::stod(p.value("unrealisedPnl", "0"));
            position.realized_pnl = std::stod(p.value("realisedPnl", "0"));
            position.leverage = std::stod(p.value("realLeverage", "1"));
            position.margin = std::stod(p.value("posMaint", "0"));

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

std::vector<SymbolInfo> KuCoinExchange::getSymbols() {
    std::string endpoint = kucoin_config_.spot
        ? "/api/v1/symbols"
        : "/api/v1/contracts/active";

    auto response = publicGet(endpoint);

    std::vector<SymbolInfo> symbols;
    if (!response.success) {
        return symbols;
    }

    try {
        auto json = response.json();
        if (json.value("code", "") != "200000") {
            return symbols;
        }

        auto data = json["data"];
        for (const auto& item : data) {
            auto info = parseSymbolInfo(item);
            updateSymbolInfo(info);
            symbols.push_back(info);
        }

    } catch (...) {
        // Return empty
    }

    return symbols;
}

std::optional<SymbolInfo> KuCoinExchange::getSymbolInfo(const std::string& symbol) {
    // Check cache first
    {
        std::lock_guard<std::mutex> lock(symbol_info_mutex_);
        auto it = symbol_info_cache_.find(symbol);
        if (it != symbol_info_cache_.end()) {
            return it->second;
        }
    }

    // Fetch from exchange
    auto symbols = getSymbols();
    for (const auto& s : symbols) {
        if (s.symbol == symbol) {
            return s;
        }
    }

    return std::nullopt;
}

std::optional<OrderBook> KuCoinExchange::getOrderBookSnapshot(const std::string& symbol, int depth) {
    std::unordered_map<std::string, std::string> params;

    std::string endpoint;
    if (kucoin_config_.spot) {
        if (depth <= 20) {
            endpoint = "/api/v1/market/orderbook/level2_20";
        } else {
            endpoint = "/api/v1/market/orderbook/level2_100";
        }
        params["symbol"] = symbol;
    } else {
        endpoint = "/api/v1/level2/snapshot";
        params["symbol"] = symbol;
    }

    auto response = publicGet(endpoint, params);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("code", "") != "200000") {
            return std::nullopt;
        }

        auto data = json["data"];

        OrderBook ob;
        ob.symbol = symbol;
        ob.exchange = "kucoin";
        ob.sequence = std::stoull(data.value("sequence", "0"));
        ob.timestamp = std::stoull(data.value("time", std::to_string(currentTimeMs())));
        ob.local_timestamp = currentTimeNs();

        auto bids = data.value("bids", nlohmann::json::array());
        for (const auto& b : bids) {
            PriceLevel level;
            level.price = std::stod(b[0].get<std::string>());
            level.quantity = std::stod(b[1].get<std::string>());
            ob.bids.push_back(level);
        }

        auto asks = data.value("asks", nlohmann::json::array());
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

std::vector<Trade> KuCoinExchange::getRecentTrades(const std::string& symbol, int limit) {
    std::string endpoint = kucoin_config_.spot
        ? "/api/v1/market/histories"
        : "/api/v1/trade/history";

    std::unordered_map<std::string, std::string> params;
    params["symbol"] = symbol;

    auto response = publicGet(endpoint, params);

    std::vector<Trade> trades;
    if (!response.success) {
        return trades;
    }

    try {
        auto json = response.json();
        if (json.value("code", "") != "200000") {
            return trades;
        }

        auto data = json["data"];
        for (const auto& t : data) {
            Trade trade;
            trade.exchange = "kucoin";
            trade.symbol = symbol;
            trade.trade_id = t.value("sequence", "");
            trade.price = std::stod(t.value("price", "0"));
            trade.quantity = std::stod(t.value("size", "0"));

            std::string time_str = t.value("time", "0");
            if (time_str.length() > 13) {
                trade.timestamp = std::stoull(time_str) / 1000000;
            } else {
                trade.timestamp = std::stoull(time_str);
            }

            trade.side = t.value("side", "buy") == "buy" ? OrderSide::Buy : OrderSide::Sell;
            trades.push_back(trade);

            if (static_cast<int>(trades.size()) >= limit) break;
        }

    } catch (...) {
        // Return empty
    }

    return trades;
}

std::optional<Ticker> KuCoinExchange::getTicker(const std::string& symbol) {
    std::string endpoint = kucoin_config_.spot
        ? "/api/v1/market/stats"
        : "/api/v1/ticker";

    std::unordered_map<std::string, std::string> params;
    params["symbol"] = symbol;

    auto response = publicGet(endpoint, params);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("code", "") != "200000") {
            return std::nullopt;
        }

        auto data = json["data"];

        Ticker ticker;
        ticker.exchange = "kucoin";
        ticker.symbol = symbol;
        ticker.last = std::stod(data.value("last", "0"));
        ticker.bid = std::stod(data.value("buy", data.value("bestBidPrice", "0")));
        ticker.ask = std::stod(data.value("sell", data.value("bestAskPrice", "0")));
        ticker.bid_qty = std::stod(data.value("bestBidSize", "0"));
        ticker.ask_qty = std::stod(data.value("bestAskSize", "0"));
        ticker.high_24h = std::stod(data.value("high", "0"));
        ticker.low_24h = std::stod(data.value("low", "0"));
        ticker.volume_24h = std::stod(data.value("vol", data.value("volume", "0")));
        ticker.volume_quote_24h = std::stod(data.value("volValue", "0"));
        ticker.change_24h_pct = std::stod(data.value("changeRate", "0")) * 100;
        ticker.timestamp = std::stoull(data.value("time", std::to_string(currentTimeMs())));

        return ticker;

    } catch (...) {
        return std::nullopt;
    }
}

uint64_t KuCoinExchange::getServerTime() {
    auto response = publicGet("/api/v1/timestamp");

    if (response.success) {
        auto json = response.json();
        if (json.value("code", "") == "200000") {
            return json.value("data", uint64_t(0));
        }
    }

    return 0;
}

std::string KuCoinExchange::formatSymbol(const std::string& base, const std::string& quote) const {
    // KuCoin uses dash separator: BTC-USDT
    return base + "-" + quote;
}

std::pair<std::string, std::string> KuCoinExchange::parseSymbol(const std::string& symbol) const {
    auto pos = symbol.find('-');
    if (pos != std::string::npos) {
        return {symbol.substr(0, pos), symbol.substr(pos + 1)};
    }
    return {symbol, ""};
}

// ============================================================================
// Data Converters
// ============================================================================

Order KuCoinExchange::parseOrder(const nlohmann::json& data) {
    Order order;
    order.exchange = "kucoin";
    order.symbol = data.value("symbol", "");
    order.order_id = data.value("id", data.value("orderId", ""));
    order.client_order_id = data.value("clientOid", "");
    order.side = parseOrderSide(data.value("side", "buy"));
    order.type = parseOrderType(data.value("type", "limit"));

    // Status from isActive or status field
    if (data.contains("isActive")) {
        order.status = data["isActive"].get<bool>() ? OrderStatus::New : OrderStatus::Filled;
    } else {
        order.status = parseOrderStatus(data.value("status", ""));
    }

    order.time_in_force = parseTimeInForce(data.value("timeInForce", "GTC"));
    order.quantity = std::stod(data.value("size", "0"));
    order.filled_quantity = std::stod(data.value("dealSize", data.value("filledSize", "0")));
    order.remaining_quantity = order.quantity - order.filled_quantity;
    order.price = std::stod(data.value("price", "0"));

    // Calculate average price from dealFunds/dealSize
    if (order.filled_quantity > 0 && data.contains("dealFunds")) {
        double deal_funds = std::stod(data.value("dealFunds", "0"));
        order.average_price = deal_funds / order.filled_quantity;
    }

    order.stop_price = std::stod(data.value("stopPrice", "0"));
    order.commission = std::stod(data.value("fee", "0"));
    order.commission_asset = data.value("feeCurrency", "");

    // Parse timestamps - KuCoin uses milliseconds
    order.create_time = std::stoull(data.value("createdAt", "0"));
    order.update_time = order.create_time;

    order.raw = data;

    return order;
}

OrderStatus KuCoinExchange::parseOrderStatus(const std::string& status) {
    if (status == "active" || status == "open") return OrderStatus::New;
    if (status == "done" || status == "filled") return OrderStatus::Filled;
    if (status == "cancelled" || status == "canceled") return OrderStatus::Cancelled;
    if (status == "match") return OrderStatus::PartiallyFilled;
    return OrderStatus::New;
}

OrderSide KuCoinExchange::parseOrderSide(const std::string& side) {
    return (side == "buy") ? OrderSide::Buy : OrderSide::Sell;
}

OrderType KuCoinExchange::parseOrderType(const std::string& type) {
    if (type == "market") return OrderType::Market;
    if (type == "limit") return OrderType::Limit;
    if (type == "stop") return OrderType::StopMarket;
    if (type == "limit_stop" || type == "stop_limit") return OrderType::StopLimit;
    return OrderType::Limit;
}

TimeInForce KuCoinExchange::parseTimeInForce(const std::string& tif) {
    if (tif == "GTC") return TimeInForce::GTC;
    if (tif == "IOC") return TimeInForce::IOC;
    if (tif == "FOK") return TimeInForce::FOK;
    if (tif == "GTT") return TimeInForce::GTD;
    return TimeInForce::GTC;
}

std::string KuCoinExchange::orderSideToString(OrderSide side) {
    return side == OrderSide::Buy ? "buy" : "sell";
}

std::string KuCoinExchange::orderTypeToString(OrderType type) {
    switch (type) {
        case OrderType::Market: return "market";
        case OrderType::Limit: return "limit";
        case OrderType::StopMarket: return "stop";
        case OrderType::StopLimit: return "limit_stop";
        default: return "limit";
    }
}

std::string KuCoinExchange::timeInForceToString(TimeInForce tif) {
    switch (tif) {
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
        case TimeInForce::GTD: return "GTT";
        case TimeInForce::PostOnly: return "GTC";  // Use with postOnly flag
        default: return "GTC";
    }
}

SymbolInfo KuCoinExchange::parseSymbolInfo(const nlohmann::json& data) {
    SymbolInfo info;
    info.symbol = data.value("symbol", "");
    info.base_asset = data.value("baseCurrency", data.value("baseCoin", ""));
    info.quote_asset = data.value("quoteCurrency", data.value("quoteCoin", ""));
    info.trading_enabled = data.value("enableTrading", data.value("isOpen", true));

    if (kucoin_config_.spot) {
        info.min_qty = std::stod(data.value("baseMinSize", "0"));
        info.max_qty = std::stod(data.value("baseMaxSize", "0"));
        info.step_size = std::stod(data.value("baseIncrement", "0"));
        info.tick_size = std::stod(data.value("priceIncrement", "0"));
        info.min_notional = std::stod(data.value("quoteMinSize", "0"));
    } else {
        info.min_qty = std::stod(data.value("minOrderQty", data.value("lotSize", "1")));
        info.max_qty = std::stod(data.value("maxOrderQty", "0"));
        info.step_size = std::stod(data.value("lotSize", "1"));
        info.tick_size = std::stod(data.value("tickSize", "0"));
    }

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
// Factory Functions
// ============================================================================

std::shared_ptr<KuCoinExchange> createKuCoinExchange(const ExchangeConfig& config) {
    return std::make_shared<KuCoinExchange>(config);
}

std::shared_ptr<KuCoinExchange> createKuCoinExchange(const KuCoinConfig& config) {
    return std::make_shared<KuCoinExchange>(config);
}

} // namespace exchange
} // namespace hft
