#include "exchange/bitfinex.hpp"

#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/evp.h>

#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <cmath>

namespace hft {
namespace exchange {

// ============================================================================
// Constructors/Destructor
// ============================================================================

BitfinexExchange::BitfinexExchange(const ExchangeConfig& config)
    : ExchangeBase(config) {
    bitfinex_config_.api_key = config.api_key;
    bitfinex_config_.api_secret = config.api_secret;
    bitfinex_config_.testnet = config.testnet;
    bitfinex_config_.order_rate_limit = config.orders_per_second;
    bitfinex_config_.request_rate_limit = config.requests_per_minute;

    // Set wallet type based on exchange type
    if (config.type == ExchangeType::Margin) {
        bitfinex_config_.default_wallet = BitfinexWalletType::Margin;
    } else {
        bitfinex_config_.default_wallet = BitfinexWalletType::Exchange;
    }

    // Initialize REST client
    network::RestClientConfig rest_config;
    rest_config.base_url = getRestUrl();
    rest_config.name = "bitfinex_rest";
    rest_config.request_timeout_ms = config.rest_timeout_ms;
    rest_config.max_retries = config.rest_max_retries;
    rest_config.rate_limit.requests_per_second = static_cast<double>(bitfinex_config_.request_rate_limit) / 60.0;
    rest_config.rate_limit.requests_per_minute = bitfinex_config_.request_rate_limit;

    rest_client_ = std::make_shared<network::RestClient>(rest_config);

    // Initialize nonce
    nonce_ = static_cast<uint64_t>(currentTimeMs()) * 1000;
}

BitfinexExchange::BitfinexExchange(const BitfinexConfig& bitfinex_config)
    : ExchangeBase(ExchangeConfig{
        .name = "Bitfinex",
        .api_key = bitfinex_config.api_key,
        .api_secret = bitfinex_config.api_secret,
        .type = bitfinex_config.default_wallet == BitfinexWalletType::Margin
                    ? ExchangeType::Margin : ExchangeType::Spot,
        .testnet = bitfinex_config.testnet
      }),
      bitfinex_config_(bitfinex_config) {

    // Initialize REST client
    network::RestClientConfig rest_config;
    rest_config.base_url = getRestUrl();
    rest_config.name = "bitfinex_rest";
    rest_config.rate_limit.requests_per_second = static_cast<double>(bitfinex_config_.request_rate_limit) / 60.0;
    rest_config.rate_limit.requests_per_minute = bitfinex_config_.request_rate_limit;

    rest_client_ = std::make_shared<network::RestClient>(rest_config);

    // Initialize nonce
    nonce_ = static_cast<uint64_t>(currentTimeMs()) * 1000;
}

BitfinexExchange::~BitfinexExchange() {
    disconnect();
}

// ============================================================================
// URL Builders
// ============================================================================

std::string BitfinexExchange::getRestUrl() const {
    // Bitfinex doesn't have a testnet, but uses paper trading flag
    return "https://api.bitfinex.com";
}

std::string BitfinexExchange::getWsUrl() const {
    return "wss://api-pub.bitfinex.com/ws/2";
}

std::string BitfinexExchange::getWalletTypeString(BitfinexWalletType type) const {
    switch (type) {
        case BitfinexWalletType::Exchange: return "exchange";
        case BitfinexWalletType::Margin: return "margin";
        case BitfinexWalletType::Funding: return "funding";
        default: return "exchange";
    }
}

std::string BitfinexExchange::getPrecisionString(BitfinexBookPrecision prec) const {
    switch (prec) {
        case BitfinexBookPrecision::P0: return "P0";
        case BitfinexBookPrecision::P1: return "P1";
        case BitfinexBookPrecision::P2: return "P2";
        case BitfinexBookPrecision::P3: return "P3";
        case BitfinexBookPrecision::P4: return "P4";
        case BitfinexBookPrecision::R0: return "R0";
        default: return "P0";
    }
}

// ============================================================================
// Connection Management
// ============================================================================

bool BitfinexExchange::connect() {
    if (ws_connected_.load()) {
        return true;
    }

    onConnectionStatus(ConnectionStatus::Connecting);

    // Initialize WebSocket client
    network::WebSocketConfig ws_config;
    ws_config.url = getWsUrl();
    ws_config.name = "bitfinex_ws";
    ws_config.ping_interval_ms = 15000;  // Send ping every 15 seconds
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

void BitfinexExchange::disconnect() {
    stopHeartbeat();

    if (ws_client_) {
        ws_client_->disconnect();
        ws_client_.reset();
    }

    ws_connected_ = false;
    ws_authenticated_ = false;

    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        channel_subscriptions_.clear();
        symbol_to_channel_.clear();
    }

    onConnectionStatus(ConnectionStatus::Disconnected);
}

bool BitfinexExchange::isConnected() const {
    return ws_connected_.load();
}

ConnectionStatus BitfinexExchange::connectionStatus() const {
    return connection_status_.load();
}

// ============================================================================
// Signature Generation (HMAC-SHA384)
// ============================================================================

std::string BitfinexExchange::generateNonce() {
    return std::to_string(nonce_++);
}

std::string BitfinexExchange::sign(const std::string& payload) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;

    HMAC(EVP_sha384(),
         bitfinex_config_.api_secret.c_str(),
         static_cast<int>(bitfinex_config_.api_secret.length()),
         reinterpret_cast<const unsigned char*>(payload.c_str()),
         payload.length(),
         hash, &hash_len);

    std::stringstream ss;
    for (unsigned int i = 0; i < hash_len; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }

    return ss.str();
}

void BitfinexExchange::addAuthHeaders(network::HttpRequest& request, const std::string& endpoint,
                                      const std::string& body) {
    std::string nonce = generateNonce();
    std::string signature_payload = "/api" + endpoint + nonce + body;
    std::string signature = sign(signature_payload);

    request.headers["bfx-apikey"] = bitfinex_config_.api_key;
    request.headers["bfx-signature"] = signature;
    request.headers["bfx-nonce"] = nonce;
    request.headers["Content-Type"] = "application/json";
}

// ============================================================================
// WebSocket Handlers
// ============================================================================

void BitfinexExchange::handleWsOpen() {
    ws_connected_ = true;
    onConnectionStatus(ConnectionStatus::Connected);

    // Start heartbeat monitoring
    startHeartbeat();
}

void BitfinexExchange::handleWsClose(int code, const std::string& reason) {
    ws_connected_ = false;
    ws_authenticated_ = false;
    onConnectionStatus(ConnectionStatus::Reconnecting);
}

void BitfinexExchange::handleWsError(const std::string& error) {
    onError("WebSocket error: " + error);
}

void BitfinexExchange::handleWsMessage(const std::string& message, network::MessageType type) {
    try {
        auto json = nlohmann::json::parse(message);

        // Bitfinex uses different message formats:
        // 1. Object messages: {"event": "info", ...}
        // 2. Array messages: [CHANNEL_ID, DATA] or [CHANNEL_ID, "hb"] (heartbeat)

        if (json.is_object()) {
            // Event message
            std::string event = json.value("event", "");

            if (event == "info") {
                handleInfoMessage(json);
            } else if (event == "auth") {
                handleAuthMessage(json);
            } else if (event == "subscribed") {
                handleSubscribedMessage(json);
            } else if (event == "unsubscribed") {
                handleUnsubscribedMessage(json);
            } else if (event == "error") {
                handleErrorMessage(json);
            } else if (event == "conf") {
                // Configuration response, ignored
            } else if (event == "pong") {
                // Pong response
                std::lock_guard<std::mutex> lock(heartbeat_mutex_);
                last_heartbeat_ = std::chrono::steady_clock::now();
            }
        } else if (json.is_array() && json.size() >= 2) {
            // Channel data message
            int channel_id = json[0].get<int>();

            // Check for heartbeat
            if (json[1].is_string() && json[1].get<std::string>() == "hb") {
                handleHeartbeat(channel_id);
                return;
            }

            // Handle channel data
            handleChannelData(channel_id, json);
        }

    } catch (const std::exception& e) {
        onError("Message parse error: " + std::string(e.what()));
    }
}

void BitfinexExchange::handleInfoMessage(const nlohmann::json& msg) {
    int version = msg.value("version", 0);

    // Check for platform status
    if (msg.contains("platform")) {
        int status = msg["platform"].value("status", 0);
        if (status != 1) {
            onError("Bitfinex platform is in maintenance mode");
        }
    }

    // Server info received, can now authenticate and subscribe
    if (!bitfinex_config_.api_key.empty() && !ws_authenticated_.load()) {
        authenticateWebSocket();
    }
}

void BitfinexExchange::handleAuthMessage(const nlohmann::json& msg) {
    std::string status = msg.value("status", "");
    if (status == "OK") {
        ws_authenticated_ = true;
    } else {
        ws_authenticated_ = false;
        std::string err_msg = msg.value("msg", "Authentication failed");
        onError("WebSocket auth failed: " + err_msg);
    }
}

void BitfinexExchange::handleSubscribedMessage(const nlohmann::json& msg) {
    int channel_id = msg.value("chanId", 0);
    std::string channel = msg.value("channel", "");
    std::string symbol = msg.value("symbol", msg.value("pair", ""));

    ChannelSubscription sub;
    sub.channel_id = channel_id;
    sub.channel_type = channel;
    sub.symbol = symbol;
    sub.active = true;
    sub.precision = msg.value("prec", "P0");
    sub.depth = msg.value("len", 25);

    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        channel_subscriptions_[channel_id] = sub;
        symbol_to_channel_[channel + ":" + symbol] = channel_id;
    }
}

void BitfinexExchange::handleUnsubscribedMessage(const nlohmann::json& msg) {
    int channel_id = msg.value("chanId", 0);

    std::lock_guard<std::mutex> lock(channel_mutex_);
    auto it = channel_subscriptions_.find(channel_id);
    if (it != channel_subscriptions_.end()) {
        std::string key = it->second.channel_type + ":" + it->second.symbol;
        symbol_to_channel_.erase(key);
        channel_subscriptions_.erase(it);
    }
}

void BitfinexExchange::handleErrorMessage(const nlohmann::json& msg) {
    std::string err_msg = msg.value("msg", "Unknown error");
    int code = msg.value("code", 0);
    onError("Bitfinex error [" + std::to_string(code) + "]: " + err_msg);
}

void BitfinexExchange::handleHeartbeat(int channel_id) {
    std::lock_guard<std::mutex> lock(heartbeat_mutex_);
    last_heartbeat_ = std::chrono::steady_clock::now();
}

void BitfinexExchange::handleChannelData(int channel_id, const nlohmann::json& data) {
    // Channel 0 is always the authenticated channel for private data
    if (channel_id == 0) {
        if (data.size() >= 3 && data[1].is_string()) {
            std::string msg_type = data[1].get<std::string>();
            handlePrivateData(msg_type, data[2]);
        }
        return;
    }

    // Look up channel subscription
    ChannelSubscription sub;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        auto it = channel_subscriptions_.find(channel_id);
        if (it == channel_subscriptions_.end()) {
            return;  // Unknown channel
        }
        sub = it->second;
    }

    // Route to appropriate handler
    if (sub.channel_type == "book") {
        handleBookUpdate(sub, data[1]);
    } else if (sub.channel_type == "trades") {
        handleTradeUpdate(sub, data);
    } else if (sub.channel_type == "ticker") {
        handleTickerUpdate(sub, data[1]);
    }
}

// ============================================================================
// Book Update Handlers
// ============================================================================

void BitfinexExchange::handleBookUpdate(const ChannelSubscription& sub, const nlohmann::json& data) {
    if (!data.is_array()) return;

    // Check if it's a snapshot (array of arrays) or a single update (single array)
    if (data.empty()) return;

    // Check if using raw orderbook (R0 precision)
    bool is_raw = (sub.precision == "R0");

    if (data[0].is_array()) {
        // Snapshot: [[price, count, amount], ...] for P0-P4
        // Snapshot: [[order_id, price, amount], ...] for R0
        handleBookSnapshot(sub.symbol, data, is_raw);
    } else {
        // Single update: [price, count, amount] for P0-P4
        // Single update: [order_id, price, amount] for R0
        handleBookDelta(sub.symbol, data, is_raw);
    }
}

void BitfinexExchange::handleBookSnapshot(const std::string& symbol, const nlohmann::json& data,
                                          bool is_raw) {
    try {
        OrderBook ob;
        ob.symbol = symbol;
        ob.exchange = "bitfinex";
        ob.timestamp = currentTimeMs();
        ob.local_timestamp = currentTimeNs();

        for (const auto& level : data) {
            if (!level.is_array() || level.size() < 3) continue;

            double price, amount;
            uint32_t count;

            if (is_raw) {
                // Raw book format: [ORDER_ID, PRICE, AMOUNT]
                // ORDER_ID is index 0, PRICE is index 1, AMOUNT is index 2
                price = level[1].get<double>();
                amount = level[2].get<double>();
                count = 1;  // Each entry is a single order
            } else {
                // Aggregated book format: [PRICE, COUNT, AMOUNT]
                price = level[0].get<double>();
                count = static_cast<uint32_t>(level[1].get<int>());
                amount = level[2].get<double>();

                if (count == 0) continue;  // No orders at this level
            }

            PriceLevel pl;
            pl.price = price;
            pl.quantity = std::abs(amount);
            pl.order_count = count;

            // In Bitfinex: positive amount = bid, negative amount = ask
            if (amount > 0) {
                ob.bids.push_back(pl);
            } else {
                ob.asks.push_back(pl);
            }
        }

        // Sort bids descending, asks ascending
        std::sort(ob.bids.begin(), ob.bids.end(),
                  [](const PriceLevel& a, const PriceLevel& b) { return a.price > b.price; });
        std::sort(ob.asks.begin(), ob.asks.end(),
                  [](const PriceLevel& a, const PriceLevel& b) { return a.price < b.price; });

        {
            std::lock_guard<std::mutex> lock(orderbook_state_mutex_);
            orderbook_initialized_[symbol] = true;
        }

        onOrderBook(ob);

    } catch (const std::exception& e) {
        onError("Book snapshot error: " + std::string(e.what()));
    }
}

void BitfinexExchange::handleBookDelta(const std::string& symbol, const nlohmann::json& data,
                                       bool is_raw) {
    try {
        if (!data.is_array() || data.size() < 3) return;

        double price, amount;
        int count;

        if (is_raw) {
            // Raw book format: [ORDER_ID, PRICE, AMOUNT]
            // When PRICE = 0, delete the order
            price = data[1].get<double>();
            amount = data[2].get<double>();
            count = (price == 0.0) ? 0 : 1;  // PRICE=0 means delete
        } else {
            // Aggregated book format: [PRICE, COUNT, AMOUNT]
            price = data[0].get<double>();
            count = data[1].get<int>();
            amount = data[2].get<double>();
        }

        // Get cached orderbook
        auto cached = getCachedOrderBook(symbol);
        if (!cached) return;

        OrderBook ob = *cached;
        ob.timestamp = currentTimeMs();
        ob.local_timestamp = currentTimeNs();

        bool is_bid = amount > 0;
        auto& levels = is_bid ? ob.bids : ob.asks;

        // Find existing level
        auto it = std::find_if(levels.begin(), levels.end(),
            [price](const PriceLevel& l) { return std::abs(l.price - price) < 1e-10; });

        if (count == 0) {
            // Remove level
            if (it != levels.end()) {
                levels.erase(it);
            }
        } else {
            PriceLevel pl;
            pl.price = price;
            pl.quantity = std::abs(amount);
            pl.order_count = static_cast<uint32_t>(count);

            if (it != levels.end()) {
                // Update existing
                *it = pl;
            } else {
                // Insert new level in sorted order
                if (is_bid) {
                    auto pos = std::lower_bound(levels.begin(), levels.end(), pl,
                        [](const PriceLevel& a, const PriceLevel& b) { return a.price > b.price; });
                    levels.insert(pos, pl);
                } else {
                    auto pos = std::lower_bound(levels.begin(), levels.end(), pl,
                        [](const PriceLevel& a, const PriceLevel& b) { return a.price < b.price; });
                    levels.insert(pos, pl);
                }
            }
        }

        onOrderBook(ob);

    } catch (const std::exception& e) {
        onError("Book delta error: " + std::string(e.what()));
    }
}

// ============================================================================
// Trade Update Handler
// ============================================================================

void BitfinexExchange::handleTradeUpdate(const ChannelSubscription& sub, const nlohmann::json& data) {
    try {
        // data format: [CHANNEL_ID, <"te"/"tu">, [ID, MTS, AMOUNT, PRICE]]
        // or snapshot: [CHANNEL_ID, [[ID, MTS, AMOUNT, PRICE], ...]]

        if (data.size() < 2) return;

        if (data[1].is_string()) {
            // Single trade: "te" = trade execution, "tu" = trade update
            std::string trade_type = data[1].get<std::string>();
            if (trade_type == "te" && data.size() >= 3 && data[2].is_array()) {
                auto trade_data = data[2];
                Trade trade = parseTradeFromArray(trade_data, sub.symbol);
                onTrade(trade);
            }
        } else if (data[1].is_array() && !data[1].empty() && data[1][0].is_array()) {
            // Snapshot
            for (const auto& t : data[1]) {
                Trade trade = parseTradeFromArray(t, sub.symbol);
                onTrade(trade);
            }
        }

    } catch (const std::exception& e) {
        onError("Trade update error: " + std::string(e.what()));
    }
}

Trade BitfinexExchange::parseTradeFromArray(const nlohmann::json& arr, const std::string& symbol) {
    Trade trade;
    trade.exchange = "bitfinex";
    trade.symbol = symbol;

    if (arr.size() >= 4) {
        trade.trade_id = std::to_string(arr[0].get<int64_t>());
        trade.timestamp = arr[1].get<uint64_t>();
        double amount = arr[2].get<double>();
        trade.quantity = std::abs(amount);
        trade.price = arr[3].get<double>();
        trade.side = amount > 0 ? OrderSide::Buy : OrderSide::Sell;
    }

    trade.local_timestamp = currentTimeNs();
    return trade;
}

// ============================================================================
// Ticker Update Handler
// ============================================================================

void BitfinexExchange::handleTickerUpdate(const ChannelSubscription& sub, const nlohmann::json& data) {
    try {
        if (!data.is_array() || data.size() < 10) return;

        // Ticker array format:
        // [BID, BID_SIZE, ASK, ASK_SIZE, DAILY_CHANGE, DAILY_CHANGE_RELATIVE, LAST_PRICE, VOLUME, HIGH, LOW]

        Ticker ticker;
        ticker.exchange = "bitfinex";
        ticker.symbol = sub.symbol;
        ticker.bid = data[0].get<double>();
        ticker.bid_qty = data[1].get<double>();
        ticker.ask = data[2].get<double>();
        ticker.ask_qty = data[3].get<double>();
        ticker.change_24h = data[4].get<double>();
        ticker.change_24h_pct = data[5].get<double>() * 100.0;
        ticker.last = data[6].get<double>();
        ticker.volume_24h = data[7].get<double>();
        ticker.high_24h = data[8].get<double>();
        ticker.low_24h = data[9].get<double>();
        ticker.timestamp = currentTimeMs();

        onTicker(ticker);

    } catch (const std::exception& e) {
        onError("Ticker update error: " + std::string(e.what()));
    }
}

// ============================================================================
// Private Data Handlers
// ============================================================================

void BitfinexExchange::handlePrivateData(const std::string& msg_type, const nlohmann::json& data) {
    try {
        if (msg_type == "ws" || msg_type == "wu") {
            // Wallet snapshot/update
            if (data.is_array()) {
                if (!data.empty() && data[0].is_array()) {
                    // Snapshot
                    for (const auto& w : data) {
                        handleWalletUpdate(w);
                    }
                } else {
                    // Single update
                    handleWalletUpdate(data);
                }
            }
        } else if (msg_type == "os" || msg_type == "on" || msg_type == "ou" || msg_type == "oc") {
            // Order snapshot/new/update/cancel
            if (data.is_array()) {
                if (!data.empty() && data[0].is_array()) {
                    // Snapshot
                    for (const auto& o : data) {
                        handleOrderUpdate(o);
                    }
                } else {
                    // Single order
                    handleOrderUpdate(data);
                }
            }
        } else if (msg_type == "ps" || msg_type == "pn" || msg_type == "pu" || msg_type == "pc") {
            // Position snapshot/new/update/close
            if (data.is_array()) {
                if (!data.empty() && data[0].is_array()) {
                    // Snapshot
                    for (const auto& p : data) {
                        handlePositionUpdate(p);
                    }
                } else {
                    // Single position
                    handlePositionUpdate(data);
                }
            }
        } else if (msg_type == "te" || msg_type == "tu") {
            // Trade executed/update
            handleTradeExecutionUpdate(data);
        } else if (msg_type == "n") {
            // Notification
            handleNotification(data);
        }

    } catch (const std::exception& e) {
        onError("Private data error: " + std::string(e.what()));
    }
}

void BitfinexExchange::handleWalletUpdate(const nlohmann::json& data) {
    try {
        Balance balance = parseWalletFromArray(data);
        onBalance(balance);
    } catch (const std::exception& e) {
        onError("Wallet update error: " + std::string(e.what()));
    }
}

Balance BitfinexExchange::parseWalletFromArray(const nlohmann::json& arr) {
    Balance balance;
    // [WALLET_TYPE, CURRENCY, BALANCE, UNSETTLED_INTEREST, AVAILABLE_BALANCE, ...]
    if (arr.size() >= 5) {
        // std::string wallet_type = arr[0].get<std::string>();
        balance.asset = arr[1].get<std::string>();
        double total = arr[2].get<double>();
        double available = arr[4].is_null() ? total : arr[4].get<double>();
        balance.free = available;
        balance.locked = total - available;
    }
    return balance;
}

void BitfinexExchange::handleOrderUpdate(const nlohmann::json& data) {
    try {
        Order order = parseOrderFromArray(data);
        onOrder(order);
    } catch (const std::exception& e) {
        onError("Order update error: " + std::string(e.what()));
    }
}

Order BitfinexExchange::parseOrderFromArray(const nlohmann::json& arr) {
    Order order;
    order.exchange = "bitfinex";

    // Bitfinex order array:
    // [ID, GID, CID, SYMBOL, MTS_CREATE, MTS_UPDATE, AMOUNT, AMOUNT_ORIG, TYPE, TYPE_PREV,
    //  MTS_TIF, _, FLAGS, STATUS, _, _, PRICE, PRICE_AVG, PRICE_TRAILING, PRICE_AUX_LIMIT,
    //  _, _, _, NOTIFY, HIDDEN, PLACED_ID, _, _, ROUTING, _, _, META]

    if (arr.size() >= 20) {
        order.order_id = std::to_string(arr[0].get<int64_t>());
        order.client_order_id = arr[2].is_null() ? "" : std::to_string(arr[2].get<int64_t>());
        order.symbol = arr[3].get<std::string>();
        order.create_time = arr[4].get<uint64_t>();
        order.update_time = arr[5].get<uint64_t>();

        double amount = arr[6].get<double>();
        double amount_orig = arr[7].get<double>();

        order.side = parseOrderSide(amount_orig);
        order.quantity = std::abs(amount_orig);
        order.remaining_quantity = std::abs(amount);
        order.filled_quantity = order.quantity - order.remaining_quantity;

        std::string order_type = arr[8].get<std::string>();
        order.type = parseOrderType(order_type);
        order.time_in_force = parseTimeInForce(order_type);

        std::string status = arr[13].get<std::string>();
        order.status = parseOrderStatus(status);

        order.price = arr[16].is_null() ? 0.0 : arr[16].get<double>();
        order.average_price = arr[17].is_null() ? 0.0 : arr[17].get<double>();

        // Check flags
        if (arr.size() > 12 && !arr[12].is_null()) {
            int flags = arr[12].get<int>();
            order.reduce_only = (flags & FLAG_REDUCE_ONLY) != 0;
        }

        order.raw = arr;
    }

    return order;
}

void BitfinexExchange::handlePositionUpdate(const nlohmann::json& data) {
    try {
        Position position = parsePositionFromArray(data);
        onPosition(position);
    } catch (const std::exception& e) {
        onError("Position update error: " + std::string(e.what()));
    }
}

Position BitfinexExchange::parsePositionFromArray(const nlohmann::json& arr) {
    Position position;
    position.exchange = "bitfinex";

    // Position array:
    // [SYMBOL, STATUS, AMOUNT, BASE_PRICE, MARGIN_FUNDING, MARGIN_FUNDING_TYPE, PL, PL_PERC,
    //  PRICE_LIQ, LEVERAGE, FLAG, POSITION_ID, MTS_CREATE, MTS_UPDATE, _, TYPE, _, COLLATERAL,
    //  COLLATERAL_MIN, META]

    if (arr.size() >= 10) {
        position.symbol = arr[0].get<std::string>();
        double amount = arr[2].get<double>();
        position.side = amount > 0 ? OrderSide::Buy : OrderSide::Sell;
        position.quantity = std::abs(amount);
        position.entry_price = arr[3].get<double>();
        position.unrealized_pnl = arr[6].is_null() ? 0.0 : arr[6].get<double>();
        position.liquidation_price = arr[8].is_null() ? 0.0 : arr[8].get<double>();
        position.leverage = arr[9].is_null() ? 1.0 : arr[9].get<double>();

        if (arr.size() >= 14) {
            position.update_time = arr[13].get<uint64_t>();
        }
    }

    return position;
}

void BitfinexExchange::handleTradeExecutionUpdate(const nlohmann::json& data) {
    // Trade execution on your orders - can be used for fill tracking
    // [ID, SYMBOL, MTS_CREATE, ORDER_ID, EXEC_AMOUNT, EXEC_PRICE, ORDER_TYPE, ORDER_PRICE,
    //  MAKER, FEE, FEE_CURRENCY, CID]
}

void BitfinexExchange::handleNotification(const nlohmann::json& data) {
    // Notification format: [MTS, TYPE, MESSAGE_ID, _, [NOTIFY_INFO], CODE, STATUS, TEXT]
    if (data.is_array() && data.size() >= 8) {
        std::string status = data[6].get<std::string>();
        std::string text = data[7].get<std::string>();
        if (status == "ERROR") {
            onError("Notification: " + text);
        }
    }
}

// ============================================================================
// Subscription Management
// ============================================================================

void BitfinexExchange::sendSubscribe(const std::string& channel, const std::string& symbol,
                                     const std::string& precision, int depth) {
    if (!ws_client_ || !ws_connected_.load()) return;

    nlohmann::json msg;
    msg["event"] = "subscribe";
    msg["channel"] = channel;
    msg["symbol"] = symbol;

    if (channel == "book") {
        msg["prec"] = precision.empty() ? getPrecisionString(bitfinex_config_.book_precision) : precision;
        msg["freq"] = "F0";  // Real-time updates (F0) vs batched (F1)
        msg["len"] = depth;  // Integer, not string: 1, 25, 100, or 250
    }

    ws_client_->send(msg);
}

void BitfinexExchange::sendUnsubscribe(int channel_id) {
    if (!ws_client_ || !ws_connected_.load()) return;

    nlohmann::json msg;
    msg["event"] = "unsubscribe";
    msg["chanId"] = channel_id;

    ws_client_->send(msg);
}

void BitfinexExchange::authenticateWebSocket() {
    if (!ws_client_ || !ws_connected_.load()) return;
    if (bitfinex_config_.api_key.empty() || bitfinex_config_.api_secret.empty()) return;

    std::string nonce = generateNonce();
    std::string auth_payload = "AUTH" + nonce;
    std::string signature = sign(auth_payload);

    nlohmann::json auth_msg;
    auth_msg["event"] = "auth";
    auth_msg["apiKey"] = bitfinex_config_.api_key;
    auth_msg["authSig"] = signature;
    auth_msg["authNonce"] = nonce;
    auth_msg["authPayload"] = auth_payload;
    auth_msg["calc"] = 1;  // Enable balance calculations
    auth_msg["filter"] = nlohmann::json::array({"trading", "wallet", "balance"});

    ws_client_->send(auth_msg);
}

// ============================================================================
// Heartbeat Management
// ============================================================================

void BitfinexExchange::startHeartbeat() {
    if (heartbeat_running_.load()) return;

    heartbeat_running_ = true;
    {
        std::lock_guard<std::mutex> lock(heartbeat_mutex_);
        last_heartbeat_ = std::chrono::steady_clock::now();
    }

    heartbeat_thread_ = std::make_unique<std::thread>([this]() {
        while (heartbeat_running_.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(30));

            if (!heartbeat_running_.load()) break;

            // Check if we've received heartbeat recently
            std::chrono::steady_clock::time_point last;
            {
                std::lock_guard<std::mutex> lock(heartbeat_mutex_);
                last = last_heartbeat_;
            }

            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last).count();

            if (elapsed > 60) {
                onError("No heartbeat received for " + std::to_string(elapsed) + " seconds");
                // Connection might be stale, consider reconnecting
            }
        }
    });
}

void BitfinexExchange::stopHeartbeat() {
    heartbeat_running_ = false;
    if (heartbeat_thread_ && heartbeat_thread_->joinable()) {
        heartbeat_thread_->join();
    }
    heartbeat_thread_.reset();
}

// ============================================================================
// Subscription Methods
// ============================================================================

bool BitfinexExchange::subscribeOrderBook(const std::string& symbol, int depth) {
    sendSubscribe("book", symbol, "", depth);
    return true;
}

bool BitfinexExchange::unsubscribeOrderBook(const std::string& symbol) {
    int channel_id = 0;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        auto it = symbol_to_channel_.find("book:" + symbol);
        if (it != symbol_to_channel_.end()) {
            channel_id = it->second;
        }
    }

    if (channel_id > 0) {
        sendUnsubscribe(channel_id);
    }

    clearOrderBookCache(symbol);
    return true;
}

bool BitfinexExchange::subscribeTrades(const std::string& symbol) {
    sendSubscribe("trades", symbol);
    return true;
}

bool BitfinexExchange::unsubscribeTrades(const std::string& symbol) {
    int channel_id = 0;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        auto it = symbol_to_channel_.find("trades:" + symbol);
        if (it != symbol_to_channel_.end()) {
            channel_id = it->second;
        }
    }

    if (channel_id > 0) {
        sendUnsubscribe(channel_id);
    }

    return true;
}

bool BitfinexExchange::subscribeTicker(const std::string& symbol) {
    sendSubscribe("ticker", symbol);
    return true;
}

bool BitfinexExchange::unsubscribeTicker(const std::string& symbol) {
    int channel_id = 0;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        auto it = symbol_to_channel_.find("ticker:" + symbol);
        if (it != symbol_to_channel_.end()) {
            channel_id = it->second;
        }
    }

    if (channel_id > 0) {
        sendUnsubscribe(channel_id);
    }

    return true;
}

bool BitfinexExchange::subscribeUserData() {
    if (!ws_authenticated_.load()) {
        authenticateWebSocket();
    }
    return true;
}

bool BitfinexExchange::unsubscribeUserData() {
    // User data is always on channel 0 when authenticated
    // Cannot really unsubscribe, but we can track that we don't want updates
    return true;
}

// ============================================================================
// REST Helpers
// ============================================================================

network::HttpResponse BitfinexExchange::signedPost(const std::string& endpoint,
                                                   const nlohmann::json& body) {
    // Rate limiting check
    {
        std::lock_guard<std::mutex> lock(rate_limit_mutex_);
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_request_time_).count();
        double min_interval = 60000.0 / bitfinex_config_.request_rate_limit;  // ms between requests
        if (elapsed < min_interval) {
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(min_interval - elapsed)));
        }
        last_request_time_ = std::chrono::steady_clock::now();
    }

    std::string body_str = body.empty() ? "{}" : body.dump();

    network::HttpRequest request;
    request.method = network::HttpMethod::POST;
    request.path = endpoint;
    request.body = body_str;
    addAuthHeaders(request, endpoint, body_str);

    return rest_client_->execute(request);
}

network::HttpResponse BitfinexExchange::publicGet(const std::string& endpoint,
                                                  const std::unordered_map<std::string, std::string>& params) {
    return rest_client_->get(endpoint, params);
}

// ============================================================================
// Order Management
// ============================================================================

std::optional<Order> BitfinexExchange::placeOrder(const OrderRequest& request) {
    nlohmann::json body;

    // Build order type string
    std::string order_type;
    if (bitfinex_config_.default_wallet == BitfinexWalletType::Exchange) {
        order_type = "EXCHANGE ";
    }
    order_type += orderTypeToString(request.type, request.post_only);

    body["type"] = order_type;
    body["symbol"] = request.symbol;

    // Amount is signed: positive for buy, negative for sell
    double amount = request.quantity;
    if (request.side == OrderSide::Sell) {
        amount = -amount;
    }
    body["amount"] = std::to_string(amount);

    if (request.type != OrderType::Market) {
        body["price"] = std::to_string(request.price);
    }

    if (request.stop_price > 0.0) {
        body["price_aux_limit"] = std::to_string(request.stop_price);
    }

    if (!request.client_order_id.empty()) {
        body["cid"] = std::stoll(request.client_order_id);
    } else {
        body["cid"] = static_cast<int64_t>(currentTimeMs());
    }

    // Build flags
    int flags = 0;
    if (request.reduce_only) flags |= FLAG_REDUCE_ONLY;
    if (request.post_only) flags |= FLAG_POST_ONLY;
    if (flags > 0) {
        body["flags"] = flags;
    }

    auto response = signedPost("/v2/auth/w/order/submit", body);

    if (!response.success) {
        onError("Place order failed: " + response.error + " " + response.body);
        return std::nullopt;
    }

    try {
        auto json = response.json();

        // Response format: [MTS, TYPE, MESSAGE_ID, null, [ORDER_ARRAY], CODE, STATUS, TEXT]
        if (json.is_array() && json.size() >= 7) {
            std::string status = json[6].get<std::string>();
            if (status == "SUCCESS" && json[4].is_array() && json[4].size() > 0) {
                // Get the first order in the response
                auto order_data = json[4][0];
                return parseOrderFromArray(order_data);
            } else {
                std::string error_msg = json.size() > 7 ? json[7].get<std::string>() : "Unknown error";
                onError("Place order failed: " + error_msg);
                return std::nullopt;
            }
        }

        onError("Unexpected order response format");
        return std::nullopt;

    } catch (const std::exception& e) {
        onError("Parse order response failed: " + std::string(e.what()));
        return std::nullopt;
    }
}

std::optional<Order> BitfinexExchange::cancelOrder(const std::string& symbol,
                                                   const std::string& order_id) {
    nlohmann::json body;
    body["id"] = std::stoll(order_id);

    auto response = signedPost("/v2/auth/w/order/cancel", body);

    if (!response.success) {
        onError("Cancel order failed: " + response.error);
        return std::nullopt;
    }

    try {
        auto json = response.json();

        // Response format: [MTS, TYPE, MESSAGE_ID, null, [ORDER_ARRAY], CODE, STATUS, TEXT]
        if (json.is_array() && json.size() >= 7) {
            std::string status = json[6].get<std::string>();
            if (status == "SUCCESS" && json[4].is_array()) {
                // Parse the cancelled order from response
                if (!json[4].empty()) {
                    return parseOrderFromArray(json[4]);
                }
                // Fallback if order data not returned
                Order order;
                order.order_id = order_id;
                order.symbol = symbol;
                order.status = OrderStatus::Cancelled;
                return order;
            } else {
                std::string error_msg = json.size() > 7 ? json[7].get<std::string>() : "Cancel failed";
                onError("Cancel order failed: " + error_msg);
            }
        }

        return std::nullopt;

    } catch (const std::exception& e) {
        onError("Cancel order parse error: " + std::string(e.what()));
        return std::nullopt;
    }
}

std::optional<Order> BitfinexExchange::cancelOrder(const std::string& symbol,
                                                   const std::string& order_id,
                                                   const std::string& client_order_id) {
    if (!order_id.empty()) {
        return cancelOrder(symbol, order_id);
    }

    // Cancel by client order ID
    nlohmann::json body;
    body["cid"] = std::stoll(client_order_id);
    body["cid_date"] = "";  // Date is optional for recent orders

    auto response = signedPost("/v2/auth/w/order/cancel", body);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();

        if (json.is_array() && json.size() >= 7) {
            std::string status = json[6].get<std::string>();
            if (status == "SUCCESS") {
                Order order;
                order.client_order_id = client_order_id;
                order.symbol = symbol;
                order.status = OrderStatus::Cancelled;
                return order;
            }
        }

        return std::nullopt;

    } catch (...) {
        return std::nullopt;
    }
}

bool BitfinexExchange::cancelAllOrders(const std::string& symbol) {
    nlohmann::json body;
    if (!symbol.empty()) {
        body["symbol"] = symbol;
    }
    body["all"] = 1;

    auto response = signedPost("/v2/auth/w/order/cancel/multi", body);
    return response.success;
}

std::optional<Order> BitfinexExchange::getOrder(const std::string& symbol,
                                                const std::string& order_id) {
    nlohmann::json body;
    body["id"] = nlohmann::json::array({std::stoll(order_id)});

    auto response = signedPost("/v2/auth/r/orders", body);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();

        if (json.is_array() && !json.empty()) {
            return parseOrderFromArray(json[0]);
        }

        return std::nullopt;

    } catch (...) {
        return std::nullopt;
    }
}

std::vector<Order> BitfinexExchange::getOpenOrders(const std::string& symbol) {
    std::string endpoint = "/v2/auth/r/orders";
    if (!symbol.empty()) {
        endpoint += "/" + symbol;
    }

    auto response = signedPost(endpoint, {});

    std::vector<Order> orders;
    if (!response.success) {
        return orders;
    }

    try {
        auto json = response.json();

        if (json.is_array()) {
            for (const auto& item : json) {
                orders.push_back(parseOrderFromArray(item));
            }
        }

    } catch (...) {
        // Return empty
    }

    return orders;
}

std::vector<Order> BitfinexExchange::getOrderHistory(const std::string& symbol,
                                                     uint64_t start_time,
                                                     uint64_t end_time,
                                                     int limit) {
    std::string endpoint = "/v2/auth/r/orders";
    if (!symbol.empty()) {
        endpoint += "/" + symbol;
    }
    endpoint += "/hist";

    nlohmann::json body;
    if (start_time > 0) body["start"] = start_time;
    if (end_time > 0) body["end"] = end_time;
    body["limit"] = limit;

    auto response = signedPost(endpoint, body);

    std::vector<Order> orders;
    if (!response.success) {
        return orders;
    }

    try {
        auto json = response.json();

        if (json.is_array()) {
            for (const auto& item : json) {
                orders.push_back(parseOrderFromArray(item));
            }
        }

    } catch (...) {
        // Return empty
    }

    return orders;
}

// ============================================================================
// Account Information
// ============================================================================

std::optional<Account> BitfinexExchange::getAccount() {
    auto response = signedPost("/v2/auth/r/wallets", {});

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();

        Account account;
        account.exchange = "bitfinex";
        account.update_time = currentTimeMs();

        if (json.is_array()) {
            for (const auto& w : json) {
                Balance balance = parseWalletFromArray(w);
                if (balance.free > 0 || balance.locked > 0) {
                    account.balances[balance.asset] = balance;
                }
            }
        }

        // Get positions
        account.positions = getPositions();

        // Calculate unrealized PnL from positions
        for (const auto& pos : account.positions) {
            account.total_unrealized_pnl += pos.unrealized_pnl;
        }

        return account;

    } catch (const std::exception& e) {
        onError("Parse account failed: " + std::string(e.what()));
        return std::nullopt;
    }
}

std::optional<Balance> BitfinexExchange::getBalance(const std::string& asset) {
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

std::vector<Position> BitfinexExchange::getPositions(const std::string& symbol) {
    auto response = signedPost("/v2/auth/r/positions", {});

    std::vector<Position> positions;
    if (!response.success) {
        return positions;
    }

    try {
        auto json = response.json();

        if (json.is_array()) {
            for (const auto& p : json) {
                Position pos = parsePositionFromArray(p);
                if (pos.quantity > 0 && (symbol.empty() || pos.symbol == symbol)) {
                    positions.push_back(pos);
                }
            }
        }

    } catch (...) {
        // Return empty
    }

    return positions;
}

std::vector<Balance> BitfinexExchange::getWallets(BitfinexWalletType type) {
    auto response = signedPost("/v2/auth/r/wallets", {});

    std::vector<Balance> wallets;
    if (!response.success) {
        return wallets;
    }

    try {
        auto json = response.json();
        std::string type_str = getWalletTypeString(type);

        if (json.is_array()) {
            for (const auto& w : json) {
                if (w.is_array() && w.size() >= 5) {
                    std::string wallet_type = w[0].get<std::string>();
                    if (wallet_type == type_str) {
                        Balance balance = parseWalletFromArray(w);
                        wallets.push_back(balance);
                    }
                }
            }
        }

    } catch (...) {
        // Return empty
    }

    return wallets;
}

bool BitfinexExchange::transferBetweenWallets(const std::string& currency, double amount,
                                              BitfinexWalletType from, BitfinexWalletType to) {
    nlohmann::json body;
    body["from"] = getWalletTypeString(from);
    body["to"] = getWalletTypeString(to);
    body["currency"] = currency;
    body["amount"] = std::to_string(amount);

    auto response = signedPost("/v2/auth/w/transfer", body);

    if (response.success) {
        auto json = response.json();
        if (json.is_array() && json.size() >= 7) {
            return json[6].get<std::string>() == "SUCCESS";
        }
    }

    return false;
}

// ============================================================================
// Market Information
// ============================================================================

std::vector<SymbolInfo> BitfinexExchange::getSymbols() {
    // Get trading pair info
    auto conf_response = publicGet("/v2/conf/pub:info:pair", {});
    auto detail_response = publicGet("/v2/conf/pub:info:pair:futures", {});

    std::vector<SymbolInfo> symbols;

    try {
        if (conf_response.success) {
            auto json = conf_response.json();
            if (json.is_array() && !json.empty() && json[0].is_array()) {
                for (const auto& item : json[0]) {
                    auto info = parseSymbolInfo(item);
                    updateSymbolInfo(info);
                    symbols.push_back(info);
                }
            }
        }
    } catch (...) {
        // Continue with what we have
    }

    return symbols;
}

std::optional<SymbolInfo> BitfinexExchange::getSymbolInfo(const std::string& symbol) {
    // Check cache first
    {
        std::lock_guard<std::mutex> lock(symbol_info_mutex_);
        auto it = symbol_info_cache_.find(symbol);
        if (it != symbol_info_cache_.end()) {
            return it->second;
        }
    }

    // Fetch all and find
    auto symbols = getSymbols();
    for (const auto& s : symbols) {
        if (s.symbol == symbol) {
            return s;
        }
    }

    return std::nullopt;
}

SymbolInfo BitfinexExchange::parseSymbolInfo(const nlohmann::json& data) {
    SymbolInfo info;

    // Data format varies, but typically:
    // [PAIR, [PLACEHOLDER, PLACEHOLDER, PLACEHOLDER, MIN_ORDER_SIZE, MAX_ORDER_SIZE, ...]]
    if (data.is_array() && data.size() >= 2) {
        std::string pair = data[0].get<std::string>();
        info.symbol = "t" + pair;  // Add trading prefix

        if (data[1].is_array() && data[1].size() >= 5) {
            info.min_qty = data[1][3].get<double>();
            info.max_qty = data[1][4].get<double>();
        }

        // Parse base/quote from pair
        if (pair.length() >= 6) {
            // Usually format is BTCUSD or BTC:USD
            size_t sep = pair.find(':');
            if (sep != std::string::npos) {
                info.base_asset = pair.substr(0, sep);
                info.quote_asset = pair.substr(sep + 1);
            } else if (pair.length() == 6) {
                info.base_asset = pair.substr(0, 3);
                info.quote_asset = pair.substr(3);
            }
        }
    }

    info.trading_enabled = true;

    return info;
}

std::optional<OrderBook> BitfinexExchange::getOrderBookSnapshot(const std::string& symbol, int depth) {
    std::string precision = getPrecisionString(bitfinex_config_.book_precision);
    std::string endpoint = "/v2/book/" + symbol + "/" + precision;

    std::unordered_map<std::string, std::string> params;
    params["len"] = std::to_string(depth);

    auto response = publicGet(endpoint, params);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();

        OrderBook ob;
        ob.symbol = symbol;
        ob.exchange = "bitfinex";
        ob.timestamp = currentTimeMs();
        ob.local_timestamp = currentTimeNs();

        if (json.is_array()) {
            for (const auto& level : json) {
                if (!level.is_array() || level.size() < 3) continue;

                double price = level[0].get<double>();
                int count = level[1].get<int>();
                double amount = level[2].get<double>();

                if (count == 0) continue;

                PriceLevel pl;
                pl.price = price;
                pl.quantity = std::abs(amount);
                pl.order_count = static_cast<uint32_t>(count);

                if (amount > 0) {
                    ob.bids.push_back(pl);
                } else {
                    ob.asks.push_back(pl);
                }
            }
        }

        // Sort
        std::sort(ob.bids.begin(), ob.bids.end(),
                  [](const PriceLevel& a, const PriceLevel& b) { return a.price > b.price; });
        std::sort(ob.asks.begin(), ob.asks.end(),
                  [](const PriceLevel& a, const PriceLevel& b) { return a.price < b.price; });

        return ob;

    } catch (...) {
        return std::nullopt;
    }
}

std::vector<Trade> BitfinexExchange::getRecentTrades(const std::string& symbol, int limit) {
    std::string endpoint = "/v2/trades/" + symbol + "/hist";

    std::unordered_map<std::string, std::string> params;
    params["limit"] = std::to_string(limit);

    auto response = publicGet(endpoint, params);

    std::vector<Trade> trades;
    if (!response.success) {
        return trades;
    }

    try {
        auto json = response.json();

        if (json.is_array()) {
            for (const auto& t : json) {
                trades.push_back(parseTradeFromArray(t, symbol));
            }
        }

    } catch (...) {
        // Return empty
    }

    return trades;
}

std::optional<Ticker> BitfinexExchange::getTicker(const std::string& symbol) {
    std::string endpoint = "/v2/ticker/" + symbol;

    auto response = publicGet(endpoint, {});

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();

        if (json.is_array() && json.size() >= 10) {
            Ticker ticker;
            ticker.exchange = "bitfinex";
            ticker.symbol = symbol;
            ticker.bid = json[0].get<double>();
            ticker.bid_qty = json[1].get<double>();
            ticker.ask = json[2].get<double>();
            ticker.ask_qty = json[3].get<double>();
            ticker.change_24h = json[4].get<double>();
            ticker.change_24h_pct = json[5].get<double>() * 100.0;
            ticker.last = json[6].get<double>();
            ticker.volume_24h = json[7].get<double>();
            ticker.high_24h = json[8].get<double>();
            ticker.low_24h = json[9].get<double>();
            ticker.timestamp = currentTimeMs();

            return ticker;
        }

        return std::nullopt;

    } catch (...) {
        return std::nullopt;
    }
}

uint64_t BitfinexExchange::getServerTime() {
    // Bitfinex doesn't have a dedicated time endpoint
    // We use platform status which includes server time
    auto response = publicGet("/v2/platform/status", {});

    if (response.success) {
        // Just return current time as Bitfinex uses standard timestamps
        return currentTimeMs();
    }

    return 0;
}

std::string BitfinexExchange::formatSymbol(const std::string& base, const std::string& quote) const {
    // Bitfinex trading pair format: tBTCUSD or tBTC:USD for pairs with colon
    return "t" + base + quote;
}

std::pair<std::string, std::string> BitfinexExchange::parseSymbol(const std::string& symbol) const {
    // Parse tBTCUSD or tBTC:USD
    std::string pair = symbol;
    if (!pair.empty() && pair[0] == 't') {
        pair = pair.substr(1);
    }

    // Check for colon separator
    size_t sep = pair.find(':');
    if (sep != std::string::npos) {
        return {pair.substr(0, sep), pair.substr(sep + 1)};
    }

    // Common quote assets
    static const std::vector<std::string> quote_assets = {"USD", "UST", "BTC", "ETH", "EUR", "GBP", "JPY"};

    for (const auto& quote : quote_assets) {
        if (pair.length() > quote.length() &&
            pair.substr(pair.length() - quote.length()) == quote) {
            return {pair.substr(0, pair.length() - quote.length()), quote};
        }
    }

    // Default: assume 3+3 format
    if (pair.length() == 6) {
        return {pair.substr(0, 3), pair.substr(3)};
    }

    return {pair, ""};
}

// ============================================================================
// Data Converters
// ============================================================================

OrderStatus BitfinexExchange::parseOrderStatus(const std::string& status) {
    // Bitfinex status format: "ACTIVE", "EXECUTED", "PARTIALLY FILLED", "CANCELED", etc.
    if (status.find("ACTIVE") != std::string::npos) return OrderStatus::New;
    if (status.find("EXECUTED") != std::string::npos) return OrderStatus::Filled;
    if (status.find("PARTIALLY FILLED") != std::string::npos) return OrderStatus::PartiallyFilled;
    if (status.find("CANCELED") != std::string::npos) return OrderStatus::Cancelled;
    if (status.find("INSUFFICIENT") != std::string::npos) return OrderStatus::Rejected;
    if (status.find("RSN_DUST") != std::string::npos) return OrderStatus::Rejected;
    if (status.find("RSN_PAUSE") != std::string::npos) return OrderStatus::Rejected;
    return OrderStatus::Failed;
}

OrderSide BitfinexExchange::parseOrderSide(double amount) {
    return amount > 0 ? OrderSide::Buy : OrderSide::Sell;
}

OrderType BitfinexExchange::parseOrderType(const std::string& type) {
    std::string upper_type = type;
    std::transform(upper_type.begin(), upper_type.end(), upper_type.begin(), ::toupper);

    if (upper_type.find("MARKET") != std::string::npos) return OrderType::Market;
    if (upper_type.find("LIMIT") != std::string::npos) return OrderType::Limit;
    if (upper_type.find("STOP") != std::string::npos) {
        if (upper_type.find("LIMIT") != std::string::npos) return OrderType::StopLimit;
        return OrderType::StopMarket;
    }
    if (upper_type.find("TRAILING") != std::string::npos) return OrderType::TrailingStop;
    if (upper_type.find("FOK") != std::string::npos) return OrderType::FillOrKill;
    if (upper_type.find("IOC") != std::string::npos) return OrderType::ImmediateOrCancel;

    return OrderType::Limit;
}

TimeInForce BitfinexExchange::parseTimeInForce(const std::string& type) {
    std::string upper_type = type;
    std::transform(upper_type.begin(), upper_type.end(), upper_type.begin(), ::toupper);

    if (upper_type.find("FOK") != std::string::npos) return TimeInForce::FOK;
    if (upper_type.find("IOC") != std::string::npos) return TimeInForce::IOC;

    return TimeInForce::GTC;
}

std::string BitfinexExchange::orderTypeToString(OrderType type, bool post_only) {
    switch (type) {
        case OrderType::Market: return "MARKET";
        case OrderType::Limit: return post_only ? "LIMIT" : "LIMIT";  // Post-only is handled via flags
        case OrderType::StopMarket: return "STOP";
        case OrderType::StopLimit: return "STOP LIMIT";
        case OrderType::TrailingStop: return "TRAILING STOP";
        case OrderType::FillOrKill: return "FOK";
        case OrderType::ImmediateOrCancel: return "IOC";
        default: return "LIMIT";
    }
}

std::string BitfinexExchange::timeInForceToString(TimeInForce tif) {
    switch (tif) {
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
        default: return "";  // GTC is default, no suffix needed
    }
}

// ============================================================================
// Bitfinex-specific Methods
// ============================================================================

bool BitfinexExchange::setLeverage(const std::string& symbol, double leverage) {
    // Bitfinex uses margin funding for leverage, not direct leverage setting
    // This would involve position management
    return false;
}

double BitfinexExchange::getMarginInfo(const std::string& symbol) {
    nlohmann::json body;
    body["symbol"] = symbol;

    auto response = signedPost("/v2/auth/r/info/margin/" + symbol, {});

    if (response.success) {
        auto json = response.json();
        if (json.is_array() && json.size() >= 3) {
            // Returns [SYMBOL, [USER_PL, USER_SWAP, MARGIN_BALANCE, MARGIN_NET, MARGIN_REQUIRED, ...]]
            if (json[1].is_array() && json[1].size() >= 5) {
                return json[1][4].get<double>();  // Margin required
            }
        }
    }

    return 0.0;
}

// ============================================================================
// Factory Functions
// ============================================================================

std::shared_ptr<BitfinexExchange> createBitfinexExchange(const ExchangeConfig& config) {
    return std::make_shared<BitfinexExchange>(config);
}

std::shared_ptr<BitfinexExchange> createBitfinexExchange(const BitfinexConfig& config) {
    return std::make_shared<BitfinexExchange>(config);
}

} // namespace exchange
} // namespace hft
