#include "exchange/binance.hpp"

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

BinanceExchange::BinanceExchange(const ExchangeConfig& config)
    : ExchangeBase(config) {
    binance_config_.api_key = config.api_key;
    binance_config_.api_secret = config.api_secret;
    binance_config_.testnet = config.testnet;
    binance_config_.spot = (config.type == ExchangeType::Spot);
    binance_config_.order_rate_limit = config.orders_per_second;
    binance_config_.request_rate_limit = config.requests_per_minute;

    // Initialize REST client
    network::RestClientConfig rest_config;
    rest_config.base_url = getRestUrl();
    rest_config.name = "binance_rest";
    rest_config.request_timeout_ms = config.rest_timeout_ms;
    rest_config.max_retries = config.rest_max_retries;
    rest_config.rate_limit.requests_per_second = binance_config_.order_rate_limit;
    rest_config.rate_limit.requests_per_minute = binance_config_.request_rate_limit;
    rest_config.signing.api_key = config.api_key;
    rest_config.signing.api_secret = config.api_secret;
    rest_config.default_headers["X-MBX-APIKEY"] = config.api_key;

    rest_client_ = std::make_shared<network::RestClient>(rest_config);
}

BinanceExchange::BinanceExchange(const BinanceConfig& binance_config)
    : ExchangeBase(ExchangeConfig{
        .name = "Binance",
        .api_key = binance_config.api_key,
        .api_secret = binance_config.api_secret,
        .type = binance_config.spot ? ExchangeType::Spot : ExchangeType::Perpetual,
        .testnet = binance_config.testnet
      }),
      binance_config_(binance_config) {

    // Initialize REST client
    network::RestClientConfig rest_config;
    rest_config.base_url = getRestUrl();
    rest_config.name = "binance_rest";
    rest_config.rate_limit.requests_per_second = binance_config_.order_rate_limit;
    rest_config.rate_limit.requests_per_minute = binance_config_.request_rate_limit;
    rest_config.default_headers["X-MBX-APIKEY"] = binance_config_.api_key;

    rest_client_ = std::make_shared<network::RestClient>(rest_config);
}

BinanceExchange::~BinanceExchange() {
    disconnect();
}

// ============================================================================
// URL Builders
// ============================================================================

std::string BinanceExchange::getSpotRestUrl() const {
    return binance_config_.testnet
        ? "https://testnet.binance.vision/api/v3"
        : "https://api.binance.com/api/v3";
}

std::string BinanceExchange::getFuturesRestUrl() const {
    return binance_config_.testnet
        ? "https://testnet.binancefuture.com/fapi/v1"
        : "https://fapi.binance.com/fapi/v1";
}

std::string BinanceExchange::getSpotWsUrl() const {
    return binance_config_.testnet
        ? "wss://testnet.binance.vision/ws"
        : "wss://stream.binance.com:9443/ws";
}

std::string BinanceExchange::getFuturesWsUrl() const {
    return binance_config_.testnet
        ? "wss://stream.binancefuture.com/ws"
        : "wss://fstream.binance.com/ws";
}

std::string BinanceExchange::getRestUrl() const {
    return binance_config_.spot ? getSpotRestUrl() : getFuturesRestUrl();
}

std::string BinanceExchange::getWsUrl() const {
    return binance_config_.spot ? getSpotWsUrl() : getFuturesWsUrl();
}

// ============================================================================
// Connection Management
// ============================================================================

bool BinanceExchange::connect() {
    if (ws_connected_.load()) {
        return true;
    }

    onConnectionStatus(ConnectionStatus::Connecting);

    // Initialize WebSocket client for market data
    network::WebSocketConfig ws_config;
    ws_config.url = getWsUrl();
    ws_config.name = "binance_market_ws";
    ws_config.ping_interval_ms = 180000;  // Binance recommends ping every 3 minutes
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

void BinanceExchange::disconnect() {
    stopListenKeyRefresh();

    if (user_ws_client_) {
        user_ws_client_->disconnect();
        user_ws_client_.reset();
    }

    if (ws_client_) {
        ws_client_->disconnect();
        ws_client_.reset();
    }

    ws_connected_ = false;
    user_ws_connected_ = false;
    onConnectionStatus(ConnectionStatus::Disconnected);
}

bool BinanceExchange::isConnected() const {
    return ws_connected_.load();
}

ConnectionStatus BinanceExchange::connectionStatus() const {
    return connection_status_.load();
}

// ============================================================================
// WebSocket Handlers
// ============================================================================

void BinanceExchange::handleWsOpen() {
    ws_connected_ = true;
    onConnectionStatus(ConnectionStatus::Connected);
}

void BinanceExchange::handleWsClose(int code, const std::string& reason) {
    ws_connected_ = false;
    onConnectionStatus(ConnectionStatus::Reconnecting);
}

void BinanceExchange::handleWsError(const std::string& error) {
    onError("WebSocket error: " + error);
}

void BinanceExchange::handleWsMessage(const std::string& message, network::MessageType type) {
    try {
        auto json = nlohmann::json::parse(message);

        // Check for stream identifier
        std::string stream;
        nlohmann::json data;

        if (json.contains("stream")) {
            stream = json["stream"].get<std::string>();
            data = json["data"];
        } else if (json.contains("e")) {
            // Direct event
            data = json;
            stream = data["e"].get<std::string>();
        } else {
            // Subscription response or other message
            return;
        }

        // Route to appropriate handler based on stream type
        if (stream.find("@depth") != std::string::npos ||
            stream == "depthUpdate") {
            handleDepthUpdate(data);
        } else if (stream.find("@trade") != std::string::npos ||
                   stream == "trade") {
            handleTradeUpdate(data);
        } else if (stream.find("@ticker") != std::string::npos ||
                   stream.find("@bookTicker") != std::string::npos ||
                   stream == "24hrTicker") {
            handleTickerUpdate(data);
        } else if (stream == "executionReport" ||
                   stream == "ORDER_TRADE_UPDATE") {
            handleOrderUpdate(data);
        } else if (stream == "outboundAccountPosition" ||
                   stream == "ACCOUNT_UPDATE") {
            handleAccountUpdate(data);
        } else if (stream == "ACCOUNT_CONFIG_UPDATE") {
            handlePositionUpdate(data);
        }

    } catch (const std::exception& e) {
        onError("Message parse error: " + std::string(e.what()));
    }
}

void BinanceExchange::handleDepthUpdate(const nlohmann::json& data) {
    try {
        std::string symbol = data.value("s", "");
        if (symbol.empty()) return;

        uint64_t update_id = data.value("u", uint64_t(0));
        uint64_t timestamp = data.value("E", uint64_t(0));

        // Check if we need to initialize orderbook first
        {
            std::lock_guard<std::mutex> lock(orderbook_init_mutex_);
            auto it = orderbook_initialized_.find(symbol);
            if (it == orderbook_initialized_.end() || !it->second) {
                initializeOrderBook(symbol, binance_config_.orderbook_depth);
                return;
            }

            // Check sequence
            auto last_it = last_update_id_.find(symbol);
            if (last_it != last_update_id_.end() && update_id <= last_it->second) {
                return;  // Old update, skip
            }
            last_update_id_[symbol] = update_id;
        }

        // Apply delta to cached orderbook
        applyOrderBookDelta(symbol, data);

    } catch (const std::exception& e) {
        onError("Depth update error: " + std::string(e.what()));
    }
}

void BinanceExchange::handleTradeUpdate(const nlohmann::json& data) {
    try {
        Trade trade;
        trade.exchange = "binance";
        trade.symbol = data.value("s", "");
        trade.trade_id = std::to_string(data.value("t", uint64_t(0)));
        trade.price = std::stod(data.value("p", "0"));
        trade.quantity = std::stod(data.value("q", "0"));
        trade.timestamp = data.value("T", uint64_t(0));
        trade.local_timestamp = currentTimeNs();
        trade.side = data.value("m", false) ? OrderSide::Sell : OrderSide::Buy;
        trade.is_maker = data.value("m", false);

        onTrade(trade);

    } catch (const std::exception& e) {
        onError("Trade update error: " + std::string(e.what()));
    }
}

void BinanceExchange::handleTickerUpdate(const nlohmann::json& data) {
    try {
        Ticker ticker;
        ticker.exchange = "binance";
        ticker.symbol = data.value("s", "");

        // Different fields for different ticker types
        if (data.contains("b")) {  // Book ticker
            ticker.bid = std::stod(data.value("b", "0"));
            ticker.ask = std::stod(data.value("a", "0"));
            ticker.bid_qty = std::stod(data.value("B", "0"));
            ticker.ask_qty = std::stod(data.value("A", "0"));
        } else {  // 24hr ticker
            ticker.bid = std::stod(data.value("b", "0"));
            ticker.ask = std::stod(data.value("a", "0"));
            ticker.last = std::stod(data.value("c", "0"));
            ticker.volume_24h = std::stod(data.value("v", "0"));
            ticker.volume_quote_24h = std::stod(data.value("q", "0"));
            ticker.high_24h = std::stod(data.value("h", "0"));
            ticker.low_24h = std::stod(data.value("l", "0"));
            ticker.change_24h = std::stod(data.value("p", "0"));
            ticker.change_24h_pct = std::stod(data.value("P", "0"));
        }

        ticker.timestamp = data.value("E", uint64_t(0));
        onTicker(ticker);

    } catch (const std::exception& e) {
        onError("Ticker update error: " + std::string(e.what()));
    }
}

void BinanceExchange::handleOrderUpdate(const nlohmann::json& data) {
    try {
        Order order;
        order.exchange = "binance";

        if (binance_config_.spot) {
            // Spot order update
            order.symbol = data.value("s", "");
            order.order_id = std::to_string(data.value("i", uint64_t(0)));
            order.client_order_id = data.value("c", "");
            order.side = parseOrderSide(data.value("S", "BUY"));
            order.type = parseOrderType(data.value("o", "LIMIT"));
            order.status = parseOrderStatus(data.value("X", "NEW"));
            order.time_in_force = parseTimeInForce(data.value("f", "GTC"));
            order.quantity = std::stod(data.value("q", "0"));
            order.filled_quantity = std::stod(data.value("z", "0"));
            order.price = std::stod(data.value("p", "0"));
            order.stop_price = std::stod(data.value("P", "0"));
            order.average_price = std::stod(data.value("L", "0"));
            order.commission = std::stod(data.value("n", "0"));
            order.commission_asset = data.value("N", "");
            order.create_time = data.value("O", uint64_t(0));
            order.update_time = data.value("T", uint64_t(0));
        } else {
            // Futures order update (ORDER_TRADE_UPDATE)
            auto o = data["o"];
            order.symbol = o.value("s", "");
            order.order_id = std::to_string(o.value("i", uint64_t(0)));
            order.client_order_id = o.value("c", "");
            order.side = parseOrderSide(o.value("S", "BUY"));
            order.type = parseOrderType(o.value("o", "LIMIT"));
            order.status = parseOrderStatus(o.value("X", "NEW"));
            order.time_in_force = parseTimeInForce(o.value("f", "GTC"));
            order.quantity = std::stod(o.value("q", "0"));
            order.filled_quantity = std::stod(o.value("z", "0"));
            order.price = std::stod(o.value("p", "0"));
            order.average_price = std::stod(o.value("ap", "0"));
            order.stop_price = std::stod(o.value("sp", "0"));
            order.commission = std::stod(o.value("n", "0"));
            order.commission_asset = o.value("N", "");
            order.reduce_only = o.value("R", false);
            order.update_time = data.value("T", uint64_t(0));
        }

        order.remaining_quantity = order.quantity - order.filled_quantity;
        order.raw = data;

        onOrder(order);

    } catch (const std::exception& e) {
        onError("Order update error: " + std::string(e.what()));
    }
}

void BinanceExchange::handleAccountUpdate(const nlohmann::json& data) {
    try {
        if (binance_config_.spot) {
            // Spot account update
            auto balances = data.value("B", nlohmann::json::array());
            for (const auto& b : balances) {
                Balance balance;
                balance.asset = b.value("a", "");
                balance.free = std::stod(b.value("f", "0"));
                balance.locked = std::stod(b.value("l", "0"));
                onBalance(balance);
            }
        } else {
            // Futures account update
            auto account = data["a"];

            // Balances
            auto balances = account.value("B", nlohmann::json::array());
            for (const auto& b : balances) {
                Balance balance;
                balance.asset = b.value("a", "");
                balance.free = std::stod(b.value("wb", "0"));  // Wallet balance
                balance.locked = std::stod(b.value("cw", "0")) -
                                std::stod(b.value("wb", "0"));  // Cross wallet - wallet
                onBalance(balance);
            }

            // Positions
            auto positions = account.value("P", nlohmann::json::array());
            for (const auto& p : positions) {
                Position position;
                position.exchange = "binance";
                position.symbol = p.value("s", "");
                double qty = std::stod(p.value("pa", "0"));
                position.side = qty >= 0 ? OrderSide::Buy : OrderSide::Sell;
                position.quantity = std::abs(qty);
                position.entry_price = std::stod(p.value("ep", "0"));
                position.unrealized_pnl = std::stod(p.value("up", "0"));
                position.update_time = data.value("T", uint64_t(0));
                onPosition(position);
            }
        }

    } catch (const std::exception& e) {
        onError("Account update error: " + std::string(e.what()));
    }
}

void BinanceExchange::handlePositionUpdate(const nlohmann::json& data) {
    // Handle futures position updates (leverage changes, etc.)
}

// ============================================================================
// Subscription Management
// ============================================================================

bool BinanceExchange::subscribeOrderBook(const std::string& symbol, int depth) {
    std::string lower_symbol = symbol;
    std::transform(lower_symbol.begin(), lower_symbol.end(),
                   lower_symbol.begin(), ::tolower);

    std::string stream = lower_symbol + "@depth" +
                        std::to_string(depth) + "@" +
                        std::to_string(binance_config_.orderbook_update_speed) + "ms";

    sendSubscribe({stream});

    // Initialize orderbook from REST snapshot
    initializeOrderBook(symbol, depth);

    return true;
}

bool BinanceExchange::unsubscribeOrderBook(const std::string& symbol) {
    std::string lower_symbol = symbol;
    std::transform(lower_symbol.begin(), lower_symbol.end(),
                   lower_symbol.begin(), ::tolower);

    std::string stream = lower_symbol + "@depth";

    // Find and unsubscribe any matching streams
    std::vector<std::string> to_unsub;
    {
        std::lock_guard<std::mutex> lock(subscription_mutex_);
        for (const auto& [name, sub] : subscriptions_) {
            if (name.find(stream) == 0) {
                to_unsub.push_back(name);
            }
        }
    }

    if (!to_unsub.empty()) {
        sendUnsubscribe(to_unsub);
    }

    clearOrderBookCache(symbol);
    return true;
}

bool BinanceExchange::subscribeTrades(const std::string& symbol) {
    std::string lower_symbol = symbol;
    std::transform(lower_symbol.begin(), lower_symbol.end(),
                   lower_symbol.begin(), ::tolower);

    std::string stream = lower_symbol + "@trade";
    sendSubscribe({stream});
    return true;
}

bool BinanceExchange::unsubscribeTrades(const std::string& symbol) {
    std::string lower_symbol = symbol;
    std::transform(lower_symbol.begin(), lower_symbol.end(),
                   lower_symbol.begin(), ::tolower);

    std::string stream = lower_symbol + "@trade";
    sendUnsubscribe({stream});
    return true;
}

bool BinanceExchange::subscribeTicker(const std::string& symbol) {
    std::string lower_symbol = symbol;
    std::transform(lower_symbol.begin(), lower_symbol.end(),
                   lower_symbol.begin(), ::tolower);

    std::string stream = lower_symbol + "@bookTicker";
    sendSubscribe({stream});
    return true;
}

bool BinanceExchange::unsubscribeTicker(const std::string& symbol) {
    std::string lower_symbol = symbol;
    std::transform(lower_symbol.begin(), lower_symbol.end(),
                   lower_symbol.begin(), ::tolower);

    std::string stream = lower_symbol + "@bookTicker";
    sendUnsubscribe({stream});
    return true;
}

bool BinanceExchange::subscribeUserData() {
    // Get listen key
    listen_key_ = getListenKey();
    if (listen_key_.empty()) {
        onError("Failed to get listen key");
        return false;
    }

    // Connect to user data stream
    network::WebSocketConfig ws_config;
    ws_config.url = getWsUrl() + "/" + listen_key_;
    ws_config.name = "binance_user_ws";
    ws_config.ping_interval_ms = 180000;
    ws_config.auto_reconnect = true;

    user_ws_client_ = std::make_shared<network::WebSocketClient>(ws_config);

    user_ws_client_->setOnOpen([this]() {
        user_ws_connected_ = true;
    });

    user_ws_client_->setOnClose([this](int code, const std::string& reason) {
        user_ws_connected_ = false;
    });

    user_ws_client_->setOnMessage([this](const std::string& msg, network::MessageType type) {
        this->handleWsMessage(msg, type);
    });

    user_ws_client_->setOnError([this](const std::string& error) {
        this->handleWsError(error);
    });

    bool connected = user_ws_client_->connect();

    if (connected) {
        startListenKeyRefresh();
    }

    return connected;
}

bool BinanceExchange::unsubscribeUserData() {
    stopListenKeyRefresh();

    if (user_ws_client_) {
        user_ws_client_->disconnect();
        user_ws_client_.reset();
    }

    user_ws_connected_ = false;
    return true;
}

void BinanceExchange::sendSubscribe(const std::vector<std::string>& streams) {
    if (!ws_client_ || !ws_connected_.load()) {
        return;
    }

    nlohmann::json msg;
    msg["method"] = "SUBSCRIBE";
    msg["params"] = streams;
    msg["id"] = next_request_id_++;

    ws_client_->send(msg);

    // Track subscriptions
    std::lock_guard<std::mutex> lock(subscription_mutex_);
    for (const auto& stream : streams) {
        StreamSubscription sub;
        sub.stream_name = stream;
        sub.request_id = msg["id"];
        sub.active = true;
        subscriptions_[stream] = sub;
    }
}

void BinanceExchange::sendUnsubscribe(const std::vector<std::string>& streams) {
    if (!ws_client_ || !ws_connected_.load()) {
        return;
    }

    nlohmann::json msg;
    msg["method"] = "UNSUBSCRIBE";
    msg["params"] = streams;
    msg["id"] = next_request_id_++;

    ws_client_->send(msg);

    // Remove subscriptions
    std::lock_guard<std::mutex> lock(subscription_mutex_);
    for (const auto& stream : streams) {
        subscriptions_.erase(stream);
    }
}

// ============================================================================
// Listen Key Management
// ============================================================================

std::string BinanceExchange::getListenKey() {
    std::string endpoint = binance_config_.spot ? "/userDataStream" : "/listenKey";
    auto response = rest_client_->post(endpoint, "", true);

    if (response.success) {
        auto json = response.json();
        return json.value("listenKey", "");
    }

    return "";
}

bool BinanceExchange::keepAliveListenKey() {
    if (listen_key_.empty()) {
        return false;
    }

    std::string endpoint = binance_config_.spot ? "/userDataStream" : "/listenKey";
    std::unordered_map<std::string, std::string> params;
    params["listenKey"] = listen_key_;

    auto response = rest_client_->put(endpoint, "", true);
    return response.success;
}

void BinanceExchange::startListenKeyRefresh() {
    if (refresh_running_.load()) {
        return;
    }

    refresh_running_ = true;
    listen_key_refresh_thread_ = std::make_unique<std::thread>([this]() {
        while (refresh_running_.load()) {
            // Refresh every 30 minutes (listen key expires in 60 minutes)
            std::this_thread::sleep_for(std::chrono::minutes(30));

            if (!refresh_running_.load()) {
                break;
            }

            if (!keepAliveListenKey()) {
                onError("Failed to refresh listen key");
                // Try to get new listen key
                listen_key_ = getListenKey();
            }
        }
    });
}

void BinanceExchange::stopListenKeyRefresh() {
    refresh_running_ = false;

    if (listen_key_refresh_thread_ && listen_key_refresh_thread_->joinable()) {
        listen_key_refresh_thread_->join();
    }
    listen_key_refresh_thread_.reset();
}

// ============================================================================
// Orderbook Management
// ============================================================================

void BinanceExchange::initializeOrderBook(const std::string& symbol, int depth) {
    auto snapshot = getOrderBookSnapshot(symbol, depth);
    if (!snapshot) {
        onError("Failed to get orderbook snapshot for " + symbol);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(orderbook_init_mutex_);
        orderbook_initialized_[symbol] = true;
        last_update_id_[symbol] = snapshot->sequence;
    }

    onOrderBook(*snapshot);
}

void BinanceExchange::applyOrderBookDelta(const std::string& symbol,
                                          const nlohmann::json& data) {
    auto cached = getCachedOrderBook(symbol);
    if (!cached) {
        return;
    }

    OrderBook ob = *cached;
    ob.timestamp = data.value("E", uint64_t(0));
    ob.local_timestamp = currentTimeNs();
    ob.sequence = data.value("u", uint64_t(0));

    // Helper to apply updates
    auto applyUpdates = [](std::vector<PriceLevel>& levels,
                          const nlohmann::json& updates,
                          bool ascending) {
        for (const auto& update : updates) {
            double price = std::stod(update[0].get<std::string>());
            double qty = std::stod(update[1].get<std::string>());

            // Find existing level
            auto it = std::find_if(levels.begin(), levels.end(),
                [price](const PriceLevel& l) { return l.price == price; });

            if (qty == 0.0) {
                // Remove level
                if (it != levels.end()) {
                    levels.erase(it);
                }
            } else if (it != levels.end()) {
                // Update existing
                it->quantity = qty;
            } else {
                // Insert new level in sorted order
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

    // Apply bid updates (descending order)
    if (data.contains("b")) {
        applyUpdates(ob.bids, data["b"], false);
    }

    // Apply ask updates (ascending order)
    if (data.contains("a")) {
        applyUpdates(ob.asks, data["a"], true);
    }

    onOrderBook(ob);
}

// ============================================================================
// Signature Generation
// ============================================================================

std::string BinanceExchange::sign(const std::string& query_string) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;

    HMAC(EVP_sha256(),
         binance_config_.api_secret.c_str(),
         static_cast<int>(binance_config_.api_secret.length()),
         reinterpret_cast<const unsigned char*>(query_string.c_str()),
         query_string.length(),
         hash, &hash_len);

    std::stringstream ss;
    for (unsigned int i = 0; i < hash_len; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<int>(hash[i]);
    }

    return ss.str();
}

std::string BinanceExchange::buildSignedQuery(
    const std::unordered_map<std::string, std::string>& params) {

    std::stringstream ss;
    bool first = true;

    for (const auto& [key, value] : params) {
        if (!first) ss << "&";
        first = false;
        ss << key << "=" << value;
    }

    // Add timestamp
    ss << "&timestamp=" << currentTimeMs();
    ss << "&recvWindow=" << binance_config_.recv_window;

    std::string query = ss.str();
    std::string signature = sign(query);

    return query + "&signature=" + signature;
}

// ============================================================================
// REST Helpers
// ============================================================================

network::HttpResponse BinanceExchange::signedGet(
    const std::string& endpoint,
    const std::unordered_map<std::string, std::string>& params) {

    std::string query = buildSignedQuery(params);
    return rest_client_->get(endpoint + "?" + query);
}

network::HttpResponse BinanceExchange::signedPost(
    const std::string& endpoint,
    const std::unordered_map<std::string, std::string>& params) {

    std::string query = buildSignedQuery(params);
    return rest_client_->post(endpoint + "?" + query);
}

network::HttpResponse BinanceExchange::signedDelete(
    const std::string& endpoint,
    const std::unordered_map<std::string, std::string>& params) {

    std::string query = buildSignedQuery(params);
    return rest_client_->del(endpoint + "?" + query);
}

network::HttpResponse BinanceExchange::publicGet(
    const std::string& endpoint,
    const std::unordered_map<std::string, std::string>& params) {

    return rest_client_->get(endpoint, params);
}

// ============================================================================
// Order Management
// ============================================================================

std::optional<Order> BinanceExchange::placeOrder(const OrderRequest& request) {
    std::unordered_map<std::string, std::string> params;
    params["symbol"] = request.symbol;
    params["side"] = orderSideToString(request.side);
    params["type"] = orderTypeToString(request.type);
    params["quantity"] = std::to_string(request.quantity);

    if (request.type != OrderType::Market) {
        params["timeInForce"] = timeInForceToString(request.time_in_force);
        params["price"] = std::to_string(request.price);
    }

    if (request.stop_price > 0.0) {
        params["stopPrice"] = std::to_string(request.stop_price);
    }

    if (!request.client_order_id.empty()) {
        params["newClientOrderId"] = request.client_order_id;
    }

    if (request.reduce_only && !binance_config_.spot) {
        params["reduceOnly"] = "true";
    }

    std::string endpoint = binance_config_.spot ? "/order" : "/order";
    auto response = signedPost(endpoint, params);

    if (!response.success) {
        onError("Place order failed: " + response.error + " " + response.body);
        return std::nullopt;
    }

    try {
        auto json = response.json();
        return parseOrder(json);
    } catch (const std::exception& e) {
        onError("Parse order response failed: " + std::string(e.what()));
        return std::nullopt;
    }
}

std::optional<Order> BinanceExchange::cancelOrder(const std::string& symbol,
                                                  const std::string& order_id) {
    std::unordered_map<std::string, std::string> params;
    params["symbol"] = symbol;
    params["orderId"] = order_id;

    std::string endpoint = binance_config_.spot ? "/order" : "/order";
    auto response = signedDelete(endpoint, params);

    if (!response.success) {
        onError("Cancel order failed: " + response.error + " " + response.body);
        return std::nullopt;
    }

    try {
        auto json = response.json();
        return parseOrder(json);
    } catch (const std::exception& e) {
        onError("Parse cancel response failed: " + std::string(e.what()));
        return std::nullopt;
    }
}

std::optional<Order> BinanceExchange::cancelOrder(const std::string& symbol,
                                                  const std::string& order_id,
                                                  const std::string& client_order_id) {
    std::unordered_map<std::string, std::string> params;
    params["symbol"] = symbol;

    if (!order_id.empty()) {
        params["orderId"] = order_id;
    }
    if (!client_order_id.empty()) {
        params["origClientOrderId"] = client_order_id;
    }

    std::string endpoint = binance_config_.spot ? "/order" : "/order";
    auto response = signedDelete(endpoint, params);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        return parseOrder(json);
    } catch (...) {
        return std::nullopt;
    }
}

bool BinanceExchange::cancelAllOrders(const std::string& symbol) {
    std::unordered_map<std::string, std::string> params;
    params["symbol"] = symbol;

    std::string endpoint = binance_config_.spot ? "/openOrders" : "/allOpenOrders";
    auto response = signedDelete(endpoint, params);

    return response.success;
}

std::optional<Order> BinanceExchange::getOrder(const std::string& symbol,
                                               const std::string& order_id) {
    std::unordered_map<std::string, std::string> params;
    params["symbol"] = symbol;
    params["orderId"] = order_id;

    std::string endpoint = binance_config_.spot ? "/order" : "/order";
    auto response = signedGet(endpoint, params);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        return parseOrder(json);
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<Order> BinanceExchange::getOpenOrders(const std::string& symbol) {
    std::unordered_map<std::string, std::string> params;
    if (!symbol.empty()) {
        params["symbol"] = symbol;
    }

    std::string endpoint = binance_config_.spot ? "/openOrders" : "/openOrders";
    auto response = signedGet(endpoint, params);

    std::vector<Order> orders;
    if (!response.success) {
        return orders;
    }

    try {
        auto json = response.json();
        for (const auto& item : json) {
            orders.push_back(parseOrder(item));
        }
    } catch (...) {
        // Return empty on parse error
    }

    return orders;
}

std::vector<Order> BinanceExchange::getOrderHistory(const std::string& symbol,
                                                    uint64_t start_time,
                                                    uint64_t end_time,
                                                    int limit) {
    std::unordered_map<std::string, std::string> params;
    params["symbol"] = symbol;
    params["limit"] = std::to_string(limit);

    if (start_time > 0) {
        params["startTime"] = std::to_string(start_time);
    }
    if (end_time > 0) {
        params["endTime"] = std::to_string(end_time);
    }

    std::string endpoint = binance_config_.spot ? "/allOrders" : "/allOrders";
    auto response = signedGet(endpoint, params);

    std::vector<Order> orders;
    if (!response.success) {
        return orders;
    }

    try {
        auto json = response.json();
        for (const auto& item : json) {
            orders.push_back(parseOrder(item));
        }
    } catch (...) {
        // Return empty on parse error
    }

    return orders;
}

// ============================================================================
// Account Information
// ============================================================================

std::optional<Account> BinanceExchange::getAccount() {
    std::string endpoint = binance_config_.spot ? "/account" : "/account";
    auto response = signedGet(endpoint);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        Account account;
        account.exchange = "binance";
        account.update_time = json.value("updateTime", uint64_t(0));

        if (binance_config_.spot) {
            auto balances = json.value("balances", nlohmann::json::array());
            for (const auto& b : balances) {
                Balance balance;
                balance.asset = b.value("asset", "");
                balance.free = std::stod(b.value("free", "0"));
                balance.locked = std::stod(b.value("locked", "0"));

                if (balance.free > 0 || balance.locked > 0) {
                    account.balances[balance.asset] = balance;
                }
            }
        } else {
            auto assets = json.value("assets", nlohmann::json::array());
            for (const auto& a : assets) {
                Balance balance;
                balance.asset = a.value("asset", "");
                balance.free = std::stod(a.value("availableBalance", "0"));
                balance.locked = std::stod(a.value("initialMargin", "0"));
                account.balances[balance.asset] = balance;
            }

            account.total_margin = std::stod(json.value("totalMarginBalance", "0"));
            account.available_margin = std::stod(json.value("availableBalance", "0"));
            account.total_unrealized_pnl = std::stod(json.value("totalUnrealizedProfit", "0"));

            // Parse positions
            auto positions = json.value("positions", nlohmann::json::array());
            for (const auto& p : positions) {
                double qty = std::stod(p.value("positionAmt", "0"));
                if (qty == 0.0) continue;

                Position position;
                position.exchange = "binance";
                position.symbol = p.value("symbol", "");
                position.side = qty >= 0 ? OrderSide::Buy : OrderSide::Sell;
                position.quantity = std::abs(qty);
                position.entry_price = std::stod(p.value("entryPrice", "0"));
                position.unrealized_pnl = std::stod(p.value("unrealizedProfit", "0"));
                position.leverage = std::stod(p.value("leverage", "1"));
                account.positions.push_back(position);
            }
        }

        return account;

    } catch (const std::exception& e) {
        onError("Parse account failed: " + std::string(e.what()));
        return std::nullopt;
    }
}

std::optional<Balance> BinanceExchange::getBalance(const std::string& asset) {
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

std::vector<Position> BinanceExchange::getPositions(const std::string& symbol) {
    if (binance_config_.spot) {
        return {};  // No positions for spot
    }

    auto account = getAccount();
    if (!account) {
        return {};
    }

    if (symbol.empty()) {
        return account->positions;
    }

    std::vector<Position> filtered;
    for (const auto& pos : account->positions) {
        if (pos.symbol == symbol) {
            filtered.push_back(pos);
        }
    }

    return filtered;
}

// ============================================================================
// Market Information
// ============================================================================

std::vector<SymbolInfo> BinanceExchange::getSymbols() {
    std::string endpoint = binance_config_.spot ? "/exchangeInfo" : "/exchangeInfo";
    auto response = publicGet(endpoint);

    std::vector<SymbolInfo> symbols;
    if (!response.success) {
        return symbols;
    }

    try {
        auto json = response.json();
        auto symbol_list = json.value("symbols", nlohmann::json::array());

        for (const auto& s : symbol_list) {
            auto info = parseSymbolInfo(s);
            updateSymbolInfo(info);
            symbols.push_back(info);
        }
    } catch (...) {
        // Return empty on error
    }

    return symbols;
}

std::optional<SymbolInfo> BinanceExchange::getSymbolInfo(const std::string& symbol) {
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

std::optional<OrderBook> BinanceExchange::getOrderBookSnapshot(const std::string& symbol,
                                                               int depth) {
    std::unordered_map<std::string, std::string> params;
    params["symbol"] = symbol;
    params["limit"] = std::to_string(depth);

    std::string endpoint = binance_config_.spot ? "/depth" : "/depth";
    auto response = publicGet(endpoint, params);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();

        OrderBook ob;
        ob.symbol = symbol;
        ob.exchange = "binance";
        ob.sequence = json.value("lastUpdateId", uint64_t(0));
        ob.timestamp = currentTimeMs();
        ob.local_timestamp = currentTimeNs();

        // Parse bids
        auto bids = json.value("bids", nlohmann::json::array());
        for (const auto& b : bids) {
            PriceLevel level;
            level.price = std::stod(b[0].get<std::string>());
            level.quantity = std::stod(b[1].get<std::string>());
            ob.bids.push_back(level);
        }

        // Parse asks
        auto asks = json.value("asks", nlohmann::json::array());
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

std::vector<Trade> BinanceExchange::getRecentTrades(const std::string& symbol, int limit) {
    std::unordered_map<std::string, std::string> params;
    params["symbol"] = symbol;
    params["limit"] = std::to_string(limit);

    std::string endpoint = binance_config_.spot ? "/trades" : "/trades";
    auto response = publicGet(endpoint, params);

    std::vector<Trade> trades;
    if (!response.success) {
        return trades;
    }

    try {
        auto json = response.json();
        for (const auto& t : json) {
            Trade trade;
            trade.exchange = "binance";
            trade.symbol = symbol;
            trade.trade_id = std::to_string(t.value("id", uint64_t(0)));
            trade.price = std::stod(t.value("price", "0"));
            trade.quantity = std::stod(t.value("qty", "0"));
            trade.timestamp = t.value("time", uint64_t(0));
            trade.is_maker = t.value("isBuyerMaker", false);
            trade.side = trade.is_maker ? OrderSide::Sell : OrderSide::Buy;
            trades.push_back(trade);
        }
    } catch (...) {
        // Return empty on error
    }

    return trades;
}

std::optional<Ticker> BinanceExchange::getTicker(const std::string& symbol) {
    std::unordered_map<std::string, std::string> params;
    params["symbol"] = symbol;

    std::string endpoint = binance_config_.spot ? "/ticker/24hr" : "/ticker/24hr";
    auto response = publicGet(endpoint, params);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();

        Ticker ticker;
        ticker.exchange = "binance";
        ticker.symbol = symbol;
        ticker.last = std::stod(json.value("lastPrice", "0"));
        ticker.bid = std::stod(json.value("bidPrice", "0"));
        ticker.ask = std::stod(json.value("askPrice", "0"));
        ticker.bid_qty = std::stod(json.value("bidQty", "0"));
        ticker.ask_qty = std::stod(json.value("askQty", "0"));
        ticker.high_24h = std::stod(json.value("highPrice", "0"));
        ticker.low_24h = std::stod(json.value("lowPrice", "0"));
        ticker.volume_24h = std::stod(json.value("volume", "0"));
        ticker.volume_quote_24h = std::stod(json.value("quoteVolume", "0"));
        ticker.change_24h = std::stod(json.value("priceChange", "0"));
        ticker.change_24h_pct = std::stod(json.value("priceChangePercent", "0"));

        return ticker;

    } catch (...) {
        return std::nullopt;
    }
}

uint64_t BinanceExchange::getServerTime() {
    std::string endpoint = binance_config_.spot ? "/time" : "/time";
    auto response = publicGet(endpoint);

    if (response.success) {
        auto json = response.json();
        return json.value("serverTime", uint64_t(0));
    }

    return 0;
}

std::string BinanceExchange::formatSymbol(const std::string& base,
                                          const std::string& quote) const {
    return base + quote;
}

std::pair<std::string, std::string> BinanceExchange::parseSymbol(
    const std::string& symbol) const {
    // This is simplified; actual implementation should use exchange info
    // Common patterns: BTCUSDT -> BTC, USDT
    static const std::vector<std::string> quote_assets = {
        "USDT", "BUSD", "USDC", "BTC", "ETH", "BNB"
    };

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

Order BinanceExchange::parseOrder(const nlohmann::json& data) {
    Order order;
    order.exchange = "binance";
    order.symbol = data.value("symbol", "");
    order.order_id = std::to_string(data.value("orderId", uint64_t(0)));
    order.client_order_id = data.value("clientOrderId", "");
    order.side = parseOrderSide(data.value("side", "BUY"));
    order.type = parseOrderType(data.value("type", "LIMIT"));
    order.status = parseOrderStatus(data.value("status", "NEW"));
    order.time_in_force = parseTimeInForce(data.value("timeInForce", "GTC"));

    order.quantity = std::stod(data.value("origQty", "0"));
    order.filled_quantity = std::stod(data.value("executedQty", "0"));
    order.remaining_quantity = order.quantity - order.filled_quantity;
    order.price = std::stod(data.value("price", "0"));
    order.stop_price = std::stod(data.value("stopPrice", "0"));

    // Average price calculation
    double cumulative_quote = std::stod(data.value("cummulativeQuoteQty", "0"));
    if (order.filled_quantity > 0) {
        order.average_price = cumulative_quote / order.filled_quantity;
    }

    order.create_time = data.value("time", uint64_t(0));
    order.update_time = data.value("updateTime", uint64_t(0));
    order.reduce_only = data.value("reduceOnly", false);
    order.raw = data;

    return order;
}

OrderStatus BinanceExchange::parseOrderStatus(const std::string& status) {
    if (status == "NEW") return OrderStatus::New;
    if (status == "PARTIALLY_FILLED") return OrderStatus::PartiallyFilled;
    if (status == "FILLED") return OrderStatus::Filled;
    if (status == "CANCELED") return OrderStatus::Cancelled;
    if (status == "REJECTED") return OrderStatus::Rejected;
    if (status == "EXPIRED") return OrderStatus::Expired;
    if (status == "PENDING_CANCEL") return OrderStatus::PendingCancel;
    return OrderStatus::Failed;
}

OrderSide BinanceExchange::parseOrderSide(const std::string& side) {
    return (side == "BUY" || side == "buy") ? OrderSide::Buy : OrderSide::Sell;
}

OrderType BinanceExchange::parseOrderType(const std::string& type) {
    if (type == "MARKET") return OrderType::Market;
    if (type == "LIMIT") return OrderType::Limit;
    if (type == "STOP_MARKET") return OrderType::StopMarket;
    if (type == "STOP_LIMIT" || type == "STOP") return OrderType::StopLimit;
    if (type == "TAKE_PROFIT_MARKET") return OrderType::TakeProfitMarket;
    if (type == "TAKE_PROFIT" || type == "TAKE_PROFIT_LIMIT") return OrderType::TakeProfitLimit;
    if (type == "TRAILING_STOP_MARKET") return OrderType::TrailingStop;
    return OrderType::Limit;
}

TimeInForce BinanceExchange::parseTimeInForce(const std::string& tif) {
    if (tif == "GTC") return TimeInForce::GTC;
    if (tif == "IOC") return TimeInForce::IOC;
    if (tif == "FOK") return TimeInForce::FOK;
    if (tif == "GTX") return TimeInForce::PostOnly;
    return TimeInForce::GTC;
}

std::string BinanceExchange::orderSideToString(OrderSide side) {
    return side == OrderSide::Buy ? "BUY" : "SELL";
}

std::string BinanceExchange::orderTypeToString(OrderType type) {
    switch (type) {
        case OrderType::Market: return "MARKET";
        case OrderType::Limit: return "LIMIT";
        case OrderType::StopMarket: return "STOP_MARKET";
        case OrderType::StopLimit: return "STOP";
        case OrderType::TakeProfitMarket: return "TAKE_PROFIT_MARKET";
        case OrderType::TakeProfitLimit: return "TAKE_PROFIT";
        case OrderType::TrailingStop: return "TRAILING_STOP_MARKET";
        case OrderType::PostOnly: return "LIMIT";
        default: return "LIMIT";
    }
}

std::string BinanceExchange::timeInForceToString(TimeInForce tif) {
    switch (tif) {
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
        case TimeInForce::PostOnly: return "GTX";
        default: return "GTC";
    }
}

SymbolInfo BinanceExchange::parseSymbolInfo(const nlohmann::json& data) {
    SymbolInfo info;
    info.symbol = data.value("symbol", "");
    info.base_asset = data.value("baseAsset", "");
    info.quote_asset = data.value("quoteAsset", "");
    info.trading_enabled = data.value("status", "") == "TRADING";

    // Parse filters
    auto filters = data.value("filters", nlohmann::json::array());
    for (const auto& f : filters) {
        std::string filter_type = f.value("filterType", "");

        if (filter_type == "PRICE_FILTER") {
            info.min_price = std::stod(f.value("minPrice", "0"));
            info.max_price = std::stod(f.value("maxPrice", "0"));
            info.tick_size = std::stod(f.value("tickSize", "0"));
        } else if (filter_type == "LOT_SIZE") {
            info.min_qty = std::stod(f.value("minQty", "0"));
            info.max_qty = std::stod(f.value("maxQty", "0"));
            info.step_size = std::stod(f.value("stepSize", "0"));
        } else if (filter_type == "MIN_NOTIONAL" || filter_type == "NOTIONAL") {
            info.min_notional = std::stod(f.value("minNotional", "0"));
        }
    }

    // Calculate precision from step/tick size
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
// Binance-specific Methods
// ============================================================================

bool BinanceExchange::setLeverage(const std::string& symbol, int leverage) {
    if (binance_config_.spot) {
        return false;  // Not applicable for spot
    }

    std::unordered_map<std::string, std::string> params;
    params["symbol"] = symbol;
    params["leverage"] = std::to_string(leverage);

    auto response = signedPost("/leverage", params);
    return response.success;
}

bool BinanceExchange::setMarginType(const std::string& symbol, bool isolated) {
    if (binance_config_.spot) {
        return false;  // Not applicable for spot
    }

    std::unordered_map<std::string, std::string> params;
    params["symbol"] = symbol;
    params["marginType"] = isolated ? "ISOLATED" : "CROSSED";

    auto response = signedPost("/marginType", params);
    return response.success;
}

double BinanceExchange::getFundingRate(const std::string& symbol) {
    if (binance_config_.spot) {
        return 0.0;  // Not applicable for spot
    }

    std::unordered_map<std::string, std::string> params;
    params["symbol"] = symbol;

    auto response = publicGet("/fundingRate", params);

    if (response.success) {
        auto json = response.json();
        if (json.is_array() && !json.empty()) {
            return std::stod(json[0].value("fundingRate", "0"));
        }
    }

    return 0.0;
}

// ============================================================================
// Factory Functions
// ============================================================================

std::shared_ptr<BinanceExchange> createBinanceExchange(const ExchangeConfig& config) {
    return std::make_shared<BinanceExchange>(config);
}

std::shared_ptr<BinanceExchange> createBinanceExchange(const BinanceConfig& config) {
    return std::make_shared<BinanceExchange>(config);
}

} // namespace exchange
} // namespace hft
