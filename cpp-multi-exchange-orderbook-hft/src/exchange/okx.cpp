#include "exchange/okx.hpp"

#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <ctime>

namespace hft {
namespace exchange {

// ============================================================================
// Constructors/Destructor
// ============================================================================

OkxExchange::OkxExchange(const ExchangeConfig& config)
    : ExchangeBase(config) {
    okx_config_.api_key = config.api_key;
    okx_config_.api_secret = config.api_secret;
    okx_config_.passphrase = config.passphrase;
    okx_config_.testnet = config.testnet;

    switch (config.type) {
        case ExchangeType::Spot:
            okx_config_.inst_type = OkxInstType::Spot;
            break;
        case ExchangeType::Perpetual:
            okx_config_.inst_type = OkxInstType::Swap;
            break;
        case ExchangeType::Futures:
            okx_config_.inst_type = OkxInstType::Futures;
            break;
        default:
            okx_config_.inst_type = OkxInstType::Swap;
    }

    okx_config_.order_rate_limit = config.orders_per_second;
    okx_config_.request_rate_limit = config.requests_per_minute;

    // Initialize REST client
    network::RestClientConfig rest_config;
    rest_config.base_url = getRestUrl();
    rest_config.name = "okx_rest";
    rest_config.request_timeout_ms = config.rest_timeout_ms;
    rest_config.max_retries = config.rest_max_retries;
    rest_config.rate_limit.requests_per_second = okx_config_.order_rate_limit;
    rest_config.rate_limit.requests_per_minute = okx_config_.request_rate_limit;

    rest_client_ = std::make_shared<network::RestClient>(rest_config);
}

OkxExchange::OkxExchange(const OkxConfig& okx_config)
    : ExchangeBase(ExchangeConfig{
        .name = "OKX",
        .api_key = okx_config.api_key,
        .api_secret = okx_config.api_secret,
        .passphrase = okx_config.passphrase,
        .type = okx_config.inst_type == OkxInstType::Spot ? ExchangeType::Spot : ExchangeType::Perpetual,
        .testnet = okx_config.testnet
      }),
      okx_config_(okx_config) {

    // Initialize REST client
    network::RestClientConfig rest_config;
    rest_config.base_url = getRestUrl();
    rest_config.name = "okx_rest";
    rest_config.rate_limit.requests_per_second = okx_config_.order_rate_limit;
    rest_config.rate_limit.requests_per_minute = okx_config_.request_rate_limit;

    rest_client_ = std::make_shared<network::RestClient>(rest_config);
}

OkxExchange::~OkxExchange() {
    disconnect();
}

// ============================================================================
// URL Builders
// ============================================================================

std::string OkxExchange::getRestUrl() const {
    return okx_config_.testnet
        ? "https://www.okx.com"  // OKX uses same URL with simulated trading flag
        : "https://www.okx.com";
}

std::string OkxExchange::getPublicWsUrl() const {
    return okx_config_.testnet
        ? "wss://wspap.okx.com:8443/ws/v5/public?brokerId=9999"
        : "wss://ws.okx.com:8443/ws/v5/public";
}

std::string OkxExchange::getPrivateWsUrl() const {
    return okx_config_.testnet
        ? "wss://wspap.okx.com:8443/ws/v5/private?brokerId=9999"
        : "wss://ws.okx.com:8443/ws/v5/private";
}

std::string OkxExchange::getInstTypeString() const {
    switch (okx_config_.inst_type) {
        case OkxInstType::Spot: return "SPOT";
        case OkxInstType::Margin: return "MARGIN";
        case OkxInstType::Swap: return "SWAP";
        case OkxInstType::Futures: return "FUTURES";
        case OkxInstType::Option: return "OPTION";
        default: return "SWAP";
    }
}

// ============================================================================
// Connection Management
// ============================================================================

bool OkxExchange::connect() {
    if (public_ws_connected_.load()) {
        return true;
    }

    onConnectionStatus(ConnectionStatus::Connecting);

    // Initialize public WebSocket client
    network::WebSocketConfig ws_config;
    ws_config.url = getPublicWsUrl();
    ws_config.name = "okx_public_ws";
    ws_config.ping_interval_ms = 25000;  // OKX requires ping every 30s
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

void OkxExchange::disconnect() {
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

bool OkxExchange::isConnected() const {
    return public_ws_connected_.load();
}

ConnectionStatus OkxExchange::connectionStatus() const {
    return connection_status_.load();
}

// ============================================================================
// Signature Generation
// ============================================================================

std::string OkxExchange::generateTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count() % 1000;

    std::tm* gmt = std::gmtime(&time_t_now);

    std::ostringstream ss;
    ss << std::put_time(gmt, "%Y-%m-%dT%H:%M:%S");
    ss << "." << std::setfill('0') << std::setw(3) << ms << "Z";

    return ss.str();
}

std::string OkxExchange::sign(const std::string& timestamp, const std::string& method,
                              const std::string& request_path, const std::string& body) {
    std::string pre_hash = timestamp + method + request_path + body;

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;

    HMAC(EVP_sha256(),
         okx_config_.api_secret.c_str(),
         static_cast<int>(okx_config_.api_secret.length()),
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

void OkxExchange::addAuthHeaders(network::HttpRequest& request, const std::string& method,
                                 const std::string& request_path, const std::string& body) {
    std::string timestamp = generateTimestamp();
    std::string signature = sign(timestamp, method, request_path, body);

    request.headers["OK-ACCESS-KEY"] = okx_config_.api_key;
    request.headers["OK-ACCESS-SIGN"] = signature;
    request.headers["OK-ACCESS-TIMESTAMP"] = timestamp;
    request.headers["OK-ACCESS-PASSPHRASE"] = okx_config_.passphrase;

    if (okx_config_.testnet) {
        request.headers["x-simulated-trading"] = "1";
    }
}

// ============================================================================
// WebSocket Handlers
// ============================================================================

void OkxExchange::handleWsOpen(bool is_private) {
    if (is_private) {
        private_ws_connected_ = true;
        authenticatePrivateWs();
    } else {
        public_ws_connected_ = true;
        onConnectionStatus(ConnectionStatus::Connected);
    }
}

void OkxExchange::handleWsClose(int code, const std::string& reason, bool is_private) {
    if (is_private) {
        private_ws_connected_ = false;
        private_ws_authenticated_ = false;
    } else {
        public_ws_connected_ = false;
        onConnectionStatus(ConnectionStatus::Reconnecting);
    }
}

void OkxExchange::handleWsError(const std::string& error, bool is_private) {
    onError("WebSocket error (" + std::string(is_private ? "private" : "public") + "): " + error);
}

void OkxExchange::handlePublicWsMessage(const std::string& message, network::MessageType type) {
    try {
        auto json = nlohmann::json::parse(message);

        // Check for subscription response
        if (json.contains("event")) {
            std::string event = json["event"].get<std::string>();
            if (event == "error") {
                onError("Subscription error: " + json.value("msg", "unknown"));
            }
            return;
        }

        // Handle data messages
        if (!json.contains("data")) {
            return;
        }

        std::string channel;
        if (json.contains("arg") && json["arg"].contains("channel")) {
            channel = json["arg"]["channel"].get<std::string>();
        }

        std::string action = json.value("action", "");
        auto data = json["data"];

        if (channel.find("books") != std::string::npos) {
            handleOrderBookUpdate(json, action);
        } else if (channel == "trades") {
            handleTradeUpdate(data);
        } else if (channel == "tickers") {
            handleTickerUpdate(data);
        }

    } catch (const std::exception& e) {
        onError("Public WS message parse error: " + std::string(e.what()));
    }
}

void OkxExchange::handlePrivateWsMessage(const std::string& message, network::MessageType type) {
    try {
        auto json = nlohmann::json::parse(message);

        // Check for login response
        if (json.contains("event") && json["event"] == "login") {
            bool success = json.value("code", "1") == "0";
            if (success) {
                private_ws_authenticated_ = true;
            } else {
                onError("Private WS authentication failed: " + json.value("msg", "unknown"));
            }
            return;
        }

        // Check for subscription response
        if (json.contains("event")) {
            return;
        }

        // Handle data messages
        if (!json.contains("data")) {
            return;
        }

        std::string channel;
        if (json.contains("arg") && json["arg"].contains("channel")) {
            channel = json["arg"]["channel"].get<std::string>();
        }

        auto data = json["data"];

        if (channel == "orders") {
            for (const auto& d : data) {
                handleOrderUpdate(d);
            }
        } else if (channel == "positions") {
            for (const auto& d : data) {
                handlePositionUpdate(d);
            }
        } else if (channel == "account") {
            for (const auto& d : data) {
                handleAccountUpdate(d);
            }
        }

    } catch (const std::exception& e) {
        onError("Private WS message parse error: " + std::string(e.what()));
    }
}

void OkxExchange::handleOrderBookUpdate(const nlohmann::json& json, const std::string& action) {
    try {
        auto arg = json["arg"];
        std::string symbol = arg.value("instId", "");
        auto data = json["data"];

        if (data.empty()) return;

        auto book_data = data[0];

        if (action == "snapshot" || action.empty()) {
            // Full snapshot
            OrderBook ob;
            ob.symbol = symbol;
            ob.exchange = "okx";
            ob.sequence = std::stoull(book_data.value("seqId", "0"));
            ob.timestamp = std::stoull(book_data.value("ts", "0"));
            ob.local_timestamp = currentTimeNs();

            auto bids = book_data.value("bids", nlohmann::json::array());
            for (const auto& b : bids) {
                PriceLevel level;
                level.price = std::stod(b[0].get<std::string>());
                level.quantity = std::stod(b[1].get<std::string>());
                level.order_count = std::stoi(b[3].get<std::string>());
                ob.bids.push_back(level);
            }

            auto asks = book_data.value("asks", nlohmann::json::array());
            for (const auto& a : asks) {
                PriceLevel level;
                level.price = std::stod(a[0].get<std::string>());
                level.quantity = std::stod(a[1].get<std::string>());
                level.order_count = std::stoi(a[3].get<std::string>());
                ob.asks.push_back(level);
            }

            // Store checksum
            if (book_data.contains("checksum")) {
                std::lock_guard<std::mutex> lock(checksum_mutex_);
                orderbook_checksum_[symbol] = static_cast<uint32_t>(book_data["checksum"].get<int64_t>());
            }

            onOrderBook(ob);

        } else if (action == "update") {
            // Delta update
            auto cached = getCachedOrderBook(symbol);
            if (!cached) {
                return;
            }

            OrderBook ob = *cached;
            ob.sequence = std::stoull(book_data.value("seqId", "0"));
            ob.timestamp = std::stoull(book_data.value("ts", "0"));
            ob.local_timestamp = currentTimeNs();

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
                        it->order_count = std::stoi(update[3].get<std::string>());
                    } else {
                        PriceLevel level{price, qty, static_cast<uint32_t>(std::stoi(update[3].get<std::string>()))};
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

            if (book_data.contains("bids")) {
                applyUpdates(ob.bids, book_data["bids"], false);
            }
            if (book_data.contains("asks")) {
                applyUpdates(ob.asks, book_data["asks"], true);
            }

            onOrderBook(ob);
        }

    } catch (const std::exception& e) {
        onError("OrderBook update error: " + std::string(e.what()));
    }
}

void OkxExchange::handleTradeUpdate(const nlohmann::json& data) {
    try {
        for (const auto& t : data) {
            Trade trade;
            trade.exchange = "okx";
            trade.symbol = t.value("instId", "");
            trade.trade_id = t.value("tradeId", "");
            trade.price = std::stod(t.value("px", "0"));
            trade.quantity = std::stod(t.value("sz", "0"));
            trade.timestamp = std::stoull(t.value("ts", "0"));
            trade.local_timestamp = currentTimeNs();

            std::string side = t.value("side", "buy");
            trade.side = (side == "buy") ? OrderSide::Buy : OrderSide::Sell;

            onTrade(trade);
        }

    } catch (const std::exception& e) {
        onError("Trade update error: " + std::string(e.what()));
    }
}

void OkxExchange::handleTickerUpdate(const nlohmann::json& data) {
    try {
        for (const auto& t : data) {
            Ticker ticker;
            ticker.exchange = "okx";
            ticker.symbol = t.value("instId", "");
            ticker.last = std::stod(t.value("last", "0"));
            ticker.bid = std::stod(t.value("bidPx", "0"));
            ticker.ask = std::stod(t.value("askPx", "0"));
            ticker.bid_qty = std::stod(t.value("bidSz", "0"));
            ticker.ask_qty = std::stod(t.value("askSz", "0"));
            ticker.high_24h = std::stod(t.value("high24h", "0"));
            ticker.low_24h = std::stod(t.value("low24h", "0"));
            ticker.volume_24h = std::stod(t.value("vol24h", "0"));
            ticker.volume_quote_24h = std::stod(t.value("volCcy24h", "0"));
            ticker.timestamp = std::stoull(t.value("ts", "0"));

            onTicker(ticker);
        }

    } catch (const std::exception& e) {
        onError("Ticker update error: " + std::string(e.what()));
    }
}

void OkxExchange::handleOrderUpdate(const nlohmann::json& data) {
    try {
        Order order;
        order.exchange = "okx";
        order.symbol = data.value("instId", "");
        order.order_id = data.value("ordId", "");
        order.client_order_id = data.value("clOrdId", "");
        order.side = parseOrderSide(data.value("side", "buy"));
        order.type = parseOrderType(data.value("ordType", "limit"));
        order.status = parseOrderStatus(data.value("state", "live"));
        order.time_in_force = parseTimeInForce(data.value("tgtCcy", ""));

        order.quantity = std::stod(data.value("sz", "0"));
        order.filled_quantity = std::stod(data.value("accFillSz", "0"));
        order.remaining_quantity = order.quantity - order.filled_quantity;
        order.price = std::stod(data.value("px", "0"));
        order.average_price = std::stod(data.value("avgPx", "0"));
        order.commission = std::stod(data.value("fee", "0"));
        order.commission_asset = data.value("feeCcy", "");

        order.create_time = std::stoull(data.value("cTime", "0"));
        order.update_time = std::stoull(data.value("uTime", "0"));
        order.reduce_only = data.value("reduceOnly", "false") == "true";
        order.raw = data;

        onOrder(order);

    } catch (const std::exception& e) {
        onError("Order update error: " + std::string(e.what()));
    }
}

void OkxExchange::handlePositionUpdate(const nlohmann::json& data) {
    try {
        Position position;
        position.exchange = "okx";
        position.symbol = data.value("instId", "");

        std::string pos_side = data.value("posSide", "net");
        double pos = std::stod(data.value("pos", "0"));

        if (pos_side == "long" || (pos_side == "net" && pos > 0)) {
            position.side = OrderSide::Buy;
        } else {
            position.side = OrderSide::Sell;
        }

        position.quantity = std::abs(pos);
        position.entry_price = std::stod(data.value("avgPx", "0"));
        position.mark_price = std::stod(data.value("markPx", "0"));
        position.liquidation_price = std::stod(data.value("liqPx", "0"));
        position.unrealized_pnl = std::stod(data.value("upl", "0"));
        position.realized_pnl = std::stod(data.value("realizedPnl", "0"));
        position.leverage = std::stod(data.value("lever", "1"));
        position.margin = std::stod(data.value("margin", "0"));
        position.update_time = std::stoull(data.value("uTime", "0"));

        onPosition(position);

    } catch (const std::exception& e) {
        onError("Position update error: " + std::string(e.what()));
    }
}

void OkxExchange::handleAccountUpdate(const nlohmann::json& data) {
    try {
        auto details = data.value("details", nlohmann::json::array());
        for (const auto& d : details) {
            Balance balance;
            balance.asset = d.value("ccy", "");
            balance.free = std::stod(d.value("availBal", "0"));
            balance.locked = std::stod(d.value("frozenBal", "0"));
            onBalance(balance);
        }
    } catch (const std::exception& e) {
        onError("Account update error: " + std::string(e.what()));
    }
}

// ============================================================================
// Subscription Management
// ============================================================================

bool OkxExchange::subscribeOrderBook(const std::string& symbol, int depth) {
    std::string channel;
    if (depth <= 5) {
        channel = "books5";
    } else if (depth <= 50) {
        channel = "books50-l2-tbt";
    } else {
        channel = "books-l2-tbt";
    }

    nlohmann::json arg;
    arg["channel"] = channel;
    arg["instId"] = symbol;

    sendSubscribe({arg}, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.insert(channel + ":" + symbol);
    return true;
}

bool OkxExchange::unsubscribeOrderBook(const std::string& symbol) {
    std::vector<nlohmann::json> to_unsub;

    {
        std::lock_guard<std::mutex> lock(subscription_mutex_);
        for (const auto& sub : public_subscriptions_) {
            if (sub.find("books") != std::string::npos && sub.find(symbol) != std::string::npos) {
                size_t pos = sub.find(':');
                if (pos != std::string::npos) {
                    nlohmann::json arg;
                    arg["channel"] = sub.substr(0, pos);
                    arg["instId"] = sub.substr(pos + 1);
                    to_unsub.push_back(arg);
                }
            }
        }
    }

    if (!to_unsub.empty()) {
        sendUnsubscribe(to_unsub, false);
    }

    clearOrderBookCache(symbol);
    return true;
}

bool OkxExchange::subscribeTrades(const std::string& symbol) {
    nlohmann::json arg;
    arg["channel"] = "trades";
    arg["instId"] = symbol;

    sendSubscribe({arg}, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.insert("trades:" + symbol);
    return true;
}

bool OkxExchange::unsubscribeTrades(const std::string& symbol) {
    nlohmann::json arg;
    arg["channel"] = "trades";
    arg["instId"] = symbol;

    sendUnsubscribe({arg}, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.erase("trades:" + symbol);
    return true;
}

bool OkxExchange::subscribeTicker(const std::string& symbol) {
    nlohmann::json arg;
    arg["channel"] = "tickers";
    arg["instId"] = symbol;

    sendSubscribe({arg}, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.insert("tickers:" + symbol);
    return true;
}

bool OkxExchange::unsubscribeTicker(const std::string& symbol) {
    nlohmann::json arg;
    arg["channel"] = "tickers";
    arg["instId"] = symbol;

    sendUnsubscribe({arg}, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.erase("tickers:" + symbol);
    return true;
}

bool OkxExchange::subscribeUserData() {
    // Initialize private WebSocket if needed
    if (!private_ws_client_) {
        network::WebSocketConfig ws_config;
        ws_config.url = getPrivateWsUrl();
        ws_config.name = "okx_private_ws";
        ws_config.ping_interval_ms = 25000;
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
    // This will be called after successful auth
    std::vector<nlohmann::json> args;

    nlohmann::json orders_arg;
    orders_arg["channel"] = "orders";
    orders_arg["instType"] = getInstTypeString();
    args.push_back(orders_arg);

    nlohmann::json positions_arg;
    positions_arg["channel"] = "positions";
    positions_arg["instType"] = getInstTypeString();
    args.push_back(positions_arg);

    nlohmann::json account_arg;
    account_arg["channel"] = "account";
    args.push_back(account_arg);

    sendSubscribe(args, true);

    return true;
}

bool OkxExchange::unsubscribeUserData() {
    if (private_ws_client_) {
        private_ws_client_->disconnect();
        private_ws_client_.reset();
    }

    private_ws_connected_ = false;
    private_ws_authenticated_ = false;
    return true;
}

void OkxExchange::sendSubscribe(const std::vector<nlohmann::json>& args, bool is_private) {
    auto client = is_private ? private_ws_client_ : public_ws_client_;
    if (!client) return;

    nlohmann::json msg;
    msg["op"] = "subscribe";
    msg["args"] = args;

    client->send(msg);
}

void OkxExchange::sendUnsubscribe(const std::vector<nlohmann::json>& args, bool is_private) {
    auto client = is_private ? private_ws_client_ : public_ws_client_;
    if (!client) return;

    nlohmann::json msg;
    msg["op"] = "unsubscribe";
    msg["args"] = args;

    client->send(msg);
}

void OkxExchange::authenticatePrivateWs() {
    if (!private_ws_client_) return;

    std::string timestamp = std::to_string(currentTimeMs() / 1000);
    std::string sign_str = timestamp + "GET" + "/users/self/verify";

    std::string signature = sign(timestamp, "GET", "/users/self/verify", "");

    nlohmann::json auth_msg;
    auth_msg["op"] = "login";
    auth_msg["args"] = nlohmann::json::array({
        {
            {"apiKey", okx_config_.api_key},
            {"passphrase", okx_config_.passphrase},
            {"timestamp", timestamp},
            {"sign", signature}
        }
    });

    private_ws_client_->send(auth_msg);
}

// ============================================================================
// REST Helpers
// ============================================================================

network::HttpResponse OkxExchange::signedGet(
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

    network::HttpRequest request;
    request.method = network::HttpMethod::GET;
    request.path = endpoint + query_string;
    request.query_params = params;
    addAuthHeaders(request, "GET", endpoint + query_string, "");

    return rest_client_->execute(request);
}

network::HttpResponse OkxExchange::signedPost(
    const std::string& endpoint,
    const nlohmann::json& body) {

    std::string body_str = body.empty() ? "" : body.dump();

    network::HttpRequest request;
    request.method = network::HttpMethod::POST;
    request.path = endpoint;
    request.body = body_str;
    request.headers["Content-Type"] = "application/json";
    addAuthHeaders(request, "POST", endpoint, body_str);

    return rest_client_->execute(request);
}

network::HttpResponse OkxExchange::publicGet(
    const std::string& endpoint,
    const std::unordered_map<std::string, std::string>& params) {

    return rest_client_->get(endpoint, params);
}

// ============================================================================
// Order Management
// ============================================================================

std::optional<Order> OkxExchange::placeOrder(const OrderRequest& request) {
    nlohmann::json body;
    body["instId"] = request.symbol;
    body["tdMode"] = okx_config_.inst_type == OkxInstType::Spot ? "cash" : "cross";
    body["side"] = orderSideToString(request.side);
    body["ordType"] = orderTypeToString(request.type);
    body["sz"] = std::to_string(request.quantity);

    if (request.type != OrderType::Market) {
        body["px"] = std::to_string(request.price);
    }

    if (!request.client_order_id.empty()) {
        body["clOrdId"] = request.client_order_id;
    }

    if (request.reduce_only) {
        body["reduceOnly"] = true;
    }

    auto response = signedPost("/api/v5/trade/order", body);

    if (!response.success) {
        onError("Place order failed: " + response.error + " " + response.body);
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("code", "1") != "0") {
            onError("Place order failed: " + json.value("msg", "unknown"));
            return std::nullopt;
        }

        auto data = json["data"];
        if (data.empty()) {
            return std::nullopt;
        }

        Order order;
        order.order_id = data[0].value("ordId", "");
        order.client_order_id = data[0].value("clOrdId", "");
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

std::optional<Order> OkxExchange::cancelOrder(const std::string& symbol,
                                              const std::string& order_id) {
    nlohmann::json body;
    body["instId"] = symbol;
    body["ordId"] = order_id;

    auto response = signedPost("/api/v5/trade/cancel-order", body);

    if (!response.success) {
        onError("Cancel order failed: " + response.error);
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("code", "1") != "0") {
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

std::optional<Order> OkxExchange::cancelOrder(const std::string& symbol,
                                              const std::string& order_id,
                                              const std::string& client_order_id) {
    nlohmann::json body;
    body["instId"] = symbol;

    if (!order_id.empty()) {
        body["ordId"] = order_id;
    }
    if (!client_order_id.empty()) {
        body["clOrdId"] = client_order_id;
    }

    auto response = signedPost("/api/v5/trade/cancel-order", body);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("code", "1") != "0") {
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

bool OkxExchange::cancelAllOrders(const std::string& symbol) {
    // OKX requires canceling orders one by one, so get open orders first
    auto orders = getOpenOrders(symbol);
    bool all_cancelled = true;

    for (const auto& order : orders) {
        auto result = cancelOrder(order.symbol, order.order_id);
        if (!result) {
            all_cancelled = false;
        }
    }

    return all_cancelled;
}

std::optional<Order> OkxExchange::getOrder(const std::string& symbol,
                                           const std::string& order_id) {
    std::unordered_map<std::string, std::string> params;
    params["instId"] = symbol;
    params["ordId"] = order_id;

    auto response = signedGet("/api/v5/trade/order", params);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("code", "1") != "0") {
            return std::nullopt;
        }

        auto data = json["data"];
        if (data.empty()) {
            return std::nullopt;
        }

        return parseOrder(data[0]);

    } catch (...) {
        return std::nullopt;
    }
}

std::vector<Order> OkxExchange::getOpenOrders(const std::string& symbol) {
    std::unordered_map<std::string, std::string> params;
    params["instType"] = getInstTypeString();
    if (!symbol.empty()) {
        params["instId"] = symbol;
    }

    auto response = signedGet("/api/v5/trade/orders-pending", params);

    std::vector<Order> orders;
    if (!response.success) {
        return orders;
    }

    try {
        auto json = response.json();
        if (json.value("code", "1") != "0") {
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

std::vector<Order> OkxExchange::getOrderHistory(const std::string& symbol,
                                                uint64_t start_time,
                                                uint64_t end_time,
                                                int limit) {
    std::unordered_map<std::string, std::string> params;
    params["instType"] = getInstTypeString();
    if (!symbol.empty()) {
        params["instId"] = symbol;
    }
    params["limit"] = std::to_string(limit);

    if (start_time > 0) {
        params["begin"] = std::to_string(start_time);
    }
    if (end_time > 0) {
        params["end"] = std::to_string(end_time);
    }

    auto response = signedGet("/api/v5/trade/orders-history", params);

    std::vector<Order> orders;
    if (!response.success) {
        return orders;
    }

    try {
        auto json = response.json();
        if (json.value("code", "1") != "0") {
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

std::optional<Account> OkxExchange::getAccount() {
    auto response = signedGet("/api/v5/account/balance");

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("code", "1") != "0") {
            return std::nullopt;
        }

        Account account;
        account.exchange = "okx";
        account.update_time = currentTimeMs();

        auto data = json["data"];
        if (!data.empty()) {
            auto acc_data = data[0];
            account.total_margin = std::stod(acc_data.value("totalEq", "0"));
            account.available_margin = std::stod(acc_data.value("availBal", "0"));

            auto details = acc_data.value("details", nlohmann::json::array());
            for (const auto& d : details) {
                Balance balance;
                balance.asset = d.value("ccy", "");
                balance.free = std::stod(d.value("availBal", "0"));
                balance.locked = std::stod(d.value("frozenBal", "0"));
                account.balances[balance.asset] = balance;
            }
        }

        // Get positions
        account.positions = getPositions();

        // Calculate total unrealized PnL
        for (const auto& pos : account.positions) {
            account.total_unrealized_pnl += pos.unrealized_pnl;
        }

        return account;

    } catch (const std::exception& e) {
        onError("Parse account failed: " + std::string(e.what()));
        return std::nullopt;
    }
}

std::optional<Balance> OkxExchange::getBalance(const std::string& asset) {
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

std::vector<Position> OkxExchange::getPositions(const std::string& symbol) {
    std::unordered_map<std::string, std::string> params;
    params["instType"] = getInstTypeString();
    if (!symbol.empty()) {
        params["instId"] = symbol;
    }

    auto response = signedGet("/api/v5/account/positions", params);

    std::vector<Position> positions;
    if (!response.success) {
        return positions;
    }

    try {
        auto json = response.json();
        if (json.value("code", "1") != "0") {
            return positions;
        }

        auto data = json["data"];
        for (const auto& p : data) {
            double pos = std::stod(p.value("pos", "0"));
            if (pos == 0.0) continue;

            Position position;
            position.exchange = "okx";
            position.symbol = p.value("instId", "");

            std::string pos_side = p.value("posSide", "net");
            if (pos_side == "long" || (pos_side == "net" && pos > 0)) {
                position.side = OrderSide::Buy;
            } else {
                position.side = OrderSide::Sell;
            }

            position.quantity = std::abs(pos);
            position.entry_price = std::stod(p.value("avgPx", "0"));
            position.mark_price = std::stod(p.value("markPx", "0"));
            position.liquidation_price = std::stod(p.value("liqPx", "0"));
            position.unrealized_pnl = std::stod(p.value("upl", "0"));
            position.realized_pnl = std::stod(p.value("realizedPnl", "0"));
            position.leverage = std::stod(p.value("lever", "1"));
            position.margin = std::stod(p.value("margin", "0"));
            position.update_time = std::stoull(p.value("uTime", "0"));

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

std::vector<SymbolInfo> OkxExchange::getSymbols() {
    std::unordered_map<std::string, std::string> params;
    params["instType"] = getInstTypeString();

    auto response = publicGet("/api/v5/public/instruments", params);

    std::vector<SymbolInfo> symbols;
    if (!response.success) {
        return symbols;
    }

    try {
        auto json = response.json();
        if (json.value("code", "1") != "0") {
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

std::optional<SymbolInfo> OkxExchange::getSymbolInfo(const std::string& symbol) {
    // Check cache first
    {
        std::lock_guard<std::mutex> lock(symbol_info_mutex_);
        auto it = symbol_info_cache_.find(symbol);
        if (it != symbol_info_cache_.end()) {
            return it->second;
        }
    }

    std::unordered_map<std::string, std::string> params;
    params["instType"] = getInstTypeString();
    params["instId"] = symbol;

    auto response = publicGet("/api/v5/public/instruments", params);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("code", "1") != "0") {
            return std::nullopt;
        }

        auto data = json["data"];
        if (!data.empty()) {
            return parseSymbolInfo(data[0]);
        }

    } catch (...) {
        // Fall through
    }

    return std::nullopt;
}

std::optional<OrderBook> OkxExchange::getOrderBookSnapshot(const std::string& symbol, int depth) {
    std::unordered_map<std::string, std::string> params;
    params["instId"] = symbol;
    params["sz"] = std::to_string(depth);

    auto response = publicGet("/api/v5/market/books", params);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("code", "1") != "0") {
            return std::nullopt;
        }

        auto data = json["data"];
        if (data.empty()) {
            return std::nullopt;
        }

        auto book_data = data[0];

        OrderBook ob;
        ob.symbol = symbol;
        ob.exchange = "okx";
        ob.timestamp = std::stoull(book_data.value("ts", "0"));
        ob.local_timestamp = currentTimeNs();

        auto bids = book_data.value("bids", nlohmann::json::array());
        for (const auto& b : bids) {
            PriceLevel level;
            level.price = std::stod(b[0].get<std::string>());
            level.quantity = std::stod(b[1].get<std::string>());
            level.order_count = std::stoi(b[3].get<std::string>());
            ob.bids.push_back(level);
        }

        auto asks = book_data.value("asks", nlohmann::json::array());
        for (const auto& a : asks) {
            PriceLevel level;
            level.price = std::stod(a[0].get<std::string>());
            level.quantity = std::stod(a[1].get<std::string>());
            level.order_count = std::stoi(a[3].get<std::string>());
            ob.asks.push_back(level);
        }

        return ob;

    } catch (...) {
        return std::nullopt;
    }
}

std::vector<Trade> OkxExchange::getRecentTrades(const std::string& symbol, int limit) {
    std::unordered_map<std::string, std::string> params;
    params["instId"] = symbol;
    params["limit"] = std::to_string(limit);

    auto response = publicGet("/api/v5/market/trades", params);

    std::vector<Trade> trades;
    if (!response.success) {
        return trades;
    }

    try {
        auto json = response.json();
        if (json.value("code", "1") != "0") {
            return trades;
        }

        auto data = json["data"];
        for (const auto& t : data) {
            Trade trade;
            trade.exchange = "okx";
            trade.symbol = symbol;
            trade.trade_id = t.value("tradeId", "");
            trade.price = std::stod(t.value("px", "0"));
            trade.quantity = std::stod(t.value("sz", "0"));
            trade.timestamp = std::stoull(t.value("ts", "0"));
            trade.side = t.value("side", "buy") == "buy" ? OrderSide::Buy : OrderSide::Sell;
            trades.push_back(trade);
        }

    } catch (...) {
        // Return empty
    }

    return trades;
}

std::optional<Ticker> OkxExchange::getTicker(const std::string& symbol) {
    std::unordered_map<std::string, std::string> params;
    params["instId"] = symbol;

    auto response = publicGet("/api/v5/market/ticker", params);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        if (json.value("code", "1") != "0") {
            return std::nullopt;
        }

        auto data = json["data"];
        if (data.empty()) {
            return std::nullopt;
        }

        auto t = data[0];

        Ticker ticker;
        ticker.exchange = "okx";
        ticker.symbol = symbol;
        ticker.last = std::stod(t.value("last", "0"));
        ticker.bid = std::stod(t.value("bidPx", "0"));
        ticker.ask = std::stod(t.value("askPx", "0"));
        ticker.bid_qty = std::stod(t.value("bidSz", "0"));
        ticker.ask_qty = std::stod(t.value("askSz", "0"));
        ticker.high_24h = std::stod(t.value("high24h", "0"));
        ticker.low_24h = std::stod(t.value("low24h", "0"));
        ticker.volume_24h = std::stod(t.value("vol24h", "0"));
        ticker.volume_quote_24h = std::stod(t.value("volCcy24h", "0"));
        ticker.timestamp = std::stoull(t.value("ts", "0"));

        return ticker;

    } catch (...) {
        return std::nullopt;
    }
}

uint64_t OkxExchange::getServerTime() {
    auto response = publicGet("/api/v5/public/time");

    if (response.success) {
        auto json = response.json();
        if (json.value("code", "1") == "0") {
            auto data = json["data"];
            if (!data.empty()) {
                return std::stoull(data[0].value("ts", "0"));
            }
        }
    }

    return 0;
}

std::string OkxExchange::formatSymbol(const std::string& base, const std::string& quote) const {
    // OKX format: BTC-USDT, BTC-USDT-SWAP
    std::string symbol = base + "-" + quote;

    switch (okx_config_.inst_type) {
        case OkxInstType::Swap:
            return symbol + "-SWAP";
        case OkxInstType::Futures:
            // For futures, would need contract date
            return symbol;
        default:
            return symbol;
    }
}

std::pair<std::string, std::string> OkxExchange::parseSymbol(const std::string& symbol) const {
    // Parse BTC-USDT or BTC-USDT-SWAP
    size_t first_dash = symbol.find('-');
    if (first_dash == std::string::npos) {
        return {symbol, ""};
    }

    std::string base = symbol.substr(0, first_dash);

    size_t second_dash = symbol.find('-', first_dash + 1);
    std::string quote;

    if (second_dash != std::string::npos) {
        quote = symbol.substr(first_dash + 1, second_dash - first_dash - 1);
    } else {
        quote = symbol.substr(first_dash + 1);
    }

    return {base, quote};
}

// ============================================================================
// Data Converters
// ============================================================================

Order OkxExchange::parseOrder(const nlohmann::json& data) {
    Order order;
    order.exchange = "okx";
    order.symbol = data.value("instId", "");
    order.order_id = data.value("ordId", "");
    order.client_order_id = data.value("clOrdId", "");
    order.side = parseOrderSide(data.value("side", "buy"));
    order.type = parseOrderType(data.value("ordType", "limit"));
    order.status = parseOrderStatus(data.value("state", "live"));

    order.quantity = std::stod(data.value("sz", "0"));
    order.filled_quantity = std::stod(data.value("accFillSz", "0"));
    order.remaining_quantity = order.quantity - order.filled_quantity;
    order.price = std::stod(data.value("px", "0"));
    order.average_price = std::stod(data.value("avgPx", "0"));
    order.commission = std::stod(data.value("fee", "0"));
    order.commission_asset = data.value("feeCcy", "");

    order.create_time = std::stoull(data.value("cTime", "0"));
    order.update_time = std::stoull(data.value("uTime", "0"));
    order.reduce_only = data.value("reduceOnly", "false") == "true";
    order.raw = data;

    return order;
}

OrderStatus OkxExchange::parseOrderStatus(const std::string& status) {
    if (status == "live") return OrderStatus::New;
    if (status == "partially_filled") return OrderStatus::PartiallyFilled;
    if (status == "filled") return OrderStatus::Filled;
    if (status == "canceled" || status == "cancelled") return OrderStatus::Cancelled;
    if (status == "mmp_canceled") return OrderStatus::Cancelled;
    return OrderStatus::Failed;
}

OrderSide OkxExchange::parseOrderSide(const std::string& side) {
    return (side == "buy") ? OrderSide::Buy : OrderSide::Sell;
}

OrderType OkxExchange::parseOrderType(const std::string& type) {
    if (type == "market") return OrderType::Market;
    if (type == "limit") return OrderType::Limit;
    if (type == "post_only") return OrderType::PostOnly;
    if (type == "fok") return OrderType::FillOrKill;
    if (type == "ioc") return OrderType::ImmediateOrCancel;
    return OrderType::Limit;
}

TimeInForce OkxExchange::parseTimeInForce(const std::string& tif) {
    if (tif == "GTC") return TimeInForce::GTC;
    if (tif == "IOC") return TimeInForce::IOC;
    if (tif == "FOK") return TimeInForce::FOK;
    return TimeInForce::GTC;
}

std::string OkxExchange::orderSideToString(OrderSide side) {
    return side == OrderSide::Buy ? "buy" : "sell";
}

std::string OkxExchange::orderTypeToString(OrderType type) {
    switch (type) {
        case OrderType::Market: return "market";
        case OrderType::Limit: return "limit";
        case OrderType::PostOnly: return "post_only";
        case OrderType::FillOrKill: return "fok";
        case OrderType::ImmediateOrCancel: return "ioc";
        default: return "limit";
    }
}

std::string OkxExchange::timeInForceToString(TimeInForce tif) {
    switch (tif) {
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
        default: return "GTC";
    }
}

SymbolInfo OkxExchange::parseSymbolInfo(const nlohmann::json& data) {
    SymbolInfo info;
    info.symbol = data.value("instId", "");
    info.base_asset = data.value("baseCcy", "");
    info.quote_asset = data.value("quoteCcy", "");
    info.trading_enabled = data.value("state", "") == "live";

    info.min_qty = std::stod(data.value("minSz", "0"));
    info.max_qty = std::stod(data.value("maxLmtSz", "0"));
    info.step_size = std::stod(data.value("lotSz", "0"));
    info.tick_size = std::stod(data.value("tickSz", "0"));

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
// OKX-specific Methods
// ============================================================================

bool OkxExchange::setLeverage(const std::string& symbol, int leverage, const std::string& margin_mode) {
    nlohmann::json body;
    body["instId"] = symbol;
    body["lever"] = std::to_string(leverage);
    body["mgnMode"] = margin_mode;

    auto response = signedPost("/api/v5/account/set-leverage", body);
    return response.success && response.json().value("code", "1") == "0";
}

bool OkxExchange::setPositionMode(const std::string& mode) {
    nlohmann::json body;
    body["posMode"] = mode;

    auto response = signedPost("/api/v5/account/set-position-mode", body);
    return response.success && response.json().value("code", "1") == "0";
}

double OkxExchange::getFundingRate(const std::string& symbol) {
    std::unordered_map<std::string, std::string> params;
    params["instId"] = symbol;

    auto response = publicGet("/api/v5/public/funding-rate", params);

    if (response.success) {
        auto json = response.json();
        if (json.value("code", "1") == "0") {
            auto data = json["data"];
            if (!data.empty()) {
                return std::stod(data[0].value("fundingRate", "0"));
            }
        }
    }

    return 0.0;
}

// ============================================================================
// Factory Functions
// ============================================================================

std::shared_ptr<OkxExchange> createOkxExchange(const ExchangeConfig& config) {
    return std::make_shared<OkxExchange>(config);
}

std::shared_ptr<OkxExchange> createOkxExchange(const OkxConfig& config) {
    return std::make_shared<OkxExchange>(config);
}

} // namespace exchange
} // namespace hft
