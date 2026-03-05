#include "exchange/htx.hpp"

#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <zlib.h>

#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <cmath>

namespace hft {
namespace exchange {

// ============================================================================
// Constructors/Destructor
// ============================================================================

HTXExchange::HTXExchange(const ExchangeConfig& config)
    : ExchangeBase(config) {
    htx_config_.api_key = config.api_key;
    htx_config_.api_secret = config.api_secret;
    htx_config_.testnet = config.testnet;
    htx_config_.spot = (config.type == ExchangeType::Spot);
    htx_config_.order_rate_limit = config.orders_per_second;
    htx_config_.request_rate_limit = config.requests_per_minute;

    // Initialize REST client
    network::RestClientConfig rest_config;
    rest_config.base_url = getRestUrl();
    rest_config.name = "htx_rest";
    rest_config.request_timeout_ms = config.rest_timeout_ms;
    rest_config.max_retries = config.rest_max_retries;
    rest_config.rate_limit.requests_per_second = htx_config_.order_rate_limit;
    rest_config.rate_limit.requests_per_minute = htx_config_.request_rate_limit;

    rest_client_ = std::make_shared<network::RestClient>(rest_config);
}

HTXExchange::HTXExchange(const HTXConfig& htx_config)
    : ExchangeBase(ExchangeConfig{
        .name = "HTX",
        .api_key = htx_config.api_key,
        .api_secret = htx_config.api_secret,
        .type = htx_config.spot ? ExchangeType::Spot : ExchangeType::Perpetual,
        .testnet = htx_config.testnet
      }),
      htx_config_(htx_config) {

    // Initialize REST client
    network::RestClientConfig rest_config;
    rest_config.base_url = getRestUrl();
    rest_config.name = "htx_rest";
    rest_config.rate_limit.requests_per_second = htx_config_.order_rate_limit;
    rest_config.rate_limit.requests_per_minute = htx_config_.request_rate_limit;

    rest_client_ = std::make_shared<network::RestClient>(rest_config);
}

HTXExchange::~HTXExchange() {
    disconnect();
}

// ============================================================================
// URL Builders
// ============================================================================

std::string HTXExchange::getSpotRestUrl() const {
    return htx_config_.testnet
        ? "https://api.testnet.huobi.pro"
        : "https://api.huobi.pro";
}

std::string HTXExchange::getFuturesRestUrl() const {
    return htx_config_.testnet
        ? "https://api.hbdm.vn"
        : "https://api.hbdm.com";
}

std::string HTXExchange::getSpotWsUrl() const {
    return htx_config_.testnet
        ? "wss://api.testnet.huobi.pro/ws"
        : "wss://api.huobi.pro/ws";
}

std::string HTXExchange::getFuturesWsUrl() const {
    return htx_config_.testnet
        ? "wss://api.hbdm.vn/linear-swap-ws"
        : "wss://api.hbdm.com/linear-swap-ws";
}

std::string HTXExchange::getRestUrl() const {
    return htx_config_.spot ? getSpotRestUrl() : getFuturesRestUrl();
}

std::string HTXExchange::getWsUrl() const {
    return htx_config_.spot ? getSpotWsUrl() : getFuturesWsUrl();
}

std::string HTXExchange::getPrivateWsUrl() const {
    if (htx_config_.spot) {
        return htx_config_.testnet
            ? "wss://api.testnet.huobi.pro/ws/v2"
            : "wss://api.huobi.pro/ws/v2";
    } else {
        return htx_config_.testnet
            ? "wss://api.hbdm.vn/linear-swap-notification"
            : "wss://api.hbdm.com/linear-swap-notification";
    }
}

// ============================================================================
// Connection Management
// ============================================================================

bool HTXExchange::connect() {
    if (public_ws_connected_.load()) {
        return true;
    }

    onConnectionStatus(ConnectionStatus::Connecting);

    // Initialize public WebSocket client
    // Note: HTX sends GZIP compressed binary messages
    network::WebSocketConfig ws_config;
    ws_config.url = getWsUrl();
    ws_config.name = "htx_public_ws";
    ws_config.ping_interval_ms = 0;  // HTX uses custom ping/pong with timestamps
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

void HTXExchange::disconnect() {
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

bool HTXExchange::isConnected() const {
    return public_ws_connected_.load();
}

ConnectionStatus HTXExchange::connectionStatus() const {
    return connection_status_.load();
}

// ============================================================================
// GZIP Decompression
// ============================================================================

std::string HTXExchange::decompressGzip(const std::string& compressed) {
    z_stream zs;
    memset(&zs, 0, sizeof(zs));

    // Use MAX_WBITS + 16 for gzip decoding
    if (inflateInit2(&zs, MAX_WBITS + 16) != Z_OK) {
        return "";
    }

    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.data()));
    zs.avail_in = static_cast<uInt>(compressed.size());

    int ret;
    char buffer[32768];
    std::string decompressed;

    do {
        zs.next_out = reinterpret_cast<Bytef*>(buffer);
        zs.avail_out = sizeof(buffer);

        ret = inflate(&zs, Z_NO_FLUSH);

        if (decompressed.size() < zs.total_out) {
            decompressed.append(buffer, zs.total_out - decompressed.size());
        }
    } while (ret == Z_OK);

    inflateEnd(&zs);

    if (ret != Z_STREAM_END) {
        return "";
    }

    return decompressed;
}

// ============================================================================
// Signature Generation
// ============================================================================

std::string HTXExchange::generateTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm* gmt = std::gmtime(&time_t_now);

    std::ostringstream ss;
    ss << std::put_time(gmt, "%Y-%m-%dT%H:%M:%S");

    return ss.str();
}

std::string HTXExchange::urlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (char c : value) {
        // Keep alphanumeric and other safe characters
        if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << std::uppercase;
            escaped << '%' << std::setw(2) << int(static_cast<unsigned char>(c));
            escaped << std::nouppercase;
        }
    }

    return escaped.str();
}

std::string HTXExchange::sign(const std::string& method, const std::string& host,
                              const std::string& path, const std::string& params) {
    // Create pre-hash string: METHOD\nHOST\nPATH\nPARAMS
    std::string pre_hash = method + "\n" + host + "\n" + path + "\n" + params;

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;

    HMAC(EVP_sha256(),
         htx_config_.api_secret.c_str(),
         static_cast<int>(htx_config_.api_secret.length()),
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

std::string HTXExchange::buildSignedQuery(const std::string& method, const std::string& path,
                                          std::unordered_map<std::string, std::string>& params) {
    // Add required auth parameters
    params["AccessKeyId"] = htx_config_.api_key;
    params["SignatureMethod"] = "HmacSHA256";
    params["SignatureVersion"] = "2";
    params["Timestamp"] = urlEncode(generateTimestamp());

    // Sort parameters alphabetically and build query string
    std::vector<std::pair<std::string, std::string>> sorted_params(params.begin(), params.end());
    std::sort(sorted_params.begin(), sorted_params.end());

    std::stringstream ss;
    bool first = true;
    for (const auto& [key, value] : sorted_params) {
        if (!first) ss << "&";
        first = false;
        ss << key << "=" << urlEncode(value);
    }
    std::string query_string = ss.str();

    // Extract host from REST URL
    std::string url = getRestUrl();
    std::string host;
    size_t start = url.find("://");
    if (start != std::string::npos) {
        start += 3;
        size_t end = url.find('/', start);
        host = url.substr(start, end - start);
    }

    // Generate signature
    std::string signature = sign(method, host, path, query_string);

    // Add signature to query string
    return query_string + "&Signature=" + urlEncode(signature);
}

// ============================================================================
// WebSocket Handlers
// ============================================================================

void HTXExchange::handleWsOpen(bool is_private) {
    if (is_private) {
        private_ws_connected_ = true;
        authenticatePrivateWs();
    } else {
        public_ws_connected_ = true;
        onConnectionStatus(ConnectionStatus::Connected);
    }
}

void HTXExchange::handleWsClose(int code, const std::string& reason, bool is_private) {
    if (is_private) {
        private_ws_connected_ = false;
        private_ws_authenticated_ = false;
    } else {
        public_ws_connected_ = false;
        onConnectionStatus(ConnectionStatus::Reconnecting);
    }
}

void HTXExchange::handleWsError(const std::string& error, bool is_private) {
    onError("WebSocket error (" + std::string(is_private ? "private" : "public") + "): " + error);
}

void HTXExchange::handlePublicWsMessage(const std::string& message, network::MessageType type) {
    try {
        // HTX sends GZIP compressed messages
        std::string decompressed;
        if (type == network::MessageType::Binary) {
            decompressed = decompressGzip(message);
            if (decompressed.empty()) {
                onError("Failed to decompress GZIP message");
                return;
            }
        } else {
            decompressed = message;
        }

        auto json = nlohmann::json::parse(decompressed);

        // Handle ping - must respond with pong
        if (json.contains("ping")) {
            uint64_t ts = json["ping"].get<uint64_t>();
            sendPong(ts, false);
            return;
        }

        // Handle pong response (ignore)
        if (json.contains("pong")) {
            return;
        }

        // Handle subscription response
        if (json.contains("subbed")) {
            std::string subbed = json.value("subbed", "");
            std::string status = json.value("status", "");
            if (status != "ok") {
                onError("Subscription failed for " + subbed + ": " + json.value("err-msg", "unknown"));
            }
            return;
        }

        // Handle data messages
        if (!json.contains("ch")) {
            return;
        }

        std::string channel = json["ch"].get<std::string>();

        // Route to appropriate handler based on channel
        if (channel.find(".depth.") != std::string::npos) {
            handleDepthUpdate(json, channel);
        } else if (channel.find(".trade.") != std::string::npos) {
            handleTradeUpdate(json);
        } else if (channel.find(".ticker") != std::string::npos) {
            handleTickerUpdate(json);
        }

    } catch (const std::exception& e) {
        onError("Public WS message parse error: " + std::string(e.what()));
    }
}

void HTXExchange::handlePrivateWsMessage(const std::string& message, network::MessageType type) {
    try {
        // HTX private WS also sends GZIP compressed messages
        std::string decompressed;
        if (type == network::MessageType::Binary) {
            decompressed = decompressGzip(message);
            if (decompressed.empty()) {
                onError("Failed to decompress GZIP message");
                return;
            }
        } else {
            decompressed = message;
        }

        auto json = nlohmann::json::parse(decompressed);

        // Handle ping - must respond with pong
        if (json.contains("ping") || json.contains("action") && json["action"] == "ping") {
            uint64_t ts;
            if (json.contains("ping")) {
                ts = json["ping"].get<uint64_t>();
            } else {
                ts = json["data"]["ts"].get<uint64_t>();
            }
            sendPong(ts, true);
            return;
        }

        // Handle authentication response
        if (json.contains("action") && json["action"] == "req" &&
            json.contains("ch") && json["ch"] == "auth") {
            int code = json.value("code", -1);
            if (code == 200) {
                private_ws_authenticated_ = true;
            } else {
                onError("Private WS authentication failed: " + json.value("message", "unknown"));
            }
            return;
        }

        // Handle subscription response
        if (json.contains("action") && json["action"] == "sub") {
            int code = json.value("code", -1);
            if (code != 200) {
                onError("Private subscription failed: " + json.value("message", "unknown"));
            }
            return;
        }

        // Handle data messages
        std::string action = json.value("action", "");
        if (action != "push") {
            return;
        }

        std::string channel = json.value("ch", "");
        auto data = json.value("data", nlohmann::json{});

        if (channel.find("orders") != std::string::npos) {
            handleOrderUpdate(data);
        } else if (channel.find("accounts") != std::string::npos) {
            handleAccountUpdate(data);
        }

    } catch (const std::exception& e) {
        onError("Private WS message parse error: " + std::string(e.what()));
    }
}

void HTXExchange::handleDepthUpdate(const nlohmann::json& json, const std::string& channel) {
    try {
        // Extract symbol from channel: market.{symbol}.depth.step0
        size_t start = channel.find('.') + 1;
        size_t end = channel.find('.', start);
        std::string symbol = channel.substr(start, end - start);

        auto tick = json["tick"];

        OrderBook ob;
        ob.symbol = symbol;
        ob.exchange = "htx";
        ob.timestamp = json.value("ts", uint64_t(0));
        ob.local_timestamp = currentTimeNs();
        ob.sequence = tick.value("version", uint64_t(0));

        // Parse bids
        auto bids = tick.value("bids", nlohmann::json::array());
        for (const auto& b : bids) {
            PriceLevel level;
            level.price = b[0].get<double>();
            level.quantity = b[1].get<double>();
            ob.bids.push_back(level);
        }

        // Parse asks
        auto asks = tick.value("asks", nlohmann::json::array());
        for (const auto& a : asks) {
            PriceLevel level;
            level.price = a[0].get<double>();
            level.quantity = a[1].get<double>();
            ob.asks.push_back(level);
        }

        // Store sequence for delta updates
        {
            std::lock_guard<std::mutex> lock(orderbook_seq_mutex_);
            orderbook_seq_[symbol] = ob.sequence;
        }

        onOrderBook(ob);

    } catch (const std::exception& e) {
        onError("Depth update error: " + std::string(e.what()));
    }
}

void HTXExchange::handleTradeUpdate(const nlohmann::json& json) {
    try {
        std::string channel = json["ch"].get<std::string>();

        // Extract symbol from channel: market.{symbol}.trade.detail
        size_t start = channel.find('.') + 1;
        size_t end = channel.find('.', start);
        std::string symbol = channel.substr(start, end - start);

        auto tick = json["tick"];
        auto data = tick.value("data", nlohmann::json::array());

        for (const auto& t : data) {
            Trade trade;
            trade.exchange = "htx";
            trade.symbol = symbol;
            trade.trade_id = std::to_string(t.value("tradeId", uint64_t(0)));
            trade.price = t.value("price", 0.0);
            trade.quantity = t.value("amount", 0.0);
            trade.timestamp = t.value("ts", uint64_t(0));
            trade.local_timestamp = currentTimeNs();

            std::string direction = t.value("direction", "buy");
            trade.side = (direction == "buy") ? OrderSide::Buy : OrderSide::Sell;

            onTrade(trade);
        }

    } catch (const std::exception& e) {
        onError("Trade update error: " + std::string(e.what()));
    }
}

void HTXExchange::handleTickerUpdate(const nlohmann::json& json) {
    try {
        std::string channel = json["ch"].get<std::string>();

        // Extract symbol from channel: market.{symbol}.ticker
        size_t start = channel.find('.') + 1;
        size_t end = channel.find('.', start);
        std::string symbol = channel.substr(start, end - start);

        auto tick = json["tick"];

        Ticker ticker;
        ticker.exchange = "htx";
        ticker.symbol = symbol;
        ticker.last = tick.value("close", 0.0);
        ticker.bid = tick.value("bid", 0.0);
        ticker.ask = tick.value("ask", 0.0);
        ticker.bid_qty = tick.value("bidSize", 0.0);
        ticker.ask_qty = tick.value("askSize", 0.0);
        ticker.high_24h = tick.value("high", 0.0);
        ticker.low_24h = tick.value("low", 0.0);
        ticker.volume_24h = tick.value("amount", 0.0);
        ticker.volume_quote_24h = tick.value("vol", 0.0);
        ticker.timestamp = json.value("ts", uint64_t(0));

        // Calculate 24h change
        double open = tick.value("open", 0.0);
        if (open > 0) {
            ticker.change_24h = ticker.last - open;
            ticker.change_24h_pct = (ticker.change_24h / open) * 100.0;
        }

        onTicker(ticker);

    } catch (const std::exception& e) {
        onError("Ticker update error: " + std::string(e.what()));
    }
}

void HTXExchange::handleOrderUpdate(const nlohmann::json& data) {
    try {
        Order order;
        order.exchange = "htx";
        order.symbol = data.value("symbol", "");
        order.order_id = std::to_string(data.value("orderId", uint64_t(0)));
        order.client_order_id = data.value("clientOrderId", "");

        std::string type_str = data.value("type", "buy-limit");
        // HTX type format: buy-limit, sell-limit, buy-market, etc.
        if (type_str.find("buy") != std::string::npos) {
            order.side = OrderSide::Buy;
        } else {
            order.side = OrderSide::Sell;
        }

        if (type_str.find("market") != std::string::npos) {
            order.type = OrderType::Market;
        } else if (type_str.find("limit") != std::string::npos) {
            order.type = OrderType::Limit;
        } else if (type_str.find("ioc") != std::string::npos) {
            order.type = OrderType::ImmediateOrCancel;
        } else if (type_str.find("fok") != std::string::npos) {
            order.type = OrderType::FillOrKill;
        }

        order.status = parseOrderStatus(data.value("orderStatus", "submitted"));
        order.quantity = std::stod(data.value("orderSize", "0"));
        order.filled_quantity = std::stod(data.value("tradeVolume", "0"));
        order.remaining_quantity = order.quantity - order.filled_quantity;
        order.price = std::stod(data.value("orderPrice", "0"));
        order.average_price = std::stod(data.value("tradePrice", "0"));

        order.create_time = data.value("orderCreateTime", uint64_t(0));
        order.update_time = data.value("lastActTime", uint64_t(0));
        order.raw = data;

        onOrder(order);

    } catch (const std::exception& e) {
        onError("Order update error: " + std::string(e.what()));
    }
}

void HTXExchange::handleAccountUpdate(const nlohmann::json& data) {
    try {
        std::string currency = data.value("currency", "");
        std::string account_type = data.value("accountType", "");

        Balance balance;
        balance.asset = currency;
        balance.free = std::stod(data.value("available", "0"));
        balance.locked = std::stod(data.value("balance", "0")) - balance.free;

        onBalance(balance);

    } catch (const std::exception& e) {
        onError("Account update error: " + std::string(e.what()));
    }
}

// ============================================================================
// Subscription Management
// ============================================================================

void HTXExchange::sendSubscribe(const std::string& channel, bool is_private) {
    auto client = is_private ? private_ws_client_ : public_ws_client_;
    if (!client) return;

    if (is_private) {
        nlohmann::json msg;
        msg["action"] = "sub";
        msg["ch"] = channel;
        client->send(msg);
    } else {
        nlohmann::json msg;
        msg["sub"] = channel;
        msg["id"] = std::to_string(next_req_id_++);
        client->send(msg);
    }
}

void HTXExchange::sendUnsubscribe(const std::string& channel, bool is_private) {
    auto client = is_private ? private_ws_client_ : public_ws_client_;
    if (!client) return;

    if (is_private) {
        nlohmann::json msg;
        msg["action"] = "unsub";
        msg["ch"] = channel;
        client->send(msg);
    } else {
        nlohmann::json msg;
        msg["unsub"] = channel;
        msg["id"] = std::to_string(next_req_id_++);
        client->send(msg);
    }
}

void HTXExchange::sendPong(uint64_t ts, bool is_private) {
    auto client = is_private ? private_ws_client_ : public_ws_client_;
    if (!client) return;

    if (is_private) {
        nlohmann::json msg;
        msg["action"] = "pong";
        msg["data"]["ts"] = ts;
        client->send(msg);
    } else {
        nlohmann::json msg;
        msg["pong"] = ts;
        client->send(msg);
    }
}

void HTXExchange::authenticatePrivateWs() {
    if (!private_ws_client_) return;

    std::string timestamp = generateTimestamp();

    // Build auth params
    std::unordered_map<std::string, std::string> params;
    params["accessKey"] = htx_config_.api_key;
    params["signatureMethod"] = "HmacSHA256";
    params["signatureVersion"] = "2.1";
    params["timestamp"] = timestamp;

    // Sort and build query string
    std::vector<std::pair<std::string, std::string>> sorted_params(params.begin(), params.end());
    std::sort(sorted_params.begin(), sorted_params.end());

    std::stringstream ss;
    bool first = true;
    for (const auto& [key, value] : sorted_params) {
        if (!first) ss << "&";
        first = false;
        ss << key << "=" << urlEncode(value);
    }
    std::string query_string = ss.str();

    // Extract host from private WS URL
    std::string url = getPrivateWsUrl();
    std::string host;
    size_t start = url.find("://");
    if (start != std::string::npos) {
        start += 3;
        size_t end = url.find('/', start);
        host = url.substr(start, end - start);
    }

    // Generate signature
    std::string signature = sign("GET", host, "/ws/v2", query_string);

    // Build auth message
    nlohmann::json auth_msg;
    auth_msg["action"] = "req";
    auth_msg["ch"] = "auth";
    auth_msg["params"]["authType"] = "api";
    auth_msg["params"]["accessKey"] = htx_config_.api_key;
    auth_msg["params"]["signatureMethod"] = "HmacSHA256";
    auth_msg["params"]["signatureVersion"] = "2.1";
    auth_msg["params"]["timestamp"] = timestamp;
    auth_msg["params"]["signature"] = signature;

    private_ws_client_->send(auth_msg);
}

bool HTXExchange::subscribeOrderBook(const std::string& symbol, int depth) {
    // HTX channel format: market.{symbol}.depth.step0
    // step0 is best precision, step1-step5 are aggregated
    std::string channel = "market." + symbol + ".depth.step0";
    sendSubscribe(channel, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.insert(channel);
    return true;
}

bool HTXExchange::unsubscribeOrderBook(const std::string& symbol) {
    std::vector<std::string> to_unsub;

    {
        std::lock_guard<std::mutex> lock(subscription_mutex_);
        for (const auto& sub : public_subscriptions_) {
            if (sub.find("market." + symbol + ".depth") != std::string::npos) {
                to_unsub.push_back(sub);
            }
        }
        for (const auto& sub : to_unsub) {
            public_subscriptions_.erase(sub);
        }
    }

    for (const auto& sub : to_unsub) {
        sendUnsubscribe(sub, false);
    }

    clearOrderBookCache(symbol);
    return true;
}

bool HTXExchange::subscribeTrades(const std::string& symbol) {
    std::string channel = "market." + symbol + ".trade.detail";
    sendSubscribe(channel, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.insert(channel);
    return true;
}

bool HTXExchange::unsubscribeTrades(const std::string& symbol) {
    std::string channel = "market." + symbol + ".trade.detail";
    sendUnsubscribe(channel, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.erase(channel);
    return true;
}

bool HTXExchange::subscribeTicker(const std::string& symbol) {
    std::string channel = "market." + symbol + ".ticker";
    sendSubscribe(channel, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.insert(channel);
    return true;
}

bool HTXExchange::unsubscribeTicker(const std::string& symbol) {
    std::string channel = "market." + symbol + ".ticker";
    sendUnsubscribe(channel, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.erase(channel);
    return true;
}

bool HTXExchange::subscribeUserData() {
    // Initialize private WebSocket if needed
    // Note: HTX sends GZIP compressed binary messages
    if (!private_ws_client_) {
        network::WebSocketConfig ws_config;
        ws_config.url = getPrivateWsUrl();
        ws_config.name = "htx_private_ws";
        ws_config.ping_interval_ms = 0;  // HTX uses custom ping/pong
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

    // Subscribe to user data channels after authentication
    // orders#{symbol} or orders#* for all
    sendSubscribe("orders#*", true);
    sendSubscribe("accounts.update#2", true);  // #2 for balance change including available

    return true;
}

bool HTXExchange::unsubscribeUserData() {
    if (private_ws_client_) {
        private_ws_client_->disconnect();
        private_ws_client_.reset();
    }

    private_ws_connected_ = false;
    private_ws_authenticated_ = false;
    return true;
}

// ============================================================================
// REST Helpers
// ============================================================================

network::HttpResponse HTXExchange::signedGet(
    const std::string& endpoint,
    std::unordered_map<std::string, std::string> params) {

    std::string query = buildSignedQuery("GET", endpoint, params);

    network::HttpRequest request;
    request.method = network::HttpMethod::GET;
    request.path = endpoint + "?" + query;

    return rest_client_->execute(request);
}

network::HttpResponse HTXExchange::signedPost(
    const std::string& endpoint,
    const nlohmann::json& body) {

    std::unordered_map<std::string, std::string> params;
    std::string query = buildSignedQuery("POST", endpoint, params);

    std::string body_str = body.empty() ? "" : body.dump();

    network::HttpRequest request;
    request.method = network::HttpMethod::POST;
    request.path = endpoint + "?" + query;
    request.body = body_str;
    request.headers["Content-Type"] = "application/json";

    return rest_client_->execute(request);
}

network::HttpResponse HTXExchange::publicGet(
    const std::string& endpoint,
    const std::unordered_map<std::string, std::string>& params) {

    return rest_client_->get(endpoint, params);
}

// ============================================================================
// Account ID Management
// ============================================================================

std::string HTXExchange::getAccountId() {
    {
        std::lock_guard<std::mutex> lock(account_id_mutex_);
        if (!account_id_.empty()) {
            return account_id_;
        }
    }

    // Fetch account ID from API
    auto response = signedGet("/v1/account/accounts");

    if (!response.success) {
        onError("Failed to get account ID: " + response.error);
        return "";
    }

    try {
        auto json = response.json();
        if (json.value("status", "") != "ok") {
            onError("Get account ID failed: " + json.value("err-msg", "unknown"));
            return "";
        }

        auto data = json["data"];
        for (const auto& acc : data) {
            std::string type = acc.value("type", "");
            // For spot, use "spot" account; for futures, use "linear-swap"
            if ((htx_config_.spot && type == "spot") ||
                (!htx_config_.spot && type == "linear-swap")) {
                std::lock_guard<std::mutex> lock(account_id_mutex_);
                account_id_ = std::to_string(acc.value("id", uint64_t(0)));
                return account_id_;
            }
        }

        // If no matching type, use first account
        if (!data.empty()) {
            std::lock_guard<std::mutex> lock(account_id_mutex_);
            account_id_ = std::to_string(data[0].value("id", uint64_t(0)));
            return account_id_;
        }

    } catch (const std::exception& e) {
        onError("Parse account ID failed: " + std::string(e.what()));
    }

    return "";
}

// ============================================================================
// Order Management
// ============================================================================

std::optional<Order> HTXExchange::placeOrder(const OrderRequest& request) {
    std::string account_id = getAccountId();
    if (account_id.empty()) {
        onError("Failed to get account ID for order placement");
        return std::nullopt;
    }

    nlohmann::json body;
    body["account-id"] = account_id;
    body["symbol"] = request.symbol;
    body["amount"] = std::to_string(request.quantity);

    // Build order type string: buy-limit, sell-market, buy-ioc, etc.
    std::string order_type;
    order_type = (request.side == OrderSide::Buy) ? "buy-" : "sell-";

    switch (request.type) {
        case OrderType::Market:
            order_type += "market";
            break;
        case OrderType::Limit:
            order_type += "limit";
            body["price"] = std::to_string(request.price);
            break;
        case OrderType::ImmediateOrCancel:
            order_type += "ioc";
            body["price"] = std::to_string(request.price);
            break;
        case OrderType::FillOrKill:
            order_type += "fok";
            body["price"] = std::to_string(request.price);
            break;
        case OrderType::PostOnly:
            order_type += "limit-maker";
            body["price"] = std::to_string(request.price);
            break;
        case OrderType::StopLimit:
            order_type += "stop-limit";
            body["price"] = std::to_string(request.price);
            body["stop-price"] = std::to_string(request.stop_price);
            break;
        default:
            order_type += "limit";
            body["price"] = std::to_string(request.price);
    }

    body["type"] = order_type;

    if (!request.client_order_id.empty()) {
        body["client-order-id"] = request.client_order_id;
    }

    std::string endpoint = htx_config_.spot ? "/v1/order/orders/place" : "/linear-swap-api/v1/swap_order";
    auto response = signedPost(endpoint, body);

    if (!response.success) {
        onError("Place order failed: " + response.error + " " + response.body);
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("status", "") != "ok") {
            onError("Place order failed: " + json.value("err-msg", "unknown"));
            return std::nullopt;
        }

        Order order;
        order.exchange = "htx";
        order.order_id = json.value("data", "");
        order.client_order_id = request.client_order_id;
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

std::optional<Order> HTXExchange::cancelOrder(const std::string& symbol,
                                              const std::string& order_id) {
    std::string endpoint = htx_config_.spot
        ? "/v1/order/orders/" + order_id + "/submitcancel"
        : "/linear-swap-api/v1/swap_cancel";

    nlohmann::json body;
    if (!htx_config_.spot) {
        body["order_id"] = order_id;
        body["contract_code"] = symbol;
    }

    auto response = signedPost(endpoint, body);

    if (!response.success) {
        onError("Cancel order failed: " + response.error);
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("status", "") != "ok") {
            onError("Cancel order failed: " + json.value("err-msg", "unknown"));
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

std::optional<Order> HTXExchange::cancelOrder(const std::string& symbol,
                                              const std::string& order_id,
                                              const std::string& client_order_id) {
    std::string endpoint;
    nlohmann::json body;

    if (!client_order_id.empty()) {
        endpoint = htx_config_.spot
            ? "/v1/order/orders/submitCancelClientOrder"
            : "/linear-swap-api/v1/swap_cancel";
        body["client-order-id"] = client_order_id;
        if (!htx_config_.spot) {
            body["contract_code"] = symbol;
        }
    } else {
        return cancelOrder(symbol, order_id);
    }

    auto response = signedPost(endpoint, body);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("status", "") != "ok") {
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

bool HTXExchange::cancelAllOrders(const std::string& symbol) {
    std::string account_id = getAccountId();
    if (account_id.empty()) {
        return false;
    }

    nlohmann::json body;
    body["account-id"] = account_id;
    if (!symbol.empty()) {
        body["symbol"] = symbol;
    }

    std::string endpoint = htx_config_.spot
        ? "/v1/order/orders/batchCancelOpenOrders"
        : "/linear-swap-api/v1/swap_cancelall";

    auto response = signedPost(endpoint, body);
    return response.success && response.json().value("status", "") == "ok";
}

std::optional<Order> HTXExchange::getOrder(const std::string& symbol,
                                           const std::string& order_id) {
    std::string endpoint = htx_config_.spot
        ? "/v1/order/orders/" + order_id
        : "/linear-swap-api/v1/swap_order_info";

    std::unordered_map<std::string, std::string> params;
    if (!htx_config_.spot) {
        params["order_id"] = order_id;
        params["contract_code"] = symbol;
    }

    auto response = htx_config_.spot
        ? signedGet(endpoint)
        : signedGet(endpoint, params);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("status", "") != "ok") {
            return std::nullopt;
        }

        return parseOrder(json["data"]);

    } catch (...) {
        return std::nullopt;
    }
}

std::vector<Order> HTXExchange::getOpenOrders(const std::string& symbol) {
    std::string account_id = getAccountId();
    if (account_id.empty()) {
        return {};
    }

    std::unordered_map<std::string, std::string> params;
    params["account-id"] = account_id;
    if (!symbol.empty()) {
        params["symbol"] = symbol;
    }

    std::string endpoint = htx_config_.spot
        ? "/v1/order/openOrders"
        : "/linear-swap-api/v1/swap_openorders";

    auto response = signedGet(endpoint, params);

    std::vector<Order> orders;
    if (!response.success) {
        return orders;
    }

    try {
        auto json = response.json();
        if (json.value("status", "") != "ok") {
            return orders;
        }

        auto data = json["data"];
        for (const auto& item : data) {
            orders.push_back(parseOrder(item));
        }

    } catch (...) {
        // Return empty
    }

    return orders;
}

std::vector<Order> HTXExchange::getOrderHistory(const std::string& symbol,
                                                uint64_t start_time,
                                                uint64_t end_time,
                                                int limit) {
    std::unordered_map<std::string, std::string> params;
    params["symbol"] = symbol;
    params["states"] = "filled,partial-canceled,canceled";
    params["size"] = std::to_string(limit);

    if (start_time > 0) {
        params["start-time"] = std::to_string(start_time);
    }
    if (end_time > 0) {
        params["end-time"] = std::to_string(end_time);
    }

    std::string endpoint = htx_config_.spot
        ? "/v1/order/orders"
        : "/linear-swap-api/v1/swap_hisorders";

    auto response = signedGet(endpoint, params);

    std::vector<Order> orders;
    if (!response.success) {
        return orders;
    }

    try {
        auto json = response.json();
        if (json.value("status", "") != "ok") {
            return orders;
        }

        auto data = json["data"];
        for (const auto& item : data) {
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

std::optional<Account> HTXExchange::getAccount() {
    std::string account_id = getAccountId();
    if (account_id.empty()) {
        return std::nullopt;
    }

    std::string endpoint = htx_config_.spot
        ? "/v1/account/accounts/" + account_id + "/balance"
        : "/linear-swap-api/v1/swap_account_info";

    auto response = signedGet(endpoint);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("status", "") != "ok") {
            return std::nullopt;
        }

        Account account;
        account.exchange = "htx";
        account.update_time = currentTimeMs();

        if (htx_config_.spot) {
            auto data = json["data"];
            auto list = data.value("list", nlohmann::json::array());

            for (const auto& item : list) {
                std::string currency = item.value("currency", "");
                std::string type = item.value("type", "");
                double balance = std::stod(item.value("balance", "0"));

                auto it = account.balances.find(currency);
                if (it == account.balances.end()) {
                    Balance b;
                    b.asset = currency;
                    account.balances[currency] = b;
                    it = account.balances.find(currency);
                }

                if (type == "trade") {
                    it->second.free = balance;
                } else if (type == "frozen") {
                    it->second.locked = balance;
                }
            }
        } else {
            auto data = json["data"];
            for (const auto& item : data) {
                Balance balance;
                balance.asset = item.value("symbol", "");
                balance.free = std::stod(item.value("withdraw_available", "0"));
                balance.locked = std::stod(item.value("margin_frozen", "0"));
                account.balances[balance.asset] = balance;

                account.total_margin += std::stod(item.value("margin_balance", "0"));
                account.available_margin += std::stod(item.value("margin_available", "0"));
                account.total_unrealized_pnl += std::stod(item.value("profit_unreal", "0"));
            }

            // Get positions for futures
            account.positions = getPositions();
        }

        return account;

    } catch (const std::exception& e) {
        onError("Parse account failed: " + std::string(e.what()));
        return std::nullopt;
    }
}

std::optional<Balance> HTXExchange::getBalance(const std::string& asset) {
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

std::vector<Position> HTXExchange::getPositions(const std::string& symbol) {
    if (htx_config_.spot) {
        return {};  // No positions for spot
    }

    std::unordered_map<std::string, std::string> params;
    if (!symbol.empty()) {
        params["contract_code"] = symbol;
    }

    auto response = signedGet("/linear-swap-api/v1/swap_position_info", params);

    std::vector<Position> positions;
    if (!response.success) {
        return positions;
    }

    try {
        auto json = response.json();
        if (json.value("status", "") != "ok") {
            return positions;
        }

        auto data = json["data"];
        for (const auto& p : data) {
            double volume = std::stod(p.value("volume", "0"));
            if (volume == 0.0) continue;

            Position position;
            position.exchange = "htx";
            position.symbol = p.value("contract_code", "");

            std::string direction = p.value("direction", "buy");
            position.side = (direction == "buy") ? OrderSide::Buy : OrderSide::Sell;
            position.quantity = volume;
            position.entry_price = std::stod(p.value("cost_hold", "0"));
            position.mark_price = std::stod(p.value("last_price", "0"));
            position.liquidation_price = std::stod(p.value("liquidation_price", "0"));
            position.unrealized_pnl = std::stod(p.value("profit_unreal", "0"));
            position.realized_pnl = std::stod(p.value("profit", "0"));
            position.leverage = std::stod(p.value("lever_rate", "1"));
            position.margin = std::stod(p.value("position_margin", "0"));

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

std::vector<SymbolInfo> HTXExchange::getSymbols() {
    std::string endpoint = htx_config_.spot
        ? "/v1/common/symbols"
        : "/linear-swap-api/v1/swap_contract_info";

    auto response = publicGet(endpoint);

    std::vector<SymbolInfo> symbols;
    if (!response.success) {
        return symbols;
    }

    try {
        auto json = response.json();
        if (json.value("status", "") != "ok") {
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

std::optional<SymbolInfo> HTXExchange::getSymbolInfo(const std::string& symbol) {
    // Check cache first
    {
        std::lock_guard<std::mutex> lock(symbol_info_mutex_);
        auto it = symbol_info_cache_.find(symbol);
        if (it != symbol_info_cache_.end()) {
            return it->second;
        }
    }

    // Fetch all symbols and find the one we want
    auto symbols = getSymbols();
    for (const auto& s : symbols) {
        if (s.symbol == symbol) {
            return s;
        }
    }

    return std::nullopt;
}

std::optional<OrderBook> HTXExchange::getOrderBookSnapshot(const std::string& symbol, int depth) {
    std::unordered_map<std::string, std::string> params;
    params["symbol"] = symbol;
    params["type"] = "step0";  // Best precision
    params["depth"] = std::to_string(depth);

    std::string endpoint = htx_config_.spot
        ? "/market/depth"
        : "/linear-swap-ex/market/depth";

    auto response = publicGet(endpoint, params);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("status", "") != "ok") {
            return std::nullopt;
        }

        auto tick = json["tick"];

        OrderBook ob;
        ob.symbol = symbol;
        ob.exchange = "htx";
        ob.timestamp = json.value("ts", uint64_t(0));
        ob.local_timestamp = currentTimeNs();
        ob.sequence = tick.value("version", uint64_t(0));

        auto bids = tick.value("bids", nlohmann::json::array());
        for (const auto& b : bids) {
            PriceLevel level;
            level.price = b[0].get<double>();
            level.quantity = b[1].get<double>();
            ob.bids.push_back(level);
        }

        auto asks = tick.value("asks", nlohmann::json::array());
        for (const auto& a : asks) {
            PriceLevel level;
            level.price = a[0].get<double>();
            level.quantity = a[1].get<double>();
            ob.asks.push_back(level);
        }

        return ob;

    } catch (...) {
        return std::nullopt;
    }
}

std::vector<Trade> HTXExchange::getRecentTrades(const std::string& symbol, int limit) {
    std::unordered_map<std::string, std::string> params;
    params["symbol"] = symbol;
    params["size"] = std::to_string(limit);

    std::string endpoint = htx_config_.spot
        ? "/market/history/trade"
        : "/linear-swap-ex/market/history/trade";

    auto response = publicGet(endpoint, params);

    std::vector<Trade> trades;
    if (!response.success) {
        return trades;
    }

    try {
        auto json = response.json();
        if (json.value("status", "") != "ok") {
            return trades;
        }

        auto data = json["data"];
        for (const auto& tick : data) {
            auto tick_data = tick.value("data", nlohmann::json::array());
            for (const auto& t : tick_data) {
                Trade trade;
                trade.exchange = "htx";
                trade.symbol = symbol;
                trade.trade_id = std::to_string(t.value("tradeId", uint64_t(0)));
                trade.price = t.value("price", 0.0);
                trade.quantity = t.value("amount", 0.0);
                trade.timestamp = t.value("ts", uint64_t(0));
                trade.side = t.value("direction", "buy") == "buy" ? OrderSide::Buy : OrderSide::Sell;
                trades.push_back(trade);
            }
        }

    } catch (...) {
        // Return empty
    }

    return trades;
}

std::optional<Ticker> HTXExchange::getTicker(const std::string& symbol) {
    std::unordered_map<std::string, std::string> params;
    params["symbol"] = symbol;

    std::string endpoint = htx_config_.spot
        ? "/market/detail/merged"
        : "/linear-swap-ex/market/detail/merged";

    auto response = publicGet(endpoint, params);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("status", "") != "ok") {
            return std::nullopt;
        }

        auto tick = json["tick"];

        Ticker ticker;
        ticker.exchange = "htx";
        ticker.symbol = symbol;
        ticker.last = tick.value("close", 0.0);
        ticker.high_24h = tick.value("high", 0.0);
        ticker.low_24h = tick.value("low", 0.0);
        ticker.volume_24h = tick.value("amount", 0.0);
        ticker.volume_quote_24h = tick.value("vol", 0.0);
        ticker.timestamp = json.value("ts", uint64_t(0));

        // Bid/ask from orderbook
        auto bid = tick.value("bid", nlohmann::json::array());
        auto ask = tick.value("ask", nlohmann::json::array());
        if (bid.size() >= 2) {
            ticker.bid = bid[0].get<double>();
            ticker.bid_qty = bid[1].get<double>();
        }
        if (ask.size() >= 2) {
            ticker.ask = ask[0].get<double>();
            ticker.ask_qty = ask[1].get<double>();
        }

        double open = tick.value("open", 0.0);
        if (open > 0) {
            ticker.change_24h = ticker.last - open;
            ticker.change_24h_pct = (ticker.change_24h / open) * 100.0;
        }

        return ticker;

    } catch (...) {
        return std::nullopt;
    }
}

uint64_t HTXExchange::getServerTime() {
    auto response = publicGet("/v1/common/timestamp");

    if (response.success) {
        auto json = response.json();
        if (json.value("status", "") == "ok") {
            return json.value("data", uint64_t(0));
        }
    }

    return 0;
}

std::string HTXExchange::formatSymbol(const std::string& base, const std::string& quote) const {
    // HTX format: btcusdt (lowercase, no separator)
    std::string symbol = base + quote;
    std::transform(symbol.begin(), symbol.end(), symbol.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return symbol;
}

std::pair<std::string, std::string> HTXExchange::parseSymbol(const std::string& symbol) const {
    // Simplified: assume 3-4 char base + quote
    static const std::vector<std::string> quote_assets = {"usdt", "btc", "eth", "husd"};

    std::string lower = symbol;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& quote : quote_assets) {
        if (lower.length() > quote.length() &&
            lower.substr(lower.length() - quote.length()) == quote) {
            std::string base = lower.substr(0, lower.length() - quote.length());
            std::transform(base.begin(), base.end(), base.begin(), ::toupper);
            std::string q = quote;
            std::transform(q.begin(), q.end(), q.begin(), ::toupper);
            return {base, q};
        }
    }
    return {symbol, ""};
}

// ============================================================================
// Data Converters
// ============================================================================

Order HTXExchange::parseOrder(const nlohmann::json& data) {
    Order order;
    order.exchange = "htx";
    order.symbol = data.value("symbol", "");
    order.order_id = std::to_string(data.value("id", uint64_t(0)));
    order.client_order_id = data.value("client-order-id", "");

    std::string type_str = data.value("type", "buy-limit");
    // HTX type format: buy-limit, sell-limit, buy-market, etc.
    if (type_str.find("buy") != std::string::npos) {
        order.side = OrderSide::Buy;
    } else {
        order.side = OrderSide::Sell;
    }

    if (type_str.find("market") != std::string::npos) {
        order.type = OrderType::Market;
    } else if (type_str.find("ioc") != std::string::npos) {
        order.type = OrderType::ImmediateOrCancel;
        order.time_in_force = TimeInForce::IOC;
    } else if (type_str.find("fok") != std::string::npos) {
        order.type = OrderType::FillOrKill;
        order.time_in_force = TimeInForce::FOK;
    } else if (type_str.find("limit-maker") != std::string::npos) {
        order.type = OrderType::PostOnly;
        order.time_in_force = TimeInForce::PostOnly;
    } else {
        order.type = OrderType::Limit;
        order.time_in_force = TimeInForce::GTC;
    }

    order.status = parseOrderStatus(data.value("state", "submitted"));
    order.quantity = std::stod(data.value("amount", "0"));
    order.filled_quantity = std::stod(data.value("field-amount", "0"));
    order.remaining_quantity = order.quantity - order.filled_quantity;
    order.price = std::stod(data.value("price", "0"));

    // Calculate average price from filled cash amount
    double filled_cash = std::stod(data.value("field-cash-amount", "0"));
    if (order.filled_quantity > 0 && filled_cash > 0) {
        order.average_price = filled_cash / order.filled_quantity;
    }

    order.commission = std::stod(data.value("field-fees", "0"));
    order.create_time = data.value("created-at", uint64_t(0));
    order.update_time = data.value("finished-at", uint64_t(0));
    order.raw = data;

    return order;
}

OrderStatus HTXExchange::parseOrderStatus(const std::string& status) {
    if (status == "submitted" || status == "pre-submitted") return OrderStatus::New;
    if (status == "partial-filled") return OrderStatus::PartiallyFilled;
    if (status == "filled") return OrderStatus::Filled;
    if (status == "canceled" || status == "partial-canceled") return OrderStatus::Cancelled;
    if (status == "rejected") return OrderStatus::Rejected;
    if (status == "canceling") return OrderStatus::PendingCancel;
    return OrderStatus::Failed;
}

OrderSide HTXExchange::parseOrderSide(const std::string& side) {
    return (side == "buy" || side.find("buy") != std::string::npos) ? OrderSide::Buy : OrderSide::Sell;
}

OrderType HTXExchange::parseOrderType(const std::string& type) {
    if (type.find("market") != std::string::npos) return OrderType::Market;
    if (type.find("ioc") != std::string::npos) return OrderType::ImmediateOrCancel;
    if (type.find("fok") != std::string::npos) return OrderType::FillOrKill;
    if (type.find("limit-maker") != std::string::npos) return OrderType::PostOnly;
    return OrderType::Limit;
}

TimeInForce HTXExchange::parseTimeInForce(const std::string& tif) {
    if (tif == "ioc") return TimeInForce::IOC;
    if (tif == "fok") return TimeInForce::FOK;
    if (tif == "gtc") return TimeInForce::GTC;
    return TimeInForce::GTC;
}

std::string HTXExchange::orderSideToString(OrderSide side) {
    return side == OrderSide::Buy ? "buy" : "sell";
}

std::string HTXExchange::orderTypeToString(OrderType type) {
    switch (type) {
        case OrderType::Market: return "market";
        case OrderType::Limit: return "limit";
        case OrderType::ImmediateOrCancel: return "ioc";
        case OrderType::FillOrKill: return "fok";
        case OrderType::PostOnly: return "limit-maker";
        default: return "limit";
    }
}

std::string HTXExchange::timeInForceToString(TimeInForce tif) {
    switch (tif) {
        case TimeInForce::GTC: return "gtc";
        case TimeInForce::IOC: return "ioc";
        case TimeInForce::FOK: return "fok";
        default: return "gtc";
    }
}

SymbolInfo HTXExchange::parseSymbolInfo(const nlohmann::json& data) {
    SymbolInfo info;

    if (htx_config_.spot) {
        info.symbol = data.value("symbol", "");
        info.base_asset = data.value("base-currency", "");
        info.quote_asset = data.value("quote-currency", "");
        info.trading_enabled = data.value("state", "") == "online";
        info.min_qty = std::stod(data.value("min-order-amt", "0"));
        info.max_qty = std::stod(data.value("max-order-amt", "0"));
        info.min_notional = std::stod(data.value("min-order-value", "0"));
        info.price_precision = data.value("price-precision", 8);
        info.qty_precision = data.value("amount-precision", 8);

        // Calculate tick and step size from precision
        info.tick_size = 1.0 / std::pow(10, info.price_precision);
        info.step_size = 1.0 / std::pow(10, info.qty_precision);
    } else {
        info.symbol = data.value("contract_code", "");
        info.base_asset = data.value("symbol", "");
        info.quote_asset = "USDT";
        info.trading_enabled = data.value("contract_status", 0) == 1;
        info.tick_size = std::stod(data.value("price_tick", "0.01"));
        info.step_size = std::stod(data.value("contract_size", "1"));

        // Calculate precision from tick/step size
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
    }

    return info;
}

// ============================================================================
// Orderbook Management
// ============================================================================

void HTXExchange::initializeOrderBook(const std::string& symbol, int depth) {
    auto snapshot = getOrderBookSnapshot(symbol, depth);
    if (!snapshot) {
        onError("Failed to get orderbook snapshot for " + symbol);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(orderbook_seq_mutex_);
        orderbook_seq_[symbol] = snapshot->sequence;
    }

    onOrderBook(*snapshot);
}

void HTXExchange::applyOrderBookSnapshot(const std::string& symbol, const nlohmann::json& data) {
    OrderBook ob;
    ob.symbol = symbol;
    ob.exchange = "htx";
    ob.timestamp = currentTimeMs();
    ob.local_timestamp = currentTimeNs();

    auto bids = data.value("bids", nlohmann::json::array());
    for (const auto& b : bids) {
        PriceLevel level;
        level.price = b[0].get<double>();
        level.quantity = b[1].get<double>();
        ob.bids.push_back(level);
    }

    auto asks = data.value("asks", nlohmann::json::array());
    for (const auto& a : asks) {
        PriceLevel level;
        level.price = a[0].get<double>();
        level.quantity = a[1].get<double>();
        ob.asks.push_back(level);
    }

    onOrderBook(ob);
}

// ============================================================================
// HTX-specific Methods
// ============================================================================

bool HTXExchange::setLeverage(const std::string& symbol, int leverage) {
    if (htx_config_.spot) {
        return false;  // Not applicable for spot
    }

    nlohmann::json body;
    body["contract_code"] = symbol;
    body["lever_rate"] = leverage;

    auto response = signedPost("/linear-swap-api/v1/swap_switch_lever_rate", body);
    return response.success && response.json().value("status", "") == "ok";
}

bool HTXExchange::setMarginType(const std::string& symbol, bool isolated) {
    if (htx_config_.spot) {
        return false;  // Not applicable for spot
    }

    nlohmann::json body;
    body["contract_code"] = symbol;
    body["margin_mode"] = isolated ? "isolated" : "cross";

    auto response = signedPost("/linear-swap-api/v1/swap_cross_switch_position_mode", body);
    return response.success && response.json().value("status", "") == "ok";
}

double HTXExchange::getFundingRate(const std::string& symbol) {
    if (htx_config_.spot) {
        return 0.0;  // Not applicable for spot
    }

    std::unordered_map<std::string, std::string> params;
    params["contract_code"] = symbol;

    auto response = publicGet("/linear-swap-api/v1/swap_funding_rate", params);

    if (response.success) {
        auto json = response.json();
        if (json.value("status", "") == "ok") {
            auto data = json["data"];
            return std::stod(data.value("funding_rate", "0"));
        }
    }

    return 0.0;
}

// ============================================================================
// Factory Functions
// ============================================================================

std::shared_ptr<HTXExchange> createHTXExchange(const ExchangeConfig& config) {
    return std::make_shared<HTXExchange>(config);
}

std::shared_ptr<HTXExchange> createHTXExchange(const HTXConfig& config) {
    return std::make_shared<HTXExchange>(config);
}

} // namespace exchange
} // namespace hft
