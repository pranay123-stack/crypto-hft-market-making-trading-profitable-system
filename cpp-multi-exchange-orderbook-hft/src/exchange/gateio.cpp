#include "exchange/gateio.hpp"

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

GateIOExchange::GateIOExchange(const ExchangeConfig& config)
    : ExchangeBase(config) {
    gateio_config_.api_key = config.api_key;
    gateio_config_.api_secret = config.api_secret;
    gateio_config_.testnet = config.testnet;
    gateio_config_.spot = (config.type == ExchangeType::Spot);
    gateio_config_.order_rate_limit = config.orders_per_second;
    gateio_config_.request_rate_limit = config.requests_per_minute;

    // Initialize REST client
    network::RestClientConfig rest_config;
    rest_config.base_url = getRestUrl();
    rest_config.name = "gateio_rest";
    rest_config.request_timeout_ms = config.rest_timeout_ms;
    rest_config.max_retries = config.rest_max_retries;
    rest_config.rate_limit.requests_per_second = gateio_config_.order_rate_limit;
    rest_config.rate_limit.requests_per_minute = gateio_config_.request_rate_limit;

    rest_client_ = std::make_shared<network::RestClient>(rest_config);
}

GateIOExchange::GateIOExchange(const GateIOConfig& gateio_config)
    : ExchangeBase(ExchangeConfig{
        .name = "GateIO",
        .api_key = gateio_config.api_key,
        .api_secret = gateio_config.api_secret,
        .type = gateio_config.spot ? ExchangeType::Spot : ExchangeType::Perpetual,
        .testnet = gateio_config.testnet
      }),
      gateio_config_(gateio_config) {

    // Initialize REST client
    network::RestClientConfig rest_config;
    rest_config.base_url = getRestUrl();
    rest_config.name = "gateio_rest";
    rest_config.rate_limit.requests_per_second = gateio_config_.order_rate_limit;
    rest_config.rate_limit.requests_per_minute = gateio_config_.request_rate_limit;

    rest_client_ = std::make_shared<network::RestClient>(rest_config);
}

GateIOExchange::~GateIOExchange() {
    disconnect();
}

// ============================================================================
// URL Builders
// ============================================================================

std::string GateIOExchange::getSpotRestUrl() const {
    // Gate.io uses the same URL for testnet and production
    // Testnet is handled via different credentials
    return "https://api.gateio.ws/api/v4";
}

std::string GateIOExchange::getFuturesRestUrl() const {
    return "https://api.gateio.ws/api/v4";
}

std::string GateIOExchange::getSpotWsUrl() const {
    return "wss://api.gateio.ws/ws/v4/";
}

std::string GateIOExchange::getFuturesWsUrl() const {
    return "wss://fx-ws.gateio.ws/v4/ws/usdt";
}

std::string GateIOExchange::getRestUrl() const {
    return gateio_config_.spot ? getSpotRestUrl() : getFuturesRestUrl();
}

std::string GateIOExchange::getWsUrl() const {
    return gateio_config_.spot ? getSpotWsUrl() : getFuturesWsUrl();
}

// ============================================================================
// Connection Management
// ============================================================================

bool GateIOExchange::connect() {
    if (public_ws_connected_.load()) {
        return true;
    }

    onConnectionStatus(ConnectionStatus::Connecting);

    // Initialize public WebSocket client
    network::WebSocketConfig ws_config;
    ws_config.url = getWsUrl();
    ws_config.name = "gateio_public_ws";
    ws_config.ping_interval_ms = 15000;  // Gate.io recommends ping every 15s
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

void GateIOExchange::disconnect() {
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

bool GateIOExchange::isConnected() const {
    return public_ws_connected_.load();
}

ConnectionStatus GateIOExchange::connectionStatus() const {
    return connection_status_.load();
}

// ============================================================================
// WebSocket Handlers
// ============================================================================

void GateIOExchange::handleWsOpen(bool is_private) {
    if (is_private) {
        private_ws_connected_ = true;
        authenticatePrivateWs();
    } else {
        public_ws_connected_ = true;
        onConnectionStatus(ConnectionStatus::Connected);
    }
}

void GateIOExchange::handleWsClose(int code, const std::string& reason, bool is_private) {
    if (is_private) {
        private_ws_connected_ = false;
        private_ws_authenticated_ = false;
    } else {
        public_ws_connected_ = false;
        onConnectionStatus(ConnectionStatus::Reconnecting);
    }
}

void GateIOExchange::handleWsError(const std::string& error, bool is_private) {
    onError("WebSocket error (" + std::string(is_private ? "private" : "public") + "): " + error);
}

void GateIOExchange::handlePublicWsMessage(const std::string& message, network::MessageType type) {
    try {
        auto json = nlohmann::json::parse(message);

        // Check for subscription response or error
        if (json.contains("event")) {
            std::string event = json["event"].get<std::string>();
            if (event == "subscribe" || event == "unsubscribe") {
                // Subscription acknowledgment
                return;
            }
            if (event == "error") {
                onError("WebSocket error: " + json.value("message", "unknown"));
                return;
            }
        }

        // Handle data updates
        std::string channel = json.value("channel", "");
        auto result = json.value("result", nlohmann::json{});

        if (channel == "spot.order_book_update" || channel == "futures.order_book_update") {
            handleOrderBookDelta(json);
        } else if (channel == "spot.order_book" || channel == "futures.order_book") {
            handleOrderBookSnapshot(json);
        } else if (channel == "spot.trades" || channel == "futures.trades") {
            handleTradeUpdate(json);
        } else if (channel == "spot.tickers" || channel == "futures.tickers") {
            handleTickerUpdate(json);
        }

    } catch (const std::exception& e) {
        onError("Public WS message parse error: " + std::string(e.what()));
    }
}

void GateIOExchange::handlePrivateWsMessage(const std::string& message, network::MessageType type) {
    try {
        auto json = nlohmann::json::parse(message);

        // Check for auth response
        if (json.contains("event") && json["event"] == "auth") {
            std::string result = json.value("result", nlohmann::json{}).value("status", "");
            if (result == "success") {
                private_ws_authenticated_ = true;
            } else {
                onError("Private WS authentication failed");
            }
            return;
        }

        // Check for subscription response
        if (json.contains("event")) {
            std::string event = json["event"].get<std::string>();
            if (event == "subscribe" || event == "unsubscribe") {
                return;
            }
        }

        // Handle private data updates
        std::string channel = json.value("channel", "");
        auto result = json.value("result", nlohmann::json::array());

        if (channel == "spot.orders" || channel == "futures.orders") {
            for (const auto& data : result) {
                handleOrderUpdate(data);
            }
        } else if (channel == "spot.usertrades" || channel == "futures.usertrades") {
            // User trades are handled within order updates
        } else if (channel == "spot.balances" || channel == "futures.balances") {
            for (const auto& data : result) {
                handleBalanceUpdate(data);
            }
        } else if (channel == "futures.positions") {
            for (const auto& data : result) {
                handlePositionUpdate(data);
            }
        }

    } catch (const std::exception& e) {
        onError("Private WS message parse error: " + std::string(e.what()));
    }
}

void GateIOExchange::handleOrderBookSnapshot(const nlohmann::json& json) {
    try {
        auto result = json["result"];
        std::string symbol = result.value("s", "");  // Gate.io uses "s" for symbol
        if (symbol.empty()) {
            symbol = result.value("currency_pair", "");
        }

        OrderBook ob;
        ob.symbol = symbol;
        ob.exchange = "gateio";
        ob.timestamp = result.value("t", uint64_t(0));
        ob.local_timestamp = currentTimeNs();

        // Gate.io uses "id" or "u" for sequence
        if (result.contains("id")) {
            ob.sequence = result["id"].get<uint64_t>();
        } else if (result.contains("u")) {
            ob.sequence = result["u"].get<uint64_t>();
        }

        // Parse bids - Gate.io format: [["price", "quantity"], ...]
        auto bids = result.value("bids", nlohmann::json::array());
        for (const auto& b : bids) {
            PriceLevel level;
            if (b.is_array() && b.size() >= 2) {
                level.price = std::stod(b[0].get<std::string>());
                level.quantity = std::stod(b[1].get<std::string>());
            } else {
                level.price = std::stod(b.value("p", "0"));
                level.quantity = std::stod(b.value("s", "0"));
            }
            ob.bids.push_back(level);
        }

        // Parse asks
        auto asks = result.value("asks", nlohmann::json::array());
        for (const auto& a : asks) {
            PriceLevel level;
            if (a.is_array() && a.size() >= 2) {
                level.price = std::stod(a[0].get<std::string>());
                level.quantity = std::stod(a[1].get<std::string>());
            } else {
                level.price = std::stod(a.value("p", "0"));
                level.quantity = std::stod(a.value("s", "0"));
            }
            ob.asks.push_back(level);
        }

        // Update sequence
        {
            std::lock_guard<std::mutex> lock(orderbook_seq_mutex_);
            orderbook_seq_[symbol] = ob.sequence;
            orderbook_initialized_[symbol] = true;
        }

        onOrderBook(ob);

    } catch (const std::exception& e) {
        onError("OrderBook snapshot error: " + std::string(e.what()));
    }
}

void GateIOExchange::handleOrderBookDelta(const nlohmann::json& json) {
    try {
        auto result = json["result"];
        std::string symbol = result.value("s", "");
        if (symbol.empty()) {
            symbol = result.value("currency_pair", "");
        }

        uint64_t seq = 0;
        if (result.contains("u")) {
            seq = result["u"].get<uint64_t>();
        } else if (result.contains("U")) {
            seq = result["U"].get<uint64_t>();
        }

        // Check if we have the base snapshot
        {
            std::lock_guard<std::mutex> lock(orderbook_init_mutex_);
            auto it = orderbook_initialized_.find(symbol);
            if (it == orderbook_initialized_.end() || !it->second) {
                // Need snapshot first
                return;
            }
        }

        // Check sequence
        {
            std::lock_guard<std::mutex> lock(orderbook_seq_mutex_);
            auto it = orderbook_seq_.find(symbol);
            if (it != orderbook_seq_.end() && seq <= it->second) {
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
        ob.timestamp = result.value("t", uint64_t(0));
        ob.local_timestamp = currentTimeNs();

        // Apply delta updates
        auto applyUpdates = [](std::vector<PriceLevel>& levels,
                              const nlohmann::json& updates,
                              bool ascending) {
            for (const auto& update : updates) {
                double price, qty;
                if (update.is_array() && update.size() >= 2) {
                    price = std::stod(update[0].get<std::string>());
                    qty = std::stod(update[1].get<std::string>());
                } else {
                    price = std::stod(update.value("p", "0"));
                    qty = std::stod(update.value("s", "0"));
                }

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

        if (result.contains("b") || result.contains("bids")) {
            auto bids = result.contains("b") ? result["b"] : result["bids"];
            applyUpdates(ob.bids, bids, false);
        }
        if (result.contains("a") || result.contains("asks")) {
            auto asks = result.contains("a") ? result["a"] : result["asks"];
            applyUpdates(ob.asks, asks, true);
        }

        onOrderBook(ob);

    } catch (const std::exception& e) {
        onError("OrderBook delta error: " + std::string(e.what()));
    }
}

void GateIOExchange::handleTradeUpdate(const nlohmann::json& json) {
    try {
        auto result = json["result"];

        // Can be array or single object
        auto trades = result.is_array() ? result : nlohmann::json::array({result});

        for (const auto& t : trades) {
            Trade trade;
            trade.exchange = "gateio";
            trade.symbol = t.value("currency_pair", t.value("s", ""));
            trade.trade_id = std::to_string(t.value("id", uint64_t(0)));
            trade.price = std::stod(t.value("price", t.value("p", "0")));
            trade.quantity = std::stod(t.value("amount", t.value("s", "0")));
            trade.timestamp = t.value("create_time_ms", t.value("t", uint64_t(0)));
            trade.local_timestamp = currentTimeNs();

            std::string side = t.value("side", "buy");
            trade.side = (side == "buy") ? OrderSide::Buy : OrderSide::Sell;

            onTrade(trade);
        }

    } catch (const std::exception& e) {
        onError("Trade update error: " + std::string(e.what()));
    }
}

void GateIOExchange::handleTickerUpdate(const nlohmann::json& json) {
    try {
        auto result = json["result"];

        Ticker ticker;
        ticker.exchange = "gateio";
        ticker.symbol = result.value("currency_pair", result.value("s", ""));
        ticker.last = std::stod(result.value("last", "0"));
        ticker.bid = std::stod(result.value("highest_bid", result.value("b", "0")));
        ticker.ask = std::stod(result.value("lowest_ask", result.value("a", "0")));
        ticker.high_24h = std::stod(result.value("high_24h", "0"));
        ticker.low_24h = std::stod(result.value("low_24h", "0"));
        ticker.volume_24h = std::stod(result.value("base_volume", result.value("v", "0")));
        ticker.volume_quote_24h = std::stod(result.value("quote_volume", "0"));
        ticker.change_24h_pct = std::stod(result.value("change_percentage", "0"));

        onTicker(ticker);

    } catch (const std::exception& e) {
        onError("Ticker update error: " + std::string(e.what()));
    }
}

void GateIOExchange::handleOrderUpdate(const nlohmann::json& data) {
    try {
        Order order;
        order.exchange = "gateio";
        order.symbol = data.value("currency_pair", data.value("contract", ""));
        order.order_id = data.value("id", data.value("order_id", ""));
        order.client_order_id = data.value("text", "");
        order.side = parseOrderSide(data.value("side", "buy"));
        order.type = parseOrderType(data.value("type", "limit"));
        order.status = parseOrderStatus(data.value("status", "open"));

        order.quantity = std::stod(data.value("amount", data.value("size", "0")));
        order.filled_quantity = std::stod(data.value("filled_total", data.value("fill_price", "0")));
        order.remaining_quantity = std::stod(data.value("left", "0"));
        order.price = std::stod(data.value("price", "0"));
        order.average_price = std::stod(data.value("avg_deal_price", data.value("fill_price", "0")));
        order.commission = std::stod(data.value("fee", "0"));
        order.commission_asset = data.value("fee_currency", "");

        // Gate.io timestamps are in seconds, convert to ms
        order.create_time = static_cast<uint64_t>(std::stod(data.value("create_time", "0")) * 1000);
        order.update_time = static_cast<uint64_t>(std::stod(data.value("update_time", "0")) * 1000);
        order.raw = data;

        onOrder(order);

    } catch (const std::exception& e) {
        onError("Order update error: " + std::string(e.what()));
    }
}

void GateIOExchange::handlePositionUpdate(const nlohmann::json& data) {
    try {
        Position position;
        position.exchange = "gateio";
        position.symbol = data.value("contract", "");

        double size = std::stod(data.value("size", "0"));
        position.side = size >= 0 ? OrderSide::Buy : OrderSide::Sell;
        position.quantity = std::abs(size);
        position.entry_price = std::stod(data.value("entry_price", "0"));
        position.mark_price = std::stod(data.value("mark_price", "0"));
        position.liquidation_price = std::stod(data.value("liq_price", "0"));
        position.unrealized_pnl = std::stod(data.value("unrealised_pnl", "0"));
        position.realized_pnl = std::stod(data.value("realised_pnl", "0"));
        position.leverage = std::stod(data.value("leverage", "1"));
        position.margin = std::stod(data.value("margin", "0"));
        position.update_time = currentTimeMs();

        onPosition(position);

    } catch (const std::exception& e) {
        onError("Position update error: " + std::string(e.what()));
    }
}

void GateIOExchange::handleBalanceUpdate(const nlohmann::json& data) {
    try {
        Balance balance;
        balance.asset = data.value("currency", data.value("text", ""));
        balance.free = std::stod(data.value("available", "0"));
        balance.locked = std::stod(data.value("freeze", data.value("locked", "0")));
        onBalance(balance);
    } catch (const std::exception& e) {
        onError("Balance update error: " + std::string(e.what()));
    }
}

// ============================================================================
// Signature Generation (HMAC-SHA512)
// ============================================================================

std::string GateIOExchange::sign(const std::string& method, const std::string& path,
                                  const std::string& query_string, const std::string& body,
                                  const std::string& timestamp) {
    // Gate.io v4 API signature:
    // Sign = HexEncode(HMAC_SHA512(secret_key, message))
    // message = request_method + "\n" + request_path + "\n" + query_string + "\n" + hashed_body + "\n" + timestamp

    // Hash body with SHA512 (Gate.io requires hashing even empty bodies)
    unsigned char body_hash[SHA512_DIGEST_LENGTH];
    SHA512(reinterpret_cast<const unsigned char*>(body.c_str()), body.length(), body_hash);

    std::stringstream body_ss;
    for (int i = 0; i < SHA512_DIGEST_LENGTH; i++) {
        body_ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(body_hash[i]);
    }
    std::string hashed_body = body_ss.str();

    // Build message
    std::string message = method + "\n" + path + "\n" + query_string + "\n" + hashed_body + "\n" + timestamp;

    // HMAC-SHA512
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;

    HMAC(EVP_sha512(),
         gateio_config_.api_secret.c_str(),
         static_cast<int>(gateio_config_.api_secret.length()),
         reinterpret_cast<const unsigned char*>(message.c_str()),
         message.length(),
         hash, &hash_len);

    // Hex encode
    std::stringstream ss;
    for (unsigned int i = 0; i < hash_len; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }

    return ss.str();
}

std::string GateIOExchange::generateWsSignature(const std::string& channel,
                                                 const std::string& event,
                                                 const std::string& timestamp) {
    // WebSocket authentication signature
    // sign = HMAC-SHA512(secret_key, "channel=" + channel + "&event=" + event + "&time=" + timestamp)
    std::string message = "channel=" + channel + "&event=" + event + "&time=" + timestamp;

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;

    HMAC(EVP_sha512(),
         gateio_config_.api_secret.c_str(),
         static_cast<int>(gateio_config_.api_secret.length()),
         reinterpret_cast<const unsigned char*>(message.c_str()),
         message.length(),
         hash, &hash_len);

    std::stringstream ss;
    for (unsigned int i = 0; i < hash_len; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }

    return ss.str();
}

// ============================================================================
// Subscription Management
// ============================================================================

bool GateIOExchange::subscribeOrderBook(const std::string& symbol, int depth) {
    std::string channel = gateio_config_.spot ? "spot.order_book_update" : "futures.order_book_update";

    nlohmann::json payload;
    payload["time"] = static_cast<int64_t>(currentTimeMs() / 1000);
    payload["channel"] = channel;
    payload["event"] = "subscribe";
    payload["payload"] = nlohmann::json::array({symbol, std::to_string(gateio_config_.orderbook_update_speed) + "ms"});

    sendSubscribe({payload}, false);

    // Also subscribe to full orderbook for initial snapshot
    std::string snapshot_channel = gateio_config_.spot ? "spot.order_book" : "futures.order_book";
    nlohmann::json snapshot_payload;
    snapshot_payload["time"] = static_cast<int64_t>(currentTimeMs() / 1000);
    snapshot_payload["channel"] = snapshot_channel;
    snapshot_payload["event"] = "subscribe";
    snapshot_payload["payload"] = nlohmann::json::array({symbol, std::to_string(depth), std::to_string(gateio_config_.orderbook_update_speed) + "ms"});

    sendSubscribe({snapshot_payload}, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.insert(channel + ":" + symbol);
    return true;
}

bool GateIOExchange::unsubscribeOrderBook(const std::string& symbol) {
    std::string channel = gateio_config_.spot ? "spot.order_book_update" : "futures.order_book_update";

    nlohmann::json payload;
    payload["time"] = static_cast<int64_t>(currentTimeMs() / 1000);
    payload["channel"] = channel;
    payload["event"] = "unsubscribe";
    payload["payload"] = nlohmann::json::array({symbol});

    sendUnsubscribe({payload}, false);

    {
        std::lock_guard<std::mutex> lock(subscription_mutex_);
        public_subscriptions_.erase(channel + ":" + symbol);
    }

    // Clear initialization state
    {
        std::lock_guard<std::mutex> lock(orderbook_init_mutex_);
        orderbook_initialized_.erase(symbol);
    }

    clearOrderBookCache(symbol);
    return true;
}

bool GateIOExchange::subscribeTrades(const std::string& symbol) {
    std::string channel = gateio_config_.spot ? "spot.trades" : "futures.trades";

    nlohmann::json payload;
    payload["time"] = static_cast<int64_t>(currentTimeMs() / 1000);
    payload["channel"] = channel;
    payload["event"] = "subscribe";
    payload["payload"] = nlohmann::json::array({symbol});

    sendSubscribe({payload}, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.insert(channel + ":" + symbol);
    return true;
}

bool GateIOExchange::unsubscribeTrades(const std::string& symbol) {
    std::string channel = gateio_config_.spot ? "spot.trades" : "futures.trades";

    nlohmann::json payload;
    payload["time"] = static_cast<int64_t>(currentTimeMs() / 1000);
    payload["channel"] = channel;
    payload["event"] = "unsubscribe";
    payload["payload"] = nlohmann::json::array({symbol});

    sendUnsubscribe({payload}, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.erase(channel + ":" + symbol);
    return true;
}

bool GateIOExchange::subscribeTicker(const std::string& symbol) {
    std::string channel = gateio_config_.spot ? "spot.tickers" : "futures.tickers";

    nlohmann::json payload;
    payload["time"] = static_cast<int64_t>(currentTimeMs() / 1000);
    payload["channel"] = channel;
    payload["event"] = "subscribe";
    payload["payload"] = nlohmann::json::array({symbol});

    sendSubscribe({payload}, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.insert(channel + ":" + symbol);
    return true;
}

bool GateIOExchange::unsubscribeTicker(const std::string& symbol) {
    std::string channel = gateio_config_.spot ? "spot.tickers" : "futures.tickers";

    nlohmann::json payload;
    payload["time"] = static_cast<int64_t>(currentTimeMs() / 1000);
    payload["channel"] = channel;
    payload["event"] = "unsubscribe";
    payload["payload"] = nlohmann::json::array({symbol});

    sendUnsubscribe({payload}, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.erase(channel + ":" + symbol);
    return true;
}

bool GateIOExchange::subscribeUserData() {
    // Initialize private WebSocket if needed
    if (!private_ws_client_) {
        network::WebSocketConfig ws_config;
        ws_config.url = getWsUrl();
        ws_config.name = "gateio_private_ws";
        ws_config.ping_interval_ms = 15000;
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

    // Subscribe to user data channels
    std::string orders_channel = gateio_config_.spot ? "spot.orders" : "futures.orders";
    std::string balances_channel = gateio_config_.spot ? "spot.balances" : "futures.balances";

    std::string timestamp = std::to_string(currentTimeMs() / 1000);

    // Orders subscription with auth
    nlohmann::json orders_payload;
    orders_payload["time"] = std::stoll(timestamp);
    orders_payload["channel"] = orders_channel;
    orders_payload["event"] = "subscribe";
    orders_payload["payload"] = nlohmann::json::array({"!all"});
    orders_payload["auth"] = {
        {"method", "api_key"},
        {"KEY", gateio_config_.api_key},
        {"SIGN", generateWsSignature(orders_channel, "subscribe", timestamp)}
    };

    sendSubscribe({orders_payload}, true);

    // Balances subscription with auth
    nlohmann::json balances_payload;
    balances_payload["time"] = std::stoll(timestamp);
    balances_payload["channel"] = balances_channel;
    balances_payload["event"] = "subscribe";
    balances_payload["payload"] = nlohmann::json::array();
    balances_payload["auth"] = {
        {"method", "api_key"},
        {"KEY", gateio_config_.api_key},
        {"SIGN", generateWsSignature(balances_channel, "subscribe", timestamp)}
    };

    sendSubscribe({balances_payload}, true);

    // Positions subscription for futures
    if (!gateio_config_.spot) {
        nlohmann::json positions_payload;
        positions_payload["time"] = std::stoll(timestamp);
        positions_payload["channel"] = "futures.positions";
        positions_payload["event"] = "subscribe";
        positions_payload["payload"] = nlohmann::json::array({"!all"});
        positions_payload["auth"] = {
            {"method", "api_key"},
            {"KEY", gateio_config_.api_key},
            {"SIGN", generateWsSignature("futures.positions", "subscribe", timestamp)}
        };

        sendSubscribe({positions_payload}, true);
    }

    return true;
}

bool GateIOExchange::unsubscribeUserData() {
    if (private_ws_client_) {
        private_ws_client_->disconnect();
        private_ws_client_.reset();
    }

    private_ws_connected_ = false;
    private_ws_authenticated_ = false;
    return true;
}

void GateIOExchange::sendSubscribe(const std::vector<nlohmann::json>& channels, bool is_private) {
    auto client = is_private ? private_ws_client_ : public_ws_client_;
    if (!client) return;

    for (const auto& msg : channels) {
        client->send(msg);
    }
}

void GateIOExchange::sendUnsubscribe(const std::vector<nlohmann::json>& channels, bool is_private) {
    auto client = is_private ? private_ws_client_ : public_ws_client_;
    if (!client) return;

    for (const auto& msg : channels) {
        client->send(msg);
    }
}

void GateIOExchange::authenticatePrivateWs() {
    // Gate.io authenticates per-channel, not globally
    // Authentication is included in subscription messages
    private_ws_authenticated_ = true;
}

// ============================================================================
// REST Helpers
// ============================================================================

network::HttpResponse GateIOExchange::signedGet(
    const std::string& endpoint,
    const std::unordered_map<std::string, std::string>& params) {

    // Build query string
    std::string query_string;
    if (!params.empty()) {
        std::stringstream ss;
        bool first = true;
        for (const auto& [key, value] : params) {
            if (!first) ss << "&";
            first = false;
            ss << key << "=" << value;
        }
        query_string = ss.str();
    }

    std::string timestamp = std::to_string(currentTimeMs() / 1000);
    std::string signature = sign("GET", endpoint, query_string, "", timestamp);

    network::HttpRequest request;
    request.method = network::HttpMethod::GET;
    request.path = endpoint + (query_string.empty() ? "" : "?" + query_string);
    request.headers["KEY"] = gateio_config_.api_key;
    request.headers["SIGN"] = signature;
    request.headers["Timestamp"] = timestamp;
    request.headers["Content-Type"] = "application/json";

    return rest_client_->execute(request);
}

network::HttpResponse GateIOExchange::signedPost(
    const std::string& endpoint,
    const nlohmann::json& body) {

    std::string body_str = body.empty() ? "" : body.dump();
    std::string timestamp = std::to_string(currentTimeMs() / 1000);
    std::string signature = sign("POST", endpoint, "", body_str, timestamp);

    network::HttpRequest request;
    request.method = network::HttpMethod::POST;
    request.path = endpoint;
    request.body = body_str;
    request.headers["KEY"] = gateio_config_.api_key;
    request.headers["SIGN"] = signature;
    request.headers["Timestamp"] = timestamp;
    request.headers["Content-Type"] = "application/json";

    return rest_client_->execute(request);
}

network::HttpResponse GateIOExchange::signedDelete(
    const std::string& endpoint,
    const std::unordered_map<std::string, std::string>& params) {

    // Build query string
    std::string query_string;
    if (!params.empty()) {
        std::stringstream ss;
        bool first = true;
        for (const auto& [key, value] : params) {
            if (!first) ss << "&";
            first = false;
            ss << key << "=" << value;
        }
        query_string = ss.str();
    }

    std::string timestamp = std::to_string(currentTimeMs() / 1000);
    std::string signature = sign("DELETE", endpoint, query_string, "", timestamp);

    network::HttpRequest request;
    request.method = network::HttpMethod::DELETE;
    request.path = endpoint + (query_string.empty() ? "" : "?" + query_string);
    request.headers["KEY"] = gateio_config_.api_key;
    request.headers["SIGN"] = signature;
    request.headers["Timestamp"] = timestamp;
    request.headers["Content-Type"] = "application/json";

    return rest_client_->execute(request);
}

network::HttpResponse GateIOExchange::publicGet(
    const std::string& endpoint,
    const std::unordered_map<std::string, std::string>& params) {

    return rest_client_->get(endpoint, params);
}

// ============================================================================
// Order Management
// ============================================================================

std::optional<Order> GateIOExchange::placeOrder(const OrderRequest& request) {
    nlohmann::json body;

    if (gateio_config_.spot) {
        // Spot order
        body["currency_pair"] = request.symbol;
        body["side"] = orderSideToString(request.side);
        body["type"] = orderTypeToString(request.type);
        body["amount"] = std::to_string(request.quantity);
        body["time_in_force"] = timeInForceToString(request.time_in_force);

        if (request.type != OrderType::Market) {
            body["price"] = std::to_string(request.price);
        }

        if (!request.client_order_id.empty()) {
            body["text"] = request.client_order_id;
        }
    } else {
        // Futures order
        body["contract"] = request.symbol;
        body["size"] = static_cast<int64_t>(request.quantity) * (request.side == OrderSide::Buy ? 1 : -1);
        body["tif"] = timeInForceToString(request.time_in_force);

        if (request.type != OrderType::Market) {
            body["price"] = std::to_string(request.price);
        }

        if (!request.client_order_id.empty()) {
            body["text"] = request.client_order_id;
        }

        if (request.reduce_only) {
            body["reduce_only"] = true;
        }
    }

    std::string endpoint = gateio_config_.spot ? "/spot/orders" : "/futures/usdt/orders";
    auto response = signedPost(endpoint, body);

    if (!response.success) {
        onError("Place order failed: " + response.error + " " + response.body);
        return std::nullopt;
    }

    try {
        auto json = response.json();

        // Check for error response
        if (json.contains("label")) {
            onError("Place order failed: " + json.value("message", json.value("label", "unknown")));
            return std::nullopt;
        }

        return parseOrder(json);

    } catch (const std::exception& e) {
        onError("Parse order response failed: " + std::string(e.what()));
        return std::nullopt;
    }
}

std::optional<Order> GateIOExchange::cancelOrder(const std::string& symbol,
                                                  const std::string& order_id) {
    std::string endpoint;
    std::unordered_map<std::string, std::string> params;

    if (gateio_config_.spot) {
        endpoint = "/spot/orders/" + order_id;
        params["currency_pair"] = symbol;
    } else {
        endpoint = "/futures/usdt/orders/" + order_id;
    }

    auto response = signedDelete(endpoint, params);

    if (!response.success) {
        onError("Cancel order failed: " + response.error);
        return std::nullopt;
    }

    try {
        auto json = response.json();

        if (json.contains("label")) {
            onError("Cancel order failed: " + json.value("message", json.value("label", "unknown")));
            return std::nullopt;
        }

        return parseOrder(json);

    } catch (...) {
        return std::nullopt;
    }
}

std::optional<Order> GateIOExchange::cancelOrder(const std::string& symbol,
                                                  const std::string& order_id,
                                                  const std::string& client_order_id) {
    // Gate.io primarily uses order_id for cancellation
    // If client_order_id provided, we could look up the order first
    return cancelOrder(symbol, order_id);
}

bool GateIOExchange::cancelAllOrders(const std::string& symbol) {
    std::string endpoint;
    std::unordered_map<std::string, std::string> params;

    if (gateio_config_.spot) {
        endpoint = "/spot/orders";
        params["currency_pair"] = symbol;
        params["side"] = "all";
    } else {
        endpoint = "/futures/usdt/orders";
        params["contract"] = symbol;
    }

    auto response = signedDelete(endpoint, params);
    return response.success;
}

std::optional<Order> GateIOExchange::getOrder(const std::string& symbol,
                                               const std::string& order_id) {
    std::string endpoint;
    std::unordered_map<std::string, std::string> params;

    if (gateio_config_.spot) {
        endpoint = "/spot/orders/" + order_id;
        params["currency_pair"] = symbol;
    } else {
        endpoint = "/futures/usdt/orders/" + order_id;
    }

    auto response = signedGet(endpoint, params);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();

        if (json.contains("label")) {
            return std::nullopt;
        }

        return parseOrder(json);

    } catch (...) {
        return std::nullopt;
    }
}

std::vector<Order> GateIOExchange::getOpenOrders(const std::string& symbol) {
    std::string endpoint;
    std::unordered_map<std::string, std::string> params;

    if (gateio_config_.spot) {
        endpoint = "/spot/orders";
        if (!symbol.empty()) {
            params["currency_pair"] = symbol;
        }
        params["status"] = "open";
    } else {
        endpoint = "/futures/usdt/orders";
        if (!symbol.empty()) {
            params["contract"] = symbol;
        }
        params["status"] = "open";
    }

    auto response = signedGet(endpoint, params);

    std::vector<Order> orders;
    if (!response.success) {
        return orders;
    }

    try {
        auto json = response.json();

        if (json.is_array()) {
            for (const auto& item : json) {
                orders.push_back(parseOrder(item));
            }
        }

    } catch (...) {
        // Return empty
    }

    return orders;
}

std::vector<Order> GateIOExchange::getOrderHistory(const std::string& symbol,
                                                    uint64_t start_time,
                                                    uint64_t end_time,
                                                    int limit) {
    std::string endpoint;
    std::unordered_map<std::string, std::string> params;

    if (gateio_config_.spot) {
        endpoint = "/spot/orders";
        params["currency_pair"] = symbol;
        params["status"] = "finished";
        params["limit"] = std::to_string(limit);
    } else {
        endpoint = "/futures/usdt/orders";
        params["contract"] = symbol;
        params["status"] = "finished";
        params["limit"] = std::to_string(limit);
    }

    if (start_time > 0) {
        params["from"] = std::to_string(start_time / 1000);  // Gate.io uses seconds
    }
    if (end_time > 0) {
        params["to"] = std::to_string(end_time / 1000);
    }

    auto response = signedGet(endpoint, params);

    std::vector<Order> orders;
    if (!response.success) {
        return orders;
    }

    try {
        auto json = response.json();

        if (json.is_array()) {
            for (const auto& item : json) {
                orders.push_back(parseOrder(item));
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

std::optional<Account> GateIOExchange::getAccount() {
    Account account;
    account.exchange = "gateio";
    account.update_time = currentTimeMs();

    if (gateio_config_.spot) {
        // Get spot balances
        auto response = signedGet("/spot/accounts");

        if (!response.success) {
            return std::nullopt;
        }

        try {
            auto json = response.json();

            if (json.is_array()) {
                for (const auto& b : json) {
                    Balance balance;
                    balance.asset = b.value("currency", "");
                    balance.free = std::stod(b.value("available", "0"));
                    balance.locked = std::stod(b.value("locked", "0"));

                    if (balance.free > 0 || balance.locked > 0) {
                        account.balances[balance.asset] = balance;
                    }
                }
            }

        } catch (const std::exception& e) {
            onError("Parse account failed: " + std::string(e.what()));
            return std::nullopt;
        }
    } else {
        // Get futures account
        auto response = signedGet("/futures/usdt/accounts");

        if (!response.success) {
            return std::nullopt;
        }

        try {
            auto json = response.json();

            Balance balance;
            balance.asset = "USDT";
            balance.free = std::stod(json.value("available", "0"));
            balance.locked = std::stod(json.value("order_margin", "0"));
            account.balances["USDT"] = balance;

            account.total_margin = std::stod(json.value("total", "0"));
            account.available_margin = std::stod(json.value("available", "0"));
            account.total_unrealized_pnl = std::stod(json.value("unrealised_pnl", "0"));

        } catch (const std::exception& e) {
            onError("Parse account failed: " + std::string(e.what()));
            return std::nullopt;
        }

        // Get positions
        account.positions = getPositions();
    }

    return account;
}

std::optional<Balance> GateIOExchange::getBalance(const std::string& asset) {
    if (gateio_config_.spot) {
        std::unordered_map<std::string, std::string> params;
        params["currency"] = asset;

        auto response = signedGet("/spot/accounts", params);

        if (!response.success) {
            return std::nullopt;
        }

        try {
            auto json = response.json();

            if (json.is_array() && !json.empty()) {
                auto b = json[0];
                Balance balance;
                balance.asset = b.value("currency", "");
                balance.free = std::stod(b.value("available", "0"));
                balance.locked = std::stod(b.value("locked", "0"));
                return balance;
            }

        } catch (...) {
            return std::nullopt;
        }
    } else {
        auto account = getAccount();
        if (account) {
            auto it = account->balances.find(asset);
            if (it != account->balances.end()) {
                return it->second;
            }
        }
    }

    return std::nullopt;
}

std::vector<Position> GateIOExchange::getPositions(const std::string& symbol) {
    std::vector<Position> positions;

    if (gateio_config_.spot) {
        return positions;  // No positions for spot
    }

    std::string endpoint = "/futures/usdt/positions";
    std::unordered_map<std::string, std::string> params;

    if (!symbol.empty()) {
        params["contract"] = symbol;
    }

    auto response = signedGet(endpoint, params);

    if (!response.success) {
        return positions;
    }

    try {
        auto json = response.json();

        if (json.is_array()) {
            for (const auto& p : json) {
                double size = std::stod(p.value("size", "0"));
                if (size == 0.0) continue;

                Position position;
                position.exchange = "gateio";
                position.symbol = p.value("contract", "");
                position.side = size >= 0 ? OrderSide::Buy : OrderSide::Sell;
                position.quantity = std::abs(size);
                position.entry_price = std::stod(p.value("entry_price", "0"));
                position.mark_price = std::stod(p.value("mark_price", "0"));
                position.liquidation_price = std::stod(p.value("liq_price", "0"));
                position.unrealized_pnl = std::stod(p.value("unrealised_pnl", "0"));
                position.realized_pnl = std::stod(p.value("realised_pnl", "0"));
                position.leverage = std::stod(p.value("leverage", "1"));
                position.margin = std::stod(p.value("margin", "0"));
                position.maintenance_margin = std::stod(p.value("maintenance_margin", "0"));
                position.update_time = currentTimeMs();

                positions.push_back(position);
            }
        }

    } catch (...) {
        // Return empty
    }

    return positions;
}

// ============================================================================
// Market Information
// ============================================================================

std::vector<SymbolInfo> GateIOExchange::getSymbols() {
    std::string endpoint = gateio_config_.spot ? "/spot/currency_pairs" : "/futures/usdt/contracts";
    auto response = publicGet(endpoint);

    std::vector<SymbolInfo> symbols;
    if (!response.success) {
        return symbols;
    }

    try {
        auto json = response.json();

        if (json.is_array()) {
            for (const auto& item : json) {
                auto info = parseSymbolInfo(item);
                updateSymbolInfo(info);
                symbols.push_back(info);
            }
        }

    } catch (...) {
        // Return empty
    }

    return symbols;
}

std::optional<SymbolInfo> GateIOExchange::getSymbolInfo(const std::string& symbol) {
    // Check cache first
    {
        std::lock_guard<std::mutex> lock(symbol_info_mutex_);
        auto it = symbol_info_cache_.find(symbol);
        if (it != symbol_info_cache_.end()) {
            return it->second;
        }
    }

    std::string endpoint;
    if (gateio_config_.spot) {
        endpoint = "/spot/currency_pairs/" + symbol;
    } else {
        endpoint = "/futures/usdt/contracts/" + symbol;
    }

    auto response = publicGet(endpoint);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        return parseSymbolInfo(json);

    } catch (...) {
        return std::nullopt;
    }
}

std::optional<OrderBook> GateIOExchange::getOrderBookSnapshot(const std::string& symbol, int depth) {
    std::string endpoint;
    std::unordered_map<std::string, std::string> params;

    if (gateio_config_.spot) {
        endpoint = "/spot/order_book";
        params["currency_pair"] = symbol;
        params["limit"] = std::to_string(depth);
    } else {
        endpoint = "/futures/usdt/order_book";
        params["contract"] = symbol;
        params["limit"] = std::to_string(depth);
    }

    auto response = publicGet(endpoint, params);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();

        OrderBook ob;
        ob.symbol = symbol;
        ob.exchange = "gateio";
        ob.timestamp = currentTimeMs();
        ob.local_timestamp = currentTimeNs();

        if (json.contains("id")) {
            ob.sequence = json["id"].get<uint64_t>();
        }

        auto bids = json.value("bids", nlohmann::json::array());
        for (const auto& b : bids) {
            PriceLevel level;
            if (b.is_array() && b.size() >= 2) {
                level.price = std::stod(b[0].get<std::string>());
                level.quantity = std::stod(b[1].get<std::string>());
            }
            ob.bids.push_back(level);
        }

        auto asks = json.value("asks", nlohmann::json::array());
        for (const auto& a : asks) {
            PriceLevel level;
            if (a.is_array() && a.size() >= 2) {
                level.price = std::stod(a[0].get<std::string>());
                level.quantity = std::stod(a[1].get<std::string>());
            }
            ob.asks.push_back(level);
        }

        return ob;

    } catch (...) {
        return std::nullopt;
    }
}

std::vector<Trade> GateIOExchange::getRecentTrades(const std::string& symbol, int limit) {
    std::string endpoint;
    std::unordered_map<std::string, std::string> params;

    if (gateio_config_.spot) {
        endpoint = "/spot/trades";
        params["currency_pair"] = symbol;
        params["limit"] = std::to_string(limit);
    } else {
        endpoint = "/futures/usdt/trades";
        params["contract"] = symbol;
        params["limit"] = std::to_string(limit);
    }

    auto response = publicGet(endpoint, params);

    std::vector<Trade> trades;
    if (!response.success) {
        return trades;
    }

    try {
        auto json = response.json();

        if (json.is_array()) {
            for (const auto& t : json) {
                Trade trade;
                trade.exchange = "gateio";
                trade.symbol = symbol;
                trade.trade_id = std::to_string(t.value("id", uint64_t(0)));
                trade.price = std::stod(t.value("price", "0"));
                trade.quantity = std::stod(t.value("amount", t.value("size", "0")));

                // Gate.io uses seconds timestamps for spot, ms for futures
                if (gateio_config_.spot) {
                    trade.timestamp = static_cast<uint64_t>(std::stod(t.value("create_time", "0")) * 1000);
                } else {
                    trade.timestamp = static_cast<uint64_t>(std::stod(t.value("create_time", "0")) * 1000);
                }

                trade.side = t.value("side", "buy") == "buy" ? OrderSide::Buy : OrderSide::Sell;
                trades.push_back(trade);
            }
        }

    } catch (...) {
        // Return empty
    }

    return trades;
}

std::optional<Ticker> GateIOExchange::getTicker(const std::string& symbol) {
    std::string endpoint;
    std::unordered_map<std::string, std::string> params;

    if (gateio_config_.spot) {
        endpoint = "/spot/tickers";
        params["currency_pair"] = symbol;
    } else {
        endpoint = "/futures/usdt/tickers";
        params["contract"] = symbol;
    }

    auto response = publicGet(endpoint, params);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();

        // Gate.io returns array for tickers
        auto t = json.is_array() && !json.empty() ? json[0] : json;

        Ticker ticker;
        ticker.exchange = "gateio";
        ticker.symbol = symbol;
        ticker.last = std::stod(t.value("last", "0"));
        ticker.bid = std::stod(t.value("highest_bid", t.value("bid1_price", "0")));
        ticker.ask = std::stod(t.value("lowest_ask", t.value("ask1_price", "0")));
        ticker.bid_qty = std::stod(t.value("bid1_size", "0"));
        ticker.ask_qty = std::stod(t.value("ask1_size", "0"));
        ticker.high_24h = std::stod(t.value("high_24h", "0"));
        ticker.low_24h = std::stod(t.value("low_24h", "0"));
        ticker.volume_24h = std::stod(t.value("base_volume", t.value("volume_24h", "0")));
        ticker.volume_quote_24h = std::stod(t.value("quote_volume", t.value("volume_24h_usd", "0")));
        ticker.change_24h_pct = std::stod(t.value("change_percentage", "0"));
        ticker.timestamp = currentTimeMs();

        return ticker;

    } catch (...) {
        return std::nullopt;
    }
}

uint64_t GateIOExchange::getServerTime() {
    // Gate.io doesn't have a dedicated server time endpoint
    // Use current time
    return currentTimeMs();
}

std::string GateIOExchange::formatSymbol(const std::string& base, const std::string& quote) const {
    // Gate.io uses underscore separator: BTC_USDT
    return base + "_" + quote;
}

std::pair<std::string, std::string> GateIOExchange::parseSymbol(const std::string& symbol) const {
    auto pos = symbol.find('_');
    if (pos != std::string::npos) {
        return {symbol.substr(0, pos), symbol.substr(pos + 1)};
    }
    return {symbol, ""};
}

// ============================================================================
// Data Converters
// ============================================================================

Order GateIOExchange::parseOrder(const nlohmann::json& data) {
    Order order;
    order.exchange = "gateio";
    order.symbol = data.value("currency_pair", data.value("contract", ""));
    order.order_id = data.value("id", data.value("order_id", ""));
    order.client_order_id = data.value("text", "");
    order.side = parseOrderSide(data.value("side", "buy"));
    order.type = parseOrderType(data.value("type", "limit"));
    order.status = parseOrderStatus(data.value("status", "open"));
    order.time_in_force = parseTimeInForce(data.value("time_in_force", data.value("tif", "gtc")));

    order.quantity = std::stod(data.value("amount", data.value("size", "0")));
    order.filled_quantity = std::stod(data.value("filled_total", data.value("fill_price", "0")));
    order.remaining_quantity = std::stod(data.value("left", "0"));
    order.price = std::stod(data.value("price", "0"));
    order.average_price = std::stod(data.value("avg_deal_price", data.value("fill_price", "0")));
    order.commission = std::stod(data.value("fee", "0"));
    order.commission_asset = data.value("fee_currency", "");

    // Gate.io timestamps are in seconds
    order.create_time = static_cast<uint64_t>(std::stod(data.value("create_time", "0")) * 1000);
    order.update_time = static_cast<uint64_t>(std::stod(data.value("update_time", data.value("finish_time", "0"))) * 1000);
    order.reduce_only = data.value("reduce_only", false);
    order.raw = data;

    return order;
}

OrderStatus GateIOExchange::parseOrderStatus(const std::string& status) {
    if (status == "open") return OrderStatus::New;
    if (status == "closed" || status == "finished") return OrderStatus::Filled;
    if (status == "cancelled") return OrderStatus::Cancelled;
    if (status == "liquidated") return OrderStatus::Filled;
    return OrderStatus::Failed;
}

OrderSide GateIOExchange::parseOrderSide(const std::string& side) {
    return (side == "buy" || side == "long") ? OrderSide::Buy : OrderSide::Sell;
}

OrderType GateIOExchange::parseOrderType(const std::string& type) {
    if (type == "market") return OrderType::Market;
    if (type == "limit") return OrderType::Limit;
    return OrderType::Limit;
}

TimeInForce GateIOExchange::parseTimeInForce(const std::string& tif) {
    if (tif == "gtc") return TimeInForce::GTC;
    if (tif == "ioc") return TimeInForce::IOC;
    if (tif == "fok") return TimeInForce::FOK;
    if (tif == "poc" || tif == "post_only") return TimeInForce::PostOnly;
    return TimeInForce::GTC;
}

std::string GateIOExchange::orderSideToString(OrderSide side) {
    return side == OrderSide::Buy ? "buy" : "sell";
}

std::string GateIOExchange::orderTypeToString(OrderType type) {
    switch (type) {
        case OrderType::Market: return "market";
        case OrderType::Limit: return "limit";
        default: return "limit";
    }
}

std::string GateIOExchange::timeInForceToString(TimeInForce tif) {
    switch (tif) {
        case TimeInForce::GTC: return "gtc";
        case TimeInForce::IOC: return "ioc";
        case TimeInForce::FOK: return "fok";
        case TimeInForce::PostOnly: return "poc";
        default: return "gtc";
    }
}

SymbolInfo GateIOExchange::parseSymbolInfo(const nlohmann::json& data) {
    SymbolInfo info;

    if (gateio_config_.spot) {
        info.symbol = data.value("id", "");
        info.base_asset = data.value("base", "");
        info.quote_asset = data.value("quote", "");
        info.trading_enabled = data.value("trade_status", "") == "tradable";
        info.type = ExchangeType::Spot;

        info.min_qty = std::stod(data.value("min_base_amount", "0"));
        info.max_qty = std::stod(data.value("max_base_amount", "0"));
        info.min_notional = std::stod(data.value("min_quote_amount", "0"));

        // Calculate precision from amount_precision
        int amount_precision = data.value("amount_precision", 8);
        info.qty_precision = amount_precision;
        info.step_size = std::pow(10.0, -amount_precision);

        int precision = data.value("precision", 8);
        info.price_precision = precision;
        info.tick_size = std::pow(10.0, -precision);
    } else {
        // Futures contract
        info.symbol = data.value("name", "");
        info.base_asset = data.value("underlying", "");
        info.quote_asset = data.value("settle", "USDT");
        info.trading_enabled = data.value("in_delisting", false) == false;
        info.type = ExchangeType::Perpetual;

        info.step_size = std::stod(data.value("quanto_multiplier", "1"));
        info.tick_size = std::stod(data.value("order_price_round", "0.0001"));

        // Calculate precision
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
        info.qty_precision = 0;  // Futures use integer contract sizes
    }

    return info;
}

// ============================================================================
// Gate.io-specific Methods
// ============================================================================

bool GateIOExchange::setLeverage(const std::string& symbol, int leverage) {
    if (gateio_config_.spot) {
        return false;  // Not applicable for spot
    }

    nlohmann::json body;
    body["leverage"] = std::to_string(leverage);

    auto response = signedPost("/futures/usdt/positions/" + symbol + "/leverage", body);
    return response.success;
}

double GateIOExchange::getFundingRate(const std::string& symbol) {
    if (gateio_config_.spot) {
        return 0.0;  // Not applicable for spot
    }

    std::unordered_map<std::string, std::string> params;
    params["contract"] = symbol;

    auto response = publicGet("/futures/usdt/funding_rate", params);

    if (response.success) {
        auto json = response.json();
        if (json.is_array() && !json.empty()) {
            return std::stod(json[0].value("r", "0"));
        }
    }

    return 0.0;
}

// ============================================================================
// Factory Functions
// ============================================================================

std::shared_ptr<GateIOExchange> createGateIOExchange(const ExchangeConfig& config) {
    return std::make_shared<GateIOExchange>(config);
}

std::shared_ptr<GateIOExchange> createGateIOExchange(const GateIOConfig& config) {
    return std::make_shared<GateIOExchange>(config);
}

} // namespace exchange
} // namespace hft
