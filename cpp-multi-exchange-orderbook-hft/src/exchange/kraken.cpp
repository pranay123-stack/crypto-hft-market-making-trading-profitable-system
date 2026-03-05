#include "exchange/kraken.hpp"

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

KrakenExchange::KrakenExchange(const ExchangeConfig& config)
    : ExchangeBase(config) {
    kraken_config_.api_key = config.api_key;
    kraken_config_.api_secret = config.api_secret;
    kraken_config_.testnet = config.testnet;
    kraken_config_.spot = (config.type == ExchangeType::Spot);
    kraken_config_.order_rate_limit = config.orders_per_second;
    kraken_config_.request_rate_limit = config.requests_per_minute;

    // Initialize nonce with current timestamp in microseconds
    nonce_ = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );

    // Initialize REST client
    network::RestClientConfig rest_config;
    rest_config.base_url = getRestUrl();
    rest_config.name = "kraken_rest";
    rest_config.request_timeout_ms = config.rest_timeout_ms;
    rest_config.max_retries = config.rest_max_retries;
    rest_config.rate_limit.requests_per_second = kraken_config_.order_rate_limit;
    rest_config.rate_limit.requests_per_minute = kraken_config_.request_rate_limit;

    rest_client_ = std::make_shared<network::RestClient>(rest_config);
}

KrakenExchange::KrakenExchange(const KrakenConfig& kraken_config)
    : ExchangeBase(ExchangeConfig{
        .name = "Kraken",
        .api_key = kraken_config.api_key,
        .api_secret = kraken_config.api_secret,
        .type = kraken_config.spot ? ExchangeType::Spot : ExchangeType::Perpetual,
        .testnet = kraken_config.testnet
      }),
      kraken_config_(kraken_config) {

    // Initialize nonce
    nonce_ = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );

    // Initialize REST client
    network::RestClientConfig rest_config;
    rest_config.base_url = getRestUrl();
    rest_config.name = "kraken_rest";
    rest_config.rate_limit.requests_per_second = kraken_config_.order_rate_limit;
    rest_config.rate_limit.requests_per_minute = kraken_config_.request_rate_limit;

    rest_client_ = std::make_shared<network::RestClient>(rest_config);
}

KrakenExchange::~KrakenExchange() {
    disconnect();
}

// ============================================================================
// URL Builders
// ============================================================================

std::string KrakenExchange::getRestUrl() const {
    // Kraken doesn't have a separate testnet URL for REST
    return "https://api.kraken.com";
}

std::string KrakenExchange::getPublicWsUrl() const {
    // WebSocket v2 public endpoint
    return "wss://ws.kraken.com/v2";
}

std::string KrakenExchange::getPrivateWsUrl() const {
    // WebSocket v2 private endpoint (requires token authentication)
    return "wss://ws-auth.kraken.com/v2";
}

// ============================================================================
// Connection Management
// ============================================================================

bool KrakenExchange::connect() {
    if (public_ws_connected_.load()) {
        return true;
    }

    onConnectionStatus(ConnectionStatus::Connecting);

    // Initialize public WebSocket client
    network::WebSocketConfig ws_config;
    ws_config.url = getPublicWsUrl();
    ws_config.name = "kraken_public_ws";
    ws_config.ping_interval_ms = 30000;  // Kraken recommends ping every 30s
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

void KrakenExchange::disconnect() {
    stopTokenRefresh();

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

bool KrakenExchange::isConnected() const {
    return public_ws_connected_.load();
}

ConnectionStatus KrakenExchange::connectionStatus() const {
    return connection_status_.load();
}

// ============================================================================
// Signature Generation (Kraken HMAC-SHA512 with nonce)
// ============================================================================

std::string KrakenExchange::generateNonce() {
    // Increment and return nonce
    return std::to_string(nonce_++);
}

std::string KrakenExchange::sign(const std::string& path, const std::string& nonce,
                                  const std::string& post_data) {
    // Step 1: SHA256 of nonce + post_data
    std::string nonce_post = nonce + post_data;

    unsigned char sha256_hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(nonce_post.c_str()),
           nonce_post.length(), sha256_hash);

    // Step 2: Concatenate path with SHA256 hash
    std::string path_hash = path;
    path_hash.append(reinterpret_cast<const char*>(sha256_hash), SHA256_DIGEST_LENGTH);

    // Step 3: Base64 decode the API secret
    BIO* bio = BIO_new_mem_buf(kraken_config_.api_secret.c_str(),
                               static_cast<int>(kraken_config_.api_secret.length()));
    BIO* b64 = BIO_new(BIO_f_base64());
    bio = BIO_push(b64, bio);
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);

    std::vector<unsigned char> decoded_secret(kraken_config_.api_secret.length());
    int decoded_len = BIO_read(bio, decoded_secret.data(),
                               static_cast<int>(decoded_secret.size()));
    BIO_free_all(bio);

    if (decoded_len <= 0) {
        onError("Failed to decode API secret");
        return "";
    }

    // Step 4: HMAC-SHA512 of path+sha256hash with decoded secret
    unsigned char hmac_hash[EVP_MAX_MD_SIZE];
    unsigned int hmac_len;

    HMAC(EVP_sha512(),
         decoded_secret.data(), decoded_len,
         reinterpret_cast<const unsigned char*>(path_hash.c_str()),
         path_hash.length(),
         hmac_hash, &hmac_len);

    // Step 5: Base64 encode the result
    BIO* b64_out = BIO_new(BIO_f_base64());
    BIO* bio_mem = BIO_new(BIO_s_mem());
    bio_mem = BIO_push(b64_out, bio_mem);

    BIO_set_flags(bio_mem, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(bio_mem, hmac_hash, hmac_len);
    BIO_flush(bio_mem);

    BUF_MEM* buffer_ptr;
    BIO_get_mem_ptr(bio_mem, &buffer_ptr);

    std::string signature(buffer_ptr->data, buffer_ptr->length);
    BIO_free_all(bio_mem);

    return signature;
}

void KrakenExchange::addAuthHeaders(network::HttpRequest& request, const std::string& path,
                                     const std::string& nonce, const std::string& post_data) {
    std::string signature = sign(path, nonce, post_data);

    request.headers["API-Key"] = kraken_config_.api_key;
    request.headers["API-Sign"] = signature;
    request.headers["Content-Type"] = "application/x-www-form-urlencoded";
}

// ============================================================================
// WebSocket Handlers
// ============================================================================

void KrakenExchange::handleWsOpen(bool is_private) {
    if (is_private) {
        private_ws_connected_ = true;
        private_ws_authenticated_ = true;  // Token was included in URL or sent with subscribe
    } else {
        public_ws_connected_ = true;
        onConnectionStatus(ConnectionStatus::Connected);
    }
}

void KrakenExchange::handleWsClose(int code, const std::string& reason, bool is_private) {
    if (is_private) {
        private_ws_connected_ = false;
        private_ws_authenticated_ = false;
    } else {
        public_ws_connected_ = false;
        onConnectionStatus(ConnectionStatus::Reconnecting);
    }
}

void KrakenExchange::handleWsError(const std::string& error, bool is_private) {
    onError("WebSocket error (" + std::string(is_private ? "private" : "public") + "): " + error);
}

void KrakenExchange::handlePublicWsMessage(const std::string& message, network::MessageType type) {
    try {
        auto json = nlohmann::json::parse(message);

        // Check message type (WebSocket v2 format)
        std::string method = json.value("method", "");
        std::string channel = json.value("channel", "");

        // Handle subscription responses
        if (method == "subscribe" || method == "unsubscribe") {
            bool success = json.value("success", false);
            if (!success) {
                std::string error = json.value("error", "Unknown error");
                onError("Subscription error: " + error);
            }
            return;
        }

        // Handle pong
        if (method == "pong") {
            return;
        }

        // Handle channel data
        if (channel == "book") {
            std::string msg_type = json.value("type", "");
            if (msg_type == "snapshot") {
                handleOrderBookSnapshot(json);
            } else if (msg_type == "update") {
                handleOrderBookUpdate(json);
            }
        } else if (channel == "trade") {
            handleTradeUpdate(json);
        } else if (channel == "ticker") {
            handleTickerUpdate(json);
        } else if (channel == "ohlc") {
            handleOHLCUpdate(json);
        }

    } catch (const std::exception& e) {
        onError("Public WS message parse error: " + std::string(e.what()));
    }
}

void KrakenExchange::handlePrivateWsMessage(const std::string& message, network::MessageType type) {
    try {
        auto json = nlohmann::json::parse(message);

        std::string method = json.value("method", "");
        std::string channel = json.value("channel", "");

        // Handle subscription responses
        if (method == "subscribe" || method == "unsubscribe") {
            bool success = json.value("success", false);
            if (!success) {
                std::string error = json.value("error", "Unknown error");
                onError("Private subscription error: " + error);
            }
            return;
        }

        // Handle channel data
        if (channel == "executions") {
            handleExecutionUpdate(json);
        } else if (channel == "balances") {
            handleBalanceUpdate(json);
        }

    } catch (const std::exception& e) {
        onError("Private WS message parse error: " + std::string(e.what()));
    }
}

void KrakenExchange::handleOrderBookSnapshot(const nlohmann::json& json) {
    try {
        auto data = json["data"];
        if (data.empty()) return;

        auto book_data = data[0];
        std::string symbol = book_data.value("symbol", "");

        OrderBook ob;
        ob.symbol = fromKrakenSymbol(symbol);
        ob.exchange = "kraken";
        ob.timestamp = currentTimeMs();
        ob.local_timestamp = currentTimeNs();

        // Parse bids
        auto bids = book_data.value("bids", nlohmann::json::array());
        for (const auto& b : bids) {
            PriceLevel level;
            level.price = std::stod(b.value("price", "0"));
            level.quantity = std::stod(b.value("qty", "0"));
            ob.bids.push_back(level);
        }

        // Parse asks
        auto asks = book_data.value("asks", nlohmann::json::array());
        for (const auto& a : asks) {
            PriceLevel level;
            level.price = std::stod(a.value("price", "0"));
            level.quantity = std::stod(a.value("qty", "0"));
            ob.asks.push_back(level);
        }

        // Update sequence tracking
        if (book_data.contains("checksum")) {
            std::lock_guard<std::mutex> lock(orderbook_seq_mutex_);
            orderbook_seq_[ob.symbol] = book_data["checksum"].get<uint64_t>();
        }

        onOrderBook(ob);

    } catch (const std::exception& e) {
        onError("OrderBook snapshot error: " + std::string(e.what()));
    }
}

void KrakenExchange::handleOrderBookUpdate(const nlohmann::json& json) {
    try {
        auto data = json["data"];
        if (data.empty()) return;

        auto book_data = data[0];
        std::string kraken_symbol = book_data.value("symbol", "");
        std::string symbol = fromKrakenSymbol(kraken_symbol);

        // Get cached orderbook
        auto cached = getCachedOrderBook(symbol);
        if (!cached) {
            return;
        }

        OrderBook ob = *cached;
        ob.timestamp = currentTimeMs();
        ob.local_timestamp = currentTimeNs();

        // Apply updates
        auto applyUpdates = [](std::vector<PriceLevel>& levels,
                              const nlohmann::json& updates,
                              bool ascending) {
            for (const auto& update : updates) {
                double price = std::stod(update.value("price", "0"));
                double qty = std::stod(update.value("qty", "0"));

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

        if (book_data.contains("bids")) {
            applyUpdates(ob.bids, book_data["bids"], false);
        }
        if (book_data.contains("asks")) {
            applyUpdates(ob.asks, book_data["asks"], true);
        }

        onOrderBook(ob);

    } catch (const std::exception& e) {
        onError("OrderBook update error: " + std::string(e.what()));
    }
}

void KrakenExchange::handleTradeUpdate(const nlohmann::json& json) {
    try {
        auto data = json["data"];

        for (const auto& t : data) {
            Trade trade;
            trade.exchange = "kraken";
            trade.symbol = fromKrakenSymbol(t.value("symbol", ""));
            trade.trade_id = std::to_string(t.value("trade_id", uint64_t(0)));
            trade.price = std::stod(t.value("price", "0"));
            trade.quantity = std::stod(t.value("qty", "0"));

            std::string timestamp_str = t.value("timestamp", "");
            if (!timestamp_str.empty()) {
                // Parse ISO 8601 timestamp
                trade.timestamp = currentTimeMs();  // Simplified
            }
            trade.local_timestamp = currentTimeNs();

            std::string side = t.value("side", "buy");
            trade.side = (side == "buy") ? OrderSide::Buy : OrderSide::Sell;

            onTrade(trade);
        }

    } catch (const std::exception& e) {
        onError("Trade update error: " + std::string(e.what()));
    }
}

void KrakenExchange::handleTickerUpdate(const nlohmann::json& json) {
    try {
        auto data = json["data"];

        for (const auto& t : data) {
            Ticker ticker;
            ticker.exchange = "kraken";
            ticker.symbol = fromKrakenSymbol(t.value("symbol", ""));
            ticker.last = std::stod(t.value("last", "0"));
            ticker.bid = std::stod(t.value("bid", "0"));
            ticker.ask = std::stod(t.value("ask", "0"));
            ticker.bid_qty = std::stod(t.value("bid_qty", "0"));
            ticker.ask_qty = std::stod(t.value("ask_qty", "0"));
            ticker.volume_24h = std::stod(t.value("volume", "0"));
            ticker.high_24h = std::stod(t.value("high", "0"));
            ticker.low_24h = std::stod(t.value("low", "0"));
            ticker.change_24h = std::stod(t.value("change", "0"));
            ticker.change_24h_pct = std::stod(t.value("change_pct", "0"));
            ticker.timestamp = currentTimeMs();

            onTicker(ticker);
        }

    } catch (const std::exception& e) {
        onError("Ticker update error: " + std::string(e.what()));
    }
}

void KrakenExchange::handleOHLCUpdate(const nlohmann::json& json) {
    // OHLC updates - can be extended if needed
}

void KrakenExchange::handleExecutionUpdate(const nlohmann::json& json) {
    try {
        auto data = json["data"];

        for (const auto& exec : data) {
            std::string exec_type = exec.value("exec_type", "");

            if (exec_type == "pending_new" || exec_type == "new" ||
                exec_type == "filled" || exec_type == "partial" ||
                exec_type == "canceled" || exec_type == "expired") {

                Order order;
                order.exchange = "kraken";
                order.order_id = exec.value("order_id", "");
                order.client_order_id = exec.value("cl_ord_id", "");
                order.symbol = fromKrakenSymbol(exec.value("symbol", ""));
                order.side = parseOrderSide(exec.value("side", "buy"));
                order.type = parseOrderType(exec.value("order_type", "limit"));
                order.quantity = std::stod(exec.value("order_qty", "0"));
                order.filled_quantity = std::stod(exec.value("cum_qty", "0"));
                order.remaining_quantity = std::stod(exec.value("leaves_qty", "0"));
                order.price = std::stod(exec.value("limit_price", "0"));
                order.average_price = std::stod(exec.value("avg_price", "0"));
                order.commission = std::stod(exec.value("fee_paid", "0"));
                order.commission_asset = exec.value("fee_ccy", "");

                if (exec_type == "pending_new") {
                    order.status = OrderStatus::Pending;
                } else if (exec_type == "new") {
                    order.status = OrderStatus::New;
                } else if (exec_type == "filled") {
                    order.status = OrderStatus::Filled;
                } else if (exec_type == "partial") {
                    order.status = OrderStatus::PartiallyFilled;
                } else if (exec_type == "canceled") {
                    order.status = OrderStatus::Cancelled;
                } else if (exec_type == "expired") {
                    order.status = OrderStatus::Expired;
                }

                order.raw = exec;
                onOrder(order);
            }
        }

    } catch (const std::exception& e) {
        onError("Execution update error: " + std::string(e.what()));
    }
}

void KrakenExchange::handleBalanceUpdate(const nlohmann::json& json) {
    try {
        auto data = json["data"];

        for (const auto& bal : data) {
            auto balances = bal.value("balances", nlohmann::json::array());
            for (const auto& b : balances) {
                Balance balance;
                balance.asset = b.value("asset", "");
                balance.free = std::stod(b.value("balance", "0"));
                balance.locked = std::stod(b.value("hold_trade", "0"));
                onBalance(balance);
            }
        }

    } catch (const std::exception& e) {
        onError("Balance update error: " + std::string(e.what()));
    }
}

// ============================================================================
// Subscription Management
// ============================================================================

bool KrakenExchange::subscribeOrderBook(const std::string& symbol, int depth) {
    std::string kraken_symbol = toKrakenSymbol(symbol);

    nlohmann::json subscription;
    subscription["method"] = "subscribe";
    subscription["params"] = {
        {"channel", "book"},
        {"symbol", nlohmann::json::array({kraken_symbol})},
        {"depth", depth}
    };
    subscription["req_id"] = next_req_id_++;

    sendSubscribe(subscription, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.insert("book:" + symbol);
    return true;
}

bool KrakenExchange::unsubscribeOrderBook(const std::string& symbol) {
    std::string kraken_symbol = toKrakenSymbol(symbol);

    nlohmann::json subscription;
    subscription["method"] = "unsubscribe";
    subscription["params"] = {
        {"channel", "book"},
        {"symbol", nlohmann::json::array({kraken_symbol})}
    };
    subscription["req_id"] = next_req_id_++;

    sendUnsubscribe(subscription, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.erase("book:" + symbol);
    clearOrderBookCache(symbol);
    return true;
}

bool KrakenExchange::subscribeTrades(const std::string& symbol) {
    std::string kraken_symbol = toKrakenSymbol(symbol);

    nlohmann::json subscription;
    subscription["method"] = "subscribe";
    subscription["params"] = {
        {"channel", "trade"},
        {"symbol", nlohmann::json::array({kraken_symbol})}
    };
    subscription["req_id"] = next_req_id_++;

    sendSubscribe(subscription, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.insert("trade:" + symbol);
    return true;
}

bool KrakenExchange::unsubscribeTrades(const std::string& symbol) {
    std::string kraken_symbol = toKrakenSymbol(symbol);

    nlohmann::json subscription;
    subscription["method"] = "unsubscribe";
    subscription["params"] = {
        {"channel", "trade"},
        {"symbol", nlohmann::json::array({kraken_symbol})}
    };
    subscription["req_id"] = next_req_id_++;

    sendUnsubscribe(subscription, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.erase("trade:" + symbol);
    return true;
}

bool KrakenExchange::subscribeTicker(const std::string& symbol) {
    std::string kraken_symbol = toKrakenSymbol(symbol);

    nlohmann::json subscription;
    subscription["method"] = "subscribe";
    subscription["params"] = {
        {"channel", "ticker"},
        {"symbol", nlohmann::json::array({kraken_symbol})}
    };
    subscription["req_id"] = next_req_id_++;

    sendSubscribe(subscription, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.insert("ticker:" + symbol);
    return true;
}

bool KrakenExchange::unsubscribeTicker(const std::string& symbol) {
    std::string kraken_symbol = toKrakenSymbol(symbol);

    nlohmann::json subscription;
    subscription["method"] = "unsubscribe";
    subscription["params"] = {
        {"channel", "ticker"},
        {"symbol", nlohmann::json::array({kraken_symbol})}
    };
    subscription["req_id"] = next_req_id_++;

    sendUnsubscribe(subscription, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.erase("ticker:" + symbol);
    return true;
}

bool KrakenExchange::subscribeOHLC(const std::string& symbol, int interval) {
    std::string kraken_symbol = toKrakenSymbol(symbol);

    nlohmann::json subscription;
    subscription["method"] = "subscribe";
    subscription["params"] = {
        {"channel", "ohlc"},
        {"symbol", nlohmann::json::array({kraken_symbol})},
        {"interval", interval}
    };
    subscription["req_id"] = next_req_id_++;

    sendSubscribe(subscription, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.insert("ohlc:" + symbol);
    return true;
}

bool KrakenExchange::unsubscribeOHLC(const std::string& symbol) {
    std::string kraken_symbol = toKrakenSymbol(symbol);

    nlohmann::json subscription;
    subscription["method"] = "unsubscribe";
    subscription["params"] = {
        {"channel", "ohlc"},
        {"symbol", nlohmann::json::array({kraken_symbol})}
    };
    subscription["req_id"] = next_req_id_++;

    sendUnsubscribe(subscription, false);

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    public_subscriptions_.erase("ohlc:" + symbol);
    return true;
}

bool KrakenExchange::subscribeUserData() {
    // Get WebSocket token first
    ws_token_ = getWebSocketsToken();
    if (ws_token_.empty()) {
        onError("Failed to get WebSocket token");
        return false;
    }

    // Initialize private WebSocket if needed
    if (!private_ws_client_) {
        network::WebSocketConfig ws_config;
        ws_config.url = getPrivateWsUrl();
        ws_config.name = "kraken_private_ws";
        ws_config.ping_interval_ms = 30000;
        ws_config.auto_reconnect = true;

        private_ws_client_ = std::make_shared<network::WebSocketClient>(ws_config);

        private_ws_client_->setOnOpen([this]() {
            this->handleWsOpen(true);

            // Subscribe to executions with token
            nlohmann::json exec_sub;
            exec_sub["method"] = "subscribe";
            exec_sub["params"] = {
                {"channel", "executions"},
                {"token", ws_token_},
                {"snap_orders", true},
                {"snap_trades", false}
            };
            exec_sub["req_id"] = next_req_id_++;
            sendSubscribe(exec_sub, true);

            // Subscribe to balances with token
            nlohmann::json bal_sub;
            bal_sub["method"] = "subscribe";
            bal_sub["params"] = {
                {"channel", "balances"},
                {"token", ws_token_}
            };
            bal_sub["req_id"] = next_req_id_++;
            sendSubscribe(bal_sub, true);
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

    startTokenRefresh();
    return true;
}

bool KrakenExchange::unsubscribeUserData() {
    stopTokenRefresh();

    if (private_ws_client_) {
        private_ws_client_->disconnect();
        private_ws_client_.reset();
    }

    private_ws_connected_ = false;
    private_ws_authenticated_ = false;
    return true;
}

void KrakenExchange::sendSubscribe(const nlohmann::json& subscription, bool is_private) {
    auto client = is_private ? private_ws_client_ : public_ws_client_;
    if (!client) return;

    client->send(subscription);
}

void KrakenExchange::sendUnsubscribe(const nlohmann::json& subscription, bool is_private) {
    auto client = is_private ? private_ws_client_ : public_ws_client_;
    if (!client) return;

    client->send(subscription);
}

void KrakenExchange::resubscribeAll() {
    // Resubscribe to all tracked subscriptions
    std::lock_guard<std::mutex> lock(subscription_mutex_);
    for (const auto& sub : public_subscriptions_) {
        size_t pos = sub.find(':');
        if (pos != std::string::npos) {
            std::string channel = sub.substr(0, pos);
            std::string symbol = sub.substr(pos + 1);

            if (channel == "book") {
                subscribeOrderBook(symbol, kraken_config_.orderbook_depth);
            } else if (channel == "trade") {
                subscribeTrades(symbol);
            } else if (channel == "ticker") {
                subscribeTicker(symbol);
            }
        }
    }
}

// ============================================================================
// Token Refresh Management
// ============================================================================

void KrakenExchange::startTokenRefresh() {
    if (token_refresh_running_.load()) {
        return;
    }

    token_refresh_running_ = true;
    token_refresh_thread_ = std::make_unique<std::thread>([this]() {
        while (token_refresh_running_.load()) {
            // Token expires after about 15 minutes, refresh every 10 minutes
            std::this_thread::sleep_for(std::chrono::minutes(10));

            if (!token_refresh_running_.load()) {
                break;
            }

            std::string new_token = getWebSocketsToken();
            if (!new_token.empty()) {
                std::lock_guard<std::mutex> lock(token_mutex_);
                ws_token_ = new_token;
            } else {
                onError("Failed to refresh WebSocket token");
            }
        }
    });
}

void KrakenExchange::stopTokenRefresh() {
    token_refresh_running_ = false;

    if (token_refresh_thread_ && token_refresh_thread_->joinable()) {
        token_refresh_thread_->join();
    }
    token_refresh_thread_.reset();
}

// ============================================================================
// REST Helpers
// ============================================================================

network::HttpResponse KrakenExchange::signedPost(
    const std::string& endpoint,
    const std::unordered_map<std::string, std::string>& params) {

    std::string nonce = generateNonce();

    // Build POST data
    std::stringstream ss;
    ss << "nonce=" << nonce;
    for (const auto& [key, value] : params) {
        ss << "&" << key << "=" << value;
    }
    std::string post_data = ss.str();

    network::HttpRequest request;
    request.method = network::HttpMethod::POST;
    request.path = endpoint;
    request.body = post_data;
    addAuthHeaders(request, endpoint, nonce, post_data);

    return rest_client_->execute(request);
}

network::HttpResponse KrakenExchange::publicGet(
    const std::string& endpoint,
    const std::unordered_map<std::string, std::string>& params) {

    return rest_client_->get(endpoint, params);
}

// ============================================================================
// Order Management
// ============================================================================

std::optional<Order> KrakenExchange::placeOrder(const OrderRequest& request) {
    std::unordered_map<std::string, std::string> params;
    params["pair"] = toKrakenSymbol(request.symbol);
    params["type"] = orderSideToString(request.side);
    params["ordertype"] = orderTypeToString(request.type);
    params["volume"] = std::to_string(request.quantity);

    if (request.type != OrderType::Market) {
        params["price"] = std::to_string(request.price);
    }

    if (request.stop_price > 0.0) {
        params["price2"] = std::to_string(request.stop_price);
    }

    if (!request.client_order_id.empty()) {
        params["userref"] = request.client_order_id;
    }

    // Time in force
    std::string oflags;
    if (request.time_in_force == TimeInForce::PostOnly) {
        oflags = "post";
    } else if (request.time_in_force == TimeInForce::IOC) {
        params["timeinforce"] = "IOC";
    } else if (request.time_in_force == TimeInForce::FOK) {
        params["timeinforce"] = "FOK";
    }

    if (request.reduce_only) {
        if (!oflags.empty()) oflags += ",";
        oflags += "fciq";  // Fee in quote currency
    }

    if (!oflags.empty()) {
        params["oflags"] = oflags;
    }

    auto response = signedPost("/0/private/AddOrder", params);

    if (!response.success) {
        onError("Place order failed: " + response.error + " " + response.body);
        return std::nullopt;
    }

    try {
        auto json = response.json();

        auto errors = json.value("error", nlohmann::json::array());
        if (!errors.empty()) {
            onError("Place order failed: " + errors[0].get<std::string>());
            return std::nullopt;
        }

        auto result = json["result"];
        auto txid = result["txid"];

        Order order;
        if (txid.is_array() && !txid.empty()) {
            order.order_id = txid[0].get<std::string>();
        }
        order.symbol = request.symbol;
        order.side = request.side;
        order.type = request.type;
        order.quantity = request.quantity;
        order.price = request.price;
        order.status = OrderStatus::New;
        order.client_order_id = request.client_order_id;

        return order;

    } catch (const std::exception& e) {
        onError("Parse order response failed: " + std::string(e.what()));
        return std::nullopt;
    }
}

std::optional<Order> KrakenExchange::cancelOrder(const std::string& symbol,
                                                  const std::string& order_id) {
    std::unordered_map<std::string, std::string> params;
    params["txid"] = order_id;

    auto response = signedPost("/0/private/CancelOrder", params);

    if (!response.success) {
        onError("Cancel order failed: " + response.error);
        return std::nullopt;
    }

    try {
        auto json = response.json();

        auto errors = json.value("error", nlohmann::json::array());
        if (!errors.empty()) {
            onError("Cancel order failed: " + errors[0].get<std::string>());
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

std::optional<Order> KrakenExchange::cancelOrder(const std::string& symbol,
                                                  const std::string& order_id,
                                                  const std::string& client_order_id) {
    // Kraken can cancel by txid or userref
    std::unordered_map<std::string, std::string> params;

    if (!order_id.empty()) {
        params["txid"] = order_id;
    } else if (!client_order_id.empty()) {
        // For userref, need to get order info first then cancel by txid
        // Or use CancelOrderBatch with cl_ord_id (WebSocket v2 feature)
        params["txid"] = client_order_id;
    }

    auto response = signedPost("/0/private/CancelOrder", params);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();

        auto errors = json.value("error", nlohmann::json::array());
        if (!errors.empty()) {
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

bool KrakenExchange::cancelAllOrders(const std::string& symbol) {
    auto response = signedPost("/0/private/CancelAll");

    if (!response.success) {
        return false;
    }

    try {
        auto json = response.json();
        auto errors = json.value("error", nlohmann::json::array());
        return errors.empty();
    } catch (...) {
        return false;
    }
}

std::optional<Order> KrakenExchange::getOrder(const std::string& symbol,
                                               const std::string& order_id) {
    std::unordered_map<std::string, std::string> params;
    params["txid"] = order_id;

    auto response = signedPost("/0/private/QueryOrders", params);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();

        auto errors = json.value("error", nlohmann::json::array());
        if (!errors.empty()) {
            return std::nullopt;
        }

        auto result = json["result"];
        if (result.contains(order_id)) {
            return parseOrder(order_id, result[order_id]);
        }

    } catch (...) {
        // Fall through
    }

    return std::nullopt;
}

std::vector<Order> KrakenExchange::getOpenOrders(const std::string& symbol) {
    auto response = signedPost("/0/private/OpenOrders");

    std::vector<Order> orders;
    if (!response.success) {
        return orders;
    }

    try {
        auto json = response.json();

        auto errors = json.value("error", nlohmann::json::array());
        if (!errors.empty()) {
            return orders;
        }

        auto result = json["result"]["open"];
        for (auto& [txid, order_data] : result.items()) {
            auto order = parseOrder(txid, order_data);

            // Filter by symbol if specified
            if (symbol.empty() || order.symbol == symbol) {
                orders.push_back(order);
            }
        }

    } catch (...) {
        // Return empty
    }

    return orders;
}

std::vector<Order> KrakenExchange::getOrderHistory(const std::string& symbol,
                                                    uint64_t start_time,
                                                    uint64_t end_time,
                                                    int limit) {
    std::unordered_map<std::string, std::string> params;

    if (start_time > 0) {
        params["start"] = std::to_string(start_time / 1000);  // Convert ms to s
    }
    if (end_time > 0) {
        params["end"] = std::to_string(end_time / 1000);
    }

    auto response = signedPost("/0/private/ClosedOrders", params);

    std::vector<Order> orders;
    if (!response.success) {
        return orders;
    }

    try {
        auto json = response.json();

        auto errors = json.value("error", nlohmann::json::array());
        if (!errors.empty()) {
            return orders;
        }

        auto result = json["result"]["closed"];
        int count = 0;
        for (auto& [txid, order_data] : result.items()) {
            if (count >= limit) break;

            auto order = parseOrder(txid, order_data);

            // Filter by symbol if specified
            if (symbol.empty() || order.symbol == symbol) {
                orders.push_back(order);
                count++;
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

std::optional<Account> KrakenExchange::getAccount() {
    auto response = signedPost("/0/private/Balance");

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();

        auto errors = json.value("error", nlohmann::json::array());
        if (!errors.empty()) {
            return std::nullopt;
        }

        Account account;
        account.exchange = "kraken";
        account.update_time = currentTimeMs();

        auto result = json["result"];
        for (auto& [asset, balance_val] : result.items()) {
            Balance balance;
            balance.asset = asset;
            balance.free = std::stod(balance_val.get<std::string>());
            balance.locked = 0.0;  // Need TradeBalance for locked amounts
            account.balances[asset] = balance;
        }

        return account;

    } catch (const std::exception& e) {
        onError("Parse account failed: " + std::string(e.what()));
        return std::nullopt;
    }
}

std::optional<Account> KrakenExchange::getTradeBalance(const std::string& asset) {
    std::unordered_map<std::string, std::string> params;
    params["asset"] = asset;

    auto response = signedPost("/0/private/TradeBalance", params);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();

        auto errors = json.value("error", nlohmann::json::array());
        if (!errors.empty()) {
            return std::nullopt;
        }

        Account account;
        account.exchange = "kraken";
        account.update_time = currentTimeMs();

        auto result = json["result"];
        account.total_margin = std::stod(result.value("eb", "0"));  // Equivalent balance
        account.available_margin = std::stod(result.value("mf", "0"));  // Free margin
        account.total_unrealized_pnl = std::stod(result.value("n", "0"));  // Net PnL

        return account;

    } catch (const std::exception& e) {
        onError("Parse trade balance failed: " + std::string(e.what()));
        return std::nullopt;
    }
}

std::optional<Balance> KrakenExchange::getBalance(const std::string& asset) {
    auto account = getAccount();
    if (!account) {
        return std::nullopt;
    }

    // Try exact match first
    auto it = account->balances.find(asset);
    if (it != account->balances.end()) {
        return it->second;
    }

    // Try with Kraken prefix (X for crypto, Z for fiat)
    std::string kraken_asset = "X" + asset;
    it = account->balances.find(kraken_asset);
    if (it != account->balances.end()) {
        return it->second;
    }

    kraken_asset = "Z" + asset;
    it = account->balances.find(kraken_asset);
    if (it != account->balances.end()) {
        return it->second;
    }

    return std::nullopt;
}

std::vector<Position> KrakenExchange::getPositions(const std::string& symbol) {
    auto response = signedPost("/0/private/OpenPositions");

    std::vector<Position> positions;
    if (!response.success) {
        return positions;
    }

    try {
        auto json = response.json();

        auto errors = json.value("error", nlohmann::json::array());
        if (!errors.empty()) {
            return positions;
        }

        auto result = json["result"];
        for (auto& [pos_id, pos_data] : result.items()) {
            Position position;
            position.exchange = "kraken";
            position.symbol = fromKrakenSymbol(pos_data.value("pair", ""));

            std::string type = pos_data.value("type", "buy");
            double vol = std::stod(pos_data.value("vol", "0"));

            position.side = (type == "buy") ? OrderSide::Buy : OrderSide::Sell;
            position.quantity = vol;
            position.entry_price = std::stod(pos_data.value("cost", "0")) / vol;
            position.unrealized_pnl = std::stod(pos_data.value("net", "0"));
            position.margin = std::stod(pos_data.value("margin", "0"));

            if (symbol.empty() || position.symbol == symbol) {
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

std::vector<SymbolInfo> KrakenExchange::getSymbols() {
    auto response = publicGet("/0/public/AssetPairs");

    std::vector<SymbolInfo> symbols;
    if (!response.success) {
        return symbols;
    }

    try {
        auto json = response.json();

        auto errors = json.value("error", nlohmann::json::array());
        if (!errors.empty()) {
            return symbols;
        }

        auto result = json["result"];
        for (auto& [pair_name, pair_data] : result.items()) {
            auto info = parseSymbolInfo(pair_name, pair_data);
            updateSymbolInfo(info);
            symbols.push_back(info);

            // Cache symbol mapping
            std::string altname = pair_data.value("altname", "");
            std::string wsname = pair_data.value("wsname", "");
            if (!altname.empty()) {
                std::lock_guard<std::mutex> lock(symbol_map_mutex_);
                symbol_to_kraken_[altname] = wsname.empty() ? pair_name : wsname;
                kraken_to_symbol_[pair_name] = altname;
                if (!wsname.empty()) {
                    kraken_to_symbol_[wsname] = altname;
                }
            }
        }

    } catch (...) {
        // Return empty
    }

    return symbols;
}

std::optional<SymbolInfo> KrakenExchange::getSymbolInfo(const std::string& symbol) {
    // Check cache first
    {
        std::lock_guard<std::mutex> lock(symbol_info_mutex_);
        auto it = symbol_info_cache_.find(symbol);
        if (it != symbol_info_cache_.end()) {
            return it->second;
        }
    }

    std::unordered_map<std::string, std::string> params;
    params["pair"] = toKrakenSymbol(symbol);

    auto response = publicGet("/0/public/AssetPairs", params);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();

        auto errors = json.value("error", nlohmann::json::array());
        if (!errors.empty()) {
            return std::nullopt;
        }

        auto result = json["result"];
        for (auto& [pair_name, pair_data] : result.items()) {
            return parseSymbolInfo(pair_name, pair_data);
        }

    } catch (...) {
        // Fall through
    }

    return std::nullopt;
}

std::optional<OrderBook> KrakenExchange::getOrderBookSnapshot(const std::string& symbol, int depth) {
    std::unordered_map<std::string, std::string> params;
    params["pair"] = toKrakenSymbol(symbol);
    params["count"] = std::to_string(depth);

    auto response = publicGet("/0/public/Depth", params);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();

        auto errors = json.value("error", nlohmann::json::array());
        if (!errors.empty()) {
            return std::nullopt;
        }

        auto result = json["result"];
        // Kraken returns with the pair name as key
        for (auto& [pair_name, book_data] : result.items()) {
            OrderBook ob;
            ob.symbol = symbol;
            ob.exchange = "kraken";
            ob.timestamp = currentTimeMs();
            ob.local_timestamp = currentTimeNs();

            auto bids = book_data.value("bids", nlohmann::json::array());
            for (const auto& b : bids) {
                PriceLevel level;
                level.price = std::stod(b[0].get<std::string>());
                level.quantity = std::stod(b[1].get<std::string>());
                ob.bids.push_back(level);
            }

            auto asks = book_data.value("asks", nlohmann::json::array());
            for (const auto& a : asks) {
                PriceLevel level;
                level.price = std::stod(a[0].get<std::string>());
                level.quantity = std::stod(a[1].get<std::string>());
                ob.asks.push_back(level);
            }

            return ob;
        }

    } catch (...) {
        // Fall through
    }

    return std::nullopt;
}

std::vector<Trade> KrakenExchange::getRecentTrades(const std::string& symbol, int limit) {
    std::unordered_map<std::string, std::string> params;
    params["pair"] = toKrakenSymbol(symbol);
    params["count"] = std::to_string(limit);

    auto response = publicGet("/0/public/Trades", params);

    std::vector<Trade> trades;
    if (!response.success) {
        return trades;
    }

    try {
        auto json = response.json();

        auto errors = json.value("error", nlohmann::json::array());
        if (!errors.empty()) {
            return trades;
        }

        auto result = json["result"];
        for (auto& [pair_name, trade_data] : result.items()) {
            if (pair_name == "last") continue;  // Skip the "last" field

            for (const auto& t : trade_data) {
                Trade trade;
                trade.exchange = "kraken";
                trade.symbol = symbol;
                trade.price = std::stod(t[0].get<std::string>());
                trade.quantity = std::stod(t[1].get<std::string>());
                trade.timestamp = static_cast<uint64_t>(std::stod(t[2].get<std::string>()) * 1000);
                trade.side = t[3].get<std::string>() == "b" ? OrderSide::Buy : OrderSide::Sell;
                trade.is_maker = t[4].get<std::string>() == "l";  // 'l' for limit

                trades.push_back(trade);
            }
        }

    } catch (...) {
        // Return empty
    }

    return trades;
}

std::optional<Ticker> KrakenExchange::getTicker(const std::string& symbol) {
    std::unordered_map<std::string, std::string> params;
    params["pair"] = toKrakenSymbol(symbol);

    auto response = publicGet("/0/public/Ticker", params);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();

        auto errors = json.value("error", nlohmann::json::array());
        if (!errors.empty()) {
            return std::nullopt;
        }

        auto result = json["result"];
        for (auto& [pair_name, ticker_data] : result.items()) {
            Ticker ticker;
            ticker.exchange = "kraken";
            ticker.symbol = symbol;

            // Kraken ticker format: a[price, whole_lot_vol, lot_vol], b[...], c[...], etc.
            auto ask = ticker_data["a"];
            auto bid = ticker_data["b"];
            auto last = ticker_data["c"];
            auto volume = ticker_data["v"];
            auto high = ticker_data["h"];
            auto low = ticker_data["l"];
            auto open = ticker_data["o"];

            ticker.ask = std::stod(ask[0].get<std::string>());
            ticker.ask_qty = std::stod(ask[2].get<std::string>());
            ticker.bid = std::stod(bid[0].get<std::string>());
            ticker.bid_qty = std::stod(bid[2].get<std::string>());
            ticker.last = std::stod(last[0].get<std::string>());
            ticker.volume_24h = std::stod(volume[1].get<std::string>());  // Last 24h
            ticker.high_24h = std::stod(high[1].get<std::string>());
            ticker.low_24h = std::stod(low[1].get<std::string>());

            double open_price = std::stod(open.get<std::string>());
            ticker.change_24h = ticker.last - open_price;
            if (open_price > 0) {
                ticker.change_24h_pct = (ticker.change_24h / open_price) * 100;
            }

            ticker.timestamp = currentTimeMs();

            return ticker;
        }

    } catch (...) {
        // Fall through
    }

    return std::nullopt;
}

uint64_t KrakenExchange::getServerTime() {
    auto response = publicGet("/0/public/Time");

    if (response.success) {
        try {
            auto json = response.json();
            auto errors = json.value("error", nlohmann::json::array());
            if (errors.empty()) {
                return json["result"]["unixtime"].get<uint64_t>() * 1000;
            }
        } catch (...) {
            // Fall through
        }
    }

    return 0;
}

// ============================================================================
// Symbol Formatting
// ============================================================================

std::string KrakenExchange::formatSymbol(const std::string& base, const std::string& quote) const {
    // Kraken format: XBT/USD or XXBTZUSD
    // For WebSocket v2, use XBT/USD format
    return base + "/" + quote;
}

std::pair<std::string, std::string> KrakenExchange::parseSymbol(const std::string& symbol) const {
    // Handle XBT/USD format
    size_t slash_pos = symbol.find('/');
    if (slash_pos != std::string::npos) {
        return {symbol.substr(0, slash_pos), symbol.substr(slash_pos + 1)};
    }

    // Handle XXBTZUSD format (X prefix for crypto, Z prefix for fiat)
    // Common fiat currencies: USD, EUR, GBP, CAD, JPY, CHF
    static const std::vector<std::string> fiat_quotes = {"ZUSD", "ZEUR", "ZGBP", "ZCAD", "ZJPY", "ZCHF"};
    static const std::vector<std::string> crypto_quotes = {"XXBT", "XETH", "XLTC"};

    for (const auto& quote : fiat_quotes) {
        if (symbol.length() > quote.length() &&
            symbol.substr(symbol.length() - quote.length()) == quote) {
            std::string base = symbol.substr(0, symbol.length() - quote.length());
            // Remove X prefix from base if present
            if (!base.empty() && base[0] == 'X') {
                base = base.substr(1);
            }
            // Remove Z prefix from quote
            return {base, quote.substr(1)};
        }
    }

    // Simple fallback
    return {symbol, ""};
}

std::string KrakenExchange::toKrakenSymbol(const std::string& symbol) const {
    // Check cache first
    {
        std::lock_guard<std::mutex> lock(symbol_map_mutex_);
        auto it = symbol_to_kraken_.find(symbol);
        if (it != symbol_to_kraken_.end()) {
            return it->second;
        }
    }

    // Handle common conversions
    // BTC -> XBT
    std::string result = symbol;
    size_t pos = result.find("BTC");
    if (pos != std::string::npos) {
        result.replace(pos, 3, "XBT");
    }

    // For WebSocket v2, use slash format
    if (result.find('/') == std::string::npos && result.length() >= 6) {
        // Try to insert slash (e.g., XBTUSD -> XBT/USD)
        std::string base = result.substr(0, 3);
        std::string quote = result.substr(3);
        result = base + "/" + quote;
    }

    return result;
}

std::string KrakenExchange::fromKrakenSymbol(const std::string& kraken_symbol) const {
    // Check cache first
    {
        std::lock_guard<std::mutex> lock(symbol_map_mutex_);
        auto it = kraken_to_symbol_.find(kraken_symbol);
        if (it != kraken_to_symbol_.end()) {
            return it->second;
        }
    }

    // Handle common conversions
    std::string result = kraken_symbol;

    // XBT -> BTC
    size_t pos = result.find("XBT");
    if (pos != std::string::npos) {
        result.replace(pos, 3, "BTC");
    }

    // Remove slash if present
    pos = result.find('/');
    if (pos != std::string::npos) {
        result = result.substr(0, pos) + result.substr(pos + 1);
    }

    // Remove X/Z prefixes
    if (result.length() > 0 && (result[0] == 'X' || result[0] == 'Z')) {
        if (result.length() > 4) {
            // XXBTZUSD -> XBTUSD
            std::string clean;
            for (size_t i = 0; i < result.length(); i++) {
                if (result[i] == 'X' || result[i] == 'Z') {
                    // Skip if it's a prefix (followed by another uppercase)
                    if (i + 1 < result.length() && std::isupper(result[i + 1])) {
                        continue;
                    }
                }
                clean += result[i];
            }
            result = clean;
        }
    }

    return result;
}

// ============================================================================
// Kraken-specific Methods
// ============================================================================

std::string KrakenExchange::getWebSocketsToken() {
    auto response = signedPost("/0/private/GetWebSocketsToken");

    if (!response.success) {
        onError("Failed to get WebSocket token: " + response.error);
        return "";
    }

    try {
        auto json = response.json();

        auto errors = json.value("error", nlohmann::json::array());
        if (!errors.empty()) {
            onError("Failed to get WebSocket token: " + errors[0].get<std::string>());
            return "";
        }

        return json["result"]["token"].get<std::string>();

    } catch (const std::exception& e) {
        onError("Parse WebSocket token failed: " + std::string(e.what()));
        return "";
    }
}

std::unordered_map<std::string, nlohmann::json> KrakenExchange::getAssetInfo() {
    std::unordered_map<std::string, nlohmann::json> assets;

    auto response = publicGet("/0/public/Assets");

    if (!response.success) {
        return assets;
    }

    try {
        auto json = response.json();

        auto errors = json.value("error", nlohmann::json::array());
        if (!errors.empty()) {
            return assets;
        }

        auto result = json["result"];
        for (auto& [asset_name, asset_data] : result.items()) {
            assets[asset_name] = asset_data;
        }

    } catch (...) {
        // Return empty
    }

    return assets;
}

// ============================================================================
// Data Converters
// ============================================================================

Order KrakenExchange::parseOrder(const std::string& order_id, const nlohmann::json& data) {
    Order order;
    order.exchange = "kraken";
    order.order_id = order_id;
    order.symbol = fromKrakenSymbol(data.value("descr", nlohmann::json{}).value("pair", ""));
    order.client_order_id = std::to_string(data.value("userref", 0));

    auto descr = data.value("descr", nlohmann::json{});
    order.side = parseOrderSide(descr.value("type", "buy"));
    order.type = parseOrderType(descr.value("ordertype", "limit"));

    order.quantity = std::stod(data.value("vol", "0"));
    order.filled_quantity = std::stod(data.value("vol_exec", "0"));
    order.remaining_quantity = order.quantity - order.filled_quantity;
    order.price = std::stod(descr.value("price", "0"));
    order.average_price = std::stod(data.value("price", "0"));
    order.stop_price = std::stod(descr.value("price2", "0"));
    order.commission = std::stod(data.value("fee", "0"));

    order.status = parseOrderStatus(data.value("status", "open"));
    order.create_time = static_cast<uint64_t>(std::stod(data.value("opentm", "0")) * 1000);
    order.update_time = static_cast<uint64_t>(std::stod(data.value("closetm", "0")) * 1000);

    order.raw = data;

    return order;
}

OrderStatus KrakenExchange::parseOrderStatus(const std::string& status) {
    if (status == "pending") return OrderStatus::Pending;
    if (status == "open") return OrderStatus::New;
    if (status == "closed") return OrderStatus::Filled;
    if (status == "canceled") return OrderStatus::Cancelled;
    if (status == "expired") return OrderStatus::Expired;
    return OrderStatus::Failed;
}

OrderSide KrakenExchange::parseOrderSide(const std::string& side) {
    return (side == "buy") ? OrderSide::Buy : OrderSide::Sell;
}

OrderType KrakenExchange::parseOrderType(const std::string& type) {
    if (type == "market") return OrderType::Market;
    if (type == "limit") return OrderType::Limit;
    if (type == "stop-loss") return OrderType::StopMarket;
    if (type == "stop-loss-limit") return OrderType::StopLimit;
    if (type == "take-profit") return OrderType::TakeProfitMarket;
    if (type == "take-profit-limit") return OrderType::TakeProfitLimit;
    if (type == "trailing-stop") return OrderType::TrailingStop;
    return OrderType::Limit;
}

TimeInForce KrakenExchange::parseTimeInForce(const std::string& tif) {
    if (tif == "GTC") return TimeInForce::GTC;
    if (tif == "IOC") return TimeInForce::IOC;
    if (tif == "FOK") return TimeInForce::FOK;
    return TimeInForce::GTC;
}

std::string KrakenExchange::orderSideToString(OrderSide side) {
    return side == OrderSide::Buy ? "buy" : "sell";
}

std::string KrakenExchange::orderTypeToString(OrderType type) {
    switch (type) {
        case OrderType::Market: return "market";
        case OrderType::Limit: return "limit";
        case OrderType::StopMarket: return "stop-loss";
        case OrderType::StopLimit: return "stop-loss-limit";
        case OrderType::TakeProfitMarket: return "take-profit";
        case OrderType::TakeProfitLimit: return "take-profit-limit";
        case OrderType::TrailingStop: return "trailing-stop";
        case OrderType::PostOnly: return "limit";  // With post flag
        default: return "limit";
    }
}

std::string KrakenExchange::timeInForceToString(TimeInForce tif) {
    switch (tif) {
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
        default: return "GTC";
    }
}

SymbolInfo KrakenExchange::parseSymbolInfo(const std::string& symbol,
                                            const nlohmann::json& data) {
    SymbolInfo info;
    info.symbol = fromKrakenSymbol(data.value("altname", symbol));
    info.base_asset = data.value("base", "");
    info.quote_asset = data.value("quote", "");
    info.trading_enabled = true;  // Kraken doesn't have this field directly

    // Pair decimals
    int pair_decimals = data.value("pair_decimals", 5);
    int lot_decimals = data.value("lot_decimals", 8);

    info.tick_size = std::pow(10, -pair_decimals);
    info.step_size = std::pow(10, -lot_decimals);
    info.price_precision = pair_decimals;
    info.qty_precision = lot_decimals;

    // Order minimums
    info.min_qty = std::stod(data.value("ordermin", "0"));
    info.min_notional = std::stod(data.value("costmin", "0"));

    // Remove X/Z prefixes from assets
    if (!info.base_asset.empty() && (info.base_asset[0] == 'X' || info.base_asset[0] == 'Z')) {
        info.base_asset = info.base_asset.substr(1);
    }
    if (!info.quote_asset.empty() && (info.quote_asset[0] == 'X' || info.quote_asset[0] == 'Z')) {
        info.quote_asset = info.quote_asset.substr(1);
    }

    // Handle XBT -> BTC
    if (info.base_asset == "XBT") info.base_asset = "BTC";
    if (info.quote_asset == "XBT") info.quote_asset = "BTC";

    return info;
}

// ============================================================================
// Factory Functions
// ============================================================================

std::shared_ptr<KrakenExchange> createKrakenExchange(const ExchangeConfig& config) {
    return std::make_shared<KrakenExchange>(config);
}

std::shared_ptr<KrakenExchange> createKrakenExchange(const KrakenConfig& config) {
    return std::make_shared<KrakenExchange>(config);
}

} // namespace exchange
} // namespace hft
